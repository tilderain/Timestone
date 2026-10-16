# Effect capture findings: 2026-10-15

## Capture provenance

Analyzed game-directory `Timestone_client.previous.log` and
`Timestone_host.previous.log`, modified 2026-10-15 at approximately 07:06:57
local time. Newer-looking netplay directory names were empty; other beta DLL
logs are a different system and were not mixed into these measurements.

Client: 460 rollback windows, 7,685 captured roots, 110.1083 ms total instrumented
root time, eight calls at least 0.25 ms. Of these roots, 7,383 were game-effect
moveAfter calls (103.7658 ms); 302 standalone graph creations used 6.3425 ms.
Host: seven windows and 149 roots, too little signal for a symmetric comparison.

These are instrumented wall-clock times, not an uninstrumented A/B result.

## Dominant measured regions

| Client region | Exclusive ms across capture | Qualification |
|---|---:|---|
| Ordinary generator movement | 43.7497 | Largest aggregate phase; excludes measured emission but includes concrete particle processing. |
| Base moveAfter residual | 12.0147 | Uninstrumented boundary/virtual work and profiling overhead, not one classified operation. |
| Owner watch | 9.8520 | Includes a 0.2937-ms stage-effect outlier. |
| Attachment plus validation guard | 6.9157 | No large common-effect attachment spike. |
| Graph creation | 6.3425 | 302 calls; maximum 0.1661 ms. |
| Post-pass residual | 2.3572 | Low aggregate cost but contains the two largest individual calls. |

## Largest consecutive spikes

Both belong to an ordinary `uGameEffect` with vtable `0x140A8E180` and resource
`eft\cmn\efl\0100`:

| Frame | Root ms | Post-pass inclusive ms | Post-pass residual ms | loopNum |
|---|---:|---:|---:|---:|
| 998 | 0.7747 | 0.7328 | 0.7285 | 1 |
| 999 | 0.6112 | 0.5809 | 0.5792 | 1 |

Post-pass accounts for approximately 95% of these two calls. They are not
hundreds-of-substeps catch-up events. Light post-update accounts for only
0.0043/0.0017 ms. Particle movement accounts for 0.0325/0.0221 ms including the
outer pass. No Adhesion or Force post-update was recorded for this graph.

## Concrete render work found in Ghidra

The logged graph contains generator vtable `0x140BC5E00`. Its `+0x98` target
is `cParticleGeneratorFilter__update @ 0x1408D61C0`.

After draw eligibility checks, this callback dispatches by the particle
parameter's mode byte to four submission routines:

| Mode | Submission routine |
|---|---|
| 0 | `0x1408D7220` |
| 1 | `0x1408D6600` |
| 2 | `0x1408D6BC0` |
| 3 | `0x1408D6250` |

They interpolate particle values into stack render-request payloads and submit
them to groups embedded in `sEffect`. The shared dispatcher `0x140544200`
creates/registers per-view `uMultiBlurFilter`, `uMultiColorCorrectFilter`,
`uMultiGodRaysFilter`, or `uMultiBloomFilter`, then appends their settings.
Earlier Ghidra names incorrectly described this dispatcher and two helpers as
`sSound` methods; inspection of the concrete constructors and callers disproves
the audio interpretation. Those names were corrected.

Settings append can compare descriptor fields, release old filter resources,
and initialize replacement GPU textures:

- Blur initializer `0x140601810`: creates `mpLinearRTexture` and
  `OcclusionFactor` textures.
- Color correction initializer `0x140600F50`: creates a 256- or 1024-wide texture.
- God rays initializer `0x1406010A0`: creates multiple temporary render textures.
- Bloom initializer `0x140600D30`: creates render-target and glare textures.

Thus suppressing the ordinary draw passes does not suppress all rendering
resource work: it can originate inside effect post-update itself.

## Narrow optimization target

For an exact ordinary owner, the inspected owner callbacks used by these
submission routines are benign: `+0x120` resolves to the filter-color copy
`0x1405E2770`; `+0x130` resolves to draw-view getter `0x1401324E0`.

The targeted candidate is to omit Filter `+0x98` render-request submission
during headless resimulation for that audited owner. Preserve Filter emission,
movement, lifetime and concrete retirement (`0x1408D60C0` and
`0x1408D5F30`), generator continuation and effect callback slots. A normal live
post-update can submit from current surviving particle state; do not replay
historical submissions as a repair. Derived owner callbacks need separate
classification.

This removes render-unit/request/resource activity, not every side effect:
GPU-resource references and rendering allocator behavior intentionally stay
off the historical simulation path, consistent with the existing headless
presentation policy. It is not byte-identical restoration of rendering queues.
Verify first visible filter output, resource lifetimes and gameplay checksums.

## What is not yet proved

Filter is a strong concrete target, but its precise share of the 0.7328/0.5809-ms
post-pass is not individually measured. This graph also contains
`cParticleGeneratorPolyline` at vtable `0x140BC50A0`, whose `+0x98` target
`0x140861880` can update line-cloth geometry for a gated particle mode.
The remaining logged Billboard and PrimModel `+0x98` callbacks resolve to no-op
`0x1407E1A10`; Light is already separately measured.

Consequently, the full post-pass time is a ceiling on the relevant per-call
saving, not a promise. Total post-pass inclusive time across the client capture
was only 2.5169 ms of 110.1083 ms root time. This is principally a spike target,
not evidence for a large average resimulation speedup. Ordinary generator
movement remains the larger steady-state target.

## Profiling limitations discovered

- Model profiling hook `0x140898730` failed with MinHook status 3,
  `MH_ERROR_ALREADY_CREATED`. `rollback_runtime.cpp:636-643` already hooks that
  function and suppresses its native body during resimulation. The earlier
  proposed Model skip is already present; the missing timer does not imply
  native Model work is executing.
- All `cpu_measured_calls` and valid CPU-cycle flags are zero. The timing API
  did not yield usable cycle data in this run. Wall-clock spikes cannot be
  conclusively separated into computation, contention, GPU-driver work or
  preemption.
- The eight captured slow calls all have reported loopNum 0 or 1. This sample
  does not support large catch-up as the explanation for its slow-call tail.
- Stage 105 effects account for several other outliers, in different phases;
  there is not one demonstrated common cause for every slow effect callback.

Existing suitable Filter types were applied to the four submission prototypes
and per-particle update, and Ghidra annotations were saved.

## Implemented skip

`rollback_runtime.cpp` now hooks `0x1408D61C0` and skips it unconditionally
during resimulation. The initial implementation restricted suppression to exact
ordinary `uGameEffect` owners; that restriction was subsequently removed at the
user's request. Live updates call native submission directly. No particle,
owner, timer or callback-slot fields are rewritten. The separate Filter particle
simulation remains native. Derived-owner override equivalence remains unaudited.

This is independent of the diagnostic profiler. Regression coverage exercises
the headless/live boundary, unconditional suppression with different or missing
owners and a null generator, unchanged object bytes, and hook-installation
failures. Visual/gameplay equivalence still requires in-game validation.

## Post-fix capture and potential remaining savings

Source: game-directory `Timestone_client.log` and `Timestone_host.log`, session
starting 2026-10-15 07:55:42 and files flushed at 07:56:14. These measurements
used the initial owner-gated Filter skip, before its restriction was removed.
The baseline is `Timestone_client.previous.log` from the 07:05-07:06 session.
Different workloads and rollback depths prevent a controlled A/B speedup claim.

| Observed maximum | Baseline client | Post-fix client | Post-fix host |
|---|---:|---:|---:|
| Generator post-pass | 0.7328 ms | 0.0188 ms | 0.0086 ms |
| Effect moveAfter | 0.7747 ms | 0.3300 ms | 0.3756 ms |

Both peers recorded successful Filter-skip installation. The previous large
post-pass spikes were absent: the client post-pass maximum was about 97% lower.
This is an observed maximum comparison, not a matched benchmark or a prediction.
The host still recorded `eft\cmn\efl\0100` at frame 794: 0.3756 ms total,
only 0.0026 ms in post-update, with 0.3405 ms in ordinary Polyline movement.
That movement cost would not be removed by skipping Polyline post-update.

The post-fix client capture contains 147 windows, 10,206 effect-root calls,
130.7907 ms of effect-root time, and 989 replayed frames. Replayed frames here
means the sum of `end - start` over profiling windows, including repeated
historical frames. Effect-root time per replayed frame was 0.1322 ms versus
0.1251 ms in the baseline; no overall improvement or regression is established.

### Measured budgets, not promised savings

| Candidate | Potential saving supported by this capture | Required behavior / limitation |
|---|---|---|
| Light presentation split (`0x1408D85C0`) | A fraction of the remaining 1.5907 ms total post-pass budget. Removing the entire post-pass would save only about 0.0016 ms per replayed frame, or 1.2% of measured effect-root time; Light alone saves less. | Preserve native child acquisition and particle recycling on acquisition failure. Check consumers of omitted child presentation fields. |
| Force/GrassWind presentation (`0x1408E5600`) | Shares the same post-pass budget; not an additional 1.2%. No individual saving measured. | Preserve child creation, liveness and particle retirement; transitive wind consumers remain unaudited. |
| Reduced Billboard update (`0x14084E2C0`) | Larger opportunity, but unquantified. All ordinary generator movement totals 50.3926 ms exclusive, about 0.0510 ms per replayed frame or 38.5% of effect-root time. Billboard is only part of that budget. | Preserve motion, emission, RNG, animation completion, scale recurrences, retirement and required interpolation history. Only eligible presentation calculations can be removed. |
| Empty-stock emission early exit inside `0x140834F30` | Near zero when stock does not run out; potentially removes many redundant failed attempts during saturated bursts. Saturation frequency and wasted-attempt time have not been measured. | Stop only on empty-stock failure; preserve successful spawn behavior, requested-count denominator and once-per-frame emission serial. |
| Polyline post-update (`0x140861880`) | Part of the same small post-pass budget, not the 0.3405-ms ordinary-movement spike. No separate saving measured. | Chain mode initializes and advances cloth history and clears an initialization bit; a blanket skip does not preserve first-visible trail state. |

The ordinary-movement budget is an attribution ceiling, not wholly removable
work. As a sensitivity example only, removing 10-25% of its measured exclusive
cost would save about 0.0051-0.0127 ms per replayed frame, or 3.9-9.6% of
effect-root time. This is not a predicted achievable range: neither Billboard's
share nor its removable fraction has been measured.

All percentages above refer to profiled effect-root time, not total
resimulation time. Do not sum inclusive nested phases or add the shared
post-pass ceilings for different candidates. The figures describe this capture,
not a universal upper bound for every resource or match.

For performance return, prioritize attribution and reduction of ordinary
particle movement. Light is the clearer next presentation split, but its
average saving in this workload is bounded by the small remaining post-pass.
Model post-update is already skipped. Owner watching, attachment transforms,
linked-child completion and particle lifetime are not blanket-skip candidates.

See `effect_ordinary_movement_savings.md` for the subsequent native audit of
Polyline/Polygon movement, culling-data traversals, known-no-op callback
elision, and the constraints on reduced concrete presentation updates.

### Ordinary movement planning estimates

For the combined ordinary-movement candidates, use the following scenarios for
planning only. They apply assumed removable fractions to the measured
0.05095 ms per replayed frame of ordinary movement and 0.13225 ms per replayed
frame of effect-root time. The removable fractions have NOT been measured;
these are not benchmark results or confidence bounds.

| Scenario | Assumed ordinary-movement cost removed | Saving per replayed frame | Reduction in profiled effect-root time |
|---|---:|---:|---:|
| Conservative | 5% | 0.0025 ms | About 2% |
| Moderate | 10% | 0.0051 ms | About 4% |
| Optimistic | 20% | 0.0102 ms | About 8% |

The working planning allowance discussed is 2-4% less effect-processing time,
with 8% treated as optimistic pending measurement. This allowance is heuristic,
not evidence that even the conservative scenario is achievable. Total
resimulation improvement would be smaller because effects are only part of
the frame; no total-frame percentage is established.

- Known-no-op callback traversal: expect a small saving, limited to enabled
  callback paths; removes list walking and virtual calls.
- Culling-data traversals: the strongest concrete presentation opportunity
  identified. Polygon builds a matrix per particle, so eligible particle-heavy
  effects may benefit more than the average. Eligibility frequency and helper
  cost are not yet measured.
- Reduced color/intensity calculations: potentially adds savings, but needs
  more invasive state separation, callback handling and endpoint reconstruction.
- Empty-stock retries: likely negligible without pool exhaustion; potentially
  useful during saturated bursts, whose frequency is unmeasured.

Do not add these candidates' hypothetical percentages: they share the same
ordinary-movement budget. No removal of the observed 0.29-0.34 ms movement
spikes is predicted. Their cost has not been attributed to the removable
helpers, and wall-clock timing may include stalls or preemption. Time the
culling traversals and concrete Polygon/Polyline loops before replacing these
planning scenarios with an expected saving.
