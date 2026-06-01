// rollback.cpp
// Frame-indexed snapshot/restore for deterministic rollback netcode.
//
// Architecture:
//   A ring buffer holds up to ROLLBACK_WINDOW frame snapshots.  Each snapshot
//   is a flat memcpy dump of all MUST-snapshot regions followed by per-region
//   metadata (address + size) so that restore uses the EXACT addresses that
//   were saved — avoiding the stale-pointer problem with dynamically allocated
//   objects (projectiles, effects).
//
//   SaveSnapshot() is called at the frame boundary BEFORE the game tick.
//   RestoreSnapshot(targetFrame) rewinds to that frame for re-simulation.
//   ReSimulate(start, end) chains restore + tick + save for each frame.
//
//   Physics state (~11424 bytes) is embedded inside the sMain manager at
//   offset ~0x31A78.  Capturing the full sMain (0x40080 bytes) covers it.
//
//   Dynamic projectile/effect objects are traversed via intrusive linked
//   lists, with DTI-based sizing for each node.
//
// See doc/rollback.md for the full singleton catalog and per-field analysis.
#include "../pch.h"

#include "rollback.h"
#include "controller.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstring>
#include <new>

extern void HookLog(const char* fmt, ...);

// ===========================================================================
// Addresses — absolute VAs, converted at runtime via _addr()
// ===========================================================================
static constexpr uintptr_t PTR_SREPLAY      = 0x140d4d9d8;
static constexpr uintptr_t PTR_SCHARACTER   = 0x140d44a70;
static constexpr uintptr_t PTR_SSHOT        = 0x140d47f40;
static constexpr uintptr_t PTR_SHITSOLVER   = 0x140d44060;
static constexpr uintptr_t PTR_ANEXTBATTLE  = 0x140d47e68;
static constexpr uintptr_t PTR_SGAMEUI      = 0x140d53a18;
static constexpr uintptr_t PTR_SMAIN        = 0x140e177e8;
static constexpr uintptr_t PTR_SGAMEEFFECT  = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR     = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT        = 0x140e17698;

// sMvc3Main__move — the full per-frame update pipeline
static constexpr uintptr_t FN_SMVC3MAIN_MOVE = 0x1402594A0;

// ===========================================================================
// Offsets & Struct Constants
// ===========================================================================
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uintptr_t TEAM0_OFFSET           = 0x40;
static constexpr uintptr_t TEAM1_OFFSET           = 0x300;
static constexpr size_t    TEAM_SIZE              = 0x2C0;

static constexpr uintptr_t NODE_CHR_OFFSET        = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET       = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;

// DTI object size is encoded at +0x30: lower 23 bits = (actual_size >> 2).
// Decode: ((*(uint32_t*)(dti + 0x30) & 0x7FFFFF) << 2).
// Do NOT read +0x34 — that field is the CRC32 class-name hash, not the size.
// IMPORTANT: The DTI pointer is NOT at object+0x08. Per MtObject__isKindOf, it's
// accessed via vtable[4] = *(void**)(*(uint64_t*)object + 0x20).
static uint64_t GetObjectDti(uint64_t objPtr) {
    uint64_t vtable = *(uint64_t*)objPtr;
    if (vtable && vtable >= 0x140000000) {
        auto fn = (uint64_t(__fastcall*)(uint64_t))(*(uint64_t*)(vtable + 0x20));
        if (fn && (uint64_t)fn >= 0x140000000) {
            return fn(objPtr);
        }
    }
    return 0;
}
static uint32_t DecodeDtiSize(uint64_t dtiPtr) {
    if (!dtiPtr || dtiPtr < 0x140000000) return 0;
    return ((*(uint32_t*)(dtiPtr + 0x30) & 0x7FFFFF) << 2);
}

static constexpr size_t MAX_CHAR_SIZE        = 0x10000;
static constexpr int    MAX_REGIONS          = 512;
static constexpr int    DYNAMIC_SAFETY       = 128; // max projectile/effect nodes
static constexpr int    SSHOT_SENTINEL_COUNT = 10;  // 10 sentinel slots starting at 0x140d47f90
static constexpr size_t SSHOT_NODE_OFFSET    = 0x1450; // uShot->cChrNode offset

// ===========================================================================
// Global Re-Simulation Indicators & Hook State
// ===========================================================================
bool    g_isResimulating         = false;
int32_t g_resimulateCurrentFrame = 0;

// Pending re-sim request from FrameStepper_Process (HookUpdate thread).
// hkMvc3Main_move on the main game thread checks/resets these so the
// actual oMvc3Main_move calls run on the correct thread (avoiding D3D
// thread-safety issues in the render path).
int32_t g_resimRequestedStart   = -1;
int32_t g_resimRequestedEnd     = -1;
int32_t g_restoreRequestedFrame = -1;

// Rollback request from controller.cpp — late input arrived that differs
// from what was predicted.  Start/end inclusive frame range.
static int32_t g_rollbackReqStart = -1;
static int32_t g_rollbackReqEnd   = -1;
static uint64_t g_sMvc3Main      = 0;
static void(__fastcall* oMvc3Main_move)(uint64_t param_1) = nullptr;
// Trampoline for the render/present call — NOP'd during re-simulation
static void(__fastcall* oCallSRender)(uint64_t param_1) = nullptr;
// Trampoline for sMain__move (frame-timing / QPC / Sleep / busy-wait)
static void(__fastcall* oMain_move)(uint64_t param_1) = nullptr;
// Trampoline for the collision sound-event iterator (hook crashes during re-sim)
static void(__fastcall* oCollisionSoundUpdate)(uint64_t param_1) = nullptr;

// Frame counter / rollback state from controller.cpp
extern "C" int32_t Controller_GetNetFrameCount();
extern "C" bool    Controller_IsRollbackEnabled();
extern "C" void    Controller_SaveRollbackState();
extern "C" void    Controller_RestoreRollbackState();
extern "C" void    Controller_SilentFrameStart();
extern "C" void    Controller_SilentFrameEnd();
extern "C" void    Controller_SaveInputHistory();
extern "C" void    Controller_RestoreInputHistory();
extern "C" void    Controller_ResolveRemoteInput(int32_t frame);

// ===========================================================================
// Snapshot Region Descriptor & Ring Buffer
// ===========================================================================
struct Region {
    void*    ptr;  // source/destination address in game memory
    size_t   size; // number of bytes
};

struct SnapshotSlot {
    int32_t  frame       = -1;    // game frame this slot holds (-1 = invalid)
    uint8_t* data        = nullptr; // compact linear memory pool
    size_t   size        = 0;       // total bytes of data written
    size_t   capacity    = 0;       // allocated capacity of data[]
    Region   regions[MAX_REGIONS];  // exact addresses captured at save time
    int      regionCount = 0;       // number of populated regions[]
};

static SnapshotSlot g_slots[ROLLBACK_WINDOW];

// ===========================================================================
// Safe Dynamic List Traversal
//
// Walks an intrusive linked list starting at |listHead|, reading each node's
// DTI to determine object size.  Bounded by DYNAMIC_SAFETY to prevent
// infinite loops on corrupted lists.
// ===========================================================================
static void TraverseAndAddDynamicList(uint64_t listHead, size_t nextOffset,
                                      int& r, Region* regions, int maxRegions)
{
    uint64_t current = listHead;
    int safety = DYNAMIC_SAFETY;

    while (current && r < maxRegions && safety-- > 0) {
        // Reject obviously-bogus pointers
        if (current < 0x10000) break;

        // Read object size from DTI (accessed via vtable[4], not object+0x08)
        uint64_t dtiPtr = GetObjectDti(current);
        uint32_t objSize = DecodeDtiSize(dtiPtr);
        if (objSize == 0 || objSize > MAX_CHAR_SIZE) {
            objSize = 0x400; // conservative fallback for typical nodes
        }

        regions[r].ptr  = (void*)current;
        regions[r].size = objSize;
        ++r;

        current = *(uint64_t*)(current + nextOffset);
    }
}

// ===========================================================================
// Region List Builder — enumerates all snapshot-worthy memory
// ===========================================================================
static int BuildRegions(Region* regions, int maxRegions)
{
    if (!regions || maxRegions < 1) return 0;

    int r = 0;
    auto add = [&](void* ptr, size_t sz) {
        if (r < maxRegions && ptr && sz > 0) {
            regions[r].ptr  = ptr;
            regions[r].size = sz;
            ++r;
        }
    };

    // ---- 1. sReplay: frame counter only (sReplay+0x54). Full replay buffer
    //     has internal pointers that go stale. ---
    uint64_t sReplay = *(uint64_t*)_addr(PTR_SREPLAY);
    if (sReplay) {
        add((void*)(sReplay + 0x54), 4); // just the frame counter
    }

    // ---- 1a. Global XorShift RNG state (16 bytes, 4 × uint32) ----
    //     Used by 64/73 call sites across gameplay (hit roll, damage variance,
    //     AI decisions, projectile patterns).  Without restoring this, re-simulation
    //     with different inputs consumes RNG differently, causing desyncs in all
    //     RNG-derived behavior between the original and re-simulated timelines.
    //     Address confirmed from LEA RAX,[0x140d765d8] in MtXorShift__getGlobalState.
    add((void*)_addr(0x140d765d8), 16);

    // ---- 2. sShot manager — vtable + shot ID pool + sentinel slot array only ----
    //     Snapshotting only up to +0x1E0 (end of 10 × 0x28 sentinel slots at +0x50)
    //     to avoid corrupting CRITICAL_SECTION objects at +0x1E0 and the
    //     pointer-heavy activity/pending regions at +0x370+ that go stale.
    uint64_t sShot = *(uint64_t*)_addr(PTR_SSHOT);
    if (sShot) {
        add((void*)sShot, 0x1E0);
    }

    // ---- 2a. Slot active flags (NOT in sShot — stored in sGameConfig+0x4A89C) ---
    //     sShot__setSlotActiveFlag writes to uHaggar__getter_49F0(sGameConfig) + 0x48A6c.
    //     uHaggar__getter_49F0 returns sGameConfig + 0x1E30, so the flags live at
    //     sGameConfig + 0x1E30 + 0x48A6c = sGameConfig + 0x4A89C.  10 slots × 1 byte.
    //     Without restoring these the game sees no active projectiles and frees them,
    //     causing stale-address corruption on the next restore.
    {
        uint64_t sGameConfig = *(uint64_t*)_addr(0x140D50F88);
        if (sGameConfig) {
            add((void*)(sGameConfig + 0x4A89C), 10);
        }
    }

    // ---- 2b. uShot projectile objects — walk each sentinel's circular list ---
    //     Each sentinel starting at 0x140d47f90 + slot*0x28 is an embedded cChrNode.
    //     A sentinel's "node reference" is sentinel+0x08. mpNext at sentinel+0x10
    //     stores the first real uShot's nodeRef (= uShot + SSHOT_NODE_OFFSET + 0x08).
    //     We follow nodeRef->mpNext (at *(nodeRef + 0x08)) back to the sentinel.
    int shotCount = 0;
    {
        for (int si = 0; si < SSHOT_SENTINEL_COUNT; ++si) {
            uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + si * 0x28;
            uint64_t sentinelRef = sentinel + 0x08;
            uint64_t curRef = *(uint64_t*)(sentinel + 0x10);
            int safety = DYNAMIC_SAFETY;
            while (curRef != sentinelRef && safety-- > 0) {
                if (curRef < 0x10000) break;
                uint64_t uShot = curRef - 0x08 - SSHOT_NODE_OFFSET;

                // uShot+0x08 holds list pointers, NOT a DTI descriptor.
                // Using a DTI lookup here reads garbage and produces a 1-byte
                // snapshot, truncating the projectile.  Use a flat safe size.
                uint32_t objSize = 0x2200;

                add((void*)uShot, objSize);
                shotCount++;
                curRef = *(uint64_t*)(curRef + 0x08);
            }
        }
    }
    if (shotCount > 0) {
        HookLog("Snapshot Diagnosis: Captured %d active uShot projectiles on heap\n", shotCount);
    }

    // ---- 3. sHitSolver — collision pairs + result buffer (static array) ----
    {
        uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
        add((void*)sHitSolver, 0x34080);
    }

    // ---- 4. aNextBattle — battle state machine (static manager) ----
    {
        uint64_t aNextBattle = *(uint64_t*)_addr(PTR_ANEXTBATTLE);
        add((void*)aNextBattle, 0x20000);
    }

    // ---- 5. sGameUI — SKIPPED: has internal ptrs (ui elements, text buffers) ---
    // {
    //     uint64_t sGameUI = *(uint64_t*)_addr(PTR_SGAMEUI);
    //     add((void*)sGameUI, 0x6000);
    // }

    // ---- 6. sMain — physics, timing, frame pacing (needed for re-sim consistency) ----
    {
    //    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
    //    add((void*)sMain, 0x40080);
    }

    // ---- 7. sGameEffect — SKIPPED: individual effect nodes are captured ---
    //     via the sUnit category traversal (categories 13-16) below.  Flat-
    //     restoring sGameEffect would crash due to stale effect pointers.

    // ---- 8. sAction (fighting phase state machine) ----
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    if (sMvc3Mgr) {
        uint64_t sAction = *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
        add((void*)sAction, 0x300);
    }

    // ---- 9-14. Per-character uCharacter + 15-16. cChrTeam ----
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (sCharacter) {
        uint64_t team0 = sCharacter + TEAM0_OFFSET;
        uint64_t team1 = sCharacter + TEAM1_OFFSET;
        add((void*)team0, TEAM_SIZE);
        add((void*)team1, TEAM_SIZE);

        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;

            while (node && r < maxRegions) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (character) {
                    uint64_t vtable = *(uint64_t*)character;
                    uint64_t dtiFunc = (vtable >= 0x140000000) ? *(uint64_t*)(vtable + 0x20) : 0;
                    uint64_t dtiPtr = GetObjectDti(character);
                    uint32_t charSize = DecodeDtiSize(dtiPtr);
                    uint32_t finalSize = charSize;
                    if (finalSize == 0 || finalSize > MAX_CHAR_SIZE) {
                        finalSize = 0x8000;
                    }
                    uint32_t* animFields = (uint32_t*)(character + 0x1300);
                   // HookLog("DIAG Character[team %d]: addr=0x%llX vtable=0x%llX dtiFunc=0x%llX dtiPtr=0x%llX rawSize=%u finalSize=%u\n"
                 //           "DIAG   anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
                 //           ti, character, vtable, dtiFunc, dtiPtr, (unsigned)charSize, (unsigned)finalSize,
                 //           animFields[0], animFields[1], animFields[2], animFields[3],
                //            animFields[4], animFields[5], animFields[6], animFields[7],
                 //           animFields[8], animFields[9], animFields[10], animFields[11],
                 //           animFields[12], animFields[13], animFields[14], animFields[15]);
                    add((void*)character, finalSize);
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    // ---- 17-20. sUnit effect categories — SKIPPED (heap corruption) ---
    //     Walking effect categories 13-16 and restoring individual nodes at
    //     their heap addresses corrupts the pool allocator's free-list metadata
    //     when nodes were freed between save and restore (MtHeapAllocator crash).
    //     Instead, we zero the sGameEffect slot tables post-restore to allow
    //     fresh effect allocations without conflicting with old slots.

    return r;
}

// ===========================================================================
// Sleep-Patch & Headless Tick — game-logic-only re-simulation stepping
// ===========================================================================
static constexpr uintptr_t FN_SMAIN_MOVE            = 0x140521df0;
static constexpr uintptr_t FN_UPDATE_MANAGERS        = 0x140258540;
static constexpr uintptr_t FN_PAD_PROCESS            = 0x140289c30;
static constexpr uintptr_t FN_MVC3MGR_GETINSTANCE    = 0x140001af0;
static constexpr uintptr_t FN_MVC3MGR_GETACTION      = 0x1409629d0;
static constexpr uintptr_t FN_AFTER_GETACTION        = 0x14014f410;
static constexpr uintptr_t FN_CALL_401E0             = 0x1405bb710;  // direct call on sMain+0x401e0
static constexpr uintptr_t FN_CALL_AI                = 0x140647910;  // direct call on sMain+0x40240 (AI)
static constexpr uintptr_t FN_SMAIN_TAIL             = 0x140259ea0;  // tail function with sMain as arg
static constexpr uintptr_t FN_CALL_SRENDER           = 0x14053a250;  // call on sMain+0x40190 (sRender)
static constexpr uintptr_t FN_COLLISION_SOUND_UPDATE = 0x1405b72b0;  // collision sound-event iterator
static constexpr uintptr_t FN_RESET_HITNODES         = 0x140259e90;  // j_sHitSolver__resetActiveNodes
static constexpr uintptr_t FN_PAD_METHOD_3D00        = 0x1402a3d00;  // sPad method
static constexpr uintptr_t FN_PAD_METHOD_3DC0        = 0x1402a3dc0;  // sPad method

// sMvc3Manager subsystem getters (for MgrSubsysV30 helper)
static constexpr uintptr_t FN_GETGAMECONFIG          = 0x1401bcc50;
static constexpr uintptr_t FN_GETGAMEUI              = 0x14097d370;
static constexpr uintptr_t FN_GETSHOT                = 0x140683730;
static constexpr uintptr_t FN_GETGAMEEFFECT          = 0x140015690;
static constexpr uintptr_t FN_GETHITSOLVER           = 0x140056130;
static constexpr uintptr_t FN_GETCHARACTER           = 0x1407bcd80;
static constexpr uintptr_t FN_GETSTAGE               = 0x140259460;
static constexpr uintptr_t FN_GETREPLAY              = 0x14013c430;
static constexpr uintptr_t FN_GETBATTLESET           = 0x1408f1f10;
static constexpr uintptr_t FN_GETMVC3SET             = 0x14004b000;
static constexpr uintptr_t FN_GETNETWORK             = 0x1407cda50;
static constexpr uintptr_t FN_GETLICENSE             = 0x14010a200;
static constexpr uintptr_t FN_GETRANKING             = 0x140703370;
static constexpr uintptr_t FN_GETNETROOM             = 0x14030e6c0;
static constexpr uintptr_t FN_GETEARTHATK            = 0x140327a10;

// Sleep-patch state: the two Sleep() call sites inside sMain__move
// (0x140521df0).  Each is a 6-byte "CALL [RIP+xxx]" through the IAT.
static constexpr uintptr_t SLEEP_CALL_FIXED = 0x1405220b2;  // Sleep(dt-1)
static constexpr uintptr_t SLEEP_CALL_VAR   = 0x140521f93;  // Sleep(1)
static constexpr uintptr_t PRESENT_CALL_SITE = 0x14053709c; // CALL [RAX+0x48] inside FUN_140537080
static constexpr uintptr_t SWAPCHAIN_CALL_A  = 0x1402598c4; // vtable+0x50 on sMain+0x40208
static constexpr uintptr_t SWAPCHAIN_CALL_B  = 0x1402598d1; // vtable+0x50 on sMain+0x40270
static constexpr uintptr_t SWAPCHAIN_CALL_C  = 0x140537113; // vtable+0x50 in FUN_140537080 (inside if-block)
static uint8_t g_savedSleep[2][6] = {};
static uint8_t g_savedPresent[3] = {};
static uint8_t g_savedSwapchain[3][3] = {};
static bool    g_sleepPatched = false;
static bool    g_presentPatched = false;
static bool    g_swapchainPatched = false;

// Helper: call a vtable method on an object pointer stored at |offset| in
// sMvc3Main.  vtable_offset is the byte offset into the vtable (e.g. 0x30).
static inline void VTableCall(uint64_t sMain, uint32_t memberOffset, uint32_t vtableOffset)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    uint64_t vtable = *(uint64_t*)obj;
    if (!vtable) return;
    auto fn = *(void(__fastcall**)(uint64_t))(vtable + vtableOffset);
    if (fn) fn(obj);
}

// Helper: call a non-vtable direct function with an object from sMain member.
static inline void DirectMemberCall(uint64_t sMain, uint32_t memberOffset, uintptr_t funcAddr)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    ((void(__fastcall*)(uint64_t))_addr(funcAddr))(obj);
}

// Helper: sMvc3Manager::getInstance() → getter(instance) → vtable(vfnOffset) on result.
static inline void MgrSubsysCall(uintptr_t getterAddr, uint32_t vfnOffset)
{
    uint64_t inst = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    if (!inst) return;
    uint64_t obj = ((uint64_t(__fastcall*)(uint64_t))_addr(getterAddr))(inst);
    if (!obj) return;
    uint64_t vt = *(uint64_t*)obj;
    if (!vt) return;
    auto fn = *(void(__fastcall**)(uint64_t))(vt + vfnOffset);
    if (fn) fn(obj);
}

// Helper: Aligns restored sMain structures with actual performance counters
// to prevent the game loop from computing a negative/excessive frame-time
// delta after re-simulation, which would cause a freeze in the busy-wait
// or in the Sleep-based frame-pacing loop.
static void Rollback_ResyncMainTiming(uint64_t sMain)
{
    if (!sMain) return;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Sync high-resolution performance-counter samples with physical counter.
    // sMain+0x40 is the "base QPC" sampled at the PREVIOUS frame boundary;
    // sMain__move computes the frame delta as (current_QPC - base) * 1000 / freq.
    // Without a valid base the first post-re-sim delta will be enormous and
    // the frame-pacer and/or tick-count multiplier will produce wrong results.
    *(uint64_t*)(sMain + 0x40)     = qpc.QuadPart;  // base QPC for delta
    *(uint64_t*)(sMain + 0x40048)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40078)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x70)     = qpc.QuadPart;

    // Reset timing deltas to 60 FPS nominal values so the next real tick
    // sees a well-formed 16 ms frame delta.
    *(uint32_t*)(sMain + 0x38)     = 16;           // ~16ms frame delta
    *(float*)(sMain + 0x80)        = 0.01666667f;  // 60fps float delta
    *(uint32_t*)(sMain + 0xA8)     = 16;           // current delta
    *(uint32_t*)(sMain + 0xA4)     = 16;           // shifted delta

    // Zero accumulated sleep/deadline calculations so they recompute fresh.
    // These are the "accumulated time" (sMain+0x60) and "deadline" (sMain+0x68)
    // used by the frame-pacing busy-wait logic.  If left stale the game may
    // spin forever waiting for sMain+0x60 < sMain+0x68 to become false.
    *(uint64_t*)(sMain + 0x60)     = 0;
    *(uint64_t*)(sMain + 0x68)     = 0;

    // Set tick count so the next real frame advances by exactly 1 step.
    // (Without this the restored frame count may jump or stall.)
    *(uint32_t*)(sMain + 0x48)     = 1;
}

void Rollback_PatchSleep(bool enable)
{
    if (enable && !g_sleepPatched) {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i) {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSleep[i], (void*)_addr(sites[i]), 6);
            memset((void*)_addr(sites[i]), 0x90, 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = true;
    } else if (!enable && g_sleepPatched) {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i) {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSleep[i], 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = false;
    }
}

void Rollback_PatchPresent(bool enable)
{
    // NOP / restore the 3-byte CALL [RAX+0x48] inside FUN_140537080.
    // This Present call blocks for ~16ms per frame under DXVK; NOPping it
    // during re-sim lets the rest of the render-sync function run normally
    // (setting render-done flags, advancing frame counter, etc.) without
    // the blocking Present.  Required because the game crashes if we skip
    // FUN_140537080 entirely (downstream game-logic depends on its state).
    if (enable && !g_presentPatched) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3,
                       PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_savedPresent, (void*)_addr(PRESENT_CALL_SITE), 3);
        memset((void*)_addr(PRESENT_CALL_SITE), 0x90, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = true;
    } else if (!enable && g_presentPatched) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3,
                       PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)_addr(PRESENT_CALL_SITE), g_savedPresent, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = false;
    }
}

void Rollback_PatchSwapchain(bool enable)
{
    // NOP / restore three vtable+0x50 call sites that are part of the
    // swapchain / end-frame path in sMvc3Main__move and FUN_140537080.
    // Each instruction is "CALL [RAX+0x50]" = FF 50 50 (3 bytes).
    //
    // These swapchain operations expect a Present to have been submitted;
    // with the Present NOP'd during re-sim they may do unnecessary work or
    // block.  NOPping them eliminates any remaining D3D9/Vulkan overhead
    // from the re-sim path.
    static const uintptr_t sites[3] = {
        SWAPCHAIN_CALL_A,
        SWAPCHAIN_CALL_B,
        SWAPCHAIN_CALL_C,
    };
    if (enable && !g_swapchainPatched) {
        for (int i = 0; i < 3; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSwapchain[i], (void*)_addr(sites[i]), 3);
            memset((void*)_addr(sites[i]), 0x90, 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = true;
    } else if (!enable && g_swapchainPatched) {
        for (int i = 0; i < 3; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSwapchain[i], 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = false;
    }
}

// ===========================================================================
// Rollback_PatchDrawCalls — NOP all remaining render/draw call sites
//
// Every render-related CALL inside sMvc3Main__move that is still running
// during re-sim.  Comment out individual entries in the `sites` array
// below to test whether skipping that call site crashes the game.
//
// Sites organised by category:
//   A–G:  vtable+0x48 indirect draw calls (3 bytes each)
//   H–K:  Direct render-function CALLs (5 bytes each)
//   L:    vtable+0x30 render-logic update (3 bytes)
// ===========================================================================
void Rollback_PatchDrawCalls(bool enable)
{
    static struct { uintptr_t addr; int n; const char* label; } const sites[] = {
        // --- vtable+0x48 DRAW calls (FF 50 48 / FF 52 48) ---
        { 0x1402594d9, 3, "draw_401a8"       },
        { 0x1402594e6, 3, "draw_40198"       },
        { 0x1402594f3, 3, "draw_401d8"       },
        { 0x140259515, 3, "draw_gameConfig"  },
        { 0x140259568, 3, "draw_40270"       },
        { 0x140259575, 3, "draw_40208"       },
        { 0x140259896, 3, "draw_401e0"       },
        // --- Direct render-function CALLs (E8 rel32 = 5 bytes) ---
        { 0x140259525, 5, "setRenderTargetAspect" },
        { 0x140259537, 5, "util_C7D0"             },
        { 0x140259548, 5, "setField_40"            },
        { 0x140259559, 5, "getter_7110"            },
        // --- vtable+0x30 render-logic update ---
        { 0x1402598ab, 3, "sRender_vtable+0x30"   },
    };
    static constexpr int N = sizeof(sites) / sizeof(sites[0]);

    // Saved bytes storage — indexed identically to sites[]
    static uint8_t saved[N][6] = {};   // max 6 bytes per site
    static bool    patched = false;

    if (enable && !patched) {
        for (int i = 0; i < N; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy(saved[i], (void*)_addr(sites[i].addr), sites[i].n);
            memset((void*)_addr(sites[i].addr), 0x90, sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
            //HookLog("Rollback: NOP'd draw call %s @ 0x%llX (%d bytes)\n",
            //        sites[i].label, (uint64_t)_addr(sites[i].addr), sites[i].n);
        }
        patched = true;
    } else if (!enable && patched) {
        for (int i = 0; i < N; ++i) {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n,
                           PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i].addr), saved[i], sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
        }
        patched = false;
    } else if (!enable && !patched) {
        return; // nothing to do
    } else {
        //HookLog("Rollback: draw calls already patched, skipping\n");
    }
}

void Rollback_HeadlessTick(uint64_t sMain)
{
    if (!sMain) 
    {
        HookLog("Rollbackheadlesstick: smain is bad?:? :>\n");
        return;
    }

    HookLog("Rollback_HeadlessTick: begin\n");

    // ---- Disable collision sound-event updates during re-sim ----
    // sCollision__move checks the flag at collisionObj+0x73a8 before
    // calling its sound-event iterator (FUN_1405B72B0).  During re-sim the
    // sound slot-index increments past the per-entry capacity, hitting a
    // null-pointer bug in the iterator.  Sound doesn't matter for headless
    // re-simulation; zeroing the flag safely skips the crashing call.
    //
    // (We also hook FUN_1405B72B0 via MinHook below as a belt-and-suspenders
    //  guard — the flag can be re-enabled on worker threads.)

    // ==================================================================
    // Phase 1 — Frame timing + Manager state + Input (always needed)
    // ==================================================================
    // 1. Frame timing — SKIP sMain__move during re-simulation.
    //    sMain__move (0x140521df0) reads real QPC, computes frame deltas,
    //    runs Sleep()/busy-wait for frame pacing, and updates sMain timing
    //    fields (0x38, 0x48, 0x60, 0x68, 0x80, etc.).  Calling it during
    //    re-sim corrupts those fields (it samples real QPC across the re-sim
    //    window) and the first post-re-sim real frame computes a stale delta
    //    that confuses the frame-pacer — causing a permanent hang.
    //
    //    Instead we just set the tick count (sMain+0x48) to 1 so any game
    //    logic that checks it sees a valid advance.  The real sMain__move
    //    runs on the next natural frame boundary after re-sim ends.
    HookLog("Rollback_HeadlessTick: Phase1 step1 — Frame timing (skipping sMain__move)\n");
    *(uint32_t*)(sMain + 0x48) = 1;
    // 2. Window-focus/manager state
    HookLog("Rollback_HeadlessTick: Phase1 step2 — Window-focus/manager state\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_UPDATE_MANAGERS))(sMain);
    // 3. Input processing (CRITICAL — gathers pad / applies historical inputs)
    HookLog("Rollback_HeadlessTick: Phase1 step3 — Input processing\n");
    uint64_t syncPad = *(uint64_t*)(sMain + 0x401c0);
    if (syncPad)
        ((void(__fastcall*)(uint64_t))_addr(FN_PAD_PROCESS))(syncPad);
    // 4. Async loader update (vtable+0x30)
    HookLog("Rollback_HeadlessTick: Phase1 step4 — Async loader update\n");
    VTableCall(sMain, 0x40260, 0x30);

    // ==================================================================
    // [SKIP] vtable+0x48 draw calls on [0x401a8], [0x40198], [0x401d8]
    // [SKIP] FUN_1405bb710 on [0x401e0] — render debug/profiling call
    // [SKIP] sRender/sPrimitive render-setup block
    // [SKIP] vtable+0x48 on [0x40270], [0x40208]
    // [SKIP] FUN_140537080 (Present!) — would crash through kiero hooks
    // ==================================================================

    // ==================================================================
    // Phase 2 — Post-render game-logic updates (mirrors original order)
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase2 begin — Post-render game-logic updates\n");
    // 5. sMvc3Area update (vtable+0x30)
    VTableCall(sMain, 0x40200, 0x30);

    // 6. sMvc3Manager singleton chain (getAction + afterAction)
    HookLog("Rollback_HeadlessTick: Phase2 step6 — sMvc3Manager singleton chain\n");
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr) {
            uint64_t action = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_MVC3MGR_GETACTION))(mgr);
            if (action)
                ((void(__fastcall*)(uint64_t))_addr(FN_AFTER_GETACTION))(action);
        }
    }

    // 7. Post-render vtable+0x30 updates (game-logic state)
    HookLog("Rollback_HeadlessTick: Phase2 step7 — Post-render vtable+0x30 updates\n");
    VTableCall(sMain, 0x40230, 0x30);  // sMvc3Vibration
    VTableCall(sMain, 0x401c0, 0x30);  // sMvc3SyncPad
    VTableCall(sMain, 0x40210, 0x30);  // sMouse
    VTableCall(sMain, 0x40228, 0x30);  // sId
    VTableCall(sMain, 0x40218, 0x30);  // sKeyboard
    VTableCall(sMain, 0x401c0, 0x50);  // sMvc3SyncPad vfn+0x50
    VTableCall(sMain, 0x401e8, 0x30);  // sMvc3Shader2
    VTableCall(sMain, 0x40240, 0x30);  // sAI

    // ---- Manager subsystems (vtable+0x30 updates) ----
    HookLog("Rollback_HeadlessTick: Phase2 — Manager subsystems\n");
    MgrSubsysCall(FN_GETGAMECONFIG, 0x30);   // sGameConfig
    MgrSubsysCall(FN_GETGAMEUI, 0x30);       // sGameUI

    // [SKIP] getShot->vtable+0x48 (draw)
    // [SKIP] getGameEffect->vtable+0x48 (draw)

    VTableCall(sMain, 0x401a0, 0x30);  // collision (sCollision__move)
    VTableCall(sMain, 0x401e0, 0x30);  // mpCollision second call

    MgrSubsysCall(FN_GETHITSOLVER, 0x30);    // sHitSolver

    VTableCall(sMain, 0x401a0, 0x68);  // physics vfn+0x68
    VTableCall(sMain, 0x401a0, 0x58);  // physics vfn+0x58

    // 8. Direct AI call (separate from vtable+0x30 above)
    HookLog("Rollback_HeadlessTick: Phase2 step8 — Direct AI call\n");
    DirectMemberCall(sMain, 0x40240, FN_CALL_AI);

    // 9. Remaining vtable+0x30 member updates
    HookLog("Rollback_HeadlessTick: Phase2 step9 — Remaining vtable+0x30 updates\n");
    VTableCall(sMain, 0x401d8, 0x30);
    VTableCall(sMain, 0x40208, 0x30);
    VTableCall(sMain, 0x401b0, 0x30);
    VTableCall(sMain, 0x401f0, 0x30);
    VTableCall(sMain, 0x40248, 0x30);
    VTableCall(sMain, 0x40258, 0x30);

    // ==================================================================
    // Phase 3 — Manager subsystem update loop (all vtable+0x30)
    //
    // NOTE: The function at 0x140259270 was previously identified as a
    // manager subsystem getter, but Ghidra analysis revealed it is actually
    // Concurrency::details::VirtualProcessor::GetOwningRoot — a ConcRT
    // thread-pool internal. Calling it with vtable+0x30 corrupts the
    // dispatch table across all 16 worker threads, causing crashes at
    // address 0x100000000.  It is intentionally excluded below.
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase3 begin — Manager subsystem update loop\n");
    // getInstance -> vtable+0x30 (manager itself)
    HookLog("Rollback_HeadlessTick: Phase3 — sMvc3Manager vtable+0x30\n");
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr) {
            uint64_t vt = *(uint64_t*)mgr;
            if (vt) {
                auto fn = *(void(__fastcall**)(uint64_t))(vt + 0x30);
                if (fn) fn(mgr);
            }
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 — Character/Stage/Shot/GameEffect vtable+0x30\n");
    MgrSubsysCall(FN_GETCHARACTER, 0x30);    // sCharacter
    MgrSubsysCall(FN_GETSTAGE, 0x30);        // sStage
    MgrSubsysCall(FN_GETSHOT, 0x30);         // sShot (update, NOT draw)
    MgrSubsysCall(FN_GETGAMEEFFECT, 0x30);   // sGameEffect (update, NOT draw)
    // getHitSolver -> j_sHitSolver__resetActiveNodes (direct, not vtable)
    HookLog("Rollback_HeadlessTick: Phase3 — Hit solver reset\n");
    {
        uint64_t inst = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (inst) {
            uint64_t hs = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_GETHITSOLVER))(inst);
            if (hs)
                ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(hs);
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 — Replay/Battle/Network/License vtable+0x30\n");
    MgrSubsysCall(FN_GETREPLAY, 0x30);       // sReplay
    // FN_MGR_GETSOMETHING (0x140259270) intentionally excluded — see note above.
    MgrSubsysCall(FN_GETBATTLESET, 0x30);    // sBattleSetting
    MgrSubsysCall(FN_GETMVC3SET, 0x30);      // sMvc3Setting
    MgrSubsysCall(FN_GETNETWORK, 0x30);      // sNetwork
    MgrSubsysCall(FN_GETLICENSE, 0x30);      // sLicense

    HookLog("Rollback_HeadlessTick: Phase3 — Remaining member vtable+0x30\n");
    VTableCall(sMain, 0x401b8, 0x30);
    VTableCall(sMain, 0x401c8, 0x30);
    VTableCall(sMain, 0x401d0, 0x30);

    HookLog("Rollback_HeadlessTick: Phase3 — Ranking/NetRoom/EarthAtk vtable+0x30\n");
    MgrSubsysCall(FN_GETRANKING, 0x30);      // sRanking
    MgrSubsysCall(FN_GETNETROOM, 0x30);      // sNetworkRoom
    MgrSubsysCall(FN_GETEARTHATK, 0x30);     // sEarthAttack

    // ==================================================================
    // Phase 4 — Tail + finalize
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase4 begin — Tail + finalize\n");
    VTableCall(sMain, 0x40198, 0x30);
    VTableCall(sMain, 0x40270, 0x30);

    // [SKIP] vtable+0x48 on [0x401e0] (draw)

    // Tail function
    HookLog("Rollback_HeadlessTick: Phase4 — sMain tail\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_SMAIN_TAIL))(sMain);

    // [SKIP] sRender game-logic state update (vtable+0x30) — handled inside
    //       FN_SMAIN_MOVE, and the internal allocator isn't snapshot-safe.
    // [SKIP] Direct sRender method — rendering is meaningless during headless re-sim.
    //[SKIP] vtable+0x50 on [0x40208] (render)
    //[SKIP] vtable+0x50 on [0x40270] (render)

    HookLog("Rollback_HeadlessTick: Phase4 — Final vtable+0x30\n");
    VTableCall(sMain, 0x401f8, 0x30);

    // ==================================================================
    // Phase 5 — Pad methods + exit check (SKIP exit during re-sim)
    // ==================================================================
    HookLog("Rollback_HeadlessTick: Phase5 begin — Pad methods\n");
    // sPad::method_3D00(0xa4) and sPad::method_3D00(0xa5) — input state flush
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa4, 1);
    }
    HookLog("Rollback_HeadlessTick: Phase5 — Pad method 0xa5\n");
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa5, 1);
    }
    // [SKIP] The ALT+F4 exit check (sMain__exit call) — we NEVER want to
    // exit during re-simulation. The original checks if F4+Enter was pressed
    // and calls sMain__exit. Skip this entirely.

    HookLog("Rollback_HeadlessTick: end\n");
}

// ===========================================================================
// Public API — Save / Restore / ReSimulate
// ===========================================================================

void Rollback_SaveSnapshot(int32_t frame)
{
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    size_t total = 0;
    for (int i = 0; i < count; ++i) {
        total += regions[i].size;
    }

    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    if (slot.capacity < total) {
        delete[] slot.data;
        slot.data = new (std::nothrow) uint8_t[total];
        if (!slot.data) {
            HookLog("Rollback: failed to allocate %zu-byte snapshot for frame %d\n",
                    total, frame);
            slot.capacity    = 0;
            slot.size        = 0;
            slot.regionCount = 0;
            return;
        }
        slot.capacity = total;
    }

    // Copy data AND save per-region metadata for safe restore
    size_t offset = 0;
    slot.regionCount = count;
    slot.frame = frame;
    for (int i = 0; i < count; ++i) {
        slot.regions[i].ptr  = regions[i].ptr;
        slot.regions[i].size = regions[i].size;
        std::memcpy(slot.data + offset, regions[i].ptr, regions[i].size);
        offset += regions[i].size;
    }
    slot.size = offset;

    // Save controller-side state (input shift buffer, pending, lastRemote)
    // so re-simulation replays identical local inputs.
    Controller_SaveRollbackState();
    
    //HookLog("Rollback: saved frame %d with %d regions, total size %zu bytes\n", frame, count, offset);
}

void Rollback_RestoreSnapshot(int32_t frame)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    // Verify the slot contains data for exactly this frame (ring-buffer safety)
    if (!slot.data || slot.size == 0 || slot.regionCount == 0 || slot.frame != frame) {
        HookLog("Rollback: no snapshot available for frame %d (slot has frame %d)\n",
                frame, slot.frame);
        return;
    }

    // Restore using SAVED region metadata (addresses + sizes), not re-resolved.
    // This ensures dynamic objects (projectiles, effects) map to the exact
    // same memory addresses that were captured, even if the game rebuilt its
    // internal lists between save and restore.
    size_t offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        size_t copySize = slot.regions[i].size;
        if (offset + copySize > slot.size) {
            copySize = slot.size - offset;
        }
        if (copySize > 0) {
            std::memcpy(slot.regions[i].ptr, slot.data + offset, copySize);
        }
        offset += slot.regions[i].size;
    }

    // Log ALL heap regions with their sizes
    offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        uint64_t raddr = (uint64_t)slot.regions[i].ptr;
        size_t rsize = slot.regions[i].size;
     //   HookLog("DIAG Restore region[%d]: addr=0x%llX size=%zu\n", i, raddr, rsize);
        uint64_t maybeVt = (raddr >= 0x100000 && raddr < 0x140000000) ? *(uint64_t*)raddr : 0;
        if (maybeVt >= 0x140000000 && rsize >= 0x1000) {
            uint32_t* animAfter = (uint32_t*)(raddr + 0x1300);
     //       HookLog("DIAG Post-restore char@0x%llX region[%d]: anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
     //               raddr, i,
      //              animAfter[0], animAfter[1], animAfter[2], animAfter[3],
      //              animAfter[4], animAfter[5], animAfter[6], animAfter[7],
          //          animAfter[8], animAfter[9], animAfter[10], animAfter[11],
       //             animAfter[12], animAfter[13], animAfter[14], animAfter[15]);
        }
        offset += rsize;
    }

    // No sentinel fix needed — uShot projectile objects were saved individually
    // (see BuildRegions section 2b) and restored at their original addresses, so
    // the sentinel→uShot linked lists remain self-consistent after the memcpy.

    // Restore controller-side state so the local input shift buffer matches
    // what it was at the time this frame's snapshot was captured.
    Controller_RestoreRollbackState();

    HookLog("Rollback: restored frame %d with %d regions, total size %zu bytes\n", frame, slot.regionCount, offset);
}

void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame)
{
    if (startFrame >= endFrame) return;

    // Re-entry guard: if F7 is still held/read during re-sim, the game's
    // input processing will try to start another ReSimulate from within
    // oMvc3Main_move.  Just return — we're already in one.
    if (g_isResimulating) {
        HookLog("Rollback: ReSimulate re-entered — ignoring (already re-simulating)\n");
        return;
    }

    if (!g_sMvc3Main) {
        HookLog("Rollback: cannot re-simulate — sMvc3Main pointer not captured\n");
        return;
    }

    g_isResimulating = true;

    HookLog("Rollback: [%s] re-simulating frames %d \u2192 %d\n",
            Netplay_IsHost() ? "host" : "client", startFrame, endFrame);

    //Are these really necessary? Might as well leave them in

    Rollback_PatchSleep(true); // NOP Sleep calls so frames run at full speed
    Rollback_PatchPresent(true); // NOP Present inside post-render sync
    Rollback_PatchSwapchain(true); // NOP swapchain vtable+0x50 calls
    Rollback_PatchDrawCalls(true); // NOP remaining render/draw calls

    // Save the live input history before rewinding.  Rollback_RestoreSnapshot
    // internally calls Controller_RestoreRollbackState which overwrites
    // g_oldInputs with startFrame's snapshot; capturing it first prevents
    // the physical inputs from the rolled-back window from being clobbered.
    Controller_SaveInputHistory();

    // 1. Rewind to the starting frame boundary
    Rollback_RestoreSnapshot(startFrame);

    // 2. Step forward (no snapshot saves — the re-simmed frames are now
    //    correct, and the next normal-frame save at curFrame covers us).
    //    sMain__move is hooked (skip frame-pacing) and the render function
    //    is hooked (skip Present), so the full oMvc3Main_move game tick
    //    runs at speed without timing waits or actual rendering.
    //    The hkMvc3Main_move hook returns early during re-sim so the
    //    game's own frame loop doesn't double-execute.
    //    Controller_SilentFrameStart/End maintains the local input shift
    //    buffer without draining packets or sending outbound data.
    for (int32_t f = startFrame; f < endFrame; ++f) {
        g_resimulateCurrentFrame = f;
        // Re-resolve remote input so re-sim uses the latest received data,
        // not the stale prediction that triggered the conflict.
        Controller_ResolveRemoteInput(f);
        Controller_SilentFrameStart();
        oMvc3Main_move(g_sMvc3Main);
        Controller_SilentFrameEnd();
        Controller_LogReSimFrame(f);
    }

    // Restore the local input history so the next outbound packet carries
    // the original timeline, preventing the remote side from seeing spurious
    // conflicts on already-reconciled frames.
    Controller_RestoreInputHistory();

    // 3. Resynchronize performance counter state inside sMain to match
    //    current system time, preventing the game loop from seeing a
    //    negative or excessive frame-time delta that would cause a freeze.
    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
    if (sMain) {
        //Same with this, unsure if this is really needed, might cause lag if disabled
        Rollback_ResyncMainTiming(sMain);
    }

    g_isResimulating = false;
    Rollback_PatchSleep(false); // restore Sleep calls
    Rollback_PatchPresent(false); // restore Present call
    Rollback_PatchSwapchain(false); // restore swapchain calls
    Rollback_PatchDrawCalls(false); // restore render/draw calls
    HookLog("Rollback: re-simulation complete (%d frames)\n",
            endFrame - startFrame);
}

void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame)
{
    g_rollbackReqStart = startFrame;
    g_rollbackReqEnd   = endFrame;
}

void Rollback_FreeSnapshots()
{
    // Make sure patched calls are restored before tearing down
    if (g_sleepPatched)
        Rollback_PatchSleep(false);
    if (g_presentPatched)
        Rollback_PatchPresent(false);
    if (g_swapchainPatched)
        Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false); // restore draw calls (no-op if unpatched)

    for (int i = 0; i < ROLLBACK_WINDOW; ++i) {
        delete[] g_slots[i].data;
        g_slots[i].data        = nullptr;
        g_slots[i].size        = 0;
        g_slots[i].capacity    = 0;
        g_slots[i].regionCount = 0;
        g_slots[i].frame       = -1;
    }
    g_sMvc3Main = 0;
}

// ===========================================================================
// Hook Setup — intercept sMvc3Main__move
//
// We hook the top-level frame update function (0x1402594A0) so we can:
//   1. Capture the sMvc3Main instance pointer for re-simulation calls
//   2. (Future) Gate execution during re-simulation or inject pre-frame logic
// ===========================================================================

static void __fastcall hkMvc3Main_move(uint64_t param_1)
{
    // Log EVERY call so we can tell if the game loop keeps running after re-sim
    static int32_t frameCounter = 0;
    frameCounter++;
   // HookLog("hkMvc3Main_move: frame=%d g_isResimulating=%d\n",
   //         frameCounter, (int)g_isResimulating);

    // Capture the main manager pointer on first call
    if (!g_sMvc3Main) {
        g_sMvc3Main = param_1;
        HookLog("Rollback: captured sMvc3Main @ 0x%llX\n", param_1);
    }

    // Pending snapshot-restore request from FrameStepper_Process (F6 key).
    // Execute at frame boundary (BEFORE oMvc3Main_move) to avoid mid-frame
    // memory corruption when restoring shared game state.
    static bool s_postRestoreTickDump = false;
    if (g_restoreRequestedFrame >= 0) {
        int32_t rf = g_restoreRequestedFrame;
        g_restoreRequestedFrame = -1;
        HookLog("Rollback: executing deferred restore to frame %d\n", rf);
        Rollback_RestoreSnapshot(rf);
        s_postRestoreTickDump = true;
    }

    // Pending re-sim request from HookUpdate thread (FrameStepper_Process).
    // Execute it HERE on the main game thread so sRender path runs safely.
    if (g_resimRequestedStart >= 0) {
        int32_t rs = g_resimRequestedStart;
        int32_t re = g_resimRequestedEnd;
        g_resimRequestedStart = -1;
        g_resimRequestedEnd   = -1;
        Rollback_ReSimulate(rs, re);
        // After re-sim, let the normal frame proceed
    }

    if (g_isResimulating) {
        // Must call oMvc3Main_move during re-sim — the game has an internal
        // dispatch loop that calls 0x1402594A0 (sMvc3Main__move) and expects
        // it to actually process game state.  If we return SKIPPED the
        // dispatch loop condition is never met and it spins forever.
        // Calling oMvc3Main_move here processes the frame normally (timing
        // is NOP'd by hkMain_move + render calls through without issue).
        oMvc3Main_move(param_1);
        return;
    }

    // ==================================================================
    // Auto-snapshot + rollback for netplay (only on main game thread,
    // not during re-sim).  Only active when rollback frames > 0.
    // ==================================================================
    if (Netplay_IsActive() && !g_isResimulating && Controller_IsRollbackEnabled())
    {
        // Only snapshot + rollback when in-game (aNextBattle step >= 2,
        // i.e. countdown or fighting).  Menus/loading screens don't need
        // rollback state and the frame counter jumps around there.
        typedef uint64_t(__fastcall* GetAmaterasu_t)();
        static GetAmaterasu_t getAmaterasu = (GetAmaterasu_t)_addr(0x140001A90);
        uint64_t aNextBattle = getAmaterasu();
        int step = aNextBattle ? *(int*)(aNextBattle + 0xD8) : 0;
        if (step >= 2)
        {
            int32_t curFrame = (int32_t)Controller_GetNetFrameCount();

            // Save snapshot for the current frame BEFORE the game tick
            Rollback_SaveSnapshot(curFrame);

            // Consume any pending rollback request from controller.cpp
            if (g_rollbackReqStart >= 0)
            {
                int32_t rs = g_rollbackReqStart;
                int32_t re = g_rollbackReqEnd;
                g_rollbackReqStart = -1;
                g_rollbackReqEnd   = -1;
                Rollback_ReSimulate(rs, re);
                // After re-sim, re-save the current frame's snapshot so it
                // reflects the corrected state at start of curFrame.
                Rollback_SaveSnapshot(curFrame);
            }
        }
    }

    oMvc3Main_move(param_1);

    // Post-tick diagnostic: dump character animation state after restore+tick
    if (s_postRestoreTickDump) {
        s_postRestoreTickDump = false;
        uint64_t sChar = *(uint64_t*)_addr(PTR_SCHARACTER);
        if (sChar) {
            for (int ti = 0; ti < 2; ++ti) {
                uint64_t team = sChar + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
                uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
                while (node) {
                    uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                    if (chr) {
                        uint32_t* a = (uint32_t*)(chr + 0x1300);
                 //       HookLog("DIAG Post-tick char@0x%llX team%d: anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
                  //              chr, ti,
                 //               a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                 //               a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
                    }
                    node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
                }
            }
        }
    }
}

// ===========================================================================
// Hook — collision sound-event iterator (null-pointer bug workaround)
//
// During re-simulation the sound slot-index increments past per-entry
// capacity, triggering a null-pointer dereference inside this function.
// Sound is irrelevant during headless re-sim, so we intercept and return.
// ===========================================================================
static void __fastcall hkCollisionSoundUpdate(uint64_t param_1)
{
    // Bypass during re-simulation; process normally otherwise
    if (g_isResimulating) {
        return;
    }
    if (oCollisionSoundUpdate) {
        oCollisionSoundUpdate(param_1);
    }
}

// ===========================================================================
// Hook — render/present function (FUN_14053a250)
//
// Called at the end of sMvc3Main__move as the last operation on the render
// manager (sMain+0x40190).  Performs frame-rate checking, Present(), and
// swap-chain management.
//
// During re-sim (g_isResimulating) we skip this entirely to avoid the
// ~16ms VSYNC wait per frame.  The re-sim now runs on the main game thread
// (via g_resimRequestedStart/End consumed in hkMvc3Main_move), so skipping
// this is safe — no thread-safety issue with the render path.
// ===========================================================================
static void __fastcall hkCallSRender(uint64_t param_1)
{
    if (g_isResimulating) {
        return;
    }
    oCallSRender(param_1);
}

// ===========================================================================
// Hook — sMain__move (frame timing / Sleep / busy-wait)
//
// Reads QPC, computes frame deltas, NOPs Sleep calls, and spins in a
// busy-wait for frame pacing.  During re-simulation we skip it entirely
// (just set tick count to 1) because:
//   1. Sleep is already NOP'd by Rollback_PatchSleep, but the busy-wait
//      still spins for the full frame time (~16ms @ 60fps).
//   2. Accumulated/deadline timing values drift across calls, causing
//      progressively longer busy-waits that eventually hang forever.
//   3. We sync QPC state via Rollback_ResyncMainTiming after the loop.
// ===========================================================================
static void __fastcall hkMain_move(uint64_t param_1)
{
    if (g_isResimulating) {
        // Force tick=0 so the game's dispatcher loop won't call
        // sMvc3Main__move again.  Setting tick=1 was the root cause
        // of the infinite loop: the dispatcher checked tick, saw 1,
        // called sMvc3Main__move which hit hkMvc3Main_move (SKIPPED),
        // tick was never consumed, so the dispatcher looped forever.
        *(uint32_t*)(param_1 + 0x48) = 0;
        return;
    }
    oMain_move(param_1);
}

// ===========================================================================
// VTable patch — post-render swapchain operations (0x50 slot)
//
// The two vtable+0x50 calls at the end of sMvc3Main__move (on objects at
// sMain+0x40208 and sMain+0x40270) are swapchain / D3D operations that
// depend on the render function having set up the frame.  When we NOP the
// render function during re-sim these calls hang.  Since we can't easily
// MinHook each vtable entry, we temporarily replace them with a NOP stub
// for the duration of the re-sim loop.
// ===========================================================================
void Rollback_InitHooks()
{
    MH_STATUS s = MH_CreateHook(
        (LPVOID)_addr(FN_SMVC3MAIN_MOVE),
        (LPVOID)&hkMvc3Main_move,
        (LPVOID*)&oMvc3Main_move
    );

    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SMVC3MAIN_MOVE));
        HookLog("Rollback: hooked sMvc3Main__move (0x%llX)\n",
                (uint64_t)_addr(FN_SMVC3MAIN_MOVE));
    } else {
        HookLog("Rollback: failed to hook sMvc3Main__move (%d)\n", s);
    }

    // Collision sound-event iterator — NOP during re-sim
    s = MH_CreateHook(
        (LPVOID)_addr(FN_COLLISION_SOUND_UPDATE),
        (LPVOID)&hkCollisionSoundUpdate,
        (LPVOID*)&oCollisionSoundUpdate
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_COLLISION_SOUND_UPDATE));
        HookLog("Rollback: hooked collision sound update (0x%llX)\n",
                (uint64_t)_addr(FN_COLLISION_SOUND_UPDATE));
    } else {
        HookLog("Rollback: failed to hook collision sound update (%d)\n", s);
    }

    // Render/present function — NOP during re-sim so the full game tick
    // (oMvc3Main_move) can be called from Rollback_ReSimulate without
    // actually drawing to the screen.
    s = MH_CreateHook(
        (LPVOID)_addr(FN_CALL_SRENDER),
        (LPVOID)&hkCallSRender,
        (LPVOID*)&oCallSRender
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_CALL_SRENDER));
        HookLog("Rollback: hooked sRender call (0x%llX)\n",
                (uint64_t)_addr(FN_CALL_SRENDER));
    } else {
        HookLog("Rollback: failed to hook sRender call (%d)\n", s);
    }

    // sMain__move (frame-timing / QPC / Sleep / busy-wait) — NOP during re-sim
    // so the full game tick runs at full speed without frame-pacing waits.
    s = MH_CreateHook(
        (LPVOID)_addr(FN_SMAIN_MOVE),
        (LPVOID)&hkMain_move,
        (LPVOID*)&oMain_move
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FN_SMAIN_MOVE));
        HookLog("Rollback: hooked sMain__move (0x%llX)\n",
                (uint64_t)_addr(FN_SMAIN_MOVE));
    } else {
        HookLog("Rollback: failed to hook sMain__move (%d)\n", s);
    }

}