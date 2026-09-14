# Compact Rollback Snapshots

## Summary

A compact snapshot stores only state required to reproduce future gameplay. It
does not copy complete native objects and then filter pointer-bearing fields at
restore time.

The September 11 investigation identifies significant snapshot memory traffic
and repeated identical data, but **no compact-snapshot speedup has been verified**.
Earlier 8-12% typical / 5-15% broader improvement estimates are historical
projections, not predictions established for the current build.

Larger gains may also require separate architectural changes, particularly stable
projectile identities, incremental projectile reconciliation, and removing
mandatory global `sUnit` sanitation.

## Current Baseline

The current timing reference is the user-supplied run
`rollback_profile_runs/20260911T095307.034337Z_addr_fixed_candidate_4000`:

- **379 regions / 1837.698242 KiB (1,881,803 bytes) per timed snapshot**.
- Both peers completed 4000 fighting frames with CPU mode 2, stage 105 and
  multi-resim enabled; 700 rollback windows / 4793 replayed frames combined.
- The runner reports no crash or desync artifacts. Baseline status is
  `profile_ready`, not `optimization_verified`.
- Exact historical build commit/compiler flags are not recorded in its summary.
  Source and Ghidra findings below describe the inspected code; they do not
  independently establish the profiling executable's build identity.

| Metric | Host | Client |
|---|---:|---:|
| Total rollback ms/replayed frame | 1.72046 | 1.61656 |
| Native tick ms/replayed frame, including nested mod work | 1.15850 | 1.13467 |
| Snapshot phase ms/replayed frame | 0.31417 | 0.27495 |
| Copy interval ms/replayed frame | 0.18564 | 0.18243 |
| Region restore ms/replayed frame | 0.08405 | 0.08433 |
| Entire restore ms/replayed frame | 0.23913 | 0.19824 |

The copy interval also includes camera/post-state capture. It is not an
exclusive memory-bandwidth measurement. Saving **0.1 ms/replayed frame** from
this interval alone requires approximately **54-55% less elapsed time**, including
any new comparison or bookkeeping cost. No such reduction has been measured.

Broad `sMain`, `sHitSolver`, and `sCollision` captures are already disabled in
the inspected source and absent from the inspected Timestone region builder.
Their historical sizes must not be counted as remaining opportunities.

### Historical baselines

Earlier optimization tests used a snapshot containing:

- 330 regions per frame.
- 1540.157 KiB per frame.
- Complete or broad native object bodies, even when restore consumes only
  selected ranges from those bodies.
- External animation, collision, command, and joint buffers whose derived versus
  authoritative status has not always been proven.

The historical 49-window rollback profile in `retick.md` reported:

| Metric | Measured value |
|---|---:|
| Average rollback depth | 5.4 frames |
| Average rollback cost | 13.64 ms |
| Average restore | 1.39 ms |
| Average post-resimulation save | 1.30 ms |
| Restore share | 10.2% |
| Snapshot share | 14.4% |
| Native-tick share | 75.1% |

That profile illustrates a practical limit: compact packing does not directly
remove the native-tick interval. Its 75.1% share is historical, not the current
baseline's share, and includes mod work nested inside native ticking.

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

## Constant Data Versus Temporarily Unchanged Data

**Data proven constant throughout the relevant lifetime does not need repeated
snapshotting or restoration.** Data that merely stayed unchanged during a test
does not satisfy that proof.

| Classification | Snapshot policy | Required evidence |
|---|---|---|
| Truly immutable resource data | Exclude it from per-frame state | Audit writers and prove the resource remains valid for every referencing snapshot |
| Fixed until resource/object replacement | Retain a version or stable identity for that lifetime | Identify replacement, invalidation and ownership boundaries |
| Mutable but currently unchanged | Reuse identical captured bytes or retain historical versions | Exact byte equality, or a proven complete writer-generation contract |
| Derived data | Regenerate instead of storing only if equivalent | Rebuild before the first consumer, preserving bit-exact results and side effects |
| Unknown mutable state | Keep capturing it | Do not infer irrelevance from a quiet trace or presentation-sounding name |

For example, frames 100 and 101 can share state A, but if a super changes the
live state to B at frame 102, rollback to frame 100 still needs A. Identical
snapshots permit storage reuse; they do not necessarily permit leaving the live
object untouched during restore.

This gives two distinct optimization tracks:

1. **Semantic compaction:** prove bytes unnecessary and exclude them from the
   logical snapshot.
2. **Lossless storage/copy reuse:** keep the complete logical snapshot while
   reducing physical duplication or writes.

The second track can be investigated before the first is fully mapped. It must
not be described as proof that reused fields are non-authoritative.

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

## Packing Opportunities and Current Status

### `sMain`

**Historical opportunity, already removed from current capture.** Earlier
snapshots copied `0x40080` bytes, approximately 256 KiB, despite restore consuming
only a small range set. The body includes job records, worker descriptors,
handles, locks and queues. The current builder does not add this broad region;
the old 16.6% byte-reduction and 0.03-0.08 ms/save projections are not additional
savings available now.

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
- Duplicate representations, if a current manifest proves that they exist.
  The inspected builder captures `cChrInput` inside the character body; its
  separate restore `memcpy` is not evidence of a second captured region.

This conversion has a higher correctness burden than simple manager packing
because unknown character fields have repeatedly proved authoritative through
desync analysis.

### Material-Job Tables Currently Named `uCharacter.targetTable`

The seven table pointers at character `+0x1408..+0x1438`, with packed counts at
`+0x1440`, are captured using `0xBB0`, `0xB90` and `0xB80` entry strides.
Ghidra inspection of the sampled table vtables identifies material jobs, not
collision `TargetInfo` entries:

- `0x140A8ED30`: `uMaterialVjobCngColor`; getDTI `0x14013F530`,
  move `0x140140060`.
- `0x140A8EDF0`: `uMaterialVjobCngUV`; move `0x1401409B0`.
- `uCharacter__processTarget1`, `0x140200F50`, dispatches table entries through
  virtual `+0x40`.

Each job is large partly because its bases contain **32 controller records at
`+0x50`, stride `0x28`, and 32 extended controller records at `+0x550`, stride
`0x30`**. Native constructors `0x1405496F0` and `0x14044ECC0` establish those
layouts. `uMaterialVjob` adds parent, material selector, acceleration mode,
attributes and clocks at `+0xB50..+0xB77`.

This makes bindings/identity versus mutable clocks/selectors a useful compaction
boundary to investigate. **It does not prove the controller arrays or complete
jobs immutable.** Native `uMaterialControl__trans` (`0x14054A430`), controller
apply (`0x14054A960`), color/UV moves and completion callbacks change material
state, clocks, flags and references. All mutation paths must be mapped before
excluding fields; retaining equal captured table bytes needs no such omission.

The earlier count/table mismatch crash documented in `shot.md:824` remains
relevant: leaving tables uncaptured while rewinding their counts is not a valid
optimization merely because these objects control materials.

## September 11 Exact-Byte Reuse Audit

Used existing historical files:

```text
<game>/autotest/Tron/host_1.usdp
<game>/autotest/Tron/client_1.usdp
Recorded commit: 4488c6945f0ea2573dd531a7ae865398acb49f5f, dirty
Frames: 22-276 inclusive, 255 per peer
Manifest: 198 regions / 1,322,356 bytes per snapshot
```

**These are separate, older captures, not the current dual-CPU baseline.** Their
byte equality establishes reuse potential in that workload, not current timing,
immutability across all actions, or a new correctness verdict.

| Historical region family | Bytes/snapshot | Share |
|---|---:|---:|
| Character motion-blend work | 454272 | 34.35% |
| Material-job target tables | 322368 | 24.38% |
| Character joint states | 209664 | 15.85% |
| Character bodies | 161280 | 12.20% |
| Blended-joint state | 69888 | 5.28% |
| Weapon bodies | 67200 | 5.08% |
| Everything else | 37684 | 2.85% |

| Exact-byte equality measure | Host | Client |
|---|---:|---:|
| Bytes in whole regions equal to the adjacent frame | 79.22% | 79.95% |
| Bytes in whole regions equal to previous contents of the same 16-slot ring position | 77.95% | 78.80% |
| Bytes in equal region-relative 4-KiB chunks versus the same ring position | 86.11% | 86.69% |

Material-job tables were entirely unchanged across this history. Motion-blend
regions were 92.62% / 92.55% unchanged by bytes between adjacent frames, joint
states 72.90% / 72.90%, and blended-joint state 99.61% / 99.61%.

The read-only audit tool matches region identities and compares actual payload
bytes, not stored hashes. Same-ring-position comparisons use chronological
history; they do not recreate nonmonotonic live/resimulation save order.

```bash
python3 -B tools/analyze_snapshot_copy_reuse.py \
  "../ULTIMATE MARVEL VS. CAPCOM 3/autotest/Tron/host_1.usdp"
```

### Lossless Copy/Storage Candidates

Timestone `Rollback_SaveSnapshot` at `0x3bd378ce0` copies every full region
unconditionally. Two experiments follow from the audit:

- **Compare before copying selected large buffers:** when the owned destination
  is already initialized, leave it unchanged only if exact comparison succeeds.
  Changed buffers still copy normally. New/reallocated/shared storage needs its
  original initialization path. Preserve all metadata, capture side effects and
  logical bytes. This can avoid writes but adds reads and comparisons, so it may
  regress despite high equality.
- **Share immutable captured region versions between snapshots:** retain every
  historical byte but reduce physical duplication and ring working set. This
  requires updating contiguous `slot.data + offset` consumers and ownership.
  Flattening the entire snapshot on each restore may merely move the copy cost.

Neither experiment has been implemented or performance-verified. The baseline
ring holds about 28.7 MiB of raw bytes per peer; physical sharing may help its
working set, but no hardware-cache benefit has been measured. **Do not multiply
the unchanged-byte percentage by copy time to claim a speedup.**

## State That Is Not Yet Safe to Remove

### Broad `sAction`

Earlier removal of broad `sAction` state caused defeat-replacement divergence.
The inspected current builder already disables the separate `sAction.full`
capture and retains a `0x300` region named `sAction.compact`, plus model-joint
allocation state. Do not treat the old broad region as a remaining byte-saving
target. Any further narrowing of the active representation must preserve the
team-transition fields and pass defeat/replacement tests; a "compact" label is
not proof that every captured byte is necessary or safely removable.

### Animation and Joint Buffers

Blend arrays, motion-channel storage, joint states, joint indices, and blended
joint outputs appear partially derived. Existing rollback investigation showed
that leaving them on the future timeline can split root motion and joint
transforms across timelines, including throw displacement.

They can be omitted only after restoring animation inputs, running native
evaluation before all gameplay consumers, and proving byte-for-byte equivalence
with the captured outputs.

The September 11 Ghidra inspection strengthens this caution:
`uModel__bindMotionEntryToSlot` (`0x14052A880`) writes per-joint track descriptors
and constraint state inside `0x68`-byte motion-work records.
`uModel__blendActiveMotionTransforms` (`0x140529820`) reads old blend-mask data
before resetting/rebinding a changed slot, and resets only selected fields.
An inactive slot or pending motion change is not sufficient proof that its
historical buffer can be dropped. Lossless byte reuse remains a separate option.

### Projectile and Effect Ownership

Compact scalar records do not solve projectile lifecycle ownership. The current
restore destroys and respawns shots, sanitizes global unit lists, reconstructs
effects, and repairs references. Replacing broad shot bodies with compact fields
without stable identities would leave most of that cost unchanged.

## Historical Performance Projections

The following unverified estimates use the historical 13.64 ms average rollback
and its phase shares. They predate removal of some broad manager captures and
must not be presented as forecasts for the September 11 baseline:

| Scope | Estimated saving | Estimated whole-rollback gain |
|---|---:|---:|
| Pack only currently consumed manager ranges | 0.2-0.5 ms | 1-4% |
| Compact managers and character state | 0.7-1.5 ms | 5-11% |
| Aggressive, proven authoritative-only format | 1.0-2.0 ms | 7-15% |

A mature compact implementation might reduce snapshot size from 1540 KiB to
roughly 300-700 KiB. The exact result depends primarily on whether animation,
collision, command, and joint buffers remain necessary.

One reasonable projected outcome for the measured 5.4-frame rollback is:

| Phase | Historical baseline | Projected compact result |
|---|---:|---:|
| Snapshot contribution | approximately 1.96 ms | 0.7-1.1 ms |
| Restore | approximately 1.39 ms | 1.0-1.2 ms |
| Total rollback | 13.64 ms | 11.9-12.7 ms |

These reductions do not scale directly with byte count. CRT `memcpy` is already
efficient, and snapshot work also includes region construction, ownership
validation, collision cloning, shot recipes, and metadata that compact packing
does not necessarily eliminate.

## Recommended Migration

1. Obtain a current-workload region manifest and byte-equality breakdown. The
   supplied baseline times snapshots but does not include their raw bytes.
2. Classify material-job table fields by lifetime: immutable bindings, mutable
   clocks/selectors, native-owned references and unknown state. Audit writes
   before deciding which bytes need no per-frame capture.
3. Independently test exact-byte copy avoidance on large initialized buffers.
   Consider shared captured versions only if the measured gain justifies the
   storage/reader changes.
4. Pack `CCHR_TEAM_RESTORE_RANGES` with a stable active-member identity; define
   explicit base-character and per-DTI records while retaining unknown fields
   until their role is established.
5. Verify actual duplicate regions before proposing duplicate removal. Do not
   count the already disabled manager captures or an extra restore operation as
   duplicated capture.
6. Retain animation/joint history until regeneration or irrelevance is proven;
   retain the current `sAction` transition contract through defeat/replacement.
7. Treat projectile identity/lifecycle reconciliation as a separate effort,
   with its own performance and correctness evidence.

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

For lossless copy/storage reuse, also compare complete logical snapshot bytes
and test ring wrap, repeated/nonmonotonic resimulation saves, allocation or owner
replacement, shared snapdump storage and rematch generations. Measure comparison
cost, copied bytes and post-resim work to detect displaced overhead. An
unchanged region in a recorded history is evidence for an experiment, not proof
that it can be excluded from authoritative state.

Performance claims should be normalized by snapshot count, replayed frames, and
rollback windows. Aggregate rollback time alone is too sensitive to differing
CPU battle trajectories.

## Expected Architectural Limit

Compact snapshots can improve memory traffic, ownership clarity and restore
simplicity, but do not eliminate required native simulation. The current
0.1 ms/frame goal is an investigation target, not an established compact-format
result. Earlier 8-12% compact-only and 20-40% combined architectural projections
remain unverified; the latter also assumed separate projectile/lifecycle changes.

Detailed evidence and Ghidra addresses are recorded in
[`snapshot_bandwidth_investigation_20260911.md`](snapshot_bandwidth_investigation_20260911.md)
and [`rollback_performance_review_20260911.md`](rollback_performance_review_20260911.md).
