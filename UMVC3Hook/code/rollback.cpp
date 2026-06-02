// rollback.cpp
// GGPO snapshot/restore callback implementation.
//
// Builds the set of memory regions that constitute the full game state
// (characters, projectiles, hit solver, battle state, etc.), then serializes
// them via Rollback_SaveGameState / Rollback_LoadGameState for GGPO's
// rollback engine.  Also provides Rollback_AdvanceFrame which executes
// one frame of game logic with render and timing patches applied.
//
// The patch functions (Rollback_PatchSleep, PatchPresent, etc.) disable
// frame-pacing and rendering during re-simulation so re-sim frames run
// at full speed.
#include "../pch.h"
#include "rollback.h"
#include "controller.h"
#include "mk10utils.h"
#include "ggpo_netplay.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstring>
#include <new>

extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
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

static constexpr uintptr_t FN_SMVC3MAIN_MOVE = 0x1402594A0;

// ---------------------------------------------------------------------------
// Team / character list constants
// ---------------------------------------------------------------------------
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uintptr_t TEAM0_OFFSET           = 0x40;
static constexpr uintptr_t TEAM1_OFFSET           = 0x300;
static constexpr size_t    TEAM_SIZE              = 0x2C0;
static constexpr uintptr_t NODE_CHR_OFFSET        = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET       = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;

// ===========================================================================
// Decompiled helper functions (inlined from game)
// ===========================================================================

// GetObjectDti — retrieves the DTI (Dynamic Type Info) pointer for an object
// by calling its vtable+0x20 function.
static uint64_t GetObjectDti(uint64_t objPtr)
{
    uint64_t vtable = *(uint64_t*)objPtr;
    if (vtable && vtable >= 0x140000000)
    {
        auto fn = (uint64_t(__fastcall*)(uint64_t))(*(uint64_t*)(vtable + 0x20));
        if (fn && (uint64_t)fn >= 0x140000000)
            return fn(objPtr);
    }
    return 0;
}

// DecodeDtiSize — decodes the object size from a DTI pointer.
// Size is encoded as ((dti+0x30)[31:0] & 0x7FFFFF) << 2.
static uint32_t DecodeDtiSize(uint64_t dtiPtr)
{
    if (!dtiPtr || dtiPtr < 0x140000000) return 0;
    return ((*(uint32_t*)(dtiPtr + 0x30) & 0x7FFFFF) << 2);
}

// ---------------------------------------------------------------------------
// Size / region constants
// ---------------------------------------------------------------------------
static constexpr size_t MAX_CHAR_SIZE        = 0x10000;
static constexpr int    MAX_REGIONS          = 512;
static constexpr int    DYNAMIC_SAFETY       = 128;
static constexpr int    SSHOT_SENTINEL_COUNT = 10;
static constexpr size_t SSHOT_NODE_OFFSET    = 0x1450;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
bool    g_isResimulating = false;

static uint64_t g_sMvc3Main = 0;
static void(__fastcall* oMvc3Main_move)(uint64_t param_1) = nullptr;
static void(__fastcall* oCallSRender)(uint64_t param_1) = nullptr;
static void(__fastcall* oMain_move)(uint64_t param_1) = nullptr;
static void(__fastcall* oCollisionSoundUpdate)(uint64_t param_1) = nullptr;

// ---------------------------------------------------------------------------
// Addresses — sub-function call targets inside the game's frame loop
// ---------------------------------------------------------------------------
static constexpr uintptr_t FN_SMAIN_MOVE            = 0x140521df0;
static constexpr uintptr_t FN_UPDATE_MANAGERS        = 0x140258540;
static constexpr uintptr_t FN_PAD_PROCESS            = 0x140289c30;
static constexpr uintptr_t FN_MVC3MGR_GETINSTANCE    = 0x140001af0;
static constexpr uintptr_t FN_MVC3MGR_GETACTION      = 0x1409629d0;
static constexpr uintptr_t FN_AFTER_GETACTION        = 0x14014f410;
static constexpr uintptr_t FN_CALL_401E0             = 0x1405bb710;
static constexpr uintptr_t FN_CALL_AI                = 0x140647910;
static constexpr uintptr_t FN_SMAIN_TAIL             = 0x140259ea0;
static constexpr uintptr_t FN_CALL_SRENDER           = 0x14053a250;
static constexpr uintptr_t FN_COLLISION_SOUND_UPDATE = 0x1405b72b0;
static constexpr uintptr_t FN_RESET_HITNODES         = 0x140259e90;
static constexpr uintptr_t FN_PAD_METHOD_3D00        = 0x1402a3d00;

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

// ---------------------------------------------------------------------------
// Sleep-patch state: the two Sleep() call sites inside sMain__move
// (0x140521df0).  Each is a 6-byte "CALL [RIP+xxx]" through the IAT.
// ---------------------------------------------------------------------------
static constexpr uintptr_t SLEEP_CALL_FIXED = 0x1405220b2;
static constexpr uintptr_t SLEEP_CALL_VAR   = 0x140521f93;
static constexpr uintptr_t PRESENT_CALL_SITE = 0x14053709c;
static constexpr uintptr_t SWAPCHAIN_CALL_A  = 0x1402598c4;
static constexpr uintptr_t SWAPCHAIN_CALL_B  = 0x1402598d1;
static constexpr uintptr_t SWAPCHAIN_CALL_C  = 0x140537113;
static uint8_t g_savedSleep[2][6] = {};
static uint8_t g_savedPresent[3] = {};
static uint8_t g_savedSwapchain[3][3] = {};
static bool    g_sleepPatched = false;
static bool    g_presentPatched = false;
static bool    g_swapchainPatched = false;

// ===========================================================================
// Region helpers — memory region descriptors for snapshot serialisation
// ===========================================================================
struct Region {
    void*    ptr;
    size_t   size;
};

// TraverseAndAddDynamicList — walks a dynamically-linked list of game objects
// (e.g. characters or effects) and adds each to the region array.
static void TraverseAndAddDynamicList(uint64_t listHead, size_t nextOffset,
                                      int& r, Region* regions, int maxRegions)
{
    uint64_t current = listHead;
    int safety = DYNAMIC_SAFETY;

    while (current && r < maxRegions && safety-- > 0)
    {
        if (current < 0x10000) break;
        uint64_t dtiPtr = GetObjectDti(current);
        uint32_t objSize = DecodeDtiSize(dtiPtr);
        if (objSize == 0 || objSize > MAX_CHAR_SIZE)
            objSize = 0x400;

        regions[r].ptr  = (void*)current;
        regions[r].size = objSize;
        ++r;
        current = *(uint64_t*)(current + nextOffset);
    }
}

// ===========================================================================
// BuildRegions — enumerate all memory regions that must be snapshotted.
//
// Regions captured:
//   1. sReplay frame counter
//   2. sShot (projectile manager)
//   3. sGameConfig small region
//   4. Active uShot projectiles (via sentinel-walk on heap)
//   5. sHitSolver (collision pairs + result buffer)
//   6. aNextBattle (battle state machine)
//   7. sAction (fighting phase state machine)
//   8-9. Per-character uCharacter + cChrTeam (via DTI size decode)
//
// Regions SKIPPED (stale pointer / heap corruption risk):
//   - sGameUI (internal ptrs to ui elements, text buffers)
//   - sMain (physics timing — re-synced separately)
//   - sGameEffect (effect nodes freed between save/restore)
//   - sUnit effect categories (pool allocator free-list corruption)
// ===========================================================================
static int BuildRegions(Region* regions, int maxRegions)
{
    if (!regions || maxRegions < 1) return 0;
    int r = 0;
    auto add = [&](void* ptr, size_t sz)
    {
        if (r < maxRegions && ptr && sz > 0)
        {
            regions[r].ptr  = ptr;
            regions[r].size = sz;
            ++r;
        }
    };

    // ---- 1. sReplay frame counter ----
    uint64_t sReplay = *(uint64_t*)_addr(PTR_SREPLAY);
    if (sReplay)
        add((void*)(sReplay + 0x54), 4);

    // ---- 2. Unknown small region ----
    add((void*)_addr(0x140d765d8), 16);

    // ---- 3. sShot (projectile manager) ----
    uint64_t sShot = *(uint64_t*)_addr(PTR_SSHOT);
    if (sShot)
        add((void*)sShot, 0x1E0);

    // ---- 4. sGameConfig — small region for timer/battle settings ----
    {
        uint64_t sGameConfig = *(uint64_t*)_addr(0x140D50F88);
        if (sGameConfig)
            add((void*)(sGameConfig + 0x4A89C), 10);
    }

    // ---- 5. Active uShot projectiles (dynamic list on heap) ----
    int shotCount = 0;
    {
        for (int si = 0; si < SSHOT_SENTINEL_COUNT; ++si)
        {
            uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + si * 0x28;
            uint64_t sentinelRef = sentinel + 0x08;
            uint64_t curRef = *(uint64_t*)(sentinel + 0x10);
            int safety = DYNAMIC_SAFETY;
            while (curRef != sentinelRef && safety-- > 0)
            {
                if (curRef < 0x10000) break;
                uint64_t uShot = curRef - 0x08 - SSHOT_NODE_OFFSET;
                uint32_t objSize = 0x2200;
                add((void*)uShot, objSize);
                shotCount++;
                curRef = *(uint64_t*)(curRef + 0x08);
            }
        }
    }

    // ---- 6. sHitSolver — collision pairs + result buffer (static array) ----
    {
        uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
        add((void*)sHitSolver, 0x34080);
    }

    // ---- 7. aNextBattle — battle state machine (static manager) ----
    {
        uint64_t aNextBattle = *(uint64_t*)_addr(PTR_ANEXTBATTLE);
        add((void*)aNextBattle, 0x20000);
    }

    // ---- 8. sAction (fighting phase state machine) ----
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    if (sMvc3Mgr)
    {
        uint64_t sAction = *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
        add((void*)sAction, 0x300);
    }

    // ---- 9-10. Per-character uCharacter + cChrTeam ----
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (sCharacter)
    {
        uint64_t team0 = sCharacter + TEAM0_OFFSET;
        uint64_t team1 = sCharacter + TEAM1_OFFSET;
        add((void*)team0, TEAM_SIZE);
        add((void*)team1, TEAM_SIZE);

        for (int ti = 0; ti < 2; ++ti)
        {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;

            while (node && r < maxRegions)
            {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (character)
                {
                    uint64_t dtiPtr = GetObjectDti(character);
                    uint32_t charSize = DecodeDtiSize(dtiPtr);
                    uint32_t finalSize = charSize;
                    if (finalSize == 0 || finalSize > MAX_CHAR_SIZE)
                        finalSize = 0x8000;
                    add((void*)character, finalSize);
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }

    return r;
}

// ===========================================================================
// VTable call helpers — invoke methods on game objects by vtable offset
// ===========================================================================

// VTableCall — calls a vtable method on an object pointer stored at |offset|
// in sMvc3Main.  vtable_offset is the byte offset into the vtable.
static inline void VTableCall(uint64_t sMain, uint32_t memberOffset, uint32_t vtableOffset)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    uint64_t vtable = *(uint64_t*)obj;
    if (!vtable) return;
    auto fn = *(void(__fastcall**)(uint64_t))(vtable + vtableOffset);
    if (fn) fn(obj);
}

// DirectMemberCall — calls a non-vtable direct function with an object from
// an sMain member.
static inline void DirectMemberCall(uint64_t sMain, uint32_t memberOffset, uintptr_t funcAddr)
{
    uint64_t obj = *(uint64_t*)(sMain + memberOffset);
    if (!obj) return;
    ((void(__fastcall*)(uint64_t))_addr(funcAddr))(obj);
}

// MgrSubsysCall — helper: sMvc3Manager::getInstance() -> getter(instance) ->
// vtable(vfnOffset) on result.
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

// ===========================================================================
// Rollback_ResyncMainTiming
// Aligns restored sMain structures with actual performance counters to
// prevent the game loop from computing a negative/excessive frame-time
// delta after re-simulation, which would cause a freeze in the busy-wait
// or in the Sleep-based frame-pacing loop.
// ===========================================================================
void Rollback_ResyncMainTiming(uint64_t sMain)
{
    if (!sMain) return;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Sync high-resolution performance-counter samples with physical counter.
    // sMain+0x40 is the "base QPC" sampled at the PREVIOUS frame boundary;
    // sMain__move computes the frame delta as (current_QPC - base) * 1000 / freq.
    // Without a valid base the first post-re-sim delta will be enormous.
    *(uint64_t*)(sMain + 0x40)     = qpc.QuadPart;  // base QPC for delta
    *(uint64_t*)(sMain + 0x40048)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40078)  = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x70)     = qpc.QuadPart;

    // Reset timing deltas to 60 FPS nominal values so the next real tick
    // sees a well-formed ~16ms frame delta.
    *(uint32_t*)(sMain + 0x38)     = 16;
    *(float*)(sMain + 0x80)        = 0.01666667f;
    *(uint32_t*)(sMain + 0xA8)     = 16;
    *(uint32_t*)(sMain + 0xA4)     = 16;

    // Zero accumulated sleep/deadline calculations so they recompute fresh.
    *(uint64_t*)(sMain + 0x60)     = 0;
    *(uint64_t*)(sMain + 0x68)     = 0;

    // Set tick count so the next real frame advances by exactly 1 step.
    *(uint32_t*)(sMain + 0x48)     = 1;
}

// ===========================================================================
// Patch functions — disable frame-pacing and rendering during re-sim
// ===========================================================================

// Rollback_PatchSleep — NOP the two Sleep() calls inside sMain__move
// (0x1405220b2 and 0x140521f93) so re-sim frames run at full speed without
// the frame-pacing wait.
void Rollback_PatchSleep(bool enable)
{
    if (enable && !g_sleepPatched)
    {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i)
        {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSleep[i], (void*)_addr(sites[i]), 6);
            memset((void*)_addr(sites[i]), 0x90, 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = true;
    }
    else if (!enable && g_sleepPatched)
    {
        DWORD old = 0;
        static const uintptr_t sites[2] = { SLEEP_CALL_FIXED, SLEEP_CALL_VAR };
        for (int i = 0; i < 2; ++i)
        {
            VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSleep[i], 6);
            VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
        }
        g_sleepPatched = false;
    }
}

// Rollback_PatchPresent — NOP / restore the 3-byte CALL [RAX+0x48] inside
// FUN_140537080.  This Present call blocks for ~16ms per frame under DXVK;
// NOPping it during re-sim lets the rest of the render-sync function run
// normally without the blocking Present.
void Rollback_PatchPresent(bool enable)
{
    if (enable && !g_presentPatched)
    {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_savedPresent, (void*)_addr(PRESENT_CALL_SITE), 3);
        memset((void*)_addr(PRESENT_CALL_SITE), 0x90, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = true;
    }
    else if (!enable && g_presentPatched)
    {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)_addr(PRESENT_CALL_SITE), g_savedPresent, 3);
        VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
        g_presentPatched = false;
    }
}

// Rollback_PatchSwapchain — NOP / restore three vtable+0x50 call sites
// that are part of the swapchain / end-frame path.  Each instruction is
// "CALL [RAX+0x50]" = FF 50 50 (3 bytes).  With the Present NOP'd during
// re-sim these may block unnecessarily; NOPping them eliminates remaining
// D3D9/Vulkan overhead from the re-sim path.
void Rollback_PatchSwapchain(bool enable)
{
    static const uintptr_t sites[3] = { SWAPCHAIN_CALL_A, SWAPCHAIN_CALL_B, SWAPCHAIN_CALL_C };
    if (enable && !g_swapchainPatched)
    {
        for (int i = 0; i < 3; ++i)
        {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3, PAGE_EXECUTE_READWRITE, &old);
            memcpy(g_savedSwapchain[i], (void*)_addr(sites[i]), 3);
            memset((void*)_addr(sites[i]), 0x90, 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = true;
    }
    else if (!enable && g_swapchainPatched)
    {
        for (int i = 0; i < 3; ++i)
        {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i]), 3, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i]), g_savedSwapchain[i], 3);
            VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
        }
        g_swapchainPatched = false;
    }
}

// Rollback_PatchDrawCalls — NOP all remaining render/draw call sites inside
// sMvc3Main__move.  Organised by category:
//   vtable+0x48 indirect draw calls (3 bytes each)
//   Direct render-function CALLs (5 bytes each)
//   vtable+0x30 render-logic update (3 bytes)
void Rollback_PatchDrawCalls(bool enable)
{
    static struct { uintptr_t addr; int n; } const sites[] = {
        { 0x1402594d9, 3 }, { 0x1402594e6, 3 }, { 0x1402594f3, 3 },
        { 0x140259515, 3 }, { 0x140259568, 3 }, { 0x140259575, 3 },
        { 0x140259896, 3 },
        { 0x140259525, 5 }, { 0x140259537, 5 }, { 0x140259548, 5 },
        { 0x140259559, 5 },
        { 0x1402598ab, 3 },
    };
    static constexpr int N = sizeof(sites) / sizeof(sites[0]);
    static uint8_t saved[N][6] = {};
    static bool patched = false;

    if (enable && !patched)
    {
        for (int i = 0; i < N; ++i)
        {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, PAGE_EXECUTE_READWRITE, &old);
            memcpy(saved[i], (void*)_addr(sites[i].addr), sites[i].n);
            memset((void*)_addr(sites[i].addr), 0x90, sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
        }
        patched = true;
    }
    else if (!enable && patched)
    {
        for (int i = 0; i < N; ++i)
        {
            DWORD old = 0;
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)_addr(sites[i].addr), saved[i], sites[i].n);
            VirtualProtect((LPVOID)_addr(sites[i].addr), sites[i].n, old, &old);
        }
        patched = false;
    }
}

// ===========================================================================
// GGPO Callbacks
// ===========================================================================

// ---------------------------------------------------------------------------
// Post-Restore Cleanup — clear stale character slot pointers
//
// The flat-memcpy restore of character objects brings back slot arrays at
// offset +0x6748 that reference heap objects (effects, projectiles, etc.)
// which were NOT saved in the snapshot.  Any subsequent call to
// MtObject__replaceSlot_6F30 on these slots will try to call vtable[0x78]
// (release) on a stale/corrupted object -> page fault.
// ---------------------------------------------------------------------------
static void Rollback_ClearStalePointers()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    static constexpr int SLOT_CLEAR_COUNT = 64;
    static constexpr uintptr_t SLOT_ARRAY_OFFSET = 0x6748;

    for (int ti = 0; ti < 2; ++ti)
    {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        while (node)
        {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character)
                memset((void*)(character + SLOT_ARRAY_OFFSET), 0, SLOT_CLEAR_COUNT * sizeof(uint64_t));
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
}

// Rollback_SaveGameState — GGPO save callback.  Builds the full region set
// and serialises it into a flat buffer with region metadata headers.
// Returns the buffer via the output parameter (caller frees via free_buffer).
bool Rollback_SaveGameState(unsigned char** buffer, int* len, int* checksum, int frame)
{
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return false;

    size_t total = sizeof(int);
    for (int i = 0; i < count; i++)
        total += sizeof(uint64_t) + sizeof(uint32_t) + regions[i].size;

    *buffer = (unsigned char*)malloc(total);
    if (!*buffer) return false;
    *len = (int)total;

    unsigned char* ptr = *buffer;
    *(int*)ptr = count; ptr += sizeof(int);
    for (int i = 0; i < count; i++)
    {
        *(uint64_t*)ptr = (uint64_t)regions[i].ptr; ptr += sizeof(uint64_t);
        *(uint32_t*)ptr = (uint32_t)regions[i].size; ptr += sizeof(uint32_t);
        memcpy(ptr, regions[i].ptr, regions[i].size);
        ptr += regions[i].size;
    }

    // Save controller-side state so re-simulation replays identical inputs.
    Controller_SaveRollbackState();

    *checksum = 0;
    return true;
}

// Rollback_LoadGameState — GGPO load callback.  Restores memory regions
// from a flat buffer previously created by SaveGameState.  Uses the saved
// address pointers, not re-resolved locations, ensuring dynamic objects
// (projectiles, effects) map to the same addresses that were captured.
bool Rollback_LoadGameState(unsigned char* buffer, int len)
{
    unsigned char* ptr = buffer;
    int count = *(int*)ptr; ptr += sizeof(int);

    for (int i = 0; i < count; i++)
    {
        uint64_t addr = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
        uint32_t size = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
        if (addr && size > 0 && ptr + size <= buffer + len)
            memcpy((void*)addr, ptr, size);
        ptr += size;
    }

    // Restore controller-side state so the local input shift buffer matches
    // what it was at the time this frame's snapshot was captured.
    Controller_RestoreRollbackState();

    // Clear stale character slot pointers so MtObject__replaceSlot_6F30
    // doesn't try to call vtable[0x78] (release) on objects that were
    // freed between save and restore.
    Rollback_ClearStalePointers();

    return true;
}

// Rollback_AdvanceFrame — GGPO advance_frame callback.  Runs one frame of
// game logic (full sMvc3Main__move pipeline) with timing patches applied
// so re-sim frames run at full speed without Sleep/rendering.
bool Rollback_AdvanceFrame(int flags)
{
    if (!g_sMvc3Main)
    {
        HookLog("Rollback: AdvanceFrame called but sMvc3Main not captured\n");
        return false;
    }

    g_isResimulating = true;

    // Resolve GGPO-synchronized inputs for this frame.
    GGPO_SyncInputs();

    // Apply timing/rendering patches for fast re-sim.
    Rollback_PatchSleep(true);
    Rollback_PatchPresent(true);
    Rollback_PatchSwapchain(true);
    Rollback_PatchDrawCalls(true);

    oMvc3Main_move(g_sMvc3Main);

    Rollback_PatchSleep(false);
    Rollback_PatchPresent(false);
    Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false);

    GGPO_AdvanceFrame();

    g_isResimulating = false;

    return true;
}

// ===========================================================================
// Headless Tick (alternative path, kept for reference)
//
// Runs game-logic sub-functions from sMvc3Main__move but SKIPS: Sleep
// (frame pacing), all vtable+0x48 calls (draw/render), all sRender/
// sPrimitive calls, the Present/swap at 0x140537080.  Safe to call many
// times per real frame for re-simulation.
// ===========================================================================

void Rollback_HeadlessTick(uint64_t sMain)
{
    if (!sMain) return;

    // Frame timing — skip sMain__move, just set tick count
    *(uint32_t*)(sMain + 0x48) = 1;

    // Window-focus/manager state
    ((void(__fastcall*)(uint64_t))_addr(FN_UPDATE_MANAGERS))(sMain);

    // Input processing
    uint64_t syncPad = *(uint64_t*)(sMain + 0x401c0);
    if (syncPad)
        ((void(__fastcall*)(uint64_t))_addr(FN_PAD_PROCESS))(syncPad);

    // Async loader update (vtable+0x30)
    VTableCall(sMain, 0x40260, 0x30);

    // sMvc3Area update (vtable+0x30)
    VTableCall(sMain, 0x40200, 0x30);

    // sMvc3Manager singleton chain (getAction + afterAction)
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr)
        {
            uint64_t action = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_MVC3MGR_GETACTION))(mgr);
            if (action)
                ((void(__fastcall*)(uint64_t))_addr(FN_AFTER_GETACTION))(action);
        }
    }

    // Post-render vtable+0x30 updates (game-logic state)
    VTableCall(sMain, 0x40230, 0x30);
    VTableCall(sMain, 0x401c0, 0x30);
    VTableCall(sMain, 0x40210, 0x30);
    VTableCall(sMain, 0x40228, 0x30);
    VTableCall(sMain, 0x40218, 0x30);
    VTableCall(sMain, 0x401c0, 0x50);
    VTableCall(sMain, 0x401e8, 0x30);
    VTableCall(sMain, 0x40240, 0x30);

    // Manager subsystems
    MgrSubsysCall(FN_GETGAMECONFIG, 0x30);
    MgrSubsysCall(FN_GETGAMEUI, 0x30);

    VTableCall(sMain, 0x401a0, 0x30);  // collision
    VTableCall(sMain, 0x401e0, 0x30);  // mpCollision second call
    MgrSubsysCall(FN_GETHITSOLVER, 0x30);

    VTableCall(sMain, 0x401a0, 0x68);  // physics vfn+0x68
    VTableCall(sMain, 0x401a0, 0x58);  // physics vfn+0x58

    // Direct AI call
    DirectMemberCall(sMain, 0x40240, FN_CALL_AI);

    // Remaining vtable+0x30 member updates
    VTableCall(sMain, 0x401d8, 0x30);
    VTableCall(sMain, 0x40208, 0x30);
    VTableCall(sMain, 0x401b0, 0x30);
    VTableCall(sMain, 0x401f0, 0x30);
    VTableCall(sMain, 0x40248, 0x30);
    VTableCall(sMain, 0x40258, 0x30);

    // Manager subsystem update loop
    {
        uint64_t mgr = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (mgr)
        {
            uint64_t vt = *(uint64_t*)mgr;
            if (vt)
            {
                auto fn = *(void(__fastcall**)(uint64_t))(vt + 0x30);
                if (fn) fn(mgr);
            }
        }
    }

    MgrSubsysCall(FN_GETCHARACTER, 0x30);
    MgrSubsysCall(FN_GETSTAGE, 0x30);
    MgrSubsysCall(FN_GETSHOT, 0x30);
    MgrSubsysCall(FN_GETGAMEEFFECT, 0x30);

    // Hit solver reset
    {
        uint64_t inst = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (inst)
        {
            uint64_t hs = ((uint64_t(__fastcall*)(uint64_t))_addr(FN_GETHITSOLVER))(inst);
            if (hs)
                ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(hs);
        }
    }

    MgrSubsysCall(FN_GETREPLAY, 0x30);
    MgrSubsysCall(FN_GETBATTLESET, 0x30);
    MgrSubsysCall(FN_GETMVC3SET, 0x30);
    MgrSubsysCall(FN_GETNETWORK, 0x30);
    MgrSubsysCall(FN_GETLICENSE, 0x30);

    VTableCall(sMain, 0x401b8, 0x30);
    VTableCall(sMain, 0x401c8, 0x30);
    VTableCall(sMain, 0x401d0, 0x30);

    MgrSubsysCall(FN_GETRANKING, 0x30);
    MgrSubsysCall(FN_GETNETROOM, 0x30);
    MgrSubsysCall(FN_GETEARTHATK, 0x30);

    VTableCall(sMain, 0x40198, 0x30);
    VTableCall(sMain, 0x40270, 0x30);

    // Tail function
    ((void(__fastcall*)(uint64_t))_addr(FN_SMAIN_TAIL))(sMain);

    VTableCall(sMain, 0x401f8, 0x30);

    // Pad methods — input state flush
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa4, 1);
    }
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad)
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))_addr(FN_PAD_METHOD_3D00))(pad, 0xa5, 1);
    }
}

// Rollback_FreeSavedStates — clean up patch state and reset the captured
// sMvc3Main pointer.  Call on netplay disconnect / match end.
void Rollback_FreeSavedStates()
{
    if (g_sleepPatched) Rollback_PatchSleep(false);
    if (g_presentPatched) Rollback_PatchPresent(false);
    if (g_swapchainPatched) Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false);
    g_sMvc3Main = 0;
}

// ===========================================================================
// Hooked functions
// ===========================================================================

// hkMvc3Main_move — intercepts the top-level sMvc3Main__move function
// (0x1402594A0) to capture the sMvc3Main instance pointer for re-simulation
// calls.  Forwards to the original on every call.
static void __fastcall hkMvc3Main_move(uint64_t param_1)
{
    // Capture the main manager pointer on first call
    if (!g_sMvc3Main)
    {
        g_sMvc3Main = param_1;
        HookLog("Rollback: captured sMvc3Main @ 0x%llX\n", param_1);
    }

    oMvc3Main_move(param_1);
}

// hkCollisionSoundUpdate — NOP during re-simulation.  During re-sim the
// sound slot-index increments past per-entry capacity, triggering a
// null-pointer dereference.  Sound is irrelevant during headless re-sim.
static void __fastcall hkCollisionSoundUpdate(uint64_t param_1)
{
    if (g_isResimulating) return;
    if (oCollisionSoundUpdate) oCollisionSoundUpdate(param_1);
}

// hkCallSRender — NOP during re-simulation.  Called at the end of
// sMvc3Main__move as the last operation on the render manager.
// During re-sim we skip this to avoid the ~16ms VSYNC wait per frame.
static void __fastcall hkCallSRender(uint64_t param_1)
{
    if (g_isResimulating) return;
    oCallSRender(param_1);
}

// hkMain_move — NOP during re-simulation.  sMain__move reads QPC, computes
// frame deltas, and spins in a busy-wait for frame pacing.  During re-sim
// we skip it entirely and just set tick count to 0 to prevent the game's
// dispatcher from looping infinitely.
static void __fastcall hkMain_move(uint64_t param_1)
{
    if (g_isResimulating)
    {
        *(uint32_t*)(param_1 + 0x48) = 0;
        return;
    }
    oMain_move(param_1);
}

// ===========================================================================
// Hook Setup — intercept sMvc3Main__move and related functions
// ===========================================================================
void Rollback_InitHooks()
{
    MH_STATUS s;

    // sMvc3Main__move — top-level frame update function
    s = MH_CreateHook((LPVOID)_addr(FN_SMVC3MAIN_MOVE), (LPVOID)&hkMvc3Main_move, (LPVOID*)&oMvc3Main_move);
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(FN_SMVC3MAIN_MOVE));
        HookLog("Rollback: hooked sMvc3Main__move (0x%llX)\n", (uint64_t)_addr(FN_SMVC3MAIN_MOVE));
    }
    else
        HookLog("Rollback: failed to hook sMvc3Main__move (%d)\n", s);

    // Collision sound-event iterator — NOP during re-sim
    s = MH_CreateHook((LPVOID)_addr(FN_COLLISION_SOUND_UPDATE), (LPVOID)&hkCollisionSoundUpdate, (LPVOID*)&oCollisionSoundUpdate);
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(FN_COLLISION_SOUND_UPDATE));

    // Render/present function — NOP during re-sim
    s = MH_CreateHook((LPVOID)_addr(FN_CALL_SRENDER), (LPVOID)&hkCallSRender, (LPVOID*)&oCallSRender);
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(FN_CALL_SRENDER));
        HookLog("Rollback: hooked sRender call (0x%llX)\n", (uint64_t)_addr(FN_CALL_SRENDER));
    }
    else
        HookLog("Rollback: failed to hook sRender call (%d)\n", s);

    // sMain__move (frame-timing / QPC / Sleep / busy-wait) — NOP during re-sim
    s = MH_CreateHook((LPVOID)_addr(FN_SMAIN_MOVE), (LPVOID)&hkMain_move, (LPVOID*)&oMain_move);
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(FN_SMAIN_MOVE));
        HookLog("Rollback: hooked sMain__move (0x%llX)\n", (uint64_t)_addr(FN_SMAIN_MOVE));
    }
    else
        HookLog("Rollback: failed to hook sMain__move (%d)\n", s);
}
