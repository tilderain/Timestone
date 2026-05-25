# Shot List Analysis for Rollback

Analysis performed via Ghidra MCP on `umvc3.exe.unpacked.exe`.

## Current Architecture

### sShot Singleton

A global singleton manages all shots for both players.

| Address | Symbol | Description |
|---------|--------|-------------|
| `0x140d47f40` | `QWORD_140d47f40` | `sShot` singleton pointer (stored by `sShot__sShot` at `0x140150800`) |
| `0x140d47f90` | `QWORD_140d47f90` | 10-slot shot list array (each slot = 40 bytes, total 400 bytes) |
| `0x140d48120` | `QWORD_140d48120` | 10 CRITICAL_SECTION locks (each 40 bytes, total 400 bytes) |

### sShot Struct Layout (offsets from sShot\*)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| +0x000 | — | cSystem base | vtable, etc. |
| +0x038 | 1024 | Shot ID pool | 0x10 iterations × 8 qwords per iteration = 1024 bytes cleared in `sShot__clearShotIdPool` |
| +0x050 | 400 | Slot array | 10 slots × 40 bytes each (see below) |
| +0x438 | 4 | mKillOrder | int |
| +0x43C | 4 | (unnamed) | int, set to 0 |
| +0x440 | 4 | mMaxSpreadHierarchy | int, init = 1 |
| +0x444 | 4 | mDeltaTimeCoef | float, init = 1.0 (0x3f800000) |
| +0x448 | 0x2000 | Pending unit queue | 512 entries × 16 bytes each (`sShot__enqueuePendingUnit`) |
| +0x2448 | 4 | Pending unit count | int |
| +0x2450 | 0x1000 | Activity array | 0x40 iterations × 64 bytes each = 4096 bytes (`sShot__initActivityArray`) |

### Slot Structure (40 bytes each)

```
+0x00: vtable pointer  (8 bytes, object header for cNodeList)
+0x08: CRITICAL_SECTION + cChrNode linked-list head (32 bytes)
       - +0x08: prev pointer (written by FUN_0x140003d50, read by cChrNode__getChr)
       - +0x10: next pointer (written by InitializeCriticalSection stub at 0x140057030)
       - +0x18: owner/self pointer (written by SetCriticalSectionSpinCount stub at 0x140022720)
```

The CRITICAL_SECTION and the doubly-linked-list head occupy the same memory region starting at offset +0x08 within each slot. Each slot is initialized by `sShot__initSlot` (0x140150bb0).

### Shot Objects

Shots are heap-allocated objects. Each shot embeds a `cChrNode` at a fixed offset:

- `uShot__getListNode(shot_ptr)` → `shot_ptr + 0x1450`
- `sShot__getShotFromNode(node_ptr)` → `*(node_ptr + 0x10)` (reads owner backpointer)

The cChrNode embedded in each shot:
```
shot + 0x1450: prev pointer (points to prev shot or slot list head)
shot + 0x1458: next pointer (points to next shot or slot list head)
shot + 0x1460: owner backpointer (points to shot itself)
```

### Slot Indexing

Slots are indexed by team/player ID (`param_2 & 0xffffffff`). With 10 slots:
- Typically P1 team (3 characters) uses slots 0-2 or similar
- Typically P2 team (3 characters) uses slots 3-5 or similar
- Remaining slots for assists, projectiles, etc.

### Key Operations

| Function | Address | Description |
|----------|---------|-------------|
| `sShot__addShotToList` | `0x140150ad0` | Lock slot, insert shot node via `cChrNode__insertAfter`, unlock |
| `sShot__removeShotFromList` | `0x140151420` | Lock slot, remove shot node via `cChrNode__removeFromList`, unlock |
| `sShot__getFirstShotInSlot` | `0x140151160` | Lock slot, return first shot node, unlock |
| `sShot__spawnShot` | `0x140150e00` | Allocates uShot, inits, adds to list |
| `sShot__spawnShotEx` | `0x140150f40` | Extended spawn with additional init params |
| `sShot__enqueuePendingUnit` | `0x1401512a0` | Enqueue to fixed-size ring buffer at +0x448 (cap 512) |
| `sShot__getInstance` | `0x1400047d0` | Returns singleton pointer `QWORD_140d47f40` |
| `sShot__lockSlot` | `0x140150700` | `EnterCriticalSection` wrapper |
| `sShot__unlockSlot` | `0x1401508a0` | `LeaveCriticalSection` wrapper |
| `sShot__reset` | `0x1401515e0` | Reinit all slots, clear ID pool, clear activity array |
| `sShot__initAllSlots` | `0x140150be0` | Init 10 slots (loop with step = 5 qwords) |
| `uCharacter__updateShotList` | `0x140039d70` | Character-level shot list update |
| `uCharacter__processShots` | `0x1400393f0` | Process shots for a character |
| `rShot__getData` | `0x14014feb0` | Get shot data from shot resource |

### Function Access Summary

| Key | Count |
|-----|-------|
| Direct data references to slot array (0x140d47f90) | 6 unique functions (add, remove, init, getFirst, static init, static dtor) |
| Shot list API functions (lock, unlock, add, remove, spawn, init, reset, etc.) | 13 functions |
| Functions calling sShot\_\_spawnShot/sShot\_\_spawnShotEx | 11 unique call sites |
| Functions calling sShot\_\_addShotToList directly (outside spawn) | 1 (sShot\_\_changeShotSlot — moves shot between slots) |
| Functions calling sShot\_\_removeShotFromList directly | 2 (uShot\_\_dtor + sShot\_\_changeShotSlot) |
| Functions calling sShot\_\_getFirstShotInSlot (iterate the list) | 4 callers |
| Functions calling uShot\_\_getListNode (read embedded node) | 8 callers (add, remove, 6 others for resource path construction) |
| Character virtual dispatch to uCharacter\_\_updateShotList | 118 vtable entries (every character has this in their vtable) |
| Character virtual dispatch to uCharacter\_\_processShots | 121 vtable entries (every character + overrides like uHawkeye\_\_processShots) |

**Total distinct functions that touch the shot list data structure or its API: ~16.** The vtable dispatchers account for the bulk of references (239 .rdata entries) but all route to the same two shared implementations (`uCharacter__updateShotList` and `uCharacter__processShots`).

## Existing Save State (`defaults.h`)

Current `SaveLoadState` saves/restores:
- 6 fighters × ~1000 bytes (in several ranges per fighter)
- mysterytable (9200 bytes)
- block2 (2272 bytes) + team data
- physics (11424 bytes)
- **Shot list is NOT saved or restored**

## The Problem

When rolling back to a previous game state, shots that were spawned during now-discarded frames must be removed, and shots that existed at the save point must be restored. Currently, no shot state is preserved, meaning:
- Shots spawned during rollback-simulated frames persist incorrectly
- Shots that should exist after a rollback are missing
- The per-slot linked lists become corrupted after repeated rollback cycles

## Analysis: Two Approaches

### Option A: Preallocated Array (1024 entries)

Replace the 10 linked lists with 10 fixed-size arrays of 1024 shot pointers each.

**Save:**

```c
// Single memcpy of the entire slot array (or individual slot if per-slot granularity)
memcpy(saved_slots, slot_array, sizeof(slot_array)); // 10 * 1024 * 8 = 80KB

// Deep-copy each shot that exists
for each slot:
    for each entry in slot:
        if entry[i].active:
            memcpy(saved_shot + i, current_shot, sizeof(Shot));
```

**Restore:**

```c
// Restore slot array (pointers)
memcpy(slot_array, saved_slots, sizeof(slot_array));

// Restore each shot
for each slot:
    for each entry in slot:
        if entry[i].active:
            memcpy(current_shot, saved_shot + i, sizeof(Shot));
```

**Pros:**
- O(1) save/restore for the slot metadata
- No pointer fixup needed (fixed memory layout, no heap allocs during gameplay)
- Deterministic memory usage
- Cache-friendly iteration

**Cons:**
- Maximum shot limit of 1024 total (across all 10 slots)
- The current game code uses linked list operations (`cChrNode__insertAfter`, `cChrNode__removeFromList`) — replacing requires hooking or rewriting those functions
- Doubly-linked list semantics embedded in shot creation/destruction need modification
- 80KB just for the pointer array (negligible)
- Large refactor — every function that traverses the shot list needs to use array indexing

### Option B: Snapshot Linked List with Malloc

Keep the existing linked list structure. On save, deep-copy all shot objects and serialize the list topology. On restore, free current shots and recreate the lists.

**Save:**

```c
// snapshot sShot's mutable fields (pending queue, activity array, counters, etc.)
memcpy(saved_sShot, sShot_instance, sizeof(sShot_header));

// deep-copy each slot's linked list
for each slot:
    saved_slots[i].node_count = count_nodes(slot);
    for each node in slot's list:
        shot = get_shot_from_node(node);
        saved_shots[slot_index][n] = malloc_copy(shot, sizeof_UShot);
        saved_nodes[slot_index][n] = { prev_idx, next_idx }; // serialize topology
```

**Restore:**

```c
// restore sShot metadata
memcpy(sShot_instance, saved_sShot, sizeof(sShot_header));

// destroy current shots
for each slot:
    for each node in slot's list:
        free(node);
    init_slot(slot); // reset list head

// rebuild lists from saved data
for each slot:
    for i = 0 to saved_slots[i].node_count:
        // re-alloc each shot
        new_shot = malloc(sizeof_UShot);
        memcpy(new_shot, saved_shots[slot][i], sizeof_UShot);
        // fix node pointers within shot (at shot + 0x1450)
        fixup_list_pointers(new_shot, saved_nodes[slot][i]);
        // insert into slot
        cChrNode__insertAfter(slot_head, new_shot + 0x1450);
```

**Pros:**
- No changes to game's internal shot management code
- No artificial shot limit
- Minimal memory waste (only active shots stored)

**Cons:**
- **Pointers inside shots need fixup**: The cChrNode at +0x1450 contains prev/next pointers to adjacent nodes. After `malloc_copy`, these point to the old addresses and must be fixed to new addresses.
- Shots may contain additional pointers to other game objects (e.g., owner fighter, target) that also need fixup
- malloc/free overhead during every rollback (hundreds per frame)
- Save/restore is O(n) in shot count for both time and space
- The `MVC3Rollback` already uses `std::vector<uint8_t>` for save memory — adding variable-length shot lists complicates the save buffer management
- The existing `SaveLoadPtr` API (`SaveLoadPtr(ptr, save, size)`) assumes fixed-size blocks

### Option C (Hybrid): Preallocated Shot Pool + Linked List Keep

Preallocate a fixed pool of shot objects. Keep the linked list for traversal but use the pool for deterministic allocation. Snapshot the pool bitmap + sShot metadata.

**Save:**

```c
// save: pool bitmap tells us which slots are live
memcpy(saved_pool_bitmap, pool_bitmap, pool_size / 8);
memcpy(saved_sShot, sShot_instance, sizeof(sShot_metadata));

// copy each live shot (no pointer fixup needed — same pool addresses after restore)
for each bit in pool_bitmap:
    if live:
        memcpy(saved_shot_data + i * sizeof(Shot), pool_addr + i * sizeof(Shot), sizeof(Shot));
```

**Restore:**

```c
// restore pool metadata + sShot metadata
memcpy(pool_bitmap, saved_pool_bitmap, pool_size / 8);
memcpy(sShot_instance, saved_sShot, sizeof(sShot_metadata));

// restore each live shot in-place (pool addresses don't change)
for each bit in pool_bitmap:
    if live:
        memcpy(pool_addr + i * sizeof(Shot), saved_shot_data + i * sizeof(Shot), sizeof(Shot));
```

**Pros:**
- No pointer fixup needed (shots stay at same pool addresses)
- O(1) save/restore if using a single pool memcpy and bitfield
- Keeps existing linked list traversal code intact
- Bounded memory (no malloc during gameplay)

**Cons:**
- Linked list topology still uses pointers — if a shot was freed and reallocated at a different pool index between save and restore, the saved linked list pointers are stale
- However: during rollback, the *entire state* is restored, so the pool bitmap and the linked list topology are restored atomically

## Recommendation

**Option C (Preallocated pool + keep linked list) gives the best tradeoff** for this codebase because:

1. The existing linked list operations in the game binary (`cChrNode__insertAfter`, etc.) are embedded in the game's shot management — replacing them requires hooking at minimum, and rewrites at worst.

2. A preallocated pool avoids malloc/free during gameplay AND during rollback, giving deterministic performance.

3. The pool bitmap + sShot metadata can be saved in O(1) with a single `memcpy` block, fitting nicely into the existing `SaveLoadPtr` API which uses `std::vector<uint8_t>` + offset tracking.

4. The shot ID pool at sShot +0x38 (1024 bytes) already hints at a 1024-entry architecture — this may reflect the engine's original design intent.

5. The existing `sShot__enqueuePendingUnit` already has a 512-entry fixed-size queue, suggesting the engine designers expected bounded shot counts.

**Suggested pool size: 1024 entries.** This matches the shot ID pool capacity and is large enough for any conceivable gameplay scenario (current game likely uses <100 concurrent shots).

### Integration with existing SaveLoadState

The existing code already does:

```cpp
void SaveLoadState(bool save) {
    currentSaveLoadOffset = 0;
    // ... fighter saves ...
    SaveLoadPtr(block2, save, 2272);
    SaveLoadPtr(team1, save, teamBackupSize);
    SaveLoadPtr(team2, save, teamBackupSize);
    SaveLoadPtr(physics, save, 11424);
}
```

Adding the shot pool:

```cpp
constexpr int MAX_SHOTS = 1024;
bool shotPoolInited = false;
uint8_t* shotPoolPtr = nullptr;  // preallocated pool

// In SaveLoadState (inside the save/restore loop):
if (!shotPoolInited) {
    auto sShot = *(uintptr_t*)_addr(0x140d47f40);
    shotPoolPtr = (uint8_t*)malloc(MAX_SHOTS * sizeof(rShot)); // or determine at runtime
    shotPoolInited = true;
}
SaveLoadPtr((intptr_t)shotPool, save, MAX_SHOTS * sizeof(rShot));       // shot data
SaveLoadPtr((intptr_t)slot_array, save, 10 * sizeof(Slot));             // list heads
SaveLoadPtr((intptr_t)sShot_metadata, save, sizeof(sShot_metadata));    // pending queue, etc.
```

Or, simpler: directly snapshot the sShot singleton plus slot array:

```cpp
auto sShotSingleton = *(uintptr_t*)_addr(0x140d47f40);
// sShot from +0x38 (ID pool) through +0x2450 (end of activity array)
SaveLoadPtr(sShotSingleton + 0x38, save, 0x2450 + 0x1000 - 0x38);  // the entire sShot data
```

### Unsolved: Shot Internal Pointers

This analysis assumes shots don't contain mutable pointers to game objects that also change during rollback. If shots contain e.g., a `mOwnerFighter*` that points into the fighter struct being restored, that pointer will still be valid after restore (same address). But if shots contain pointers to heap-allocated objects that are ALSO being snapshotted separately, those pointers could become stale.

A full investigation would need to examine the `rShot::cCommonData`, `rShot::cAtkData`, and `rShot::cHitData` structures (all in the `rShot` DTI area at `0x140b117a8`-`0x140b117d0`) for pointer fields.
