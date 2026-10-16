# Material resimulation skip audit

Further post-only research: `material_post_scope_20261016.md` establishes
exact weapon/helper vtables and a narrower inner-loop interception candidate.

Research: 2026-10-16, Ghidra port 8192, `umvc3.exe.unpacked.exe`, base
`0x140000000`, checked against current snapshot/restore code. This investigates
omission safety, not a new optimization implementation. Material timing remains
compiled out by default (`UMVC3_NATIVE_MATERIAL_TIMING=0`).

## Decision

| Intervention | Finding |
|---|---|
| Return from material jobs `0x140200F50` during resim | Reject as a general policy. Job active/completion state has concrete character and shot-lifecycle consumers. |
| Return from every individual material-job move | Reject. In addition to ordinary dispatch, commands can invoke job moves synchronously. |
| Return from shared material post `0x140200CC0` | Not proven safe globally. Broader caller set, temporal material state, resource references and virtual `+0x230` need a policy. |
| Coalesce allowlisted post evaluation | Plausible follow-up, not yet proven. Define material clock ownership, readers, retired objects and first-visible rebuild first. |
| Omit job application while preserving job clocks | Insufficient in general. Material feedback, controller frames, completion/reset/retirement and target-validation ordering must also survive. |

No blanket skip was installed. No claim that a gameplay desync or damage
change has been demonstrated; confirmed lifecycle/control-state differences
are already enough to reject equivalence.

## Concrete job-state consumers

### Doctor Strange reflector

`uShotLaserStrangeReflector__waitForWeaponAndCallbacks @ 0x140171580`:

1. Tick vanish state.
2. Obtain callback effect slot 3.
3. Obtain its color job 0 (`0x1401715A5`).
4. Read job active (`0x1401715B2`).
5. Only inactivity invokes `finishGameEffectModelCallbacks(UINT_MAX)`
   (`0x1401715C1`).
6. Once callback slots are empty, OR shot `+0x212C` with `0xFF`
   (`0x1401715D2`).

Freezing the job can delay callback-effect retirement and shot-state
completion. The job is accessed through an effect model, so this is a direct
counterexample to a global shared-dispatcher skip. It does not alone prove
damage/collision changes, network desync or effect-pool exhaustion.

### Common character and Galactus

Within the SPC-enabled branch of common character move `0x14004BD30`, color
job 2's active state sets character `mMaterialEffectFlags` at `+0x152C`, bit 2.
When it becomes inactive with that bit previously set, the character invokes
virtual `+0x438`, clears the bit and invokes `+0x220`. The active read is at
`0x14004C00F`. Downstream gameplay consequences of those overrides remain
unproven; their timing is nevertheless different under a full job freeze.

The same branch reads the material base-map factor through controller 0 and
can copy it to character vector `+0x68E0`. Thus at least one material value is
read back into character state; material output is not universally write-only.
The vector's confirmed getter is `0x14004BC10`, setter `0x14004C820`.
This evidence does not establish a combat use of the vector.

Galactus reads color-job active before and after common move at
`0x140086FBF` / `0x140087093`; a `1 -> 0` edge invokes `+0x438` at
`0x1400870AC`.

### Synchronous command execution

A shared animation-command tail checks zero duration at `0x140106243`,
invokes job move through `+0x40` at `0x140106255`, then reset through `+0x88`
at `0x14010625E`. Hooking individual job moves would intercept this path too;
hooking only the seven-table dispatcher would not. A final refresh cannot
simply replay all surviving active jobs and assume it reproduces completed
zero-duration commands.

## Allocation and inactive jobs

`0x1401FFCA0` constructs seven preallocated arrays using packed counts.
Color accessor `0x140042000` and pattern accessor `0x140107800` select explicit
indices after bounds checks, not the first inactive slot. Command paths at
`0x140105C0A`, `0x140105E62` and `0x140106E7F` reset/configure explicit color
slots without testing old activity. Configuration `0x1401419A0` reaches
activation `0x14013F410`, which zeros time and sets active.

There is no confirmed material-job free-list exhaustion mechanism here. Delayed
owning effect/shot retirement is a separate, confirmed risk; pressure on a
separate object pool has not been measured.

Six inspected job types run base move before guarding derived work on active.
An inactive-entry optimization must still retain base owner/target cleanup.
Animation job `0x14013FF40` is different: completion/reset/retirement handling
can execute outside the active guard. Its reset defaults `time == endTime == 0`
already satisfy the ordinary completion comparison. Do not use an unconditional
`if (!active) return` policy for every type.

Base trans precedes base target cleanup. Trans `0x14054A430` can clear an
invalid target before the later check. Removing trans wholesale can therefore
introduce a different retirement branch, even if no animation was visible.

## Material post is broader than weapons

Direct references to `0x140200CC0` include:

- Weapon post `0x14003736E` and helper wrapper `0x1400A30CA`.
- Shot post `0x1401CD3EB` and effect-model post `0x14013C6A3`.
- Stage-object path `0x1401EC525` and stage post `0x1401F462E`.
- The already-suppressed character thunk `0x1402021E0`.

The body advances materials and tail-dispatches virtual `+0x230`. Ordinary
target `0x140200B60` copies stage/cut-in light into embedded toon parameters;
arbitrary subclass overrides have not been exhaustively enumerated.

`uToon__applyCutinLight @ 0x140451410` copies scalar/vector fields to model
`+0x12C0`. `uToonModel__renderToonShading` consumes those parameters to populate
a draw parameter block. This is affirmative rendering-consumer evidence for
the inspected light path, not proof that every `+0x230` override is visual.

### Material setters are not all plain stores

For MaterialChar vtable `0x140B096D0`, animation update is `0x1405E0530`.
Its float/int/vector/texcoord setters write material constant state, but:

- Sampler setter `0x1405DEDB0` can release the old resource, increment the new
  resource reference and replace the binding.
- Texture setter `0x1405E0170` also performs release/incRef/binding changes.

Skipping evaluation changes resource-reference timing as well as visual values.
A deferred list must not retain raw material/model pointers across retirement
without validated ownership. The existing material reset/cache safeguards are
not a proof that such pointers remain valid.

The four sequence clocks are advanced only after sampling. A clock-preserving
split must retain native per-step wrap/clamp behavior and the final pre-advance
sample, not simply sample the final advanced clock. Conversely, intentionally
freezing live presentation clocks is a different policy, not an equivalent
implementation of clock-preserving replay.

## Snapshot ownership matters

`rollback_snapshot.cpp:1100` captures heap-backed material-job tables along
with their owner slots/counts. `rollback_restore.cpp:1762` rewinds table bounds
only when the live owner still identifies the captured allocation.

In contrast, character restore explicitly retains live base-model resources,
materials, caches and cache flags (`rollback_restore.cpp:199`), and preserves
the model tail `+0x1160..+0x1300`, including toon parameters. Runtime material
objects' clocks/constants are not thereby rewound with job clocks.

This split explains why neither simplistic policy is established correct:

- Running all historical post work can advance live presentation state again.
- Freezing all job work leaves rewound job clocks, active bits and lifecycle
  behind the corrected simulation.

A deliberate live-presentation policy for post work may be reasonable, as the
existing character-only skip demonstrates, but that is not validation of a
global skip for shots, effect models and arbitrary helper subclasses.

## Recommended next implementation boundary

Keep all job progression/lifecycle native. Do not use the measured whole-job
time as a removable-work measurement.

For post work, an eventual candidate should be limited to known concrete
classes/material update targets and a known `+0x230` implementation. Choose
explicitly between live-clock freezing and clock-preserving evaluation splitting.
Audit material readback/feedback for that scope and rebuild corrected bindings
and light before the first draw, including completed jobs and replacement
objects. Retired objects must not be refreshed through stale pointers.

The lower-risk alternative is to optimize repeated material-ID and animation
sequence lookup within native job ordering. That preserves the lifecycle and
write order that a broad skip would discard. Persistent caches require a
separate invalidation/ownership proof.

The latest retained fighting-state capture measured inclusive post cost at
0.176 ms client / 0.246 ms host per seven replayed frames, and jobs at
0.075 / 0.076 ms. These are whole-path budgets in one workload, not expected
savings. Neither broad skip currently has an equivalence proof or an in-game
correctness result.

Ghidra changes: applied existing `nDraw__Material` types to state lookup and
animation setters, corrected the misleading joint-lookup comment, typed the
toon-light copy wrapper, and retained targeted job-consumer/skip audit notes.
No runtime skip or material profiling was enabled by this investigation.
