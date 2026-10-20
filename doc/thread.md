# Threading Architecture

**Status: Native and plugin reference, reviewed 2026-10-26.** Native worker
creation, `executeJob`, delay-job consumption and main timing were rechecked
read-only on Marvel Ghidra **8193**, using existing `sMain` types. Plugin claims
were checked against `LaunchMenu_settings.cpp`, `rollback_runtime.cpp`,
`rollback_hooks.cpp`, `rollback_resim.cpp`, `rollback_tick.cpp`, `netplay.cpp`
and `steam_transport.cpp`. Other native layout/callback details retain their
earlier evidence scope. No database edits, build or runtime concurrency test was
performed. Imported thunk names are not proof of ConcRT APIs.

## Native Thread Roles

The game has several relevant execution roles; the regular and delay pools are
not the complete process thread census:

| Role | Count | Verified behavior |
|---|---:|---|
| Main/game thread | 1 | Runs pacing, simulation, input integration and render orchestration |
| Regular `sMain` workers | 15 | Synchronously share short job arrays with the calling thread |
| Delay-job workers | Up to 8 | Consume a separate semaphore-driven 4096-slot ring |
| UI/device and optional rendering work | Separate roles | Window/device jobs and render playback must not be conflated with simulation workers |
| Resource-loading workers | Separate pool | Not suppressed merely by serial gameplay dispatch |

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
The shutdown byte is read from the worker-context argument `+0x30`; singleton
`sMain+0x30` is the separate `cSystem::mThreadSafe` locking flag. The current
stored prototype omits that context, which survives as `in_RDX` in decompilation.

This machinery is generic. Describing it specifically as graphics loading,
texture streaming, or cleanup requires tracing individual producers.

The advanced `EntireGameSingleThreaded` option is a persistent live override,
available in the launch Settings dialog, F1 Settings tab, and Escape Settings
panel. A startup hook on `uGeometry2Group::move @ 0x14095EC90` temporarily clears
the group's `mFlgMultiThread` byte at `+0xB0` while calling the original method,
selecting its native inline callback branch. The original flag is restored
afterward. This function was previously mislabeled `sMain__dispatchAndWaitJobs`.

Delay workers are still created normally, allowing the native dispatch path to
resume when the override is disabled. The game-thread frame boundary applies
regular-job count changes: enabling saves the requested count and sets requested
and active counts to one; disabling restores the saved count using the native
setter. UI threads only persist and publish the requested toggle atomically.
No executable bytes are rewritten during live toggles. Rendering, resource
loading, and plugin-owned threads remain independent.

It also leaves independent `sResource` loading workers active. See
[`critical_section_elision.md`](critical_section_elision.md) for the remaining
thread ownership, native lock fast paths, and lock-removal safety assessment.

## Plugin Threads

Principal plugin-owned thread roles are:

| Thread | Creation | Responsibility |
|---|---|---|
| Bootstrap `MainThread` | `DllMain` process attach | Installs hooks and initializes integration outside loader-lock work |
| `Netplay_ServiceThread` | `Netplay_Init` | Owns gameplay ENet host/peer service/send/receive |
| Steam `BridgeThread` | `SteamTransport_CreateBridge` | Services separate loopback ENet endpoints and forwards Steam Networking Sockets traffic |

There is no permanent `HookUpdate` thread. Frame-stepper, controller, and
rollback work executes from installed game/render hooks.

The service thread and game thread exchange state through channel-specific
`CRITICAL_SECTION` objects. Channels cover input, delay control, re-simulation
diagnostics, ground truth, battle readiness, desync actions, restart, and
multi-resimulation state. The service thread never invokes simulation or
mutates native game objects.

## Frame and Re-simulation Ownership

The game-thread boundary is:

```text
HookedMove / controller frame boundary
  service input, readiness and timeline state as allowed by current mode
  hkMvc3MainMove
    consume deferred restore/correction and save the forward snapshot
    original native main tick
  complete controller frame bookkeeping
```

Missing remote input can be predicted within the configured budget; readiness
and opponent-wait policy can stop advancement without invoking simulation on the
network thread. Replay runs on the game thread through the implemented
`Rollback_HeadlessTick`, not full native rendered main ticks, with additional
pacing/presentation call-site suppression. The exact source functions, rather
than this ownership sketch, define pause/wait/step behavior.

Current custom netplay applies the native gameplay serialization settings. It
does not alter `sMain+0xA4/+0xA8`, so the regular job executor remains at the
user/game-selected worker count for unrelated engine work. It does not
terminate delay workers or prove unrelated asynchronous engine activity
deterministic.

## Snapshot Ownership

Native worker descriptors, event/semaphore handles, critical sections and pending
queues belong to the live process timeline. Broad `sMain` capture is currently
disabled. Retained handler ranges are historical code, not evidence that the
current system snapshots the entire allocation or safely rewinds pending jobs.
See [main.md](main.md) before re-enabling that path.

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
