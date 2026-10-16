# Effect child work during headless resimulation

Native audit: 2026-10-12, `umvc3.exe.unpacked.exe`, image base `0x140000000`.
Status: static research; no optimization implemented or runtime equivalence
test performed. See `effect_lifecycle.md` and `effect_rollback_design.md` for
the wider ownership/snapshot constraints.

## Verdict

Child effects are not one dependency class. Linked child `uEffect` lifetimes
feed parent completion and cannot generally be frozen or suppressed.
Synchronized Model/Light/GrassWind/Adhesion children have promising removable
presentation work, provided creation outcome, liveness, retirement, admission
counts and parent particle progression remain equivalent.

This qualifies the earlier blanket claim that child processing must all run:
there is evidence for separating child presentation from lifecycle. It does
not prove that all children can be omitted or that a timer-only parent is safe.

## Linked effects: preserve their completion contribution

- `cEffectUnitGenerator__move @ 0x140842E20` emits whole child effects,
  registers them at parent category + 1, and binds the parent/resource/token.
  Its zero-spawn completion branch requires parent `mChildUnitMoveNum`
  (`+0x1FC`) == 0 plus generator retiring status.
- `uEffect__sync @ 0x1405E8690` clears the aggregate root census; valid linked
  children increment it. Validity includes parent active state, generation
  token equality and nonzero parent generator count. Invalid links kill the
  child. `uGameEffect__sync @ 0x14013AC30` contains the same contract.
- `uEffect__moveChildUnit @ 0x1405E5B40` advances a child-owned particle record
  using the parent generator, issues finish/hold-release transitions, and
  derives child placement. It is not merely a draw submission.
- Parent generator continuation reaches `uEffect__moveAfter`, then effect
  sleep/kill. Shot callback pruning observes effect active state, so the
  dependency can extend from child lifetime to shot completion.

Skipping creation leaves a zero census where native execution may have live
children. Freezing children while preserving census can keep the parent alive
too long. Suppressing only their sync can make it finish too early. A reduced
implementation must reproduce child continuation/lifetime and phase ordering,
not simply retain or zero the last count.

## Synchronized children: presentation candidates

These children are stored in ordinary particle `+0x60`. They are distinct from
the linked child-effect census above.

| Family | Inspected native work | Headless opportunity and constraint |
|---|---|---|
| Model | `getSynchroModel @ 0x140896920`; generator `update @ 0x140898730`; child `moveAfter @ 0x14097AC00` | Parent post-update builds matrices/quaternions, part visibility, material color, transparency and cull payloads. Child move validates owner, copies draw/camera state, rebuilds base-model transforms and advances materials. Strong presentation candidate, not a whole-generator skip. |
| Light | `updateChildLightUnits @ 0x1408D85C0`; child PointLight `moveAfter @ 0x14097B3E0` | Parent post-update writes light parameters, transforms and cull state. Child lighting updates are presentation candidates. Preserve creation/failure and particle retirement branches. |
| Force/GrassWind | `initializeParticle @ 0x1408E4AF0`; `updateParticle @ 0x1408E5090`; `updateChildGrassWindUnits @ 0x1408E5600` | Post-update writes grass-wind position/direction/strength. Inspected force particle update reads child active state, not simulated wind output. Creation and liveness cannot simply disappear. Transitive grass-wind consumers were not exhaustively audited. |
| Adhesion | `getSynchroAdhesion @ 0x1408795F0`; generator `update @ 0x14087D590`; child `moveAfter @ 0x14097AAF0` | Parent post-update does extensive surface/geometry projection, buffer/record updates, color/UV and cull work. Child move itself only validates owner and copies draw/camera state. Preserve admission, creation, particle controls and teardown; projection remains a candidate, not certified removable. |

For exact ordinary `uGameEffect` vtable `0x140A8E180`, inspected correction
callbacks are benign: `+0x110` is no-op, `+0x118` returns original environment
map power (`0x1405E27B0`), and `+0x128` copies original light color
(`0x1405E2790`). This does not classify derived effect overrides.

The exact synchronized-model vtable `0x140BCF240` resolves base-model move's
`+0x90/+0x98` calls to local-matrix update (`0x140548570`) and world-transform/
bounds update (`0x140564320`), not skeletal gameplay animation.
`uBaseModel__moveAfter` advances material tracks. Model particle animation
completion is separately evaluated by
`cParticleGeneratorModel__moveParticleAnim @ 0x140897890`; preserve that even
if child material/transform work is omitted.

## Creation and failure are observable to the parent

- Model and Light post-update call their child getters. A null result calls
  `cParticleGenerator__closeParticle`, changing the parent's active pool.
- Force initialization returns false on child allocation failure. Its concrete
  particle update immediately returns false if the child is no longer active.
- Adhesion post-update also recycles a particle if its child getter fails.
- `ParticleRecord__validateActiveChild` checks only the child's state 1/2 and
  clears a non-active pointer. That liveness check is not child render simulation.
- `cParticleGenerator__closeParticle @ 0x140827680` requests child retirement
  before moving the parent particle from active to free lists. Final unit
  destruction is scheduled separately.

Returning null from a suppressed factory is therefore not equivalent to
successfully creating an invisible child. A proxy or deferred-creation design
would require a separate ownership/admission contract; it is not a small skip.

## Adhesion collision does not establish gameplay hit dispatch

The inspected Adhesion update queries collision/surface geometry and builds
projected records. Its call graph includes triangle/sprite-cell extraction and
line queries. No damage or attack dispatch was identified in the inspected
path. Querying collision for a decal is not evidence of a gameplay hitbox.

However, limit-flagged child creation increments `sEffect+0x40C`, destruction
decrements it (`uSynchroAdhesion__dtor @ 0x14097A950`), and admission
`sEffect__isCreateAdhesion @ 0x140543C90` compares it with limit `+0x408`.
The recovered direct call to this admission query is the Adhesion child getter.
Denied creation recycles the parent particle. This is a proven admission-to-
particle-lifetime dependency, not proof of a damage-collision dependency.

Projection also writes particle rebuild byte `+0x10A` and record count `+0x108`.
These cannot casually remain stale across skipped ticks and the first visible
update. Buffer-allocation failure is distinct from child-creation failure.

## Current source and useful interception boundary

`rollback_tick.cpp:75-118` excludes categories 14-16 and 19 from ordinary
headless move/sync. `HeadlessUnitPass` still processes retiring units in skipped
categories (`:507-522`). Thus child scheduler movement may already be absent;
another blanket child-category skip is not the principal new opportunity.

An enabled parent still executes `uEffect__updateParticleManager`, dispatching
generator virtual `+0x98`. Model, Light, Adhesion and Force child updates occur
inside these callbacks. They can allocate/configure children even when those
children receive no normal scheduler update.

The promising split is inside those concrete parent post-update callbacks:
preserve child acquisition and native null handling, particle retirement,
admission accounting, required particle-control writes and owner callbacks;
omit audited child-only material/light/geometry/transform payload generation.
Do not disable every generator `+0x98`: Custom dispatches owner `+0x180`, and
other concrete generators have different contracts.

Before enabling this split, verify exact-resource/DTI eligibility, future reads
of omitted fields, reconstruction on the first corrected draw, and shared
resource/material effects. Compare native and reduced execution for parent
pool topology, emission counters, continuation, child active states, callback
slots, shot completion, RNG and admission counts. Linked effects need a separate
continuation-equivalent design. No numeric savings are established by this audit.

## Ghidra annotations

Applied existing `uSynchroAdhesion` type to its owner-taking constructor and
to the misidentified child local in Adhesion update. No new speculative struct
layout was introduced. The runtime plugin was not changed.

## Follow-up: can we remove the linked-child wait?

Further native tracing on 2026-10-12 establishes concrete consumers of earlier
effect retirement. It does NOT establish the aggregate-generator configuration
of these resources: no loose EFL or ARC files were located in the workspace,
and this static Ghidra session does not provide a live loaded-resource graph.
The distinction is important: gameplay observes their callback effects, but
whether each specific callback is an aggregate needs asset/runtime validation.

### Black Hole: effect completion can trigger active shot expiry

`uShotVulcanBlackHole__setup @ 0x14016AC90` constructs ordinary `uGameEffect`
objects, binds Dormammu EFL with group flags 1 and 2, and stores them in shot callback
slots 1 and 2. They are not unrelated anonymous lighting units.

`uShotVulcanBlackHole__advanceAndEnterCleanupWhenCallbacksFinish @ 0x14016C0C0`
advances the shot pose and writes `mLifeFrame = -2.0f` when all callback slots
are empty. Constructor `0x14016AA50` installs vtable `0x140AA71D0`; memory at
vtable `+0x2D8` (`0x140AA74A8`) confirms this common-tick callback.

`uShot__moveAfter @ 0x1401CCEB0`, in active phase 1, tests this negative-life
sentinel and invokes virtual `+0x370`. The same Black Hole vtable resolves
`+0x370` to `uShotVulcanBlackHole__spawnEndEffectAndVanish @ 0x14016AB40`.
That handler requests callback finish and changes shot phase to 2, as well as
spawning an end effect. Earlier callback disappearance can therefore advance
the shot's active-to-vanish transition, not only free an invisible corpse.
Exact lost hit frames/damage require resource data and a runtime test.

### Ouroboros: retirement changes a later setup branch

`uShotShieldOuroboros__updateCleanupCompletion @ 0x140184090` clears character
`mUsrF & 4` when callback slots become empty, then marks the shot terminal.
Setup `0x140184110` creates an ordinary callback `uGameEffect` using Hiryu's
body-selected EFL resource. That same setup checks `mUsrF & 4`: it stores a
different concrete mode at shot `+0x2210` and selects 90.0 versus 270.0 in
the concrete field at `+0x21D0`.

This proves a later native setup can read state affected by callback retirement.
The semantic meaning of the selected float is not asserted here. Kill handler
`0x140183F80` additionally resets character ability type `0x20`; it supplies
alternate cleanup, but earlier shot retirement can still move that cleanup
earlier. Character helper `0x140093820` also clears the latch when that ability
is absent, so the callback branch is not its only writer.

### Soul Fist Absorb: retirement releases a one-shot gate

Setup `0x140195CB0` constructs an ordinary callback effect from Morrigan's
body-selected EFL, group flags 2 with an owner-dependent material mask.
Return completion `0x140195AB0` gracefully finishes callback effects and enters
phase 2. It tests owner `mUsrF & 0x10000` before spawning resource IDs 100/0x26
and requesting sound, then sets that latch (even if effect creation failed).

Cleanup `0x140195C40` clears the same latch when callback slots are empty.
Changing completion timing changes when a later return can pass this gate.
This is a proven character-state/event dependency; it is not proof that this
particular latch directly changes damage or meter.

### Why parent early completion does not detach children

Removing only the `owner+0x1FC == 0` test at `0x140842E20` makes a retiring
aggregate report false once it emits zero children, despite live linked children.
The outer effect then calls `uBaseEffect__finish` (sleep or kill disposition).

On kill, children still have `mpParentGenerator` and validate the parent's
active state/token/graph in both sync and moveAfter. They request kill or take
completion disposition when that parent becomes invalid. Thus this is an
early parent-and-child termination policy, not independent child draining.
On sleep, children inherit parent sleep; they are not automatically detached
either. Detaching needs a separate transform/time/resource ownership design.

The wait test itself is constant-time. Removing that comparison alone does
not remove child particle simulation or expensive presentation work. Most of
the hoped-for saving requires changing the simulation or retirement policy.

### Native aggregate bypass exists, but is not a no-wait option

`uEffect__setUGInvalidMode @ 0x1405E84A0` sets status bit `0x20000` at `+0x100`.
`createParticleManager @ 0x1405E2860` selects normal descriptor generators
instead of an aggregate root when this bit is set (or the resource's aggregate
mode is zero). Only property-registration references to this setter were
recovered. This selects a different construction model, not the same aggregate
graph without a completion wait. Aggregate emission, per-child movement,
repeat timing and lifetime are not demonstrated equivalent. Changing the bit
after construction does not itself rebuild the graph.

### Policy conclusion

- A resim-only no-wait change can differ from forward execution at callback
  liveness, shot phase, character latches, event dispatch and admission timing.
- Applying the policy to both forward and resim may make the changed rules
  deterministic between peers, but does not preserve native gameplay semantics.
  Historical replays also require the same rules/version.
- A specific root could be eligible if no gameplay consumer observes its or its
  descendants' state/lifetime, and RNG, admission and shared resources remain
  isolated. No such concrete resource is certified by this audit. Absence from
  shot callback slots alone is not sufficient.
- For gameplay-coupled roots, preserve logical callback lifetime and use a
  continuation-equivalent reduced child simulation. Simply keeping the callback
  pointer alive with a guessed timer is not equivalent; child finishing depends
  on emission, animation, motion, pause, finish/hold and native phase ordering.

The immediate recommendation remains the synchronized-child presentation split,
not a global change to linked-effect completion. The Black Hole common-tick
signature was corrected using the existing concrete struct in Ghidra.
