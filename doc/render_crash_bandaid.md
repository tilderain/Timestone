# Render Crash Bandaid

The render crash handling in `rollback_runtime.cpp` is a horrible bandaid hack,
not a root-cause fix.

Repeated Felicia `Forward Throw` autotests exposed stale material state and render
command arena exhaustion after match restart. Several functions in `umvc3.exe`
assume those states cannot occur and continue through null pointers. The current
workaround patches native instructions to redirect those paths to existing
cleanup code. It also skips a material destructor call that can dispatch through
a stale vtable.

This approach is intentionally version-specific and carries real risks:

- Any game executable update can invalidate the expected instructions or branch
  offsets.
- Skipping the material destructor can leak material objects during restart.
- Guarding allocation failures hides the reason the render arena is exhausted.
- Additional stale-state failures may appear elsewhere in the renderer.
- Passing the focused autotest proves only that the observed crash chain is
  suppressed; it does not prove correct material ownership or renderer state.

## Commit 64dabfe: Restart and Render Workarounds

The `fix rollback restart stale render state` commit extends the bandaid beyond
the original material-resource guard:

- `Rollback_RemoveStaleUiUnits()` walks the `sUnit` category lists and unlinks
  nodes whose vtable is the generic `MtDTI` vtable. It runs during the restart
  path and again before stale-pointer cleanup.
- `hkGameUiMove()` now advances the game UI during headless resimulation
  instead of skipping it, so UI-owned objects are less likely to remain
  registered after native destruction.
- `hkMenuOptionKbdMove()` skips a stale generic `MtDTI` object, and
  `hkMaterialResourceState()` rejects inaccessible material/resource pointers
  before calling the native material-state function.
- Snapshot restore skips regions whose first word is the `MtDTI` vtable. The
  snapshot builder also stops saving the unowned `uCharacterPost` camera state.
- The native render command allocation paths are patched at startup with
  expected-byte checks. Null command and bucket allocations branch to existing
  failure/cleanup paths, a type-3 path is skipped, and one material destructor
  call is replaced with NOPs.

These changes are defensive lifecycle workarounds, not ownership fixes. They
can unlink live objects, omit state that would otherwise be restored, alter
native control flow, or leak resources. The patches are tied to the current
executable layout and are installed only when their expected instruction bytes
match. The commit should therefore be treated as temporary diagnostic code
until the stale UI/model ownership and render-arena lifecycle are fixed at
their source.

## Current Uncommitted Worktree: Effect and Scheduler Bandaids

The worktree currently contains additional experimental changes that are not
part of `64dabfe`. They try to keep the Forward Throw path alive by modifying
several native ownership boundaries at once:

- `rollback_effect.cpp` hooks Dr. Strange's summon update, effect drawing,
  effect movement, both `uEffect` destructor paths, particle sub-buffer
  release, and pending-effect flushing. During resimulation these hooks skip
  presentation-only creation, reject inaccessible resources, detect recycled
  generator chains, force effects into the retired state, detach particle
  buffers, and remove invalid pending entries.
- `rollback_effect.cpp` also clears effect-owned resource, generator, particle
  manager, and stage references based on hard-coded offsets and pointer/vtable
  heuristics. The manager array is retained in some cases while its buffers
  are zeroed; in other cases the entire manager pointer is discarded.
- `rollback_cleanup.cpp` adds Dr. Strange-specific handling for fields
  `+0x68F0`, `+0x68F8`, and `+0x6900`. It calls a vtable method at `+0x78`
  when it appears executable, then clears the fields. The new detach function
  is currently declared and defined but has no caller, so this path is not a
  complete lifecycle solution.
- `rollback_hooks.cpp` replaces native scheduler entry rebuilding on the live
  path with a hand-written allocator and array reconstruction. It also adds
  extra gauge, keyboard-menu, material-resource, and scheduler pointer
  guards. The resimulation path still forces scheduler state to retired rather
  than rebuilding the native scheduler.
- `rollback_restore.cpp` clears the same three Dr. Strange fields after
  character restoration, in two restore locations, instead of restoring their
  historical values. This deliberately drops presentation state from the
  snapshot without proving that the live owner has released it.
- `rollback_cleanup.cpp` disables the generic stale-`sUnit` unlink operation,
  and `framestepper.cpp` removes its restart call. This avoids mutating a list
  that may still be owned by the scheduler, but leaves the original stale-node
  problem unresolved.
- `Makefile` adds `-ldbghelp`, although no matching `dbghelp` API use is
  present in the current source diff. The autotest runner and its test fixture
  also change suite-completion detection, which changes test reporting rather
  than fixing runtime ownership.

This is a bandaid because it reacts to symptoms after rollback has already
produced stale or mismatched objects. It relies on executable-specific
addresses, undocumented object offsets, vtable executability checks, direct
state-bit writes, `memset` over native ownership fields, skipped updates, and
manual allocator calls. Any of those can leak resources, double-release an
object, orphan it from `sUnit`, suppress a legitimate effect, leave the
scheduler internally inconsistent, or turn a crash into a freeze. The custom
scheduler reconstruction also does not reproduce the native rebuild contract,
so successful allocation cannot establish correctness.

Observed validation so far reflects that limitation: the original effect
destructor crash was avoided, but execution then reached scheduler rebuild and
hit a null-allocation failure. After adding the custom scheduler path, the
allocation-failure logs remained and later runs froze before the autotest suite
started. This work should therefore remain diagnostic until the actual effect
and scheduler ownership transition is identified and fixed, after which these
guards should be removed or reduced to narrowly justified validation checks.

The expected-byte checks prevent blindly patching an unknown executable, but do
not make the design sound.

The proper fix is to find and repair the rollback/restart lifecycle error that
leaves Felicia model or material state alive after its owner is retired. Once
that ownership boundary is corrected, remove the render allocation patches and
the skipped destructor. The narrow material-resource validity hook may remain as
defensive protection only if there is independent evidence that native resource
teardown can legitimately race rendering.

Do not extend this workaround with more byte patches unless a new crash must be
temporarily unblocked and its exact control flow has first been confirmed in
Ghidra. Every additional patch increases maintenance risk and moves the project
farther from the actual lifecycle fix.
