# Compact Rollback Snapshots

## Summary

A compact snapshot stores only state required to reproduce future gameplay. It
does not copy complete native objects and then filter pointer-bearing fields at
restore time.

For the current rollback implementation, compact snapshots alone are expected
to improve a typical rollback by approximately 8-12%. A broader plausible range
is 5-15%, depending on how much character, animation, collision, and command
state can be safely represented or regenerated. These figures are projections,
not benchmark results from an implemented compact format.

Larger gains require separate architectural changes, particularly stable
projectile identities, incremental projectile reconciliation, and removing
mandatory global `sUnit` sanitation.

## Current Baseline

The measured snapshot used by the existing optimization tests contains:

- 330 regions per frame.
- 1540.157 KiB per frame.
- Complete or broad native object bodies, even when restore consumes only
  selected ranges from those bodies.
- External animation, collision, command, and joint buffers whose derived versus
  authoritative status has not always been proven.

The newer 49-window rollback profile in `retick.md` reported:

| Metric | Measured value |
|---|---:|
| Average rollback depth | 5.4 frames |
| Average rollback cost | 13.64 ms |
| Average restore | 1.39 ms |
| Average post-resimulation save | 1.30 ms |
| Restore share | 10.2% |
| Snapshot share | 14.4% |
| Native-tick share | 75.1% |

This establishes a hard practical limit: compact snapshots cannot directly
remove the 75.1% currently spent executing native simulation.

## What Is Authoritative State?

Authoritative state is the minimum state that must be restored for all future
simulation results to match the original timeline. Typical examples include:

- Simulation frame and relevant timing state.
- Character position, facing, velocity, acceleration, and movement state.
- Current action, animation, action frame, transition state, and hitstop.
- Health, meter, stun, damage, combo, guard, and knockdown state.
- Character FSM flags, counters, charges, installs, and move-specific latches.
- Input history and command-matcher progress.
- Active collision-bank selectors and consumed-hit state.
- Global and per-object RNG state.
- Team state, including active member, assists, X-Factor, and tag transitions.
- Projectile logical identity, lifecycle, owner identity, flight state, and
  gameplay-facing concrete fields.

State is not automatically authoritative merely because it changes every frame.
Derived state can be omitted only when a known native operation can regenerate
it in the correct order before any gameplay consumer reads it.

## State That Should Usually Stay Live

The following state should generally remain on the live native timeline or be
reconstructed through an ownership-aware path:

- Vtables and DTI pointers.
- Heap pointers and allocator metadata.
- Intrusive-list links.
- Resource, model, texture, sound, and presentation-effect pointers.
- Array storage pointers and capacities.
- Locks, handles, worker records, and synchronization objects.
- Rendering caches and presentation-only camera state.
- Duplicate derived data that can be regenerated safely.

A pointer blacklist alone is not a compact snapshot. It still copies unknown
state and must correctly classify embedded `MtObject`s, arrays, references,
intrusive links, allocator records, selector/count fields coupled to pointers,
and null-to-non-null ownership transitions.

## Example Format

A compact character record could resemble:

```cpp
struct CompactCharacterSnapshot {
    uint32_t action;
    uint32_t animation;
    uint32_t actionFrame;
    uint32_t flags;
    int32_t health;
    int32_t hitstop;
    Vec3 position;
    Vec3 velocity;
    Vec3 acceleration;
    CharacterInputSnapshot input;
    CharacterSpecificSnapshot concrete;
};
```

The actual format should use the verified native field widths and should avoid
introducing generic containers or pointers into snapshot storage. Character-
specific records may be selected by DTI, but their contents should remain
explicit scalar/POD fields or stable rollback identities.

## Current Restore Is Already Hybrid

The current character path is not a strict whitelist-only implementation:

- Complete DTI-sized character bodies are captured.
- The common character body is broadly copied while pointer-looking fields and
  selected live ownership ranges are preserved.
- Concrete character tails are broadly restored around detected pointers and
  embedded objects.
- Generated and verified scalar ranges are reapplied after filtering because
  packed flags or float pairs can resemble pointers.

Consequently, replacing the scalar whitelist with a pointer blacklist would not
significantly reduce snapshot capture cost. It would mostly remove a small range
table pass while retaining broad copies and increasing ownership risk. A static
precomputed blacklist might save tens of microseconds per restore; dynamic
pointer discovery may instead be slower.

## High-Confidence Packing Opportunities

### `sMain`

The snapshot currently copies `0x40080` bytes, approximately 256 KiB. Restore
uses five ranges totaling approximately 180 bytes. The omitted body contains
job records, an asynchronous job ring, worker descriptors, handles, locks, and
queue state that intentionally remains live.

Packing only the consumed ranges removes approximately 16.6% of the measured
1.54 MiB snapshot. Because optimized sequential copies are cheap, expected time
savings are much smaller than the byte percentage, roughly 0.03-0.08 ms per
snapshot save.

### `cChrTeam`

Each team currently contributes `0x2C0` bytes. Restore uses the validated active
character identity and gameplay ranges beginning at `+0x48`. The vtable,
intrusive-list head, and initial ownership fields can be excluded from packing.

### Broad Character Bodies

Character snapshots should eventually contain named base-character gameplay
ranges and per-DTI concrete state rather than complete bodies. This can remove:

- Model and resource ownership state.
- Presentation and draw-list state.
- Embedded object bytes that are intentionally kept live.
- Duplicate `cChrInput` storage currently packed both inside the character body
  and as a separate region.

This conversion has a higher correctness burden than `sMain` because unknown
character fields have repeatedly proved authoritative through desync analysis.

## State That Is Not Yet Safe to Remove

### Broad `sAction`

Removing the broad `sAction` capture while retaining known compact ranges caused
a two-peer divergence during defeat replacement. The broad region therefore
contains authoritative team-transition state that has not yet been identified.
It must remain until those fields are mapped explicitly.

### Animation and Joint Buffers

Blend arrays, motion-channel storage, joint states, joint indices, and blended
joint outputs appear partially derived. Existing rollback investigation showed
that leaving them on the future timeline can split root motion and joint
transforms across timelines, including throw displacement.

They can be omitted only after restoring animation inputs, running native
evaluation before all gameplay consumers, and proving byte-for-byte equivalence
with the captured outputs.

### Projectile and Effect Ownership

Compact scalar records do not solve projectile lifecycle ownership. The current
restore destroys and respawns shots, sanitizes global unit lists, reconstructs
effects, and repairs references. Replacing broad shot bodies with compact fields
without stable identities would leave most of that cost unchanged.

## Estimated Performance

The following estimates use the measured 13.64 ms average rollback and its
reported phase shares:

| Scope | Estimated saving | Estimated whole-rollback gain |
|---|---:|---:|
| Pack only currently consumed manager ranges | 0.2-0.5 ms | 1-4% |
| Compact managers and character state | 0.7-1.5 ms | 5-11% |
| Aggressive, proven authoritative-only format | 1.0-2.0 ms | 7-15% |

A mature compact implementation might reduce snapshot size from 1540 KiB to
roughly 300-700 KiB. The exact result depends primarily on whether animation,
collision, command, and joint buffers remain necessary.

One reasonable projected outcome for the measured 5.4-frame rollback is:

| Phase | Current | Projected compact result |
|---|---:|---:|
| Snapshot contribution | approximately 1.96 ms | 0.7-1.1 ms |
| Restore | approximately 1.39 ms | 1.0-1.2 ms |
| Total rollback | 13.64 ms | 11.9-12.7 ms |

These reductions do not scale directly with byte count. CRT `memcpy` is already
efficient, and snapshot work also includes region construction, ownership
validation, collision cloning, shot recipes, and metadata that compact packing
does not necessarily eliminate.

## Recommended Migration

1. Change `sMain` capture to pack only `SMAIN_RESTORE_RANGES`.
2. Pack only `CCHR_TEAM_RESTORE_RANGES` plus a stable, validated active-member
   identity.
3. Remove proven presentation-only singleton regions while retaining gameplay
   camera state such as `uActionSideCamera`.
4. Define an explicit base-character record from the currently verified restore
   ranges.
5. Add per-DTI concrete character records from generated and verified fields.
6. Store `cChrInput` once rather than as both broad-body and external data.
7. Retain animation and joint outputs until deterministic regeneration passes.
8. Map the missing `sAction` defeat and team-transition fields before narrowing
   its broad capture.
9. Introduce stable projectile identities and a lifecycle journal before
   expecting major projectile-restore savings.

Each stage should be implemented and measured independently. Do not combine a
packing change with lifecycle or native-tick changes in the same performance
comparison.

## Validation Requirements

For every compacted subsystem:

1. Capture both the current broad representation and the proposed compact
   representation in a diagnostic build.
2. Restore from the compact representation.
3. Compare authoritative state immediately after restore and after every
   replayed frame.
4. Compare regenerated buffers byte-for-byte before their first gameplay use.
5. Run matched host/client profiles and verify snapshot bytes, workload counts,
   checksums, crash artifacts, and desync artifacts.
6. Test transitions, not only neutral gameplay: hits, trades, throws, armor,
   assists, tags, DHC, snapback, defeat replacement, installs, projectiles,
   multi-hits, hitstop, pause, slowdown, training reset, and timeout.

Performance claims should be normalized by snapshot count, replayed frames, and
rollback windows. Aggregate rollback time alone is too sensitive to differing
CPU battle trajectories.

## Expected Architectural Limit

Compact snapshots improve memory traffic, ownership clarity, diagnostics, and
restore simplicity, but they do not eliminate the dominant native simulation
cost. Based on the newer profile, a standalone compact snapshot effort should
target an 8-12% typical rollback improvement.

Combining compact snapshots with stable projectile reuse, lifecycle-delta
reconciliation, and moving global `sUnit` sanitation behind invariant failures
could plausibly produce a much larger 20-40% improvement in affected states.
That larger estimate must not be attributed to compact packing alone.
