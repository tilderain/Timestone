# Rollback Netcode — Comprehensive Collation

Collated from: `rollback.md`, `rollback_rollback_context.txt`, Ghidra MCP decompilations,
`rollback.cpp`, `rollback.h`, `framestepper.cpp`, `netplay.cpp`

---

## 1. Architecture Overview

The game calls `sMvc3Main__move(0x1402594A0)` once per frame with sMain as RCX.
This is hooked by MinHook → `hkMvc3Main_move`. During re-simulation, `Rollback_HeadlessTick`
runs instead; during normal gameplay, `oMvc3Main_move` (trampoline to original) runs.

### sMvc3Main__move Full Decompilation (0x1402594A0)

```c
void sMvc3Main__move(longlong param_1) {
    sMain__move();                                              // fps/timing, pause
    sMvc3Main__updateManagers_maybe(param_1);                   // window focus tracking
    sMvc3SyncPad__processFrame_maybe(*(param_1 + 0x401c0));     // sync pad input
    (**(code**)(**(longlong**)(param_1 + 0x40260) + 0x30))();   // PlatformUtil vfn+0x30
    (**(code**)(**(longlong**)(param_1 + 0x401a8) + 0x48))();   // Resource vfn+0x48 (draw)
    (**(code**)(**(longlong**)(param_1 + 0x40198) + 0x48))();   // Tool vfn+0x48 (draw)
    (**(code**)(**(longlong**)(param_1 + 0x401d8) + 0x48))();   // Effect vfn+0x48 (draw)
    FUN_1405bb710(*(param_1 + 0x401e0));                        // Collision
    uVar1 = sMvc3Manager__getInstance();
    plVar2 = (longlong*)sMvc3Manager__getGameConfig(uVar1);
    (**(code**)(*plVar2 + 0x48))(plVar2);                       // GameConfig vfn+0x48 (draw)
    uVar1 = sRender__getInstance();
    sRender__setRenderTargetAspect(uVar1, 2);
    uVar1 = sRender__getInstance();
    sMvc3Main__util_C7D0(uVar1, &local_res8);
    uVar1 = sPrimitive__getInstance();
    sPrimitive__setField_40(uVar1, local_res8);
    uVar1 = sPrimitive__getInstance();
    APeculiarCharacterSelectRelatedFunction__getter_7110(uVar1, local_resc);
    (**(code**)(**(longlong**)(param_1 + 0x40270) + 0x48))();   // Primitive vfn+0x48 (draw)
    (**(code**)(**(longlong**)(param_1 + 0x40208) + 0x48))();   // GpuParticle vfn+0x48 (draw)
    FUN_140537080(*(param_1 + 0x40190));                        // Render/Present
    (**(code**)(**(longlong**)(param_1 + 0x40200) + 0x30))();   // Area vfn+0x30
    uVar1 = sMvc3Manager__getInstance();
    /* WARNING: Subroutine does not return */
    sMvc3Manager__getAction(uVar1);                             // → sAction stepFighting
}
```

Note: `sMvc3Manager__getAction` at the END is marked NORETURN because the action step
is the tail-call that never returns to sMvc3Main__move — it dispatches through sAction's
step machine, then returns to the GAME MAIN LOOP, not to sMvc3Main__move.

---

## 2. sMain Layout (0x140e177e8, 0x40080 bytes)

From `sMain__move` decompilation (0x140521DF0):

| Offset    | Size | Field                    | Notes                        |
|-----------|------|--------------------------|------------------------------|
| +0x38     | 8    | frame delta (ulong)      | timing, from perf counters   |
| +0x48     | 4    | tick count / frame index | derived from perf delta      |
| +0x50     | 4    | time scale (float)       | speed multiplier             |
| +0x54     | 4    | target frame rate (f32)  | 60.0 = 60fps                 |
| +0x58     | 1    | frame lock flag          | 0 = unlocked                 |
| +0x60     | 8    | accumulated frame time   | used for sleep calculation   |
| +0x68     | 8    | last frame time base     | previous accumulated time    |
| +0x70     | 8    | last perf counter        | previous sample              |
| +0x78     | 4    | pause timer countdown    | remaining unpause frames     |
| +0x7c     | 1    | pause flag               | 1 = paused                   |
| +0x7d     | 1    | pause active flag        | derived from timer+flag      |
| +0x80     | 4    | frame delta (float)      | delta for render/animation   |
| +0x98     | 4    | job queue write index    | for job dispatch             |
| +0xa4     | 4    | prev frame delta copy    | previous +0xa8 value         |
| +0xa8     | 4    | cur frame delta          | current frametime delta      |
| +0x40028  | 8    | double timing value      | from perf counter            |
| +0x40048  | 8    | performance counter      | current sample               |
| +0x40070  | 8    | inv perf counter (dbl)   | 1.0 / counter                |
| +0x40078  | 8    | perf counter high-res    | last high-res sample         |

### sMain__move Frame Timing Flow (0x140521DF0):

1. Copy `+0xa8` → `+0xa4` (frame delta shift)
2. Check pause flag `+0x7c`: if set, decrement `+0x78` timer
3. Set `+0x7d` (pause active) = (timer == 0 && pause flag set)
4. `sMain__sampleTime_maybe()` — sample performance counters
5. Read MtPerformanceCounter, store at `+0x40078`
6. Calculate inverse: `+0x40070 = 1.0 / counter`
7. Get perf counter via `sMain__getPerfCounter_maybe()`, store at `+0x40048`
8. Delta = current - previous, store at `+0x40058`
9. Accumulate: `+0x40028` += delta, `+0x40038` = current float delta
10. Calculate frame delta: `+0x38 = ((perfDelta * 1000) / highResFreq)`
11. Calculate tick count: `+0x48 = (frameDelta * timeScale) >> ...`
12. Accumulated frame time: `+0x60 += (perfDelta * timeScale)`
13. If NOT frame-locked (`+0x58 == 0`):
    a. Calculate sleep time from frame budget
    b. Call `Sleep(budget - elapsed)` IF more than 2ms ahead of schedule
    c. **Busy-wait loop** spinning until caught up if behind schedule

### Critical Insight — sMain After Re-Sim:

After 8 headless ticks, sMain's timing state has been updated 8 times:
- `+0x48` (tick count) advanced by 8
- `+0x60` (accumulated frame time) advanced
- Performance counters (`+0x40048`, `+0x40078`) advanced

When the real game loop resumes:
- sMain__move reads current performance counter
- Calculates delta since LAST call (which was during headless tick #8, milliseconds ago)
- Delta is tiny → no Sleep called, busy-wait loop may behave differently
- Frame timing thinks we're on a different frame than expected
- The combination of advanced tick count + tiny delta creates undefined behavior

---

## 3. Collision System & Sound Event Chain

### sCollision__move Full Decompilation

Address: typically called via `VTableCall(sMain, 0x401a0, 0x30)` (the physics/contact object)

```c
// param_1 = the collision object (allocated separately, pointer at sMain+0x401a0)
void sCollision__move(longlong param_1) {
    bool bVar1;
    
    bVar1 = false;
    if (*(char *)(param_1 + 0x47) != '\0') {
        if ((*(char *)(param_1 + 0x30) != '\0') || (BYTE_140e178f0 != 0)) {
            (*(code*)&DAT_00c4f910)(param_1 + 8);  // job dispatch or sync point
        }
        bVar1 = true;
    }
    FUN_1407e1a20(param_1 + 0x43b8);   // update collision table A
    FUN_1405bb3b0(param_1);             // clear/reset collision arrays (0x29 entries × 3 groups)
    FUN_1407e2420(param_1 + 0x6860);    // update collision table B
    if (*(char *)(param_1 + 0x73a8) != '\0') {
        FUN_1405b7280(param_1 + 0x7368);  // ← SOUND EVENT UPDATE CHAIN
    }
    *(undefined2 *)(param_1 + 0x44) = 0x100;
    if ((bVar1) && ((*(char *)(param_1 + 0x30) != '\0' || (BYTE_140e178f0 != 0)))) {
        (*(code*)&DAT_00c4f928)(param_1 + 8);  // job dispatch or sync point
    }
}
```

### Key Collision Object Fields (param_1 points to the collision object)

| Offset        | Size | Field                        | Notes                        |
|---------------|------|------------------------------|------------------------------|
| +0x30         | 1    | mode flag                    | controls job dispatch        |
| +0x44         | 2    | state word                   | set to 1, then 0x100         |
| +0x47         | 1    | active flag                  | gates first dispatch block   |
| +0x280        | ~    | base for collision arrays    | used by FUN_1405bb3b0        |
| +0x43B8       | ~    | collision table A            | updated by FUN_1407e1a20     |
| +0x6860       | ~    | collision table B            | updated by FUN_1407e2420     |
| +0x7368       | ~    | **sound event sub-struct**   | passed to FUN_1405b7280      |
| +0x73a8       | 1    | **sound event enable flag**  | gates FUN_1405b7280 call     |

### Sound Event Sub-Struct (collisionObj + 0x7368)

Parsed from `FUN_1405B7280`, `FUN_1405B72B0`, `FUN_1405B75C0`, `FUN_1405B7660`:

| Offset | Size | Field              | Notes                              |
|--------|------|--------------------|------------------------------------|
| +0x00  | 4    | field_0x00         | ???                                |
| +0x10  | 4    | **entry count**    | number of collision sound entries  |
| +0x20  | 8    | **entries ptr**    | pointer to array of sound entries  |
| +0x28  | 4    | **slot index**     | current slot index (incremented each call) |
| +0x2C  | 4    | prev slot index    | previous value before increment    |
| +0x30  | 4    | bitfield           | shifted left per entry, starts at 1 |
| +0x3C  | 1    | **dispatch mode**  | 0 = direct (FUN_1405B87A0), ≠0 = job dispatch |
| +0x3D  | 1    | param for 75C0     | passed to FUN_1405B6320           |
| +0x3E  | 1    | param for 7660     | passed to FUN_1405B6510           |

### FUN_1405B7280 — Sound Event Update Driver

```c
void FUN_1405b7280(longlong param_1) {
    FUN_1405b72b0();          // SOUND JOB DISPATCH (our hook point, 0x1405B72B0)
    FUN_1405b75c0(param_1);   // collision sound processing A (always runs)
    FUN_1405b7660(param_1);   // collision sound processing B (always runs)
    *(int *)(param_1 + 0x28) = *(int *)(param_1 + 0x28) + 1;  // INCREMENT SLOT INDEX
    return;
}
```

### FUN_1405B72B0 — Sound Job Dispatcher (HOOKED)

```c
void FUN_1405b72b0(longlong param_1) {
    uVar1 = *(uint *)(param_1 + 0x10);    // entry count
    if (*(char *)(param_1 + 0x3c) == '\0') {
        // === QUICK PATH (no job dispatch) ===
        for (each entry) {
            if (bounds check and entry valid) {
                lVar5 = *(entry->array + slotIndex * 8);
            }
            if (lVar5 && *(char*)(lVar5 + 8) != '\0') {
                FUN_1405b87a0();              // direct call, no jobs
            }
        }
    } else {
        // === JOB DISPATCH PATH (queue + execute) ===
        for (each entry) {
            if (bounds check and entry valid && slotIndex < entry->capacity) {
                uVar2 = *(entry->array + slotIndex * 8);  // sound data
            }
            if (uVar2 && *(char*)(uVar2 + 8) != '\0') {
                // Submit 0x29 (41) jobs to sMain's job queue
                for (i = 0; i < 0x29; i++) {
                    jobAddr = sMain + sMain->jobCounter * 0x20;
                    sMain->jobCounter++;
                    jobAddr->index = i;
                    jobAddr->data  = uVar2;
                    jobAddr->callback = FUN_1405b8a30;
                    jobAddr->flag = 0;
                }
            }
        }
        /* WARNING: Subroutine does not return */
        sMain__executeJob(sMain, 2);         // execute queued jobs
    }
}
```

Critical detail: `sMain__executeJob(sMain, 2)` — the parameter `2` likely signals
the worker threads to process the queued jobs. If jobs are never queued (because
our hook returns early), the worker threads have nothing to do, which is harmless.
But if the main thread waits for a specific job to complete that was never queued,
that's a deadlock.

### FUN_1405B75C0 — Collision Sound Processing A

```c
void FUN_1405b75c0(longlong param_1) {
    uVar2 = *(uint *)(param_1 + 0x10);    // entry count
    uVar1 = *(param_1 + 0x3d);            // mode byte
    *(param_1 + 0x30) = 1;                // reset bitfield
    for (i = 0; i < uVar2; i++) {
        if (bounds check and entry valid) {
            if (slotIndex < entry->capacity) {
                lVar3 = entry->array[slotIndex];
            } else { lVar3 = 0; }
            if (lVar3 && *(char*)(lVar3 + 8) != '\0') {
                FUN_1405b6320(lVar3, uVar1);  // sound processing
            }
        }
        bitfield <<= 1;                    // shift bitfield for next entry
    }
}
```

### FUN_1405B7660 — Collision Sound Processing B

```c
void FUN_1405b7660(longlong param_1) {
    uVar2 = *(uint *)(param_1 + 0x10);    // entry count
    uVar1 = *(param_1 + 0x3e);            // mode byte
    *(param_1 + 0x30) = 1;                // reset bitfield
    for (i = 0; i < uVar2; i++) {
        if (bounds check and entry valid) {
            if (slotIndex < entry->capacity) {
                lVar3 = entry->array[slotIndex];
            } else { lVar3 = 0; }
            if (lVar3 && *(char*)(lVar3 + 8) != '\0'
                && *(int*)(lVar3 + 0x24b0) + *(int*)(lVar3 + 0xfbc) != 0) {
                FUN_1405b6510(lVar3, uVar1);  // extended sound processing
            }
        }
        bitfield <<= 1;
    }
}
```

---

## 4. Crash History & Analysis

### Crash 1: Sound Iterator Null-Pointer (0x1405B730F)

**Location:** Inside FUN_1405B72B0, reading `*(unknown + 8)` where `unknown` is null.
This is the `uVar2` computed from `*(entry->array + slotIndex * 8)` when `slotIndex >= entry->capacity`.

**Root Cause:** The slot index at `param_1+0x28` increments each frame (in FUN_1405B7280).
During re-sim, 8 headless ticks call sCollision__move 8 times, incrementing the slot
index 8 times. Eventually the index exceeds the per-entry capacity, the bounds check
(`slotIndex < entry->capacity`) fails, `lVar5` is set to 0, and the code reads
`*(0 + 8)` → null-pointer dereference.

**Fix:** MinHook on FUN_1405B72B0 (0x1405B72B0) → `hkCollisionSoundUpdate` returns
immediately. This prevents the slot index from being used for sound job dispatch,
but the index still increments in FUN_1405B7280.

**Status:** FIXED — crash does not occur.

### Crash 2: Render Allocator Write (0x1406393E7)

**Location:** Writing to address `0x8000` (near-null) inside a render function called
from `VTableCall(sMain, 0x40190, 0x30)` (sRender).

**Root Cause:** The headless tick calls VTableCall on sRender which accesses render
allocator globals. These globals are not snapshot-safe — during re-sim they point to
stale/incorrect addresses.

**Fix:** Skip VTableCall(sMain, 0x40190, 0x30) and DirectMemberCall(sMain, 0x40190, FN_CALL_SRENDER)
in the headless tick.

**Status:** FIXED — commented out in Phase 4.

### Crash 3: Job System Corruption (0x100000000)

**Location:** All 16 worker threads crash jumping to address 0x100000000 / 0x100000002.

**Root Cause:** The Phase 3 manager subsystem functions (MgrSubsysCall chain in the
original pipeline) modify shared job dispatch tables that persist across frames.
During re-sim, these tables get corrupted, and the next time the worker threads
try to dispatch, they jump through corrupted function pointers.

The specific function `FN_MGR_GETSOMETHING` (0x140259270) was identified in Ghidra
as `Concurrency::details::VirtualProcessor::GetOwningRoot` — NOT a manager subsystem
at all. This is a Concurrency Runtime (ConcRT) function used for thread pool
management. Calling it as a "subsystem vtable+0x30 update" directly corrupts
the ConcRT internal state that coordinates the 16 worker threads.

**Fix:** Phase 3 wrapped in `#if 0`.

**Status:** BYPASSED — Phase 3 disabled. Root cause identified (ConcRT corruption),
needs proper fix.

### Issue 4: Game Freezes After Re-Simulation (CURRENT)

**Symptoms:**
- Re-simulation runs 8 headless ticks without crashing
- All headless tick phases complete
- After re-sim returns, the game stops advancing frames (frozen display)
- The collision sound hook (hkCollisionSoundUpdate) is ALWAYS active, not gated by g_isResimulating

**Root Cause Analysis:**

The most likely cause: **sMain internal state desync**.

During 8 headless ticks, sMain's timing state (`+0x48` tick count, `+0x60` accumulated
frame time, `+0x40048` performance counters) advances by 8 frames. After re-sim, the
real game loop resumes and calls `sMvc3Main__move` → `oMvc3Main_move` → `sMain__move`.

`sMain__move` calculates frame delta against the LAST performance counter sample
(which was from headless tick #8, microseconds ago). This produces:
- A tiny delta → no Sleep → busy-wait loop spins
- `+0x48` tick count is 8+ ahead of what the animation/render state expects

Contributing factors:
1. sMain is NOT snapshotted (commented out in BuildRegions)
2. `Rollback_RestoreSnapshot(startFrame)` is commented out in ReSimulate
3. The collision sound hook returns during normal gameplay too (not just re-sim)
4. Collision object state (slot index, internal arrays) was modified by headless
   ticks and never restored
5. Phase 3 ConcRT state may have been subtly corrupted even though the crash
   is avoided by `#if 0`

**Proposed Fix A — Backup/Restore sMain:**
Save a full 0x40080-byte backup of sMain before ReSimulate, restore it after.
This preserves the re-sim's frame state in the snapshot ring buffer while allowing
the game to continue normally.

**Proposed Fix B — Gate the Collision Sound Hook:**
Gate `hkCollisionSoundUpdate` with `g_isResimulating`:
```c
if (!g_isResimulating) {
    oCollisionSoundUpdate(param_1);
}
```

**Proposed Fix C — Include sMain in Snapshots:**
Uncomment sMain in BuildRegions and the restore in ReSimulate for proper
snapshot-driven rollback.

---

## 5. sMvc3Manager Offset Map

From Ghidra decompilations of all MgrSubsys getters:

| Offset | Getter Address   | Name                      | Notes               |
|--------|------------------|---------------------------|---------------------|
| +0x48  | 0x1409629D0      | getAction                 | sAction singleton   |
| +0x78  | 0x1401BCC50      | getGameConfig             | sGameConfig         |
| +0x90  | 0x14013C430      | getReplay                 | sReplay             |
| +0xA8  | 0x14010A200      | getLicense                | sLicense (2)        |
| +0xB0  | 0x1407CDA50      | getNetwork                | sNetwork            |
| +0xB8  | 0x14030E6C0      | getNetworkRoom            | sNetworkRoom        |
| +0xC8  | 0x140327A10      | getEarthAttack            | sEarthAttack        |

Other subsystem getters (offsets not confirmed in Ghidra):
- getCharacter: 0x1407BCD80
- getStage: 0x140259460
- getShot: 0x140683730
- getGameEffect: 0x140015690
- getHitSolver: 0x140056130
- getBattleSet: 0x1408F1F10
- getMvc3Set: 0x14004B000
- getRanking: 0x140703370

---

## 6. Key Function Address Reference

### Hook Points
| Function                       | Address       | Status                    |
|-------------------------------|---------------|---------------------------|
| sMvc3Main__move               | 0x1402594A0   | HOOKED (hkMvc3Main_move)  |
| sCollisionSoundUpdate         | 0x1405B72B0   | HOOKED (hkCollisionSoundUpdate) |

### sMain Member Functions
| Function                       | Address       | Notes                     |
|-------------------------------|---------------|---------------------------|
| sMain__move (frame timing)    | 0x140521DF0   | Called from Phase 1 step1 |
| sMvc3Main__updateManagers    | 0x140258540   | Phase 1 step2             |
| sMvc3Main__tail              | 0x140259EA0   | Phase 4 tail              |
| sMain__executeJob            | 0x1405B8A30?  | Job processing function   |

### Collision Chain
| Function                       | Address       | Notes                     |
|-------------------------------|---------------|---------------------------|
| sCollision__move              | at collObj vtable+0x30 | Called via VTableCall(sMain, 0x401a0, 0x30) |
| FUN_1405BB710 (on +0x401e0)  | 0x1405BB710   | Called in original pipeline on mpCollision |
| FUN_1405B7F20 (on +0x7368)   | 0x1405B7F20   | Resets sound event arrays |
| FUN_1405B7280                 | 0x1405B7280   | Drives sound event chain  |
| FUN_1405B72B0 (hook target)  | 0x1405B72B0   | Sound job dispatcher      |
| FUN_1405B75C0                 | 0x1405B75C0   | Sound processing A        |
| FUN_1405B7660                 | 0x1405B7660   | Sound processing B        |
| FUN_1405B87A0 (quick path)   | 0x1405B87A0   | Direct call (no job)      |
| FUN_1405B8A30 (job callback) | 0x1405B8A30   | Job callback function     |
| FUN_1405BB3B0                 | 0x1405BB3B0   | Clears collision arrays   |

### Hit Solver
| Function                       | Address       | Notes                     |
|-------------------------------|---------------|---------------------------|
| j_sHitSolver__resetActiveNodes| 0x140259E90   | MtArray__deleteAll(+0x38, 1) |

### Manager Subsystem Getters (for Phase 3)
| Function                       | Address       | Notes                     |
|-------------------------------|---------------|---------------------------|
| sMvc3Manager__getGameConfig   | 0x1401BCC50   | +0x78 on sMvc3Mgr        |
| sMvc3Manager__getGameUI       | 0x14097D370   |                          |
| sMvc3Manager__getShot         | 0x140683730   |                          |
| sMvc3Manager__getGameEffect   | 0x140015690   |                          |
| sMvc3Manager__getHitSolver    | 0x140056130   |                          |
| sMvc3Manager__getCharacter    | 0x1407BCD80   |                          |
| sMvc3Manager__getStage        | 0x140259460   |                          |
| sMvc3Manager__getReplay       | 0x14013C430   | +0x90 on sMvc3Mgr        |
| sMvc3Manager__getBattleSet    | 0x1408F1F10   |                          |
| sMvc3Manager__getMvc3Set      | 0x14004B000   |                          |
| sMvc3Manager__getNetwork      | 0x1407CDA50   | +0xB0 on sMvc3Mgr        |
| sMvc3Manager__getLicense      | 0x14010A200   | +0xA8 on sMvc3Mgr        |
| sMvc3Manager__getRanking      | 0x140703370   |                          |
| sMvc3Manager__getNetworkRoom  | 0x14030E6C0   | +0xB8 on sMvc3Mgr        |
| sMvc3Manager__getEarthAttack  | 0x140327A10   | +0xC8 on sMvc3Mgr        |

### MISTAKEN — Not a manager subsystem:
| Function                       | Address       | Real Identity             |
|-------------------------------|---------------|---------------------------|
| FN_MGR_GETSOMETHING           | 0x140259270   | `Concurrency::details::VirtualProcessor::GetOwningRoot` — ConcRT thread pool function |

### Other
| Function                       | Address       | Notes                     |
|-------------------------------|---------------|---------------------------|
| sMvc3Manager__getInstance     | 0x140001AF0   |                          |
| sMvc3Manager__getAction       | 0x1409629D0   |                          |
| sAction__stepFighting         | 0x140007080   | Main fighting step        |
| FUN_140537080 (sRender Present)| 0x140537080   | Skip in headless          |
| sPad__getInstance             | 0x140001B00   | Pag method 0xa5           |

---

## 7. Singleton Pointer Storage (dereference at runtime)

| Singleton       | Storage Address   | Size     | Priority | Notes                     |
|-----------------|-------------------|----------|----------|---------------------------|
| sReplay         | 0x140d4d9d8       | ~144KB   | MUST     | Frame counter at +0x54    |
| sCharacter      | 0x140d44a70       | ~48KB    | MUST     | Teams at +0x40, +0x300    |
| sShot           | 0x140d47f40       | ~4KB     | MUST     | List heads at +0x450      |
| sHitSolver      | 0x140d44060       | ~208KB   | MUST     | Collision pairs + buffer  |
| aNextBattle     | 0x140d47e68       | ~128KB   | MUST     | Battle state machine      |
| sGameUI         | 0x140d53a18       | ~64KB    | MUST     | Pause state at +0x5AB8    |
| sMain           | 0x140e177e8       | ~256KB   | MUST     | Frame timing, job queue   |
| sGameEffect     | 0x140d46470       | ~16KB    | SHOULD   | Effect list               |
| sMvc3Manager    | 0x140d510a0       | ~2KB     | SHOULD   | Subsystem references      |
| sPad            | 0x140e174b8       | ~3KB     | SHOULD   | Only if not own input     |

// Derived constants (framestepper.cpp and controller.cpp)
static constexpr uintptr_t PTR_SREPLAY      = 0x140d4d9d8;
static constexpr uintptr_t PTR_SCHARACTER   = 0x140d44a70;
static constexpr uintptr_t PTR_SSHOT        = 0x140d47f40;
static constexpr uintptr_t PTR_SHITSOLVER   = 0x140d44060;
static constexpr uintptr_t PTR_ANEXTBATTLE  = 0x140d47e68;
static constexpr uintptr_t PTR_SGAMEUI      = 0x140d53a18;
static constexpr uintptr_t PTR_SMAIN        = 0x140e177e8;
static constexpr uintptr_t PTR_SGAMEEFFECT  = 0x140d46470;
static constexpr uintptr_t PTR_SMVC3MGR     = 0x140d510a0;

// Offsets inside sMvc3Manager (from Ghidra)
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;  // getAction
static constexpr uintptr_t SMVC3MGR_GAMECONFIG    = 0x78;  // getGameConfig
static constexpr uintptr_t SMVC3MGR_REPLAY        = 0x90;  // getReplay
static constexpr uintptr_t SMVC3MGR_LICENSE       = 0xA8;  // getLicense
static constexpr uintptr_t SMVC3MGR_NETWORK       = 0xB0;  // getNetwork
static constexpr uintptr_t SMVC3MGR_NETROOM       = 0xB8;  // getNetworkRoom
static constexpr uintptr_t SMVC3MGR_EARTHATK      = 0xC8;  // getEarthAttack

// sMain internal offsets (from sMain__move decompilation)
static constexpr uintptr_t SMAIN_PAUSE_FLAG        = 0x7C;
static constexpr uintptr_t SMAIN_PAUSE_ACTIVE      = 0x7D;
static constexpr uintptr_t SMAIN_TICK_COUNT        = 0x48;
static constexpr uintptr_t SMAIN_TIME_SCALE        = 0x50;
static constexpr uintptr_t SMAIN_PERF_COUNTER      = 0x40048;
static constexpr uintptr_t SMAIN_SLEEP_CALL_FIXED  = 0x1405220b2;
static constexpr uintptr_t SMAIN_SLEEP_CALL_VAR    = 0x140521f93;
```

---

## 10. Current HeadlessTick Phase Breakdown

```
Rollback_HeadlessTick(sMain):
├── Phase 1 — Frame timing + Manager state + Input
│   ├── FN_SMAIN_MOVE         (0x140521DF0)  — frame timing + sleep
│   ├── FN_UPDATE_MANAGERS    (0x140258540)  — window focus tracking
│   ├── FN_PAD_PROCESS        (0x140289C30)  — sync pad input
│   └── VTableCall(0x40260, 0x30)             — PlatformUtil
│
├── [SKIP] vtable+0x48 draw calls on [0x401a8], [0x40198], [0x401d8]
├── [SKIP] FUN_1405BB710 on [0x401e0] — collision debug/profiling
├── [SKIP] sRender/sPrimitive render-setup block
│
├── Phase 2 — Post-render game-logic updates
│   ├── VTableCall(0x40200, 0x30)              — Area
│   ├── sMvc3Mgr→getAction→afterAction          — Action chain
│   ├── VTableCalls: Vibration, Input, Mouse, Id, Keyboard, Shader2, AI
│   ├── MgrSubsysCalls: GameConfig, GameUI
│   ├── VTableCall(0x401a0, 0x30)              — COLLISION (sCollision__move)
│   │   └── sCollision__move
│   │       ├── FUN_1407E1A20(+0x43B8)        — collision table A
│   │       ├── FUN_1405BB3B0()                — clear arrays
│   │       ├── FUN_1407E2420(+0x6860)         — collision table B
│   │       ├── if (+0x73a8): FUN_1405B7280   — SOUND EVENTS
│   │       │   ├── FUN_1405B72B0 (HOOKED)     — job dispatch (returns)
│   │       │   ├── FUN_1405B75C0              — processing A
│   │       │   ├── FUN_1405B7660              — processing B
│   │       │   └── slotIndex++               — INDEX INCREMENTS
│   │       └── set +0x44 = 0x100
│   ├── VTableCall(0x401e0, 0x30)              — mpCollision second call
│   ├── MgrSubsysCall: HitSolver
│   ├── VTableCall(0x401a0, 0x68)              — collision vfn+0x68
│   ├── VTableCall(0x401a0, 0x58)              — collision vfn+0x58
│   ├── DirectMemberCall(0x40240, FN_CALL_AI)  — AI call
│   └── VTableCalls: [0x401d8], [0x40208], [0x401b0], [0x401f0], [0x40248], [0x40258]
│
├── Phase 3 [#if 0 — DISABLED] — Manager subsystem update loop
│   ├── sMvc3Mgr vtable+0x30
│   ├── MgrSubsysCalls: Character, Stage, Shot, GameEffect
│   ├── HitSolver resetActiveNodes
│   ├── MgrSubsysCalls: Replay, [MISTAKEN: ConcRT call], BattleSet, Mvc3Set, Network, License
│   ├── VTableCalls: [0x401b8], [0x401c8], [0x401d0]
│   └── MgrSubsysCalls: Ranking, NetworkRoom, EarthAttack
│
├── Phase 4 — Tail + finalize
│   ├── VTableCall(0x40198, 0x30)              — Tool
│   ├── VTableCall(0x40270, 0x30)              — Primitive
│   ├── FN_SMAIN_TAIL (0x140259EA0)            — main tail function
│   └── VTableCall(0x401f8, 0x30)              — ?
│
└── Phase 5 — Pad methods
    ├── sPad::method_3D00(0xa4)                 — input state flush
    └── sPad::method_3D00(0xa5)                 — input state flush
```

---

## 11. Discussion of Remaining Issues

### Freeze After Re-Sim

The most pressing issue. The headless tick runs 8 frames of game logic without
rendering. After returning, the game must continue normally. Without snapshotting/
restoring sMain, the game's internal timing state is corrupted.

**Temporary fix (in ReSimulate):** Allocate a 0x40080 byte backup of sMain at the
start of ReSimulate and restore after. This lets the game continue normally while
the re-simmed snapshots remain available in the ring buffer.

### Phase 3 ConcRT Corruption

The function at 0x140259270 was identified as
`Concurrency::details::VirtualProcessor::GetOwningRoot` — it's part of the
Concurrency Runtime thread pool manager. Calling it from MgrSubsysCall with
vtable+0x30 dispatches through whatever object is returned, operating on
ConcRT internal state. This corrupts the job dispatch table seen by all 16
worker threads, causing crashes at 0x100000000/0x100000002.

**Proper fix:** Remove the `FN_MGR_GETSOMETHING` call entirely (it was always wrong).
Then re-enable Phase 3 to see which (if any) of the remaining MgrSubsysCalls causes
issues.

### Collision Sound Hook Gating

The collision sound hook is always active. It should be gated:
```c
static void __fastcall hkCollisionSoundUpdate(uint64_t param_1) {
    if (g_isResimulating) return;
    oCollisionSoundUpdate(param_1);
}
```

### sMain Snapshot

sMain is currently excluded from BuildRegions. It needs to be included so that
restore operations return sMain to its correct state. Size: 0x40080 bytes per frame.

### Ring Buffer Slot Overwrite

During re-sim, ReSimulate saves snapshots for frames `startFrame+1` through `endFrame`.
These overwrite the ring buffer slots of previously saved frames (e.g. frame 204
overwrites frame 188's slot since 204 % 16 == 188 % 16). The caller must be aware
of this and handle it appropriately.
