# Critical-section elision investigation

Investigated 2026-10-15 against current source and Ghidra's
`ichneumon_sub/umvc3.exe.unpacked.exe`. Static findings, not a runtime benchmark.
Addresses use the preferred executable base `0x140000000`.

## Conclusion

Selective lock elision is plausible after proving exclusive ownership of the
protected data. Blanket no-op patches to native or Win32 critical-section
functions are not safe with the current `EntireGameSingleThreaded` option.
Much of the obvious parallel-only locking is already skipped by the engine.

Planning expectation: a small optimization, plausibly sub-percent to low
single-digit percent of simulation CPU time, not an established measurement.
There is no measured remaining acquisition count or lock cost to support a
tighter estimate. A lock-heavy workload could exceed that expectation; a
workload dominated by already-disabled locks could gain almost nothing.

## What single-thread mode actually changes

`UMVC3Hook/code/LaunchMenu_settings.cpp:470-518`:

- Forces `sMain__dispatchAndWaitJobs` down its native inline branch before startup.
- Supplies zero to creation of delay-job workers.
- Requests one regular job participant on the first game tick.
- Does not patch resource loading, rendering, UI/device work, or plugin threads.

`sMain__executeJob @ 0x140521230` still lazily creates regular worker threads
before selecting the inline path. With one participant they are not awakened
for these batches, but single-worker execution is not absence of all threads.

Additional native resource threads are independent of the delay-job system:

- `sResource__sResource @ 0x140515B30` creates one
  `sResource__loaderThreadProc @ 0x140519040` thread.
- It also creates one to three `sResource__loaderThreadProc_0 @ 0x140517F40`
  workers, based on detected logical processors. Even one detected processor
  selects one such worker, not zero.
- The constructor sets `sResource.cSystem.mThreadSafe = true`.
- The loader locks the shared pending-resource queue and the additional workers
  use a separate critical section around their shared work state.

`sRender__initCore @ 0x140535D20` sets `mThreadSafe = true` and creates the
render worker. Parallel-render execution is separately controlled by
`sRender+0x3D`; disabling it does not establish exclusive ownership of every
resource, allocator, UI callback, or audio object. See
[`input_to_display_timing.md`](input_to_display_timing.md) and
[`d3d9_lifecycle_repair.md`](d3d9_lifecycle_repair.md).

Plugin network synchronization must also remain intact. Its ENet service thread
continues exchanging data with the game thread under its own critical sections.
Process-wide API detours would affect plugin and third-party code too.

## Existing native lock elision

`cSystem` is `0x38` bytes: vtable at `+0`, Win64 critical section at `+8`, and
`mThreadSafe` at `+0x30`.

The wrappers at `0x140006D00` and `0x14000B860` are equivalent to:

```cpp
if (system->mThreadSafe || parallelRegionActive)
    EnterCriticalSection(&system->mCS); // Leave in the other wrapper.
```

`parallelRegionActive` is byte `0x140E178F0`, currently named
`g_sHitSolver__mCSEnabledFlag` in Ghidra. Despite that name, it is engine-wide.
`sMain__setMultiThreadFlags @ 0x140531B90` writes its boolean argument to both
this byte and allocator parallel-region byte `0x140E16CC0`.

The parallel branch of `sMain__executeJob` sets both flags before waking workers
and clears them after joining. Its one-job/one-participant branch does neither.
Thus systems with `mThreadSafe == false` already avoid the actual Windows calls
on that serial path. `cSystem__cSystem @ 0x140531AA0` initializes the critical
section regardless, but defaults `mThreadSafe` to false.

There are 783 static references to the enter wrapper in this Ghidra database.
That is neither 783 unique callers nor 783 acquisitions per tick. Many calls
only execute two flag checks and return. Removing their call sites could save
small call/branch overhead, not the cost of an actual lock acquisition.

Also, many native functions inline the conditional locking instead of calling
the wrappers, including `sUnit__addBottom @ 0x14051A6B0` and
`sEffect__getUnitNo @ 0x140543C30`. No-oping the two wrappers would not remove
all system locking.

Allocator locking has another existing fast path. For example,
`MtHeapAllocator__memAlloc @ 0x1404C9550` acquires its critical section only if:

```cpp
(allocator->mAttr & 2) ||
((allocator->mAttr & 4) && allocatorParallelRegionActive)
```

Allocators with only the parallel-region attribute already avoid locking on
the serial path. The unconditional attribute cannot safely be cleared without
auditing all threads sharing that allocator, including resource loaders.

## Candidate scope

| Target | Verified behavior | Assessment |
|---|---|---|
| Generic `cSystem` wrappers | Per-instance flag OR parallel-region flag | Do not globally bypass. Audit individual systems and their inline sites. |
| `sResource` / `sRender` locks | Constructors explicitly enable locking; independent threads exist | Retain under current single-thread option. |
| Allocator locks | Per-allocator unconditional or parallel-only attributes | Parallel-only savings already realized; shared allocators require locks. |
| RNG dispatchers `0x14014EB80`, `0x14014EAB0`, `0x14014F390` | Non-null receiver takes a shared critical section around selection and RNG mutation; null receiver path does not | Candidate for audited gameplay call sites, not blanket dispatcher no-ops. Trace every user of the protected state first. |
| Shot/stage list guard `0x140150700` / `sShot__unlockSlot` | Unconditionally locks the supplied critical section | Candidate only after proving list ownership. This guard is used by stage operations too. |
| Shot history `0x140039150` | Character shot-history lock is already optional through `lockUpdates` | Determine actual argument and callers before assuming any remaining saving. |
| Plugin netplay locks | Protect live game/service-thread communication | Retain. |

The function currently named `sShot__lockSlot` is actually a small guard
constructor: it stores the supplied critical-section pointer in its first
field and then enters it. Its paired guard destructor reads that pointer to
leave. Its current `sShot *` type in Ghidra is misleading. Do not replace the
whole constructor with `RET` while leaving its destructor active.

An unrelated remaining serial-path overhead is the atomic job cursor:
`sMain__dispatchQueuedJobsMT @ 0x140521A40` retains interlocked increments even
with one participant. This is not a Windows critical section and would not be
removed by a critical-section patch. Any non-atomic replacement would need to
be confined to a proven serial dispatch path.

## Performance estimate

For actual, removable uncontended acquisitions:

```text
saved milliseconds per tick = removable enter/leave pairs * pair cost in ns / 1e6
```

The following is sensitivity analysis, not measured game counts or measured
Windows/Wine costs. Pair costs include both enter and leave, not each operation.
The target machine and compatibility layer must supply the real costs.

| Removable pairs per simulation tick | At 50 ns/pair | At 200 ns/pair |
|---:|---:|---:|
| 100 | 0.005 ms | 0.020 ms |
| 1,000 | 0.050 ms | 0.200 ms |
| 5,000 | 0.250 ms | 1.000 ms |

For example, 1,000 removable pairs at 50 ns saves 0.05 ms. That is 2.5% of a
2-ms simulation tick, but only 0.3% of the 16.67-ms display-frame budget. Eight
such simulation ticks save 0.4 ms in aggregate, assuming similar work per tick.
This does not make a capped 60-FPS game display above 60 FPS; it adds CPU and
rollback catch-up headroom. Count the actual ticks in a rollback window rather
than assuming every window has eight.

Already-disabled wrapper calls need a different model: only call/return and
flag-check overhead remains. For scale, 1,000 pairs at an assumed 5 ns/pair
would save 0.005 ms. This is also illustrative, not a measured fast-path cost.
Adding a detour, TLS test, or allowlist lookup to these tiny wrappers could cost
more than the bypass saves.

If profiling reveals contention, enter time can be much greater than these
examples. That also disproves the assumption of exclusive ownership: dropping
the lock is not a valid fix for contention with another live user.

## Validation before an experiment

1. Profile the unchanged single-thread baseline. Count executed wrapper calls
   separately from actual acquisitions, and group acquisitions by critical-
   section address, native call site, thread, and live/resimulation phase.
   Include inline/imported calls, not just the two `cSystem` wrappers. Use
   per-thread counters and deferred reporting; avoid per-acquisition logging
   and timers whose overhead dominates the work being measured.
2. Audit all accesses to candidate protected state, including other locks,
   unguarded paths, callbacks, startup, loading, stage changes and teardown.
   Observing one acquiring thread during a quiet match alone is not proof of
   ownership. Resource and allocator paths are especially unsuitable guesses.
3. Select a small allowlist of proven game-thread-owned sites or objects.
   Preserve matching enter/leave decisions, recursion, and guard state. Do not
   toggle elision while a lock can be held. Do not globally clear thread-safe
   flags or bypass process-wide synchronization APIs.
4. A/B the narrow patch without hot-path profiling, using identical replays,
   CPU-heavy effects/projectiles, and rollback depths. Compare tick/window CPU
   and wall time distributions, checksums, visual behavior and lifetime stress
   cases. Report median and tail behavior. Keep worker scheduling and renderer
   settings fixed so job-dispatch savings are not misattributed to lock elision.

No runtime bypass was installed during this investigation. Ghidra signatures
were clarified for `sMain__setMultiThreadFlags(bool)` and
`sEffect__getUnitNo(sEffect *)` so the flag and system layout are explicit.
