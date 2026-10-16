# Concrete moveAfter optimization targets

Research: 2026-10-12, four parallel Ghidra agents plus coordinator source and
instruction review. Target `umvc3.exe.unpacked.exe`, image base `0x140000000`.
No runtime optimization or benchmark was performed. Ghidra annotations/types
were saved. This document replaces speculative percentage estimates with
specific opportunities and implementation constraints.

See `effect_child_headless_audit.md` for why removing completion waits is not
the performance fix.

## Implementation order

| Priority | Change | Evidence and expected work removed |
|---|---|---|
| 1 | Stop spawning after the first empty-stock result | Exact-output candidate; eliminates all remaining side-effect-free failed attempts in that burst. Benefit depends on pool saturation and requested count. |
| 2 | Omit non-synchronized Model post-update for exact ordinary owners | Removes a full particle traversal, interpolation and redundant environment-map-power stores. Strong local proof. |
| 3 | Light acquisition-only post-update | Preserves native child creation/failure/recycling; removes child presentation interpolation, matrices, lighting/cull payloads. Requires exclusion of scene consumers during resim. |
| 4 | Model transform-only post-update split | Removes matrix construction, normalization and quaternion decomposition while initially keeping material/parts and particle dirty-bit handling. |
| 5 | Callback-gated reduced Billboard update | Keep emission, motion, lifetime and retirement; defer sampled color/intensity and eligible presentation endpoints. Broader savings potential with stricter eligibility and final-endpoint reconstruction. |
| 6 | Preserve/reuse graph storage across restore | Larger architectural candidate for allocation/recreation spikes; requires complete state/ownership restoration, not a generic native restart. |
| 7 | Deferred Adhesion projection | Potentially substantial per-call work, but needs historical projection inputs, native query/allocation outcomes and deferred materialization. Not a trivial function skip. |

This is an implementation-risk ranking, not a measured cost ranking. The large
spike could still be in any one of these paths or in legitimate catch-up.

## 1. Exact empty-pool early exit

`cParticleGenerator__openParticle @ 0x14083F430` immediately returns null when
stock head `generator+0xF0` is null. There are no writes, RNG calls or callbacks
on that path. Subsequent failed spawn attempts cannot replenish that list.

Coordinator disassembly verified the surgical candidate in
`cParticleGenerator__move @ 0x140834F30`:

```text
0x14083505D: call openParticle
0x140835065: test rax,rax
0x140835068: jz 0x140835106   ; currently increments attempt index and retries
...
0x140835110: restore rsi
0x140835115: increment generator+0x1F8 once
```

Redirecting only that null-result branch to `0x140835110` exits the attempts
while preserving the frame serial increment, saved-register restoration and
all subsequent concrete updates/completion checks. Preserve the requested
spawn-count denominator and indices for earlier successful spawns. Do not
exit on particle initialization failure: native close returns that record to
stock for the next attempt. Exclusive ownership during this loop is required.

## 2. Non-synchronized Model no-op

In `cParticleGeneratorModel__update @ 0x140898730`, branch
`generator+0x12C & 0x400 == 0`, instructions `0x140898790..0x1408987FE` loop
over particles, interpolate position, invoke owner `+0x118`, then store particle
`+0x118` environment-map power.

Exact ordinary `uGameEffect` vtable `0x140A8E180` resolves that callback to
`0x1405E27B0`, which returns original parameter power unchanged. Model particle
initialization already stores that parameter value. Focused future-read tracing
found particle env-power reads in drawing, not retirement.

An exact-parent/exact-generator headless early return is therefore a strong
local candidate. Exclude derived callbacks and mutable parameter cases. Keep
particle initialization, animation and move untouched. This branch does not
create children. Do not confuse it with synchronized Model mode.

## 3. Light and synchronized Model splits

Light `0x1408D85C0` can be reduced to its original child getter plus null-result
`closeParticle` handling. Its successful body `0x1408D864A..0x1408D8E21` writes
child/stack presentation data, not additional parent particle state.
Preserve allocation/registration and particle `+0x60` updates. Scene registration
means omitted light fields still need a no-consumer argument during resim.

Synchronized Model `0x140898730` is more complicated:

- Keep getter `0x140896920` and native failure recycling.
- A smaller first cut omits `0x1408988BF..0x140898DFA`, transform interpolation,
  matrix construction, normalization and quaternion decomposition.
- A larger split omits the success presentation body but retains particle
  `+0x18 &= 0xFFFFFFFC` at `0x14089902F`.
- The larger split needs separate pending-refresh state: bits 0/1 govern
  material/parts updates and cannot merely be cleared and forgotten. Force
  those dirty bits before the final original presentation update.

Refresh generator-derived inputs before child-derived rendering data. Do not
advance clocks twice during refresh. Child base-model material updates are
stateful; SSS materials can allocate on animated map-size/layer changes.

Also, Light child `uLight::moveAfter @ 0x140696EB0` can allocate/update a helper
at `+0x3B8` when `+0x370 != 0`. The simplest child-side presentation suppression
excludes this case. Parent post-update removal and child scheduled movement
are different interventions; many child categories are already skipped today.

## 4. Billboard reduced presentation

Use ordinary Billboard concrete update `0x14084E2C0` and loop `0x14084EBB0`.
Keep native emission/spawn initialization, active/free topology, RNG consumption,
shared movement/lifetime, concrete visit count `+0x112` and close ordering.

Candidates to defer are color/intensity sampling and presentation endpoint
writes, sampled absolute orientation, and render-scale multiplication. Keep
animation survival, additive scale recurrence/negative retirement, and the
secondary scalar recurrence/negative retirement. These are not cosmetic.

A conservative initial gate includes no owner color/intensity callback
(`generator+0x70 & 0x0F000000 == 0`), no collision or child, audited movement
modes, and no other observer of omitted endpoints. Exact ordinary owner
callback targets should be checked, not inferred from a broad DTI family.

Rendering uses both interpolation slots. Preserve recurrent state; either run
full presentation for the last two relevant substeps or reconstruct both
endpoints from historical timers/seeds. Do not run lifecycle/callbacks twice.
Current time alone is insufficient when keyframe time selects manager, owner,
parent-owner or global time. Ordinary Billboard here does not execute the
shared UV-scroll helper, so no UV-scroll saving is claimed for this path.

## 5. Catch-up and restore churn

`uBaseEffect__updateLoopNum @ 0x1405EA140` consumes INIT status differently:

```text
INIT:   loopNum = u16(loopFrame - 1 + trunc(accumulator))
normal: accumulator += rate; loopNum = u16(trunc(accumulator))
```

Native initialization sets accumulator 1, loopFrame 1 and INIT. The loop-frame
setter accepts 1..600; authored animation commands and child startup can request
large initial counts. Restart `0x1405EFAF0` re-arms INIT and retains loopFrame.
No general substep clamp or closed-form fast-forward is output-equivalent.

Source confirms restore churn: `rollback_effect.cpp:1082-1098` reconstructs
recipes through the native factory; `:1173-1205` destroys many current effect
objects. `concrete_snapshot.cpp:969-980` restores flags and timing, including
INIT/accumulator/loopFrame, when compatible; fallback/exceptions retain parts
of the fresh timeline. `effect_generator_state.h:49-77` can re-arm a missing
single particle but does not restore pool payload/history.

These facts establish reconstruction work and a native catch-up mechanism,
NOT duplicated 600-step catch-up in the current workload. No explicit effect
fast-forward loop was found in the inspected source. Attribute INIT, restart,
ordinary rate and child startup separately before claiming an amplification bug.

The larger fix is identity-preserving graph/pool state and storage reuse, or
native-domain backing-allocation reuse with exact reset/ownership semantics.
Native restart alone is not equivalent to installing a historical graph.
Effect `+0x210` includes auxiliary allocations and is not a contiguous copy size.

## 6. Adhesion is not a blanket skip

At `0x14087D590`, rebuild checks the entire particle byte `+0x10A`, then clears
only bit 0. Persistent bit `0x10` forces repeated projection. Positive guarded
queries trigger capacity decisions, destructive buffer growth/failure, particle
`+0x108` writes and old-geometry removal; zero/invalid queries retain old geometry.

Collision traversal can allocate reusable stacks and mutate geometry stamps.
No damage dispatch was found, but query purity is disproven. Retain those
outcomes unless their shared-state irrelevance is separately demonstrated.

Projection/clipping `0x140976440` can be deferred using captured historical
triangles, record indices, parameters and protected resource identity. Rebuild
old static decals from those inputs before drawing, without repeating queries,
admission, allocations or lifecycle updates. Rebuilding from current transforms
is not equivalent. A normal first visible tick may never rebuild bit-0-only
particles, or may retain previous geometry on a zero-hit query.

## Other exact-output candidates

- Cache keyframe segment searches (`0x14080AC70`, `0x14080AE00`) by descriptor,
  resolved time/stride/loop controls while preserving interpolation and duplicate
  key boundary behavior. Do not cache randomized final values without seeds.
- Cache pure quaternion rotation or repeated camera inverse calculations by
  exact inputs; preserve surrounding transform/lifetime writes and FP order.
- Adaptive generator-to-joint membership in `0x1405E6600` can remove repeated
  list searches. Small graphs may not benefit; rebuild membership after callbacks.

## Attribution required for the massive calls

Existing profiles group unit callbacks by DTI/category/phase
(`rollback_profile.cpp:179-219`), not generator/resource. The attachment hook
(`rollback_effect.cpp:795-801`) times only attachment plus validation.

The focused missing records are resource identity, concrete generator type,
`+0x88/+0x98` durations, acquisition versus presentation time, requested/successful
spawn counts, empty-stock attempts, active particles, loopNum and INIT/startup
source. Compile-time profiling is currently default-off in
`rollback_profile.h:6-10`. Keep measurement outside ordinary particle inner loops
where possible to avoid making small calls look expensive.

No percentage or millisecond saving is established. Validate each reduced path
against native continuation, pool order, RNG, callbacks, child admission/liveness
and first visible endpoints. The first patches can be small; a full effect
rollback architecture is not a prerequisite for the exact/local candidates.
