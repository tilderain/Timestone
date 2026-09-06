# Threading Architecture

**Status: Native and plugin reference. Reviewed 2026-07-23.** Native worker
behavior is verified from thread creation, events, semaphores, and job loops.
Imported thunk names should not be promoted to ConcRT APIs without symbol proof.

## Native Thread Roles

The game has three relevant execution roles:

| Role | Count | Verified behavior |
|---|---:|---|
| Main/game thread | 1 | Runs frame pacing, game update, input integration, and rendering |
| Regular `sMain` workers | 15 | Synchronously share short job arrays with the calling thread |
| Delay-job workers | Up to 8 | Consume a separate semaphore-driven 4096-slot ring |

The executable imports ordinary Win32 thread and synchronization APIs. Native
creation functions call thunks with `CreateThread`-compatible arguments,
including `CREATE_SUSPENDED`, thread entry, context, and output thread ID, then
resume the handles. Calling the entire system ConcRT-backed is unsupported by
the currently verified call sites.

## Regular Job Workers

`sMain__spawnJobWorkerThreads` at `0x140520FC0` lazily creates workers 1 through
15. The calling thread acts as slot 0 when jobs are drained, allowing at most 16
participants.

Each `0x38`-byte descriptor contains native thread/synchronization state,
including:

- Thread handle and ID.
- Wake and idle event handles.
- Worker index/active state.
- Exit byte.

Workers start suspended. Native setup resumes each worker and waits for its
initial idle signal before continuing.

`sMain__jobloop` at `0x140521780` waits on the descriptor's wake event, resets
it, and drains the singleton's regular job array. Workers and the calling thread
atomically increment the shared cursor at `sMain+0x9C`; callback execution is
parallel and is not protected by one queue mutex.

### Dispatch modes

| Mode | Record stride | Callback arguments |
|---:|---:|---|
| 0 | `0x10` | Record argument |
| 1 | `0x10` | Record argument plus shared context on the caller path |
| 2 | `0x20` | Record argument plus record field |
| 3 | `0x20` | Record argument plus record field |
| 4 | `0x20` | Record argument plus record field |
| 5 | `0x20` | Record argument, shared context, and record field |

`sMain__executeJob` at `0x140521230` initializes workers on first use, resets
the claim cursor, stores the mode, wakes active workers, participates in the
drain, and waits for worker idle events. With one job or one active thread it
uses the inline path. See [`main.md`](main.md) for the queue layout.

## Worker Counts

`sMain__setJobThreadNum` at `0x140522450` writes requested count `+0xA8` after:

- Mapping zero to one.
- Clamping to `sMain+0x40088`, the detected logical processor count.
- Capping at 16.

`sMain__move` copies requested count `+0xA8` to active count `+0xA4` each tick.
There is no separate "max thread count" field at `+0xA8`.

`sMain__checkCPUCaps` at `0x14051FFC0` stores:

| Offset | Verified role |
|---:|---|
| `+0x40080` | CPU capability bitmask |
| `+0x40084` | Computed physical/effective core count |
| `+0x40088` | Logical processor count from system information |
| `+0x4008C` | Hyper-threading flag |
| `+0x4008D` | Normalized processor-name string |

The exact feature-name mapping of every capability bit is not required for job
count selection.

## Delay-Job Workers

`sMain__createDelayJobThread` at `0x1405202C0` caps the requested count at eight,
creates `0x38`-byte descriptors/threads suspended, applies below-normal priority
and an ideal-processor hint, then resumes them.

`sMain__delayJobloop` at `0x1405210E0` waits on the shared semaphore at
`sMain+0xC0`, conditionally enters the critical section at `sMain+0x08`, and
claims a record from:

```text
base   = sMain + 0x1FA08
slot   = readCursor & 0xFFF
stride = 0x20
```

Write/read cursors are at `+0xB0/+0xB8`. Recognized types 0 and 2 both clear the
stored function, invoke it with the stored argument when non-null, and clear the
argument. Type 2 is not proven to mean a persistent or repeating job.

This machinery is generic. Describing it specifically as graphics loading,
texture streaming, or cleanup requires tracing individual producers.

The advanced `EntireGameSingleThreaded` option forces
`sMain__dispatchAndWaitJobs @ 0x14095EC90` through its existing inline callback
branch before native startup. It also changes the logical-processor argument at
the `sMain` constructor's `sMain__createDelayJobThread` call to zero, which the
native creation function handles by creating no delay workers. The inline patch
must be installed first: suppressing workers while retaining ring submission
would leave callers waiting forever for unconsumed jobs.

The same option sets the regular-job count to one on the first game tick. It
does not disable the dedicated rendering thread or plugin-owned threads.

## Plugin Threads

The plugin has two explicit thread roles:

| Thread | Creation | Responsibility |
|---|---|---|
| Bootstrap `MainThread` | `DllMain` process attach | Installs hooks and initializes integration outside loader-lock work |
| `FreezeWatchdogThread` | `Netplay_Init` | Polls freeze diagnostics every 100 ms; never services ENet or dispatches packets |

There is no permanent `HookUpdate` thread. Frame-stepper, controller, and
rollback work executes from installed game/render hooks.

`Netplay_Pump` now runs on the live game thread and dispatches received packets
directly into feature state. No channel-specific queues or mailbox remain.
ENet calls use one short SRW lock to serialize sends from render/UI callbacks
and shutdown. Receiver callbacks run without the lock. The optional Steam
transport still has its own bridge thread and separate ENet host.

The main netplay host is unserviced during blocking native loading or a long
re-simulation. Startup and synchronized restarts use a 60-second timeout; the
battle handshake restores the shorter gameplay timeout. This is an experimental
tradeoff, not a claim that network progress continues while the game is stalled.

## Frame and Re-simulation Ownership

The game-thread boundary is:

```text
hkMvc3Main_move
  process deferred restore/re-simulation requests
  Controller_OnFrameStart
  oMvc3Main_move
  Controller_OnFrameEnd
```

Missing remote input is predicted within the rollback budget. Delay-only or
budget-exhausted waits pump ENet nonblockingly rather than depend on a background
network thread. Re-simulation also runs on the game thread and calls full native
`oMvc3Main_move` ticks while temporarily suppressing pacing/presentation call
sites.

Current custom netplay applies the native gameplay serialization settings. It
does not alter `sMain+0xA4/+0xA8`, so the regular job executor remains at the
user/game-selected worker count for unrelated engine work. It does not
terminate delay workers or prove unrelated asynchronous engine activity
deterministic.

## Snapshot Ownership

Native worker descriptors, event/semaphore handles, critical sections, and
pending callback queues belong to the live process timeline. Rollback captures
the full `sMain` allocation for diagnostics/packing but restores selected timing
ranges rather than rewinding synchronization objects or queued work.

## Key Addresses

| Address | Function |
|---|---|
| `0x14051FFC0` | Detect CPU capabilities/counts |
| `0x1405202C0` | Create delay-job workers |
| `0x140520FC0` | Create regular workers lazily |
| `0x1405210E0` | Delay-job loop |
| `0x140521230` | Submit/drain/wait regular jobs |
| `0x140521780` | Regular worker loop |
| `0x140521DF0` | Frame tick and requested-to-active count copy |
| `0x140522450` | Set requested regular-worker count |
