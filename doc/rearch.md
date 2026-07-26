# Rollback Performance Findings and Re-Architecture

## Status

The current rollback implementation is functionally capable of restoring game
state and replaying corrected inputs, but its generic snapshot and native object
reconstruction architecture cannot reliably achieve a hard target of less than
2 ms per replayed frame.

This is not evidence that rollback netcode is impossible for UMVC3. It means the
current implementation has reached costs that require architectural changes,
not only additional micro-optimizations.

## Observed Costs

Recent profiles show the following ranges:

| Operation | Observed cost | Frequency |
|---|---:|---|
| Native gameplay tick | 2-6.5 ms | Per replayed frame |
| Snapshot save | 4-8 ms | Per replayed frame |
| Frame JSON diagnostics | 1-6 ms | Per replayed frame |
| Snapshot restore | 38-96+ ms | Once per rollback |
| Projectile concrete restore, before optimization | About 5.7 ms | Per shot |
| Projectile concrete restore, after memory-region caching | About 1.0-1.1 ms | Per shot |
| `sUnit` sanitation passes | About 20-24 ms combined | Once per rollback |

Projectile-heavy states are particularly expensive because restore currently
destroys current shots, invokes native factories for all historical shots, and
then reconstructs each shot's concrete body and collision topology.

One-frame corrections are disproportionately expensive. The fixed restore cost
cannot be amortized across several replayed frames, so a one-frame rollback can
cost more per replayed frame than a deeper rollback.

## Confirmed Bottlenecks

### Projectile reconstruction

The initial aggregate `Shots/effects` measurement included several independent
operations. Detailed profiling separated these into:

- Pre-restore `sUnit` list sanitation.
- Effect retirement.
- Current-shot destruction.
- Post-destruction `sUnit` sanitation.
- Effect-generator sanitation.
- Native shot construction.
- Concrete shot restoration.
- Registration and final fixups.

Native shot construction itself is relatively cheap. The dominant per-shot cost
was `ConcreteShotSnapshot_Restore()`.

The concrete restore originally scanned pointer-heavy shot bodies and called
`VirtualQuery()` repeatedly. Caching committed memory regions within one
allocation-free concrete restore reduced the measured cost from approximately
5.7 ms per shot to approximately 1.0-1.1 ms per shot, an improvement of roughly
81 percent.

Further profiling showed that most remaining concrete restore time came from two
complete embedded-object discovery scans, one over the live body and one over
the saved body. These scans now reject candidates unless the candidate qword is
inside the `umvc3.exe` image, because a valid inline `MtObject` must begin with a
game-owned vtable. This avoids memory queries and DTI dispatch for ordinary heap
pointers and pointer-like scalar values.

### Global unit sanitation

`SanitizeUnitCategoryLists()` runs before shot teardown and again after native
shot destructors have changed effect and unit topology. These two passes have
cost approximately 20-24 ms combined even in relatively light states.

The second pass is currently needed because shot destruction can directly
release attached effects while leaving invalid entries in native category and
multi-mask lists. The two passes therefore cannot simply be merged without
changing object lifecycle ownership.

A per-pass unit-validation cache was added so a unit appearing in multiple
lists does not repeat DTI ancestry and executable-address validation. This is an
incremental improvement, but global sanitation remains the wrong steady-state
operation for a low-latency rollback engine.

### Snapshot capture

Snapshot saving currently rebuilds the region manifest and copies dynamic,
pointer-heavy game state for every replayed frame. `BuildRegions()` alone often
costs approximately 3 ms, while the complete save costs approximately 4-8 ms.

This exceeds the desired total frame budget before native simulation is
considered. Fixed game objects and stable allocations need cached manifests or
explicit compact snapshot layouts.

### Native simulation

The native tick has measured between approximately 2 and 6.5 ms per replayed
frame. Therefore, even a hypothetical zero-cost restore and snapshot system
cannot guarantee a hard 2 ms total while the full native frame path is replayed.

A lower target requires a narrower deterministic gameplay tick that excludes
presentation and unrelated manager work.

### Diagnostics

Frame JSON generation has measured between approximately 1 and 6 ms per replayed
frame. This instrumentation is useful for development but cannot remain enabled
in a production latency configuration.

Profiling also adds overhead. Production measurements must be repeated with
NDJSON and detailed profiling disabled after correctness is established.

## Architectural Floor

With the existing destroy/respawn and dynamic-region design, realistic costs are
approximately:

- Light rollback: 8-15 ms per replayed frame.
- Projectile-heavy rollback: 15-30 ms per replayed frame.
- One-frame correction: poor amortization due to fixed restore overhead.

These are not hard limits, but they indicate that incremental tuning will not
naturally converge on a universal 2 ms target.

## Required Re-Architecture

### 1. Stable projectile identities and storage

Projectiles need rollback-stable identities, such as a slot index plus generation
counter. When the current and target timelines contain the same logical shot and
concrete type, restore should rewind that live object in place instead of running
its destructor and factory.

Stable identity does not require replacing every game pointer with a handle.
The rollback layer can maintain a mapping between logical identities and current
native addresses while preserving native pointers at engine boundaries.

The initial goal should be reuse, not a complete custom allocator. Reuse matching
objects first and construct or destroy only the difference between the current
and target projectile sets.

### 2. Explicit projectile lifecycle journal

Record projectile lifecycle events by frame:

- Spawn identity and recipe.
- Despawn identity.
- Concrete type.
- Owner and slot information.
- Native-address generation.

Restore can then reconcile current and target sets:

1. Reuse matching live shots.
2. Retire shots absent from the target frame.
3. Spawn shots missing from the current timeline.
4. Rewind gameplay scalars and collision state for reused and newly created
   shots.

This changes restore complexity from rebuilding every shot to processing the
lifecycle delta plus state copies.

### 3. Maintain list invariants directly

The normal rollback path should not scan and repair every `sUnit` list twice.
Projectile and effect lifecycle hooks should update rollback-owned records at the
same points where native list membership changes.

`SanitizeUnitCategoryLists()` should remain available as an assertion failure or
recovery mechanism, not as mandatory rollback work. Development builds can run
periodic validation to verify the lifecycle journal without paying that cost in
production.

### 4. Cache snapshot manifests

Separate snapshot regions into:

- Fixed-address regions valid for the match lifetime.
- Allocation-backed regions with an owner pointer and generation.
- Lifecycle-managed objects such as shots and effects.

Build fixed manifests once when a match starts. Rebuild an allocation-backed
entry only when its owner pointer or generation changes. Do not rediscover the
entire graph for every replayed frame.

### 5. Compact deterministic snapshots

Replace broad pointer-heavy body scans with explicit deterministic state where
layouts are understood. Compact snapshots should contain gameplay scalars,
timers, RNG, collision consumption, positions, actions, and other proven state.

Live resources, intrusive links, allocator metadata, models, effects, and
presentation ownership should remain on the current native timeline.

Unknown fields should be added based on divergence evidence rather than copied
speculatively. Snapshot coverage tooling can identify fields that are captured
but fail to restore, or fields missing from deterministic state.

### 6. Isolate presentation systems

Sound, particles, rendering, UI, diagnostic output, and presentation-only
schedulers should not advance during replay. They should be reconciled once the
simulation reaches the current frame.

The current hooks already skip several presentation systems during
re-simulation. This boundary should become explicit and complete rather than a
collection of exceptional hooks.

Effects require special handling because some effect-backed objects participate
in attack or attachment ownership. Separate gameplay-relevant effect state from
particle-generator presentation state instead of restoring complete effect
graphs.

### 7. Minimal deterministic tick

Profile the native frame path and identify the smallest sequence required to
advance deterministic combat state. The replay tick should exclude:

- Rendering and presentation synchronization.
- Sound scene updates.
- UI updates.
- Diagnostics and serialization.
- Unrelated resource and scheduler work.
- Particle-generator simulation unless proven gameplay-relevant.

The target should first be a repeatable 2-3 ms gameplay tick. A universal total
below 2 ms may remain unrealistic, but reducing the native tick is mandatory for
single-frame corrections to fit comfortably within one presentation frame.

### 8. Production diagnostics mode

Provide distinct runtime modes:

- Correctness mode: full NDJSON, divergence data, expensive sanitation, and
  detailed profiling.
- Performance profiling mode: timing counters without frame JSON.
- Production mode: no per-frame JSON, no broad sanitation unless an invariant
  fails, and minimal telemetry.

Performance conclusions should be based on production mode after correctness
has been verified with the same test cases.

## Staged Targets

A practical sequence of goals is:

| Stage | Target |
|---|---|
| Restore | Under 5 ms fixed cost in common states |
| Snapshot save | Under 1 ms per replayed frame |
| Gameplay tick | Under 2-3 ms per replayed frame |
| Diagnostics | Disabled in production |
| Six-frame rollback | Under 20-25 ms total |
| One-frame rollback | Under 8-10 ms total |

These targets prioritize a playable correction budget rather than an arbitrary
per-frame number. At 60 FPS, one presentation frame is approximately 16.67 ms.
Common one- and two-frame corrections should ideally complete within that
budget. Deeper or pathological projectile-heavy corrections may require bounded
degradation, skipped rendering work, or an occasional short hitch.

## Recommended Implementation Order

1. Disable frame JSON during performance testing and establish a production
   baseline.
2. Cache fixed snapshot manifests and invalidate only changed allocation-backed
   entries.
3. Introduce logical shot identities and record lifecycle events without
   changing restore behavior.
4. Verify the identity journal against current `sShot` and `sUnit` lists in
   development builds.
5. Reuse matching current shots during restore and reconstruct only set
   differences.
6. Move global list sanitation behind invariant failures once journal-based
   lifecycle maintenance is proven.
7. Replace broad concrete body scans with explicit per-class or generated scalar
   restore ranges.
8. Profile and narrow the native replay tick.
9. Establish separate correctness, profiling, and production configurations.

## Conclusion

UMVC3 rollback is possible, and the existing implementation demonstrates the
core correctness path. The current performance problem is not the fundamental
cost of rewinding combat state. It is largely the cost of treating rollback as a
generic heap save-state system: discovering dynamic memory, validating ownership,
destroying native objects, reconstructing them, and running development
diagnostics on every replayed frame.

A strict universal target below 2 ms per replayed frame is unlikely with the
full native frame and current object reconstruction path. A playable system that
keeps common corrections within a 16.67 ms presentation budget is plausible,
but it requires a rollback-specific projectile lifecycle, cached and compact
snapshots, presentation isolation, and a narrower deterministic tick.
