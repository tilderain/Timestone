# Render Crash Bandaid

The render and restart handling in `rollback_runtime.cpp` is defensive bandaid
code, not a root-cause fix. Repeated character autotests exposed stale material,
animation, UI, and effect state after synchronized match restarts. Several
functions in `umvc3.exe` assume allocation and ownership operations cannot fail,
then continue through null or recycled objects.

These workarounds are version-specific and carry real risks:

- Any game executable update can invalidate expected instructions, branch
  offsets, or hook targets.
- Null guards can hide the allocation or ownership failure that caused the
  invalid object.
- Skipping an optional UI or animation operation can produce missing
  presentation state without crashing.
- Pointer and vtable checks cannot prove that a mapped object is semantically
  owned by the current match.
- Preserving character-owned effects can prevent a crash while retaining a
  resource longer than intended.
- The autotest result is not proof that rollback state, UI ownership, or
  host/client synchronization is correct.

## Earlier Work

Commit `64dabfe` (`fix rollback restart stale render state`) added the first
restart and render workarounds:

- `Rollback_RemoveStaleUiUnits()` walks `sUnit` category lists and unlinks
  generic `MtDTI` nodes during restart cleanup.
- `hkGameUiMove()` controls whether GameUI advances during headless
  resimulation.
- `hkMenuOptionKbdMove()` and `hkMaterialResourceState()` reject stale or
  inaccessible objects before native code dispatches through them.
- Snapshot restore skips regions whose first word is the generic `MtDTI`
  vtable, and snapshot construction omits the unowned `uCharacterPost` camera
  state.
- Native render-command allocation branches were patched with expected-byte
  checks, including the material-destructor NOP.

Those changes remain temporary lifecycle workarounds. They can unlink live
objects, omit state that would otherwise be restored, alter native control
flow, or leak resources. The original render-command patch is no longer
invoked by the current code.

## Current Commit

This commit contains the following additional restart, rollback, and autotest
changes.

### Persistent Character Effects

`rollback_effect.cpp` now identifies effects that belong to active roster
characters before rollback cleanup retires or sanitizes them:

- It checks character effect slots at `+0x68F0`, `+0x68F8`, `+0x6900`, and
  `+0x6920`.
- It checks the native owner link at `uEffect+0x280`, covering indexed or
  hyper-effect slots that are not represented by the fixed offsets.
- `Effect_RetireCurrent()` leaves these effects on the live native timeline.
- `Effect_SanitizeRetired()` does not clear their live ownership or resource
  state.
- `rollback_restore.cpp` no longer clears Dr. Strange's live effect slots
  after restoring a character snapshot.

This is deliberately narrower than restoring historical effect pointers from a
snapshot. Presentation objects remain owned by the live native character
timeline rather than being replaced with pointers from an old simulation
state.

### Restart And Resimulation Barriers

Rollback requests are now rejected or cleared while a netplay match is not
fully synchronized or is transitioning through a restart:

- `Rollback_ClearPendingReSimulation()` centralizes clearing queued rollback,
  restore, and F7 diagnostic requests.
- Incoming restart handling clears pending resimulation before applying the new
  restart state.
- Main movement, request creation, request consumption, and the resimulation
  loop all prevent stale pre-restart corrections from entering the new match.
- `FrameStepper_IsMatchRestartPending()` exposes the restart barrier to the
  rollback modules.
- GameUI movement is suppressed during headless resimulation so presentation
  resources are not mutated on every correction; the next live frame advances
  that native lifecycle.
- Categories excluded from headless gameplay movement still receive a
  cleanup-only pass for retired units. Native `sUnit__moveCategory` applies its
  state 3/4 destruction state machine to every category before filtering active
  object callbacks. Skipping a category entirely therefore starved effects in
  categories 14-16, UI units, and other presentation lanes of the two passes
  needed to remove multi-mask links, unlink the primary unit, and invoke its
  deleting destructor. The cleanup-only pass reproduces those native lifecycle
  transitions without moving, synchronizing, initializing, or drawing active
  objects.

The scheduler rebuild hook and hand-written scheduler allocator were removed.
Native scheduler allocation and rebuild behavior is left intact instead of
reconstructing entry arrays or forcing scheduler state to retired.

### Native Null And Stale-Object Guards

`rollback_runtime.cpp` adds narrow guards around restart-time failures:

- `uBattleUi__setup` has expected-byte patches at `0x1402D2A32`,
  `0x1402D2AA2`, `0x1402D2B19`, `0x1402D2BAA`, `0x1402D2C79`,
  `0x1402D2EB1`, `0x1402D2F09`, and `0x1402D3146`. Failed gauge, timer, icon,
  score, room-watch, and ambient-light allocations skip their native
  initialization blocks.
- `uBattleUiHit__setup` has expected-byte patches at `0x1402D88F3`,
  `0x1402D8972`, and `0x1402D8A42` so failed hit-model allocations advance
  their loops without passing null units to animation or position code.
- `MtObject__isAliveState` rejects null, unmapped, or recycled vtables before
  native callers use the result to dispatch another virtual method.
- `MtAnim__setFrameIndex` rejects an absent animation object or inaccessible
  frame-index field.
- `MtObject__setFlag0x800` rejects absent or invalid objects.
- `uCutIn__addCharacter` and
  `uCutIn__setVariantPropagateToCam` reject failed optional cut-in objects.
- The character position helper at `0x1400E3020` preserves its stiff-damage
  side effect but skips an absent `cChr+0x6428` position source.
- `MtAnim__updateFrame` returns the native no-frame value `0xFFFFFFFF` for
  malformed animation tables.
- `uBaseModel__resetMaterials` validates and clears stale material-cache
  entries before native teardown, allowing valid material entries to follow the
  native release path.
- A version-checked vectored exception handler handles the null model result at
  `nDraw_Material__clone+0x1F` (`0x1405DCA7F`) and resumes at the native clone
  epilogue so the caller can use its existing fallback path.
- A version-checked vectored exception handler handles the null optional cursor
  returned while `uBtResultScoreVersus` initializes. At `0x14030A734`, native
  code dereferences `uBtResultScoreVersus+0x248` despite the preceding lookup
  accepting null; recovery skips that cursor dispatch and resumes at the native
  epilogue (`0x14030A741`).

The autotest-only hook that skipped `uScheduler__rebuildEntriesFromResource`
was tested and removed. It avoided the immediate scheduler crash, but made the
HUD, toon effects, and stage invisible, and still froze after roughly 36 Iron
Man restarts. It is not a valid workaround.

The effect sanitizer introduced in `ae90bb0` originally cleared every particle
manager sub-buffer and set the manager count to zero when an effect generator
was stale. Native `uEffect__releaseGenerator` at `0x1405E6890` uses that count to
call `cParticleManager__releaseSubBuffer` at `0x14080C120`, then releases the
manager array. Clearing valid ownership fields therefore suppressed frees on
every rollback and could exhaust the shared allocator until effects, shots, and
UI objects stopped allocating. The sanitizer now clears only inaccessible
sub-buffer pointers. It preserves valid buffers and readable manager arrays for
native destruction; malformed arrays retain their allocation pointer with a
zero count so native teardown can at least release the containing array.

These guards can suppress a crash while leaving a missing UI model, hit
effect, cut-in, animation frame, or position update. They should be removed or
reduced once the allocation and ownership transition is fixed at its source.

### Dual-Training Startup

`dllmain.cpp` changes dual-training startup to avoid a host/child D3D9 race
under Wine or Proton:

- The child process launch is deferred until startup patches are complete and
  the host has initialized its real D3D9 device.
- `Direct3DCreate9` and `IDirect3D9::CreateDevice` are hooked so device
  creation is serialized through the named cross-process mutex
  `Local\\UMVC3Rollback_D3D9CreateDevice`.
- `CreateDevice` retries `D3DERR_NOTAVAILABLE` up to eight times.
- The ENet host is created before the deferred child launch, and the existing
  launch handshake runs from `MainThread` rather than blocking initialization.
- Netplay initialization can now be performed after the peer connects through
  `FrameStepper_InitNetplay()`.
- `-autotest-port=<port>` accepts an explicit ENet port and passes it through
  the host/client launch flow. The valid range is `1` through `65535`.

This changes startup ordering and adds process-wide synchronization, but does
not make D3D9 or the native game initialization thread-safe in general.

### Autotest Changes

- Runtime autotest command collection skips the second command matcher. This
  avoids collecting a duplicate or invalid command source exposed by the
  character runtime layout.
- The Python autotest runner accepts and validates `--port`, adds
  `-autotest-port` to the launch command, and records the selected port in the
  generated plan.
- A regression test verifies that the port reaches both the command line and
  generated plan.
- The AI desync testing specification documents the optional port argument.

These changes improve test isolation and reporting; they do not fix rollback
desynchronization by themselves.

## Validation And Remaining Risks

The changes were developed against repeated synchronized character autotests.
The observed progress improved from early restart failures to substantially
later suite generations, but the run still exposed checksum divergence and
late restart synchronization issues. A completed host suite must therefore not
be treated as proof of client correctness.

The expected-byte checks prevent blindly patching an unknown executable, but do
not make the design sound. The proper fix remains identifying the rollback and
restart ownership transition that leaves stale UI, model, material, scheduler,
or effect state alive after a character or match is retired.

Do not extend this workaround with more byte patches unless a new crash must be
temporarily unblocked and its exact control flow has first been confirmed in
Ghidra. Every additional patch increases maintenance risk and moves the
project farther from the actual lifecycle fix.
