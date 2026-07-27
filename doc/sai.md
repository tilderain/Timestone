# `sAI` native behavior and rollback safety

**Native reference.** Verified in Ghidra against
`ichneumon_sub/umvc3.exe.unpacked.exe` on 2026-07-27. Addresses use the
preferred image base `0x140000000`.

## Conclusion

`sAI` is MT Framework's generic AI task, FSM, and user-process service. It is
not UMVC3's fighter CPU-opponent controller. Fighter CPU decisions are handled
by `uChrCpu` and `rChrCpu`, principally through `uChrCpu::moveAfter` and
`uChrCpu_processScript`.

The stage-object users found in Ghidra are primarily presentation systems:
background animation, environmental effects, visibility/culling, camera shake,
and scripted visual reactions. UMVC3's selectable stages appear to share the
same combat rules: no stage-specific hazards, walls, damage, pushback, or stage
boundary simulation was found in this analysis.

Skipping both `sAI` frame callbacks during rollback re-simulation is therefore
likely gameplay-safe when the purpose is to avoid replaying presentation-stage
tasks. The remaining unresolved question is whether the `sAI` queued-task list
can contain a non-stage, gameplay-authoritative task during a match. That is a
narrower risk than the stage-object state itself and should be checked before
making the optimization unconditional.

Disabling `sAI` identically and permanently on both peers before a match should
remain deterministic, but can suppress visual stage updates and is not
equivalent to preserving native presentation. It is not a supported way to
disable fighter CPU opponents.

## Singleton and layout

- `sAI::getInstance`: `0x1401CFF50`
- singleton storage: `0x140E18EE8`
- `sAI` constructor: `0x140646D00`
- allocated instance size: `0x80`
- handler-list head: `sAI+0x48`
- handler-list tail: `sAI+0x50`
- queued-task head: `sAI+0x58`
- queued-task tail: `sAI+0x60`
- CPU frequency used by task performance watchdogs: `sAI+0x32`
- task-performance table count/pointer: `sAI+0x6C` / `sAI+0x70`

The constructor builds a performance-information table from the AI DTI tree
and registers service handlers including `cAISvDynamicEnum` and
`cAISvUserProcess`. `sAI::findHandler` at `0x140647710` is consequently a
service-registry lookup, not a fighter-input operation.

## Per-frame calls

`sMvc3Main::move` invokes two `sAI` passes:

1. The `sAI` vtable `+0x30` call resolves to `sAI::move` at `0x140647850`.
2. A later direct call invokes `sAI::subStepHelper` at `0x140647910`.

Both walk the intrusive handler list at `sAI+0x48` and invoke each selected
handler's vtable `+0x30`. Handler flags at `handler+0x14` split work between the
passes:

- `sAI::move` handles entries with flag bit 0 clear.
- `sAI::subStepHelper` handles entries with flag bit 0 set.
- Flag bit 1 participates in pause filtering in both passes.

After its handler pass, `sAI::move` also walks the queued-task list at
`sAI+0x58`, appends jobs to `sMain`'s job array, and calls
`sMain::executeJob` at `0x140521230`. Job completion runs
`sAI__jobCompletionHandler` at `0x1409051E0`, which can invoke a task callback
and unlink the task from the `sAI` queue.

## Stage-task connection

`cAITask` construction at `0x1406482F0` registers the task through the `sAI`
service registry. Confirmed constructors embedding a `cAITask` include:

- `uStageObject::ctor` at `0x1401EAFD0`
- `uStage::ctor` at `0x1401F25F0`

Stage-object code exposes FSM commands such as random-value updates, pending
action transitions, visibility changes, deletion, and task/job allocation.
These are evidence that stage objects do real work, but not evidence that they
affect combat. `uStageObject::filterAndAddHitNode` does submit a hit-solver node
for character-related stage reactions; the recovered callbacks use character
camera/action-side objects and do not show damage, pushback, boundary, or
fighter-state changes.

## Fighter CPU distinction

UMVC3 fighter CPU control is separate:

- `cChr::setCpuController`: `0x14005A2A0`, stores the controller at
  `cChr+0x67F0`
- `uChrCpu::moveAfter`: `0x140120AE0`
- `uChrCpu_processScript`: `0x140116C40`
- `rChrCpu_dispatchScriptState`: `0x140116CE0`

Disabling the two `sAI` calls therefore is not a supported way to disable CPU
opponent decisions.

## Rollback implementation impact

The current headless tick calls both native passes in
`UMVC3Hook/code/rollback_tick.cpp`:

- `mpAI` vtable `+0x30` near line 248
- `sAI::subStepHelper` near line 281

That is the current conservative choice, but the stage-object snapshot concern
should not be treated as established gameplay state. `Rollback_BuildRegions`
does not snapshot the pointed-to `sAI` object or its task lists, and it does not
capture ordinary stage objects. Based on the recovered stage code, this mainly
means presentation state can differ after a rollback, not that fighter combat
state necessarily diverges.

The open rollback question is the queued-task list at `sAI+0x58`: `sAI::move`
transfers queued tasks into `sMain` jobs and executes them. Before skipping the
passes unconditionally, instrument that list during an active match and
identify the concrete task types. If it contains only stage/presentation tasks,
omitting `sAI` from re-simulation is a reasonable optimization.

If profiling motivates removal, add diagnostics around `sAI+0x58` and task
status/type fields, then compare fighter state and checksums with and without
the calls. Stage visual differences should be treated separately from combat
desync. Static-stage smoke tests are useful, but the queued-task types are the
key missing evidence for an unconditional change.
