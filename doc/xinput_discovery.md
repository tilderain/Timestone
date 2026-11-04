# Asynchronous XInput Discovery

Implemented in `UMVC3Hook/code/xinput_discovery.cpp`. Enabled for mod gameplay
independently of backend diagnostics. Normal Launch disables the owned hooks and
signals the worker to stop. Startup before gameplay activation remains native.

## Native Evidence

Ghidra, `umvc3.exe.unpacked.exe`, port 8192, preferred base `0x140000000`:

- `sPad::redetectSlot` (`0x140514950`) limits attempts to 1000 ms per game slot,
  then scans up to four XInput user indices. Its call at `0x140514A29` reaches
  the `XInputGetState` delay-import thunk at `0x1409981C1`.
- Native discovery skips a user already owned by a slot with `slot+8 == 1`,
  `slot+0xB >= 4`, and matching user index at `slot+0x18`. The four slots start
  at `sPad+0x58`, stride `0x2E0`. The implementation preserves this exclusion.
- `sPad::queryXInputCaps` (`0x140514450`) has one recognized direct caller,
  discovery at `0x140514A5A`. Its call at `0x140514476` reaches the capabilities
  thunk at `0x1409981E5`, with flags `XINPUT_FLAG_GAMEPAD` (1). Native code maps
  the returned subtype/flags into slot fields, which we leave to the original.
- The imported module is `XINPUT1_3.dll` (string at `0x140BD4BD0`). The worker
  loads that same module and resolves its exports, not the game's hooked thunks.
- `sPad::Pad::readPad` calls discovery only for kind-zero slots. Active XInput
  reads use the state thunk separately; failure closes the native slot. The
  explicit `sPad::Pad::detect` path can also request a close/reopen of an
  occupied slot; that uncommon path retains synchronous native behavior.

## Behavior

Three gameplay-owned detours wrap redetection and the two game import thunks.
The movement port omits backend timing hooks and lag diagnostics.

1. When an empty slot needs discovery, the game thread requests samples for
   unassigned XInput users. Repeated requests coalesce while pending.
2. Until every candidate has a fresh sample, return false from discovery without
   entering native discovery, changing its timer, or starting DirectInput fallback.
3. The worker performs state queries and, for successful state queries, capability
   queries. It holds no cache lock during either driver call and never accesses
   native game objects, memory layouts, or hook trampolines.
4. Once ready, the game thread calls original discovery with a thread-local
   snapshot. Its XInput calls read that snapshot. Slot selection, subtype
   conversion, buffer initialization, and DirectInput fallback remain native.
5. Outside that discovery scope, every state read calls original XInput with the
   caller's actual output pointer. Gameplay does not consume cached button state.
6. Failed live reads increment a per-user generation, invalidating old samples
   even if still within the one-second cache lifetime. Generation changes during
   a worker query also prevent consumption of that result.

The game thread uses a try-lock only. Publication contention defers discovery;
it never waits for the worker. Assigned users are excluded from ordinary
discovery requests, so this does not add repeated worker polls of active pads.
The underlying backend can still serialize queries across different users.

## Lifecycle And Failure

The worker starts during hook installation, but makes no controller queries
until mod gameplay requests discovery. All three hooks are created before any
are queued. Backend/event/thread initialization failures retain native behavior.
The ASI is pinned before worker creation. Stop only signals an atomic state and
event; it never joins a possibly blocked driver call under the loader lock.
An in-flight query can finish after Stop, but cannot access native objects or
freed MinHook trampolines. The event remains allocated for the process lifetime.

No timeout can cancel an arbitrary blocked XInput driver call safely. A stuck
worker delays new device discovery, not frame advancement. DirectInput/WMI
fallback and active XInput reads are still synchronous and can independently
stall. A hotplug can wait for cache expiry, worker completion, and native
redetection eligibility; this is not an instantaneous device-change mechanism.

## Validation

Mock tests compile the production module without lag instrumentation. They
cover pending/expiry/coalescing, assigned-user exclusion, live-read forwarding,
failure-output preservation, disconnect generations, lock contention, worker
publication, backend/thread failures, and Normal Launch/stop passthrough.

Runtime validation still requires captures and controller connect/disconnect
tests on the actual Windows/Wine/Steam Input setup. Check that recurring main
thread discovery spikes disappear, active-input timing stays low, controllers
reconnect, and Normal Launch still behaves natively. This change does not prove
that all input or presentation hitches are resolved.
