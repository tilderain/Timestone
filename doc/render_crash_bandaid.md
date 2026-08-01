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
