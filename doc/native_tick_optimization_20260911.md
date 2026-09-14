# Native tick optimization research — 2026-09-11

## Scope and result

Inspected `doc/ai_rollback_optimization.md`, the raw host/client profiles in
`rollback_profile_runs/20260911T095307.034337Z_addr_fixed_candidate_4000`,
current hooks, and native code in Ghidra (`8192`,
`ichneumon_sub/umvc3.exe.unpacked.exe`). Native addresses below use image base
`0x140000000`.

**Profile status: `profile_ready`. Research only; no optimization has been
implemented or verified by this investigation.** CPU optimization is excluded
at the user's request: the dual-CPU workload exercises combat but its AI cost
is not a target for human-versus-human matches.

The best narrow presentation candidate is **Kobun's material/lighting tail**.
The best newly identified state-preserving algorithmic candidate is **effect
manager-to-generator membership bookkeeping**. For larger gains, split the
shared character motion evaluator into measured sub-buckets before attempting
native math or animation changes.

## Configuration and qualification

- Target: 4,000 fighting frames, both teams under CPU control via the runner.
- Both sides completed; recorded game exit code 0; runner reports no crash or
  desync artifacts. This is artifact evidence, not a new live state comparison.
- Every rollback entry has positive depth; raw phase/headless timing values
  checked during aggregation were finite and non-negative.
- Multi-resim: true on both sides. Observed maximum depth: 7.
- Roster inferred from six fighter DTIs: Tron, Doctor Doom, Felicia, Arthur,
  Iron Man, and Strider (`uHiryu`). Stage and exact delay configuration are not
  recorded in the supplied summary. Do not infer them from today's INI.
- Historical build commit/compiler and exact build switches are not recorded
  in the supplied summary. The inspection checkout was clean at
  `30491c30dbf29d75b81ef560e4a01483c7883816`; this does not identify the capture's
  build. Current code confirms several earlier optimizations already exist.
- Historical profiles were read directly; no game launch or build was needed.

### Rollback totals

| Metric | Host | Client |
|---|---:|---:|
| Windows | 447 | 253 |
| Replayed frames | 3,043 | 1,750 |
| Depth mean / median / max | 6.808 / 7 / 7 | 6.917 / 7 / 7 |
| Total rollback ms | 5,235.364 | 2,828.974 |
| ms/replayed frame | 1.72046 | 1.61656 |
| Window mean / P50 ms | 11.712 / 11.487 | 11.182 / 10.787 |
| Window P95 / max ms | 15.082 / 20.548 | 14.166 / 24.353 |
| Per-window maximum frame P50 / P95 ms | 1.727 / 2.473 | 1.659 / 2.329 |
| Maximum replay-frame ms | 3.560 | 3.516 |

Combined: 700 windows, 4,793 replayed frames, 8,064.338 ms,
**1.68252 ms/replayed frame**. All per-frame figures use total time divided by
total replayed frames, not an average of window averages.

Depth buckets: host has four 4-frame windows (16 frames, 2.09036 ms/frame),
and 443 windows in the 5–8 bucket (3,027 frames, 1.71851 ms/frame). All client
windows are in the 5–8 bucket. There is insufficient shallow-depth evidence
for a useful depth-dependent conclusion.

### Main phases

Each cell is aggregate ms / ms per replayed frame / share of rollback time.
`native_tick_max` is a maximum, not an additive phase.

| Phase | Host | Client |
|---|---:|---:|
| Native tick | 3525.304 / 1.15850 / 67.34% | 1985.681 / 1.13467 / 70.19% |
| Frame snapshot | 956.011 / 0.31417 / 18.26% | 481.164 / 0.27495 / 17.01% |
| Restore | 727.683 / 0.23913 / 13.90% | 346.922 / 0.19824 / 12.26% |
| Frame input end | 3.749 / 0.00123 / 0.072% | 2.104 / 0.00120 / 0.074% |
| Frame input | 2.110 / 0.00069 / 0.040% | 1.282 / 0.00073 / 0.045% |
| Cut-in repair | 1.410 / 0.00046 / 0.027% | 0.780 / 0.00045 / 0.028% |
| Unattributed residual | 18.315 / 0.00602 / 0.350% | 10.567 / 0.00604 / 0.374% |

Input-history save/restore, timing resync, and patch enable/disable each cost
less than 0.00009 ms/frame. No separate `input_availability` phase is present
in this capture. Largest native-tick window: host 14.3105 ms, client 19.1889 ms;
largest frame-snapshot window: 4.0779 / 5.3503 ms; largest restore:
4.3353 / 5.7241 ms.

### Headless tick

| Bucket | Host ms/frame | Client ms/frame | Max individual tick, host / client ms |
|---|---:|---:|---:|
| Unit move | 0.71389 | 0.73395 | 1.9546 / 2.0586 |
| Unit sync, both late phases | 0.38697 | 0.34408 | 2.0086 / 1.5735 |
| Collision hit | 0.02704 | 0.02607 | 0.2105 / 0.2288 |
| Early draw/collision | 0.00888 | 0.00886 | 0.1278 / 0.1969 |
| Render begin/area | 0.00706 | 0.00703 | 0.0388 / 0.1051 |
| Simulation tail | 0.00592 | 0.00627 | 0.2348 / 0.2150 |
| Manager core | 0.00563 | 0.00538 | 0.0676 / 0.0981 |
| Action | 0.00050 | 0.00046 | 0.1303 / 0.0183 |

Input-device, gameplay-setup, timing-input, viewport, network-service, render,
post-frame and Alt-F4 buckets each cost less than 0.0004 ms/frame. Movement
and synchronization together account for approximately 95% of headless time.
This capture provides little reason to pursue the already suppressed outer
render path.

`sync_job` invokes object vtable **+0x50** and `sync_save` invokes **+0x48**;
neither label means the mod's snapshot serializer. Source:
`UMVC3Hook/code/rollback_tick.cpp:226–265,412–486`.

### Relevant native object costs

| Object / phase / category | DTI | Host / client ms/frame | Calls/frame host / client | Max call host / client us |
|---|---|---:|---:|---:|
| All category-8 movement, including dispatch | mixed | 0.55550 / 0.59517 | 1 category pass | 1706.7 / 1902.5 per pass |
| Tron move / 8 | `0x140D45F20` | 0.17367 / 0.15860 | 1 / 1 | 875.1 / 845.0 |
| Doom move / 8 | `0x140D45068` | 0.12045 / 0.08647 | 1 / 1 | 562.0 / 543.9 |
| Felicia move / 8 | `0x140D45138` | 0.08244 / 0.09529 | 1 / 1 | 697.7 / 775.2 |
| Arthur move / 8 | `0x140D44E48` | 0.06800 / 0.10472 | 1 / 1 | 504.0 / 721.4 |
| Iron Man move / 8 | `0x140D45518` | 0.05015 / 0.07723 | 1 / 1 | 528.5 / 555.0 |
| Strider move / 8 | `0x140D45450` | 0.04429 / 0.05621 | 1 / 1 | 765.8 / 663.4 |
| Game effect sync_save / 13 | `0x140D46E90` | 0.04226 / 0.04348 | 8.005 / 7.873 | 1195.3 / 1123.4 |
| Game effect sync_save / 7 | `0x140D46E90` | 0.02235 / 0.02226 | 1 / 1 | 381.6 / 150.5 |
| Kobun sync_save / 9 | `0x140D455B0` | 0.02860 / 0.02218 | 1 / 1 | 707.9 / 351.4 |
| Character pre move / 7 | `0x140D44DB0` | 0.02831 / 0.02636 | 1 / 1 | 406.6 / 163.7 |
| Character post sync_job / 18 | `0x140D44D60` | 0.02275 / 0.02351 | 1 / 1 | 171.8 / 162.9 |
| Side camera move / 3 | `0x140D44268` | 0.01575 / 0.01499 | 1 / 1 | 140.2 / 116.2 |

These are nested measurements, not independent costs to add to native tick.
Ordinary category-13 effects recorded 24,577 / 13,885 visits versus 24,358 /
13,777 timed late calls; registration visits do not all pass dispatch gates.
The larger category labels are generic, so the DTI and phase identify the target.

## Candidate 1: Kobun material animation and lighting

**Highest-confidence narrow presentation lead; measured ceiling
0.02218–0.02860 ms/frame**, or about 0.155–0.200 ms per seven replayed frames.
This is the entire callback's cost, not a guaranteed saving. Aggregate measured
cost: host 87.0216 ms, client 38.8100 ms. It is approximately 6.4–7.4% of unit
sync time and is a roster-dependent secondary optimization.

Verified exact-class dispatch:

```text
uKobun vtable 0x140A79480
  +0x48 -> 0x1400A30A0   uActorGameModel__moveAfterIfActive
       +0xF8 -> 0x14009DBD0   activity/owner predicate
       +0x248 -> 0x1407E1A10  no-op for this vtable
       -> 0x140200CC0         uActorGameModel__updateMaterialsAndCutinLight
           -> 0x140451370 -> 0x14055FAC0 material animation loop
           +0x230 -> 0x140200B60        cut-in lighting
```

The activity predicate checks owner liveness and Kobun/owner state. Preserve
that predicate when testing an inner-tail exclusion. The +0x248 no-op proof
is specific to this vtable; other actor subclasses can override it.

The material loop walks `mpMaterials`, calls material virtual +0x38 with unit
delta time, and ORs the material-animation/cache predicate into model cache
flags. Cut-in lighting reaches `uToon__applyCutinLight`, updating the embedded
toon parameters at model +0x12C0.

An important additional finding: the material operation is **temporal**.
`nDraw::MaterialChar` vtable `0x140B096D0`, slot +0x38, resolves to
`0x1405E0530` (misnamed `nDraw_Material__destroy`). It samples animation tracks,
dispatches material setters, advances four channel frame values at
`+0x74 + channel*8`, and wraps/clamps them. Thus a final-call-only refresh is
not equivalent to replaying all material updates. Other material DTIs and their
setter side effects still need auditing.

Current `hkCharacterModelPostMove` at `rollback_runtime.cpp:685` suppresses the
character-only thunk **0x1402021E0**. It does not suppress the Kobun path above.
`hkToonMaterialUpdate` hooks **0x140564110**, repairs pending material caches,
and still calls native; Kobun instead reaches the **0x14055FAC0** material loop.

Proposed experiment: apply the existing live-presentation-timeline policy to
the exact Kobun material/lighting tail during resim, preserving its native
movement, motion, owner checks and gameplay state. Prove whether its material
clocks should remain live, and explicitly restore final lighting if necessary.
Do not globally skip `0x140200CC0` on the strength of the Kobun vtable alone.

`uGameEffectModel__moveAfter` at **0x14013C510** also calls `0x140200CC0`,
but its outer callback advances lifetime, animation commands, model motion,
boundary kill/finish state and virtual callbacks. Only the audited inner tail
is a possible extension. Its category-13 total is 0.01072 / 0.00103 ms/frame;
the removable material fraction is unknown.

Validation: Tron/Kobun idle and attacks, assists, Servbot projectiles, supers,
cut-ins and rematches; compare materials on the first corrected draw plus
actions, positions, collision, RNG, owner links and final checksums. No gain
is verified until these pass.

## Candidate 2: Linearize particle-manager membership bookkeeping

**Best exact-output algorithmic lead**, but leaf timing is missing.

`uGameEffect__moveAfter` at **0x14013A9B0** delegates to
`uEffect__moveAfter`. Each ordinary/aggregate substep calls
**0x1405E6600**, `uEffect__finalizeParticleManagersAfterSubstep`, after generator
updates. Disassembly confirms that this helper has **no function calls**,
virtual dispatches, locks, allocation, cleanup calls, or RNG draws.

It performs:

1. Clear effect `+0x20A` (`mJointMoveNum`, uint16).
2. Iterate `mJointNum` at `+0x208`, manager array at `+0x220`, stride `0x150`.
3. For each active manager (`+0x114 & 1`), copy current XYZ `+0x30..38`
   to previous XYZ `+0xC0..C8`, and zero `+0xCC`.
4. Scan generators from effect `+0x218`, next at generator `+0x18`, looking
   for `(generator[+0x20] & 7) != 0` and uint16 `generator[+0x30] == managerIndex`.
5. Increment the count once per matched manager; otherwise clear that manager's
   active bit.

The search is **O(active managers × generators)**. A temporary membership
bitmap built from the final generator list permits **O(generators + managers)**
while retaining every manager write, count width, and activity decision.

Correctness details:

- Membership uses **bits 0–2**, not just generator active bit 0.
- Multiple generators referring to one manager increment the count only once.
- Preserve the XYZ copy even when the manager is about to become inactive.
- Build membership after native generator callbacks, inside this helper;
  `0x1405E67C0` invokes generator +0x88 and may mutate state/topology.
- No cache may survive a substep. Restore and native updates alter membership.
- A bounded stack bitmap with native fallback avoids new allocation/lifetime
  machinery. Choose capacity after observing actual manager counts; keep the
  zero-manager path cheap. Prove no concurrent mutation at this call boundary.

Relevant parent budget: ordinary `uGameEffect` late calls in categories 7 and
13 together cost **0.06461 / 0.06574 ms/frame**. Including model effects gives
0.07533 / 0.06677 ms/frame, but model effects do not establish additional
membership-helper savings. Only a fraction of the ordinary-effect budget is
available to this helper. No numeric speedup can yet be claimed.

Next measurement: time this helper and record manager count, active-manager
count, generator count, and actual comparison count. Compare those outputs
against an original-on-cloned-bookkeeping-state reference, including duplicate
indices, bits 1/2 without bit 0, empty lists, and fallback-sized graphs. Then
run the same rollback workload and focused projectile/effect lifetime cases.

### Static follow-up: expected size of the saving

Further analysis of allocator **0x1405E0950** and generator-list builder
**0x1405E2860** substantially narrows the opportunity:

- These are particle-manager records, not skeletal joints. Ordinary effects
  allocate one manager per distinct generator resource index (`node+0x32`).
  The allocator assigns compact manager IDs in first-encounter list order,
  writes them to `node+0x30`, and points `node+0x28` at the manager record.
- On a freshly constructed ordinary graph, manager count J is at most generator
  count G; shared resources can make J much smaller than G. This does not prove
  an unchanged list after later lifetime operations or restoration.
- Aggregate mode allocates exactly one manager and constructs one aggregate
  generator. There is no repeated-list-scan opportunity in that normal case.
- The helper is only **0xB0 bytes**, including its return, and has no stack
  frame. It is integer control flow and scalar memory copies, with no floating
  arithmetic. The current tiny leaf is already cheap for small graphs.
- The caller invokes it once per actual substep, after generator processing.
  Paused, rejected, and zero-substep paths need not invoke it. Effect callback
  count is not a measurement of this helper's call count.

Let A be active managers, and p_i the number of nodes visited before the first
qualifying match for active manager i (G if unmatched). The actual search cost
is **C = sum(p_i)**, bounded by A*G. It is not necessarily J*G. A linked node
test executes roughly 5–9 x86 instructions, depending on status and match,
with a dependent next-pointer load on a miss. The writes and manager iteration
remain necessary after any optimization.

For all-active graphs with one live generator per manager in manager-ID order:

| J = G | Original node tests | One full bitmap scan | Node tests avoided |
|---:|---:|---:|---:|
| 1 | 1 | 1 | 0 |
| 4 | 10 | 4 | 6 |
| 8 | 36 | 8 | 28 |
| 16 | 136 | 16 | 120 |
| 32 | 528 | 32 | 496 |

With J=1 and G=16 where the head matches, the original performs **one** node
test; an unconditional bitmap builder performs **16**. With A=0 the original
does no list scanning at all. Therefore an unconditional replacement is not
recommended.

#### Conditional cycle-to-time model, not a benchmark

For illustration only, assume nine helper calls per replayed frame, one
substep per effect, a 3-GHz effective CPU, cache-hot lists, and 5–15 cycles
per eliminated node visit. Nine is approximately the measured ordinary-effect
late callback density; actual helper/substep density is unknown. The cycle
range is a sensitivity assumption, not an instruction-latency measurement.

```text
gross saved ms/frame = helperCallsPerFrame * (C - scannedNodesNew)
                      * cyclesPerAvoidedVisit / (clockGHz * 1,000,000)
```

| Graph in every assumed call | Gross search saving ms/frame | Seven-frame window |
|---|---:|---:|
| J=G=4 | 0.00009–0.00027 | 0.00063–0.00189 ms |
| J=G=8 | 0.00042–0.00126 | 0.00294–0.00882 ms |
| J=G=16 | 0.00180–0.00540 | 0.0126–0.0378 ms |
| J=G=32 | 0.00744–0.02232 | 0.0521–0.1562 ms |

These are **gross**, optimistic savings before bitmap operations, extra manager
passes, interception and fallback overhead. They do not establish actual J/G
values in the supplied capture. Repeated scans warm the list, so charging every
eliminated visit a cold DRAM miss would seriously overestimate the gain.

Under this model, reaching 0.01 ms/frame at nine calls/frame and 10 cycles/visit
requires avoiding about **333 node visits per call**. Reaching 0.05 ms/frame
requires about **1,667 per call**, before replacement overhead. Small graphs
cannot supply that work. The measured ordinary-effect parent budget of about
0.065 ms/frame also includes generation, particles, transforms, history and
lifetime; it is not the saving available to this helper.

#### Better implementation shape, if the workload warrants it

For an audited bounded case such as J<=64, a register-sized active/pending mask
can avoid a cleared array:

1. Preserve native behavior for J=0/1 and use native fallback beyond capacity.
2. Traverse managers, performing the required XYZ/W writes and collecting the
   initially active mask. With no active managers, finish with count zero.
3. Traverse generators once, recognizing status `&7`, validating the manager
   index before shifts, recording matches and clearing pending bits.
4. Stop early when every initially active manager has a match.
5. Clear unmatched manager active bits and store the matched-manager count.

This adds a manager pass, but avoids scanning irrelevant suffixes and needless
work for inactive effects. Count each manager once regardless of duplicate
generators. Actual small-graph cutoff needs measurement. Reordering the
bookkeeping requires exclusive ownership during the helper and the normal
separate node/manager allocations; do not generalize to corrupt/aliased graphs.

**Static verdict:** a valid narrow algorithmic opportunity, but likely a minor
optimization if graphs have only a few managers. The analysis does not justify
expecting 0.05–0.1 ms/frame or attributing millisecond effect spikes to this
helper. Larger savings require frequent, genuinely multi-manager graphs. The
minimum decisive runtime evidence is J, A, G, actual visited-node count C, and
substep-call density; there is no need for a broad new profiler to answer this.

### Effect tail spikes deserve separate attribution

- Client window **1485–1492**: category-13 `uGameEffect` late calls cost
  **6.9974 ms over 64 calls**, versus about 5.52 us/call across the run.
  Unit sync costs 9.8832 ms; the attachment hook costs only 0.0952 ms.
  A single 1.1234-ms maximum call cannot explain the whole 6.9974-ms bucket.
- Host window **1475–1482**: the same object bucket costs 1.5049 ms,
  maximum 1.1953 ms; attachment hook only 0.0800 ms.
- Host **1165–1172** is different: attachment hook costs 1.2235 ms and has
  a 1.1659-ms maximum. Do not assign all three spikes the same cause.

Profile generator +0x88 by concrete generator DTI/resource and substep count,
alongside prepare/finalize costs. Existing JSON cannot identify the responsible
generator, distinguish extra emission from scheduling pauses, or establish that
membership scanning caused these spikes.

## Candidate 3: Shared character motion — larger budget, deeper measurement

Follow-up: [determinism-preserving character movement investigation](character_movement_optimization_20260911.md)
identifies a verified quadratic command-index lookup, measures its isolated
native-instruction cost, and investigates exact-result caching below the motion
sampler's stateful cursor updates.

Category-8 movement costs **0.5555–0.5952 ms/frame**, approximately 48–52% of
native tick. Tron leads both peers, but `uTron__tick` at **0x1400E52D0** begins
with shared `uCharacter__move_base` at **0x14004BD30**. Its object bucket does
not isolate Tron-specific code.

The shared path performs input/command matching, action dispatch, model motion,
joint/constraint evaluation, position integration, and sphere rebuilding.
Observed full-input-path calls average **2.461 / 2.719 per replayed frame**,
despite every main fighter receiving one movement callback. Six callbacks do
not prove six expensive full updates or redundant reserve work.

Relevant motion helpers:

- `uModel__evaluateSimpleJointTransforms`: **0x14052CC70**.
- `uModel__evaluateLayeredJointTransforms`: **0x14052D740**.
- `uModel__resetJointTransformsToBindPose`: **0x14056E210**.
- `uModel__evaluateJointMatrices`: required ordered per-joint matrices and
  optional joint-controller virtual +0x28 calls.

A concrete caching hypothesis exists in the simple evaluator: for a joint
without a rotation track, when its identity/additive gates permit, it repeatedly
converts a resource bind matrix into a quaternion (including square root/divide).
The resource matrix table is read through `rModel +0x80`, stride `0x40`.
The bind-pose reset helper performs the same type of conversion.

Potential optimization: precompute **only the resource-derived quaternion**,
keyed by resource identity/generation and joint index. Keep dynamic translation,
motion sampling, blending order, joint writes, constraints, and all gameplay
evaluation intact. In particular the reset helper takes translation from the
live joint record, so caching the entire resulting joint record is invalid.

This is a lower-priority hypothesis, not a ready patch: branch frequency,
resource immutability through all writers, resource replacement/invalidation,
actual leaf cost and bit-exact floating-point results need proving. Inlined
code makes a narrow interception harder than the effect bookkeeping helper.
No percentage of the character budget can responsibly be promised from this
profile. First measure shared motion versus commands and collision rebuilding,
with joint counts and action IDs, using nested scopes without summing them twice.

## Mod-owned work and small follow-ups

### Native attachment hook is mislabeled in the profile

`kirigakure_effect_update` actually hooks **0x14013B050**,
`uGameEffect__updateAttachmentAndTransform`, shared by ordinary effects.
It costs 41.8567 / 23.0078 ms total, **0.01375 / 0.01315 ms/frame**,
9.038 / 8.892 calls/frame. Its scope includes native work, not just validation.

Native work changes parent-relative orientation, height-based scale, world
matrices, camera-relative position and ground clamps. It feeds the subsequent
effect update and must not be skipped as a supposed Strider-only visual helper.

`hkKirigakureEffectUpdate` (`rollback_effect.cpp:745`) adds
`HasCallableDtiVirtual(parent)`. A future split should measure guard versus
native time before considering immutable-vtable validation caching. Current
memory/executable validation already contains caches and fast paths
(`rollback_memory.cpp:163–173,209–248`), so old VirtualQuery savings cannot be
assumed to remain available.

`hkGetJointPos` (`rollback_runtime.cpp:652`, native **0x140549F50**) scans and
validates an entire effect-model entry table before native scans it again for a
matching masked ID and returns the first matching entry. This is another
possible excess-traversal site, but it is not timed in this capture. Narrowing
validation changes the behavior when an unrelated later entry is invalid;
do not silently remove that safety policy or cache mutable entries across
restore/reconstruction.

### Dispatch residual is a measurement lead, not free savings

Category time minus nested object time across the three passes is approximately
**0.1057 / 0.1001 ms/frame**. It includes DTI lookup, filtering, lifecycle work,
job execution, watchdog calls, profiler bookkeeping and wrapper gaps. Source
currently resolves DTI in the traversal and again in profiled callbacks.
Production bypasses the profiling wrapper, so eliminating the second lookup
would mainly improve the instrumented measurement. Do not label this residual
native gameplay cost or optimize profiler overhead without a control capture.

### Previously investigated small presentation paths

- Deferred bounds are already implemented at `rollback_runtime.cpp:694–735`.
  Remaining `model_bounds_from_joints` cost is **0.00469 / 0.00479 ms/frame**,
  including final flush calls in the rollback profile. Do not claim the older
  ~0.027-ms/frame deferred-bounds saving again.
- Voice parameter wrapper **0x1402B1280** still enters a critical section,
  applies voice pause parameters and stores per-slot state. It lacks a direct
  timing bucket. If pursued, preserve required state/ref handling and RNG;
  presentation suppression is not permission to skip voice-selection RNG.
- Camera-mask/aux-texture helpers are existing secondary leads; historical
  aux-texture measurements were around 0.001 ms/frame, not current evidence
  of a substantial remaining win.
- Character post-processing, side-camera updates, shot hitboxes and model joint
  evaluation retain known gameplay/state consumers; no blanket skip is justified.

## Snapshot/restore context and A/B caveat

Snapshot workload is exactly **379 regions, 1837.698 KiB/save** on both sides.
There are 3,015 / 1,733 timed saves, totaling 928.639 / 472.094 ms
(0.30801 / 0.27241 ms/save). Copying accounts for 60.8% / 67.6%, collision
clones 12.4% / 14.0%, and shot recipes 24.5% / 15.8% of timed snapshot cost.
Region construction is about 1.9% / 2.1%; allocation/recycling is below 0.3%.
Each save captures 44 collision wrappers and approximately 216 children.

Restore averages **1.6279 / 1.3712 ms/window**. Host/client shares:
regions 35.1% / 42.5%, shots 51.5% / 39.3%, stale-pointer repair 7.6% / 11.0%,
collision clones 2.0% / 2.4%, UI 1.9% / 2.2%; setup/owner/hit-solver/effect-slot
work forms the small remainder. Shot respawn costs 218.2166 / 69.6833 ms,
including concrete overlay 142.7043 / 41.7546 ms; these are nested totals.
Concrete pointer scans cost 84.4065 / 26.8928 ms. Native shot collection itself
is only 0.0937 / 0.0524 ms over the runs.

Restore workload per window: saved shots 3.743 / 1.225, current shots
3.774 / 1.225, effect visits 36.98 / 34.74, sanitizer calls 4.73 / 4.67,
generator nodes 33.06 / 31.39. These object differences matter for comparisons.

Post-resim is separate: 447 / 253 entries, total **207.0029 / 96.7743 ms**
(0.4631 / 0.3825 ms/entry), almost entirely final snapshot saves
(206.7252 / 96.5950 ms). Resolve is 0.2777 / 0.1793 ms total; notify is zero.

Top hook contexts: collision move costs 0.02556 / 0.02453 ms/frame;
unit removal 0.00772 / 0.00401; shot hitboxes 0.00582 / 0.00276. Unit-removal
sub-buckets are nested, so adding them to unit removal double-counts time.

The supplied `comparison.json` reports **+6.81% aggregate ms/frame** versus
`20260911T095048.763939Z_addr_runtime_baseline_4000`. That does not by itself
prove a regression in fixed-address lookup. For example:

| Natural workload | Baseline host → candidate host | Baseline client → candidate client |
|---|---:|---:|
| Vulcan Kobun Launcher move calls/frame | 0.700 → 3.426 | 0.586 → 0.995 |
| Full frame-input calls/frame | 2.863 → 2.461 | 2.841 → 2.719 |
| Average rollback depth | 6.984 → 6.808 | 6.970 → 6.917 |

Projectile and character workloads changed in opposite directions. Use this
capture to rank current hotspots; an address-lookup A/B verdict requires its
own workload-normalized attribution.

## Recommended next action

1. Measure the exact Kobun material/lighting tail and the effect membership
   helper, with a timer-overhead control and natural workload counters.
2. If Kobun material work dominates its late callback, test a narrowly scoped
   presentation-timeline exclusion and first-corrected-draw validation.
3. If membership comparisons are substantial, test the one-call temporary bitmap
   with exact output comparison and normal rollback correctness checks.
4. Investigate the effect tail spike with generator/resource attribution; split
   shared character motion only if larger gains remain necessary.

Keep these as independent candidates. No CPU optimization or speedup from
removing required simulation is part of this recommendation.
