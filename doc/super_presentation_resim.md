# Super presentation during resimulation

Investigation: 2026-09-11, Ghidra instance 8192,
`umvc3.exe.unpacked.exe`, image base `0x140000000`.

Status: ExAtkSpace-only retention trial implemented below; live visual testing
and in-game timing remain outstanding. Cut-in preservation is still a proposed
design, with no runtime changes in this trial.

## Verified ExAtkSpace ownership and loss path

- `sAction+0x148` holds the current attack-space object. The compact action
  restore preserves this live pointer (`SACTION_RESTORE_RANGES`).
- `uExAtkSpace__ctor @ 0x140136D90` calls
  `uExAtkSpace__spawnOwnedEffects @ 0x140137D90`. The latter creates four possible
  child effects at `+0x1D18/+0x1D20/+0x1D28/+0x1D30`, sets owner/attachment and
  render flags, and registers the children in category 14. Most use the short
  effect factory with resource type `0x65`; the optional stage child uses type
  `0xB`. Retaining only the parent model cannot retain this visual graph.
- Before the trial, `Effect_RetireCurrent` called `DetachExAtkSpaceChildren`, which unconditionally
  zeros those four fields. Its category 13–16 pass then destroys ordinary
  effects, including the former children. Generic effect recipe reconstruction
  does not reconnect the four ExAtkSpace owner slots.
- `uExAtkSpace__syncJobLifecycle @ 0x140138250` is virtual `+0x50` (previously
  named `getName`). It clears dead child references, samples character flags
  `+0x1508 & 0x40000`, and consults the action's destroy request. It can send
  children finish requests through `+0xD8` and set local step 1.
- `uExAtkSpace__moveAfter @ 0x140137B60` is virtual `+0x48` (previously named
  `getBoundary`). At step 1, absence of all four children advances to step 2;
  step 2 calls the parent's native finalize virtual `+0x78`.
- `uExAtkSpace__finalizeOwnedEffects @ 0x140137A10` (previously named `setModel`)
  finalizes the model, sets local step 3, retires children through native
  `+0x78`, and clears the owner slots.
- Category 13 runs during headless resimulation. Thus its live, unrewound
  lifecycle can also observe historical character/action state.

The prior detachment was an intentional dangling-pointer/crash fix; see
`doc/shot.md` around the 2026-07-24 23:31:48 crash. Removing the memset while
continuing to destroy the children would reintroduce that bug.

## ExAtkSpace-only trial implementation

`rollback_effect.cpp` now collects the current native owner's four children in
a fixed 32-byte stack array. It verifies the owner implementation, requires live
owner/child states, and validates child effect types. Retirement retains those
effects and their owner references; invalid/dead/unretained child slots are
still detached before destruction. Both ordinary retirement and detached-resim
effect reclamation exclude retained children. Capture omits independent recipes
for them, and restore reuses an already retained child if a recipe matches.

`rollback_tick.cpp` excludes ExAtkSpace from headless move, sync-job, and
sync-save object dispatch using its common native virtual `+0x50` implementation
at `0x140138250`. Category 13 already uses this dispatch in both profiled and
non-profiled builds. This adds a virtual-slot comparison to the existing path,
without another DTI hierarchy walk. Initialization, state 3/4 cleanup, and
ordinary native frame updates still run normally.

Additional Ghidra evidence: `sAction__createByLevelAndAttach @ 0x14014DB70`
finalizes the current ExAtkSpace before creating/registering its replacement.
The trial leaves this native replacement path intact: a replayed startup can
restart the backdrop graph, but does not leave a second active owner. It also
leaves action-side explicit teardown intact, including the auxiliary-object
handling in `sAction__subStepHelper @ 0x14014F410`.

The intended result is that resim within an existing super keeps its background
and does not advance its finishing lifecycle against historical character flags.
Native ordinary frames still finish/interrupt the backdrop. This is presentation
retention rather than exact historical visual rewind; no particle snapshots,
new hooks, event tracker, or additional native simulation calls were added.

Runtime checks for this trial: repeated resim inside a super, rollback across
startup, DHC/level replacement, natural expiry, interruption, and repeated
restore without accumulating effect units. Cut-in disappearance is not addressed
by this trial.

## Verified cut-in ownership and loss paths

### Native registry

`uCutIn__ctor @ 0x140448C10` registers with `sGameUI`, and
`uCutIn__dtor @ 0x140448E50` unregisters. Their common helper at `0x1402BFCF0`
has been typed and renamed `sGameUI__registerCutin` in Ghidra:

- `sGameUI+0x5670`: native `MtArray<uCutIn*>` header.
- `sGameUI+0x5678`: count.
- `sGameUI+0x5688`: entry allocation pointer.
- `sMvc3Manager+0x80`: `sGameUI*`.

The existing visibility/orphan repair instead reads `sMvc3Manager+0xB50`.
That is not the registry used by these constructors/destructors. Ghidra's
`sMvc3Manager` layout is only `0x170` bytes. The existing orphan helper can
also write zero to that incorrect offset. Its call sites must be distinguished
from `Rollback_SyncActiveCharacterVisibility`, which is actually called at
resim exit. The latter can route real cut-in participants back to normal
rendering because the ownership test fails.

The registry supports multiple cut-ins; a single global cut-in pointer is not
an adequate replacement, particularly for team supers/DHC.

### Update/retirement

`anmchrCmd1_interpreter`, command `0x86` (allocation around `0x1400FD326`),
constructs a cut-in, adds the attacker/associated characters, sets the variant,
and registers it in category 3. The command allocates a new object whenever it
is replayed; there is no native rollback-event deduplication here.

`uCutIn__init_spawnChildren @ 0x14044B660` creates:

- The owned camera at `+0x670`, registered in the parent's category + 1 (4 for
  battle cut-ins).
- Scheduler/light children, many registered in category 0.
- UI sprite state and the native participant/collection arrays.

`uCutIn__moveAfter @ 0x14044AF10`:

- Caches the first participant's **motion number** at `+0x7A8`. It obtains the
  number through `uModel__getMotionSlot(0)` and `MtAnimState__getMotionNo`.
  A later mismatch calls native finalize. The previous Ghidra comment called
  this a hardware-affinity/group comparison; that comment was corrected.
- Advances elapsed time `+0x750`, compares duration `+0x754`, advances fade
  timers `+0x79C/+0x7A0`, and checks the owned camera's animation end.
- Rebuilds the three bands of collected model/effect/shot references and
  applies cut-in lighting/material state.

Current headless filtering enables category 3 but excludes camera category 4
and scheduler category 0. The cut-in object is not captured as a concrete
snapshot. Consequently an existing cut-in can be aged again or finalized
against a rewound participant motion, while its child timeline is not aged
consistently.

`uCutIn__destroy @ 0x14044A900` restores normal draw routing, clears participants,
retires camera/scheduler children, and destroys its owned sprite. Changing the
parent's alive flag afterward cannot reconstruct this graph.

### Render state matters separately from object lifetime

`uCutIn__setCutinRenderMode @ 0x14044B310` routes characters and weapons into
cut-in draw bands and remembers original visibility at `+0x758` (16 entries).
The enable path is guarded by the byte at `+0x7A4`. A repeated enable call does
not necessarily repair routing overwritten by snapshot restoration when this
byte is already set. Repair must preserve the original visibility history
needed by native teardown.

`uCutIn__render @ 0x1404497E0` uses the owned camera and the collected render
graph. Those collections contain shot/effect references, so retaining a cut-in
also requires rebuilding its collections after shot/effect reconstruction,
before any renderer consumes them.

## Recommended low-overhead preservation design

Use **presentation-frontier retention** for already displayed super graphs:
keep their native allocations and presentation clocks at the last displayed
frame while gameplay rewinds and catches up. This avoids reconstructing
cameras, schedulers, native arrays, material resources, or particle pools on
every rollback.

Implementation needs the following coordinated pieces:

1. At restore entry, collect the current ExAtkSpace and its four owned children,
   plus active cut-ins from the native `sGameUI` registry. Retain complete live
   graphs. Exclude retained ExAtkSpace children from both ordinary effect
   retirement **and** independent recipe reconstruction/capture. Do not leave
   raw child references pointing at destroyed allocations.
2. Keep retained super presentation from advancing/finalizing against historical
   gameplay during catch-up. Cover move and both sync phases, and account for
   action-side attack-space replacement. The non-profiled category-3 path
   currently bypasses `IsHeadlessGameplayUnitObject`, so editing that predicate
   alone would not fix normal builds.
3. Reconcile births by logical event identity (simulation frame, character,
   command/variant), rather than recycled allocation address. Replayed events
   already represented by a retained graph should not allocate another graph.
   Newly confirmed events must still create presentation; corrected-away events
   must retire through native finalization. Blindly suppressing all cut-in
   creation during resim would lose newly confirmed supers.
4. After catch-up, rebuild cut-in collected references once from the current
   shot/effect graph, and repair participant/weapon render routing from the
   real active cut-ins. Preserve native original-visibility bookkeeping and
   include every active cut-in, not just the current point character.
5. Resume normal native updates/retirement on the next ordinary frame. Finished,
   interrupted, or cancelled supers must release their graphs normally.

This is a proposed implementation, not a verified runtime fix. The cheapest
partial edits (only excluding category 3, only preserving child slots, or only
restoring alive flags) each leave a known lifecycle/ownership problem above.

### Cost model

- ExAtkSpace discovery/retirement membership: four pointer slots; comparisons
  can be folded into the existing effect pass.
- Cut-in discovery: one pass over its dedicated native registry, rather than
  every scheduler category or particle graph.
- Retained update filtering: cheap object/type membership at existing dispatch
  points; no new particle work.
- Collection/render repair: once at catch-up completion, not once per resim
  frame. Reuse native collection capacity.
- Event reconciliation: small logical records; no historical animation replay
  or frame-age-dependent loops.

The work should remove repeated native allocations and cut-in update work, but
there is no measured frame-time percentage. Idle-frame overhead should be
limited to empty-owner/registry checks.

## Validation cases for implementation

- Restore repeatedly inside a live blue background or portrait: graph identities
  and displayed clocks remain stable; no progressive early disappearance.
- Rewind across super startup: one graph per logical event, including a super
  first discovered on the corrected trajectory.
- Correct a predicted super away: no retained permanent overlay/hidden fighter.
- Single supers, team hypers, DHC, simultaneous cut-ins, and interrupt/end paths.
- Reconstructed shots/effects during a cut-in: no stale collection references.
- Normal exit restores participant and weapon draw routing/materials and retires
  all children; repeated rollbacks do not accumulate units or allocations.
- Profile ordinary restore/resim and effect-heavy resim separately, with unit
  profiling both enabled and disabled.
