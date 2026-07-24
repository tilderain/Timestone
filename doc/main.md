# Frame Timing and Job Execution (`sMain`)

**Status: Native reference with rollback caveats. Reviewed 2026-07-23.** Current
rollback captures `0x40080` bytes from `sMain`, but restores only selected timing
ranges. Worker handles, callback queues, ring buffers, and thread descriptors
remain owned by the live timeline.

## Scope

`sMain` combines frame pacing with two distinct job systems:

- `sMain__move` at `0x140521DF0` updates frame timing and pacing state.
- `sMain__executeJob` at `0x140521230` drains a short-lived regular job array,
  using the main thread and optional worker threads.
- `sMain__delayJobloop` at `0x1405210E0` consumes a separate 4096-entry
  asynchronous ring through up to eight delay-job workers.

The layouts and control flow below are established by direct native accesses.
Subsystem-purpose claims require caller analysis and are not inferred merely
from the generic job machinery.

## Relevant Layout

| Offset | Type | Verified role |
|---:|---|---|
| `+0x30` | byte | Shutdown/finalization flag consulted by delay workers |
| `+0x38` | `uint64_t` | Millisecond timer derived from QPC |
| `+0x40` | `uint64_t` | Timer QPC origin |
| `+0x48` | `uint32_t` | Frame timer derived from FPS and the millisecond timer |
| `+0x50` | `float` | Target FPS |
| `+0x54` | `float` | Maximum FPS divisor/cap input |
| `+0x58` | byte | Frame-wait mode |
| `+0x5C` | `float` | Timing/FPS sample updated by `sMain__updateTiming` |
| `+0x60` | `int64_t` | Scaled real-counter accumulator |
| `+0x68` | `int64_t` | Ideal-counter accumulator |
| `+0x70` | `int64_t` | Previous QPC sample |
| `+0x78` | `uint32_t` | Pause/skip countdown |
| `+0x7C` | byte | Pause flag |
| `+0x7D` | byte | Pause-edge result for the current tick |
| `+0x80` | `float` | Frame delta used by simulation |
| `+0x84` | `float` | Delta overflow border |
| `+0x88` | `float` | Absolute delta limit |
| `+0x8C` | `float` | Global speed multiplier |
| `+0x90` | `float` | Delayed-frame/overflow accumulator |
| `+0x98` | `uint32_t` | Regular job count |
| `+0x9C` | `uint32_t` | Shared regular-job claim cursor |
| `+0xA0` | `uint32_t` | Regular-job dispatch mode, values 0 through 5 |
| `+0xA4` | `uint32_t` | Active regular-job thread count |
| `+0xA8` | `uint32_t` | Requested regular-job thread count |
| `+0xAC` | `uint32_t` | Delay-job worker count |
| `+0xB0` | `uint64_t` | Delay-job write cursor |
| `+0xB8` | `uint64_t` | Delay-job read cursor |
| `+0xC0` | handle | Delay-job semaphore |
| `+0xC8` | array | Regular job records, with `0x10` or `0x20` stride by mode |
| `+0x1FA08` | array | 4096 delay-job records, each `0x20` bytes |
| `+0x3FAC8` | array | Regular worker descriptors, each `0x38` bytes |
| `+0x3FE48` | array | Delay-job worker descriptors, each `0x38` bytes |
| `+0x40010` onward | fields | Secondary timing/process-counter state |

The global used by `sMain__jobloop` points to the `sMain` singleton. It is not a
separate global job-queue object.

## Frame Timing (`0x140521DF0`)

At entry, `sMain__move` copies requested worker count `+0xA8` to active count
`+0xA4`. It then updates pause state, samples the performance counter, updates
the millisecond/frame timers, and advances the scaled counter accumulator.

### Frame-wait clear

When `+0x58` is false, the target fraction is `mFps / mMaxFps`. The function:

1. Computes `mDeltaTime` from `(mRealCounter - mIdealCounter) / qpcFrequency`.
2. Sleeps for most of a positive scheduling gap when the computed wait is at
   least two milliseconds.
3. Re-samples QPC in a tight loop until the target fraction is reached.
4. Clamps a negative delta to zero.
5. Converts a delta at or above `mDeltaTimeBorder` to `1.0f` and accumulates the
   excess in `mDelayFrame`.
6. Applies `mDeltaTimeLimite` and advances the ideal counter by the accepted
   delta in QPC units.

### Frame-wait set

When `+0x58` is true, the function sleeps in one-millisecond increments while
the real accumulator is behind the ideal accumulator. It then advances the
ideal accumulator by one QPC-frequency interval, sets delta to `1.0f`, and
prevents the real accumulator from remaining ahead.

Finally, both paths multiply delta by `mGlobalSpeed` and execute an engine sync
point. These operations establish native pacing behavior. They do not, by
themselves, establish where retail Steam socket I/O or every netpad operation
runs; those claims need independent call-chain analysis.

## Regular Jobs

### Thread-count selection

`sMain__setJobThreadNum` at `0x140522450` writes requested count `+0xA8`. It
maps zero to one, clamps to the detected logical-processor count, and caps the
result at 16. `sMain__move` copies the requested value to active count `+0xA4`
on each tick.

### Submission and completion

`sMain__executeJob` at `0x140521230` is a synchronous submit-and-wait operation:

1. Lazily creates regular worker threads.
2. Resets claim cursor `+0x9C` and stores the requested mode at `+0xA0`.
3. If there is one job or one active thread, drains the array inline.
4. Otherwise wakes active workers, lets the calling thread drain jobs too, and
   waits for all participating worker idle events.
5. Clears the job count on completion.

`sMain__jobloop` at `0x140521780` is the worker-side loop. Each descriptor has a
wake event at `+0x18`, an idle event at `+0x20`, and an exit byte at `+0x30`.
Workers wait on their wake events, reset them, atomically claim indices through
`+0x9C`, invoke callbacks, and signal idle completion.

There are six dispatch modes. Modes 0 and 1 use `0x10`-byte records; modes 2
through 5 use `0x20`-byte records. Callback signatures vary by mode, including
optional shared and per-record arguments. The claim operation is lock-free
interlocked increment, not mutex serialization of callback execution.

## Delay Jobs

`sMain__createDelayJobThread` at `0x1405202C0` caps delay workers at eight,
creates them suspended, assigns below-normal priority and an ideal processor,
then resumes them. This pool is separate from regular jobs.

Each `sMain__delayJobloop` worker:

1. Waits on the shared semaphore at `+0xC0`.
2. Conditionally enters the critical section at `sMain+0x08`.
3. Claims the next slot using cursors `+0xB8` and `+0xB0`.
4. Resolves the slot at `+0x1FA08 + ((readCursor & 0xFFF) * 0x20)`.
5. Clears the slot function before invoking it, invokes recognized types 0 and
   2, and clears the argument afterward.
6. Sleeps for one millisecond when awakened without a claimable job.

Both currently decompiled recognized types call the stored function with the
stored argument. Calling one type "repeating" is unsupported: the loop clears
the function and argument for both.

## Rollback Implications

The plugin's `SMAIN_SIZE` is `0x40080`, large enough to capture the timing tail,
job arrays, and descriptors. Restore deliberately applies only these ranges:

```text
+0x0030 size 0x64
+0x00A4 size 0x1C
+0x40010 size 0x10
+0x40028 size 0x18
+0x40048 size 0x14
```

This preserves live synchronization objects and pending-work ownership. During
custom netplay, current code also forces requested and active regular-job counts
to one around relevant ticks. That is plugin policy, not native `sMain`
behavior; native online additionally disables selected gameplay parallel paths
at battle setup, as documented in `native_online_saction_time_sync.md`.

## Key Addresses

| Address | Function |
|---|---|
| `0x1405202C0` | `sMain__createDelayJobThread` |
| `0x1405210E0` | `sMain__delayJobloop` |
| `0x140521230` | `sMain__executeJob` |
| `0x140521780` | `sMain__jobloop` |
| `0x140521A40` | Calling-thread regular-job drain |
| `0x140521DF0` | `sMain__move` |
| `0x140522450` | `sMain__setJobThreadNum` |
