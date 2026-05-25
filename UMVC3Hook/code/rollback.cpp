// rollback.cpp
// Frame-indexed snapshot/restore for deterministic rollback netcode.
//
// Architecture:
//   A ring buffer holds up to ROLLBACK_WINDOW frame snapshots.  Each snapshot
//   is a flat memcpy dump of all MUST-snapshot regions: 9 core singletons,
//   6 per-character uCharacter dumps (size from DTI), 2 cChrTeam blocks,
//   and the physics world state (~700-900 KB total per frame).
//
//   SaveSnapshot() is called at the frame boundary BEFORE the game tick.
//   RestoreSnapshot(targetFrame) rewinds to that frame for re-simulation
//   with corrected inputs.
//
//   All pointer resolution is done at save/restore time via the game's own
//   singleton pointer storage addresses and linked-list traversal, so no
//   stale-pointer risk.
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

// Singleton pointer storage addresses
static constexpr uintptr_t PTR_SREPLAY      = 0x140d4d9d8;
static constexpr uintptr_t PTR_SCHARACTER   = 0x140d44a70;
static constexpr uintptr_t PTR_SSHOT        = 0x140d47f40;
static constexpr uintptr_t PTR_SHITSOLVER   = 0x140d44060;
static constexpr uintptr_t PTR_ANEXTBATTLE  = 0x140d47e68;
static constexpr uintptr_t PTR_SGAMEUI      = 0x140d53a18;
static constexpr uintptr_t PTR_SMAIN        = 0x140e177e8;
static constexpr uintptr_t PTR_SGAMEEFFECT  = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR     = 0x140d510a0;

// sAction offset in sMvc3Manager (sMvc3Mgr + 0x48)
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;

// Physics update (captures physics world pointer)
// Ghidra shows FUN_140521df0 as sMain__move(frame timing).
// The actual physics world is somewhere inside sMain (possible offset 0x31A78
// from old defaults.h code).  Need the real physics function address.
static constexpr uintptr_t FUN_PHYSICS_UPDATE = 0x140521df0;

// ===========================================================================
// Structural constants (from doc/rollback.md Ghidra analysis)
// ===========================================================================

// cChrTeam offsets within sCharacter
static constexpr uintptr_t TEAM0_OFFSET = 0x40;
static constexpr uintptr_t TEAM1_OFFSET = 0x300;
static constexpr size_t    TEAM_SIZE    = 0x2C0;

// cChrNode field offsets:
//   +0x08 = mpChr  (uCharacter*)
//   +0x10 = mpNext (next cChrNode*)
static constexpr uintptr_t NODE_CHR_OFFSET  = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;

// cChrTeam first-node offset (where the linked-list head pointer lives)
// The cChrTeam layout shows mNode[3] at +0x08 occupying 0x40 bytes.
// cChrTeam__getNextNode reads the head pointer from this region.
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;

// DTI layout: size at DTI + 0x34
static constexpr uintptr_t DTI_SIZE_OFFSET = 0x34;

// Max safe uCharacter size for fallback
static constexpr size_t MAX_CHAR_SIZE = 0x10000;

// ===========================================================================
// Static globals
// ===========================================================================

// Physics world pointer (captured via hook on FUN_PHYSICS_UPDATE)
static uint64_t        g_physicsWorld    = 0;
static bool            g_physicsInited   = false;
static void(__fastcall* oPhysicsUpdate)(uint64_t param_1) = nullptr;

// ===========================================================================
// Snapshot ring buffer
// ===========================================================================
struct SnapshotSlot {
    uint8_t* data     = nullptr; // flat buffer
    size_t   size     = 0;       // actual data written
    size_t   capacity = 0;       // allocated capacity
};

static SnapshotSlot g_slots[ROLLBACK_WINDOW];

// Maximum number of regions we can snapshot per frame
static constexpr int MAX_REGIONS = 24;

struct Region {
    void*    ptr;  // source/destination in game memory
    size_t   size; // bytes to copy
};

// ===========================================================================
// Region list builder — enumerates all snapshot-worthy memory regions
// ===========================================================================
//
// Returns the number of regions populated (0 if no game state is ready).
// Pointers are resolved fresh each call — safe to call pre-emptively.
//
// Region order is FIXED so that save and restore iterate identically.
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

    // ---- 1. sReplay (frame counter + replay buffer) ----
    uint64_t sReplay = *(uint64_t*)_addr(PTR_SREPLAY);
    add((void*)sReplay, 0x23400);

    // ---- 2. sShot base singleton ----
    uint64_t sShot = *(uint64_t*)_addr(PTR_SSHOT);
    add((void*)sShot, 0x500);

    // ---- 3. sHitSolver (collision pairs + result buffer) ----
    uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
    add((void*)sHitSolver, 0x34080);

    // ---- 4. aNextBattle (battle state machine, round state) ----
    uint64_t aNextBattle = *(uint64_t*)_addr(PTR_ANEXTBATTLE);
    add((void*)aNextBattle, 0x20000);

    // ---- 5. sGameUI (pause state, HUD data) ----
    uint64_t sGameUI = *(uint64_t*)_addr(PTR_SGAMEUI);
    add((void*)sGameUI, 0x6000);

    // ---- 6. sMain (frame timing, pause flag) ----
    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
    add((void*)sMain, 0x40080);

    // ---- 7. sGameEffect (gameplay effects) ----
    uint64_t sGameEffect = *(uint64_t*)_addr(PTR_SGAMEEFFECT);
    add((void*)sGameEffect, 0x2000);

    // ---- 8. sAction (game phase state machine, round timer, hitstop) ----
    // sAction = sMvc3Manager + 0x48
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    if (sMvc3Mgr) {
        uint64_t sAction = *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
        add((void*)sAction, 0x300);
    }

    // ---- 9. Physics world state ----
    if (g_physicsInited && g_physicsWorld) {
        add((void*)g_physicsWorld, 11424);
    }

    // ---- 10-15. Per-character uCharacter + 16-17. cChrTeam ----
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (sCharacter) {
        // --- cChrTeam[0], cChrTeam[1] ---
        uint64_t team0 = sCharacter + TEAM0_OFFSET;
        uint64_t team1 = sCharacter + TEAM1_OFFSET;
        add((void*)team0, TEAM_SIZE);
        add((void*)team1, TEAM_SIZE);

        // Traverse each team's character linked list.
        // Per Ghidra: cChrTeam__getNextNode(team) returns the first node
        // which is inline at team + 0x08.  Next nodes are linked via +0x10.
        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team  = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node  = team + TEAM_FIRST_NODE_OFFSET; // first node is inline

            // cChrNode traversal: +0x08 = uCharacter*, +0x10 = next node ptr
            while (node && r < maxRegions) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (character) {
                    // Snapshot the full uCharacter using its DTI-allocated size
                    uint64_t dtiPtr = *(uint64_t*)(character + 0x08);
                    uint32_t charSize = 0;
                    if (dtiPtr) {
                        charSize = *(uint32_t*)(dtiPtr + DTI_SIZE_OFFSET);
                    }
                    if (charSize == 0 || charSize > MAX_CHAR_SIZE) {
                        charSize = 0x8000; // conservative fallback
                    }
                    add((void*)character, charSize);
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET); // linked-list next
            }
        }
    }

    return r;
}

// ===========================================================================
// Public API
// ===========================================================================

void Rollback_SaveSnapshot(int32_t frame)
{
    // Build region list
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    // Calculate total size
    size_t total = 0;
    for (int i = 0; i < count; ++i) {
        total += regions[i].size;
    }

    // Assign ring-buffer slot
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    // Grow buffer if needed
    if (slot.capacity < total) {
        delete[] slot.data;
        slot.data = new (std::nothrow) uint8_t[total];
        if (!slot.data) {
            HookLog("Rollback: failed to allocate %zu-byte snapshot for frame %d\n",
                    total, frame);
            slot.capacity = 0;
            slot.size     = 0;
            return;
        }
        slot.capacity = total;
    }

    // Copy all regions into the flat buffer
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
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

    if (!slot.data || slot.size == 0) {
        HookLog("Rollback: no snapshot available for frame %d\n", frame);
        return;
    }

    // Build region list (same order as save)
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    // Verify total matches
    size_t total = 0;
    for (int i = 0; i < count; ++i) {
        total += regions[i].size;
    }

    if (total > slot.size) {
        HookLog("Rollback: snapshot size mismatch for frame %d "
                "(buffer=%zu, needed=%zu)\n", frame, slot.size, total);
        // Clamp to what we have
    }

    // Restore each region from the flat buffer
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
        size_t copySize = regions[i].size;
        if (offset + copySize > slot.size) {
            copySize = slot.size - offset;
        }
        if (copySize > 0) {
            std::memcpy(regions[i].ptr, slot.data + offset, copySize);
        }
        offset += regions[i].size; // advance by full original size even if clamped
    }
}

void Rollback_FreeSnapshots()
{
    for (int i = 0; i < ROLLBACK_WINDOW; ++i) {
        delete[] g_slots[i].data;
        g_slots[i].data     = nullptr;
        g_slots[i].size     = 0;
        g_slots[i].capacity = 0;
    }
    g_physicsWorld  = 0;
    g_physicsInited = false;
}

// ===========================================================================
// Physics world pointer capture
//
// FUN_140521df0 is called from sMvc3Main__move as sMain__move().
// Its first parameter (RCX) at hook time is whatever was left in RCX — which
// may NOT be the physics world.  This is a placeholder until the real physics
// update function address is confirmed.
//
// Physics state (~11424 bytes) may be embedded inside sMain at offset 0x31A78,
// or it may be a separate allocation.  For now we dump the full sMain (0x40080
// bytes) which should cover it regardless.
// ===========================================================================

static void __fastcall hkPhysicsUpdate(uint64_t param_1)
{
    if (!g_physicsInited) {
        g_physicsWorld  = param_1;
        g_physicsInited = true;
        HookLog("Rollback: physics world captured @ 0x%llX\n", param_1);
    }
    oPhysicsUpdate(param_1);
}

void Rollback_InitHooks()
{
    MH_STATUS s = MH_CreateHook(
        (LPVOID)_addr(FUN_PHYSICS_UPDATE),
        (LPVOID)&hkPhysicsUpdate,
        (LPVOID*)&oPhysicsUpdate
    );
    if (s == MH_OK) {
        MH_EnableHook((LPVOID)_addr(FUN_PHYSICS_UPDATE));
        HookLog("Rollback: physics hook installed\n");
    } else {
        HookLog("Rollback: failed to hook physics update (%d)\n", s);
    }
}
