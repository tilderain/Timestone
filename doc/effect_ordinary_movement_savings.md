# Ordinary particle movement savings audit

Research: 2026-10-15, Ghidra `umvc3.exe.unpacked.exe`, image base
`0x140000000`. No runtime optimization was implemented by this audit.
See `effect_profile_findings_20261015.md` for capture provenance and aggregate
savings limits, and `effect_moveafter_optimization_targets.md` for earlier work.

## What the current profile measures

`ordinary_move` hooks `cParticleGenerator__move @ 0x140834F30`, the ordinary
generator virtual `+0x88`. It includes more than position integration:

1. Generator preparation, emission phase/count selection and path maintenance.
2. Opening/initializing particles and incrementing emission serials.
3. Generator culling-direction update.
4. Concrete virtual `+0xB8`: shared motion/lifetime, concrete particle updates,
   optional owner callbacks and culling-data generation.
5. Per-particle flag propagation and generator completion handling.

The profiler separately times emission selection but does not split shared
motion, concrete updates or culling helpers. Thus exclusive time still includes
spawn initialization, multiple particle walks, concrete presentation work and
required simulation. It is not all removable.

| 07:55 capture | Ordinary calls | Inclusive | Exclusive | Largest call |
|---|---:|---:|---:|---:|
| Client | 24,878 | 56.7965 ms | 50.3926 ms | 0.2876 ms |
| Host | 9,322 | 28.7393 ms | 26.2907 ms | 0.3405 ms |

Client exclusive cost averages about 0.00203 ms per ordinary call and
0.05095 ms per replayed frame, or 38.5% of measured effect-root time. This is
the encompassing budget, not an achievable saving. Timings include profiler
overhead, and usable CPU-cycle measurements were unavailable.

The retained slow samples identify two concrete families:

- Host frame 794, `eft\cmn\efl\0100`: Polyline vtable `0x140BC50A0`,
  0.3405 ms in one move, ten concrete particle visits, no requested spawns.
- Client frame 483, `chr\Chris\effect\efl\0016`: Polygon vtable
  `0x140BC52B0`, 0.2916 ms across five grouped moves, with a 0.2876-ms maximum
  ordinary call, three concrete visits and no requested spawns for that group.

Groups combine matching vtable/descriptor pairs, not necessarily one generator
instance. Visits count entries entering the concrete pass, including those
recycled there; they do not count every shared-loop or history operation.
The profile does not establish whether these outliers are computation,
contention or preemption, or which helper caused them. Empty-stock retry
removal cannot explain either highlighted zero-request group.

## 1. Culling-data traversals inside movement

These are new concrete presentation-deferral candidates distinct from generator
`+0x98` post-update. Skipping draw/post-update alone does not remove them.

| Entry point | Work | Relevant callers |
|---|---|---|
| `0x1408361C0`, `moveCullingDir` | Transforms one generator culling vector using the joint matrix and writes its selected history endpoint. | Ordinary `move`, once per call when auxiliary data exists. |
| `0x140836330`, `transformAndStoreParticleVerts` | Walks active particles, transforms each base culling vector by the joint 3x3 and writes the selected culling-buffer endpoint. | Billboard, Polyline, Texline, Line, Filter and other concrete loops. |
| `0x14087C930`, `buildPerParticleBillboardTransforms` | Walks active particles, calls `calcParticleMatrix_scalarScale` for each, transforms its base culling vector and writes the selected endpoint. | Polygon, AxisPolygon, ClothPolygon, Adhesion and LightShaft loops. |

The two per-particle helpers are gated by nonzero `mParticleCullingOffset` and
a nonempty active list in the inspected Polyline/Billboard/Polygon callers.
Their bodies do not advance emission, age, RNG, retirement, visit counts or
pool links. The Polygon helper includes considerably more arithmetic than a
simple direction transform: its matrix helper builds Euler rotation and can
perform additional orientation/matrix calculations depending on mode.

This is the clearest place to measure next, especially the Polygon matrix
helper and Polyline vector traversal. It does NOT certify a blanket skip:

- Audit readers of the auxiliary culling data, including other movement modes
  and callbacks, rather than relying on the helper's name or draw dispatch.
- Check side effects of transitive matrix helpers for the eligible modes.
- Preserve both interpolation endpoints before the corrected frame is drawn.
  A first live call writes only one selected slot; the other can remain stale.
- Keeping the last two relevant native updates, or reconstructing both endpoints
  from their actual historical inputs, needs explicit scheduling. Two frames
  are not automatically two updates when loop counts vary.
- Shared helpers serve other generator families; proving Polygon/Polyline
  eligibility does not authorize suppressing every caller.

## 2. Elide the known no-op owner callback traversal

Billboard `+0xB8 @ 0x14084EBB0`, Polyline `+0xB8 @ 0x14085EF50` and Polygon
`+0xB8 @ 0x14087C3B0` optionally walk surviving particles to invoke owner
virtual `+0x110` with color/intensity pointers. The enable flag is the low
nibble of generator byte `+0x73`.

Memory at ordinary `uGameEffect` vtable `0x140A8E180 + 0x110` resolves to
`SharedVirtual__noOpStub @ 0x1407E1A10`; decompilation confirms a bare return.
For that actual callback target, bypassing the callback-only traversal removes
list walking and virtual calls without changing particle fields or requiring
presentation reconstruction. The gate can test the actual no-op callback
target rather than assuming all derived owners are equivalent.

This is a strong local exact-output candidate, not a measured large saving.
Its workload frequency is unknown. Hooking the no-op stub itself saves nothing;
the traversal/call site has to be bypassed. Other callback targets must retain
native behavior and current color/intensity inputs.

## 3. Reduced concrete presentation calculations

### Polyline

`moveTrack @ 0x14085F050` includes intensity sampling, two color tracks and
alpha application. Those are candidates for deferred presentation when there
is no observer requiring them. However, the same function also contains:

- Animation exhaustion and scale-update survival checks.
- Additive width recurrences at particle `+0xD4`/`+0xDC`, with deltas
  `+0xD8`/`+0xE0`; nonpositive results can retire the particle.
- `moveParticleLineOfs @ 0x1408394E0`, whose mode-specific functions maintain
  trail history, execute owner callbacks, or return retirement decisions.

In particular, follow mode `0x140839200` shifts historical positions, then
`calcParticleLineFollowOfs @ 0x14081B420` derives current/previous relative
track geometry. Preserve the historical samples; freezing the whole track
update is not equivalent to omitting a draw submission. Separating derived
geometry from history maintenance is a larger, mode-specific candidate.

### Polygon

`updateParticle @ 0x14087C4B0` is the concrete body for the client spike family.
Potential presentation work includes intensity/color/alpha evaluation and the
final velocity normalization into orientation endpoints `+0xA0`/`+0xB0`.
These outputs still need a future-read/callback audit and correct refresh.

Keep animation survival, scale survival, velocity updates, dirty-bit behavior,
and additive width recurrences at `+0xE0`/`+0xE4` and `+0xE8`/`+0xEC` with
deltas `+0x114`/`+0x118`: nonpositive results return false and recycle particles.
Do not treat these widths as disposable render-only state.

### Billboard and shared UV scrolling

The earlier Billboard candidate remains valid as research: selectively omit
eligible color/intensity and absolute presentation calculations, not motion,
animation/scale survival or recurrent orientation. Both visible endpoints and
owner callback inputs must be correct. See the earlier optimization document.

Polyline and Polygon, unlike the inspected ordinary Billboard body, can call
`moveParticleTexScroll @ 0x14083F080`. Its modes include accumulated UV offsets,
velocity multiplication, accumulated keyframe results and paired-endpoint
wrapping. The helper is not a blanket skip even though UVs are visual.

## 4. Empty-stock emission early exit

The earlier exact-output candidate remains: `openParticle @ 0x14083F430`
returns null immediately when stock head `+0xF0` is null, without writes, RNG
or callbacks. In the ordinary attempt loop, no stock producer runs between
that failure and the next open attempt. Bypass remaining failed attempts while
preserving the nonzero-request frame serial increment and all later updates.
Do not stop after initialization failure, which returns the particle to stock.
Exclusive ownership during the loop is required.

No measured saturated-burst frequency or wasted-attempt duration is available.
This may be nearly free in normal workloads and useful only under exhaustion.

## Priority and validation

First measure the two per-particle culling helpers and the concrete
Polyline/Polygon loops. Prefer per-generator/per-traversal timing to timing
every particle; extra hot-loop timers could dominate these microsecond-scale
calls. Record culling eligibility, motion/track mode, active visits and callback
target so the capture identifies how often a reduced path would apply.

The known-no-op callback traversal and empty-stock retry bypass have the
strongest local equivalence arguments. Culling deferral is a concrete
presentation opportunity but still needs reader and endpoint validation.
Broader Polyline/Polygon/Billboard reductions require more state separation.
There is no supported percentage forecast for any individual candidate yet.

Compare native/reduced continuation results, active/free list order, concrete
visit counts, emission serials, RNG, callbacks, child retirement and first-visible
interpolation. Do not skip the common motion/lifetime dispatcher
`0x140839820`: even motion mode zero advances age/slots and runs survival logic.

Ghidra annotations were added to the culling helpers and Polygon concrete body;
the shared intensity helper's generator parameter was typed as the existing
`cParticleGenerator` struct. No runtime code or profiler hooks were changed.
