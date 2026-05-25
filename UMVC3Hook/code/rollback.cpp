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

// ===========================================================================
// Snapshot Region Descriptor & Ring Buffer
// ===========================================================================
struct Region {
    void*    ptr;  // source/destination address in game memory
    size_t   size; // number of bytes
};

struct SnapshotSlot {
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

    // ---- 2. sShot manager base (no dynamic projectile list, base only) ----
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

    // ---- 5. sGameUI — SKIPPED: has internal ptrs ---
    // {
    //     uint64_t sGameUI = *(uint64_t*)_addr(PTR_SGAMEUI);
    //     add((void*)sGameUI, 0x6000);
    // }

    // ---- 6. sMain — SKIPPED: timing/perf data with embedded ptrs ----
    // {
    //     uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
    //     add((void*)sMain, 0x40080);
    // }

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

    if (!slot.data || slot.size == 0 || slot.regionCount == 0) {
        HookLog("Rollback: no snapshot available for frame %d\n", frame);
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

    // Rewind to the starting frame boundary
    Rollback_RestoreSnapshot(startFrame);
    if (g_slots[startFrame % ROLLBACK_WINDOW].size == 0) {
        HookLog("Rollback: re-simulate failed — no snapshot at frame %d\n", startFrame);
        g_isResimulating = true;
        return;
    }

    // Step forward, saving corrected state after each frame
    HookLog("Rollback: re-simulating frames %d → %d\n", startFrame, endFrame);
    for (int32_t f = startFrame; f < endFrame; ++f) {
        g_resimulateCurrentFrame = f;

        // Run the full game update pipeline
        oMvc3Main_move(g_sMvc3Main);

        // Save the corrected state as the snapshot for frame f+1
        Rollback_SaveSnapshot(f + 1);
    }

    g_isResimulating = false;
    HookLog("Rollback: re-simulation complete (%d frames)\n",
            endFrame - startFrame);
}

void Rollback_FreeSnapshots()
{
    for (int i = 0; i < ROLLBACK_WINDOW; ++i) {
        delete[] g_slots[i].data;
        g_slots[i].data        = nullptr;
        g_slots[i].size        = 0;
        g_slots[i].capacity    = 0;
        g_slots[i].regionCount = 0;
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
    oMvc3Main_move(param_1);
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
}
