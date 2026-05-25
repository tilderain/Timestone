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
#include "mk10utils.h"
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
static constexpr uintptr_t DTI_SIZE_OFFSET        = 0x34;

// Default list-head offsets inside sShot / sGameEffect managers
// NOTE: sShot's list head is NOT inside the manager — it's at a separate
// global array 0x140d47f90 (5 slots × 0x28 stride).  The 0x38 offset was
// wrong (reads a float = crash).  Dynamic traversal is disabled until the
// correct global addresses are wired in.
//static constexpr uintptr_t SSHOT_LIST_HEAD_OFFSET       = 0x38;
//static constexpr uintptr_t SGAMEEFFECT_LIST_HEAD_OFFSET = 0x38;

static constexpr size_t MAX_CHAR_SIZE  = 0x10000;
static constexpr int    MAX_REGIONS    = 64;
static constexpr int    DYNAMIC_SAFETY = 128; // max projectile/effect nodes

// ===========================================================================
// Global Re-Simulation Indicators & Hook State
// ===========================================================================
bool    g_isResimulating         = false;
int32_t g_resimulateCurrentFrame = 0;

static uint64_t g_sMvc3Main      = 0;
static void(__fastcall* oMvc3Main_move)(uint64_t param_1) = nullptr;
// Trampoline for the collision sound-event iterator (hook crashes during re-sim)
static void(__fastcall* oCollisionSoundUpdate)(uint64_t param_1) = nullptr;

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

        // Read object size from DTI at +0x08
        uint64_t dtiPtr = *(uint64_t*)(current + 0x08);
        uint32_t objSize = 0;
        if (dtiPtr && dtiPtr >= 0x140000000) {
            objSize = *(uint32_t*)(dtiPtr + DTI_SIZE_OFFSET);
        }
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

    // ---- 2. sShot manager — base state (projectile type tracking, etc.) ----
    {
        uint64_t sShot = *(uint64_t*)_addr(PTR_SSHOT);
        add((void*)sShot, 0x500);
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

    // ---- 7. sGameEffect — SKIPPED: has dynamic effect list ---
    // {
    //     uint64_t sGameEffect = *(uint64_t*)_addr(PTR_SGAMEEFFECT);
    //     add((void*)sGameEffect, 0x2000);
    // }

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
                    uint64_t dtiPtr = *(uint64_t*)(character + 0x08);
                    uint32_t charSize = 0;
                    if (dtiPtr && dtiPtr >= 0x140000000) {
                        charSize = *(uint32_t*)(dtiPtr + DTI_SIZE_OFFSET);
                    }
                    if (charSize == 0 || charSize > MAX_CHAR_SIZE) {
                        charSize = 0x8000;
                    }
                    add((void*)character, charSize);
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    return r;
}

// ===========================================================================
// Sleep-Patch & Headless Tick — game-logic-only re-simulation stepping
//
// These are defined here (before ReSimulate/FreeSnapshots) so they can be
// used by the Public API below.
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
static constexpr uintptr_t FN_MGR_GETSOMETHING       = 0x140259270;  // manager -> subsys, vtable+0x30
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
static uint8_t g_savedSleep[2][6] = {};
static bool    g_sleepPatched = false;

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

void Rollback_HeadlessTick(uint64_t sMain)
{
    if (!sMain) return;

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
    // 1. Frame timing (Sleep calls are NOP'd by Rollback_PatchSleep)
    HookLog("Rollback_HeadlessTick: Phase1 step1 — Frame timing\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_SMAIN_MOVE))(sMain);
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

    VTableCall(sMain, 0x401a0, 0x30);  // unknown physics/contact object
    VTableCall(sMain, 0x401e0, 0x30);  // second call on this member

    MgrSubsysCall(FN_GETHITSOLVER, 0x30);    // sHitSolver

    VTableCall(sMain, 0x401a0, 0x68);  // physics vfn+0x68 (contact response)
    VTableCall(sMain, 0x401a0, 0x58);  // physics vfn+0x58 (contact response)

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
    // TEMP DISABLED: re-sim corrupts the job-system dispatch table across all
    // 16 worker threads (crashes jumping to 0x100000000).  Re-enable one
    // subsystem at a time after finding which one triggers the corruption.
    // ==================================================================
#if 0
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
    MgrSubsysCall(FN_MGR_GETSOMETHING, 0x30); // unknown subsys
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
#endif

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

    // Post-restore: clear sShot projectile list sentinels so the game
    // doesn't traverse stale pointers from the restored manager base.
    // sShot__addShotToList uses sentinels at 0x140d47f90 + (slot*0x28),
    // each with mpNext at +0x10. Zeroing that makes the list appear empty.
    {
        for (int si = 0; si < 6; ++si) {
            uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + si * 0x28;
            *(uint64_t*)(sentinel + 0x10) = 0;
        }
    }
}

void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame)
{
    if (startFrame >= endFrame) return;
    if (!g_sMvc3Main) {
        HookLog("Rollback: cannot re-simulate — sMvc3Main pointer not captured\n");
        return;
    }

    g_isResimulating = true;
    Rollback_PatchSleep(true); // NOP Sleep calls so frames run at full speed

    // Rewind to the starting frame boundary, then step forward re-saving
    //Rollback_RestoreSnapshot(startFrame);
    HookLog("Rollback: re-simulating frames %d \u2192 %d\n", startFrame, endFrame);
    for (int32_t f = startFrame; f < endFrame; ++f) {
        g_resimulateCurrentFrame = f;
        Rollback_HeadlessTick(g_sMvc3Main);
        Rollback_SaveSnapshot(f + 1);
    }

    g_isResimulating = false;
    Rollback_PatchSleep(false); // restore Sleep calls
    HookLog("Rollback: re-simulation complete (%d frames)\n",
            endFrame - startFrame);
}

void Rollback_FreeSnapshots()
{
    // Make sure Sleep calls are restored before tearing down
    if (g_sleepPatched)
        Rollback_PatchSleep(false);

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
    // Capture the main manager pointer on first call
    if (!g_sMvc3Main) {
        g_sMvc3Main = param_1;
        HookLog("Rollback: captured sMvc3Main @ 0x%llX\n", param_1);
    }

    if (g_isResimulating) {
        Rollback_HeadlessTick(param_1);
    } else {
        oMvc3Main_move(param_1);
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
    // just return — no sound events during re-simulation
}

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
}
