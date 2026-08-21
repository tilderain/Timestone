# Material Cache Bandaids: Potential Root Fix and Removal Plan

## Status

This document describes a suspected rollback snapshot corruption path and a
plan for removing the runtime material-cache guards after the root cause is
verified. The snapshot overlap is currently a hypothesis, not a confirmed
diagnosis.

The crash logger's `character_dti` interior-object label is not proof of an
overlap. It classifies addresses using a nominal `0x6A60` character extent, so
an address reported as `uFeliciaF+0x6970` may be a separate allocation that
happens to follow the character.

## Observed Failure

The recurring failure sequence was:

1. A character material cache initially contained valid `nDraw::MaterialChar`
   objects.
2. After rollback, one or more cache entries contained null pointers or objects
   reduced to the generic `MtObject` vtable at `0x140A6A510`.
3. Native material traversals assumed every entry was valid and dispatched
   virtual methods through the stale object.
4. Repairing the cache repeatedly delayed the crash but eventually exhausted or
   corrupted renderer/allocator state.

Relevant native paths include:

| Address | Function | Failure |
|---:|---|---|
| `0x14055FBC0` | `uBaseModel__resetMaterials` | Releases and rebuilds runtime material entries |
| `0x14055FDC0` | `uBaseModel_vfn22` | Iterates cached entries for render-binding refresh |
| `0x140564110` | `uToonModel__method_140564110` | Calls material virtual `+0x38` |
| `0x1405DCCA0` | cached render-binding refresh | Assumes its material/model argument is non-null |
| `0x140549F50` | `uModel__getJointPosOrZeroByMaskedId` | Assumes nested model entries are non-null |

Material objects and their reference counts are process-local renderer state.
They must not be restored from a gameplay snapshot or released repeatedly while
headless resimulation is replaying historical action transitions.

## Current Mitigations

`rollback_runtime.cpp` currently contains defensive behavior:

- Character and character-weapon material resets are suppressed during
  resimulation.
- Suppressed resets mark the model for one deferred live validation.
- `uBaseModel_vfn22` and `uToonModel__method_140564110` repair a marked cache
  before entering native unchecked traversals.
- `uModel__getJointPosOrZeroByMaskedId` returns the native default vector when a
  nested entry is missing during resimulation.
- Character model post-move and particle-generator model presentation updates
  are omitted during headless replay.
- `uBaseModel__resetMaterials` clears recycled entries before native code can
  invoke their unrelated virtual methods.

These measures are useful crash containment, but the material reset suppression,
pending repair list, and render-time validation are bandaids. They react to a
split renderer timeline instead of proving that snapshots cannot corrupt it.

## Primary Root-Cause Hypothesis

`BuildRegionsUncached()` historically expanded every character smaller than
`0x6A60` to the nominal character size. A valid helper subclass can be smaller
than that fallback. Capturing the expanded range may therefore include the next
heap allocation.

One observed layout was:

```text
uFeliciaF object       = 0xF7F4770
Hulk material entries = 0xF7FB0E0
difference             = 0x6970
```

If the live `uFeliciaF` DTI size is below `0x6970`, a forced `0x6A60` snapshot
region includes Hulk's material pointer array. Restoring that region can
resurrect historical material pointers after their reference-counted objects
have been destroyed.

The current snapshot code uses the reported DTI size when it is at least the
verified shared `cChr` boundary `0x68F0`; `0x6A60` is now only a fallback for an
invalid size. This is a plausible root-level correction, but it must be verified
against runtime allocation boundaries.

## Verification Plan

### 1. Record Exact Object Extents

For each object in both character lists, capture the following in a debug-only
snapshot manifest:

- Object address.
- DTI address and name.
- Raw `DecodeDtiSize()` result.
- Final snapshot region size.
- End address of the region.

The manifest should be emitted once per roster generation or included in the
existing snapdump output. It should not become permanent per-frame logging.

### 2. Record Process-Local Material Ranges

For every live character model, record:

- Model address.
- Material entry-array address at `model+0x110`.
- Entry count at `model+0x118`.
- Byte interval `[entries, entries + count * 8)`.

Compare these intervals against every rollback region. Any intersection is a
snapshot contract violation unless the region is explicitly the material owner,
which character gameplay snapshots must never be.

### 3. Add Region-Overlap Validation

In debug builds, sort snapshot regions by start address and reject or report:

- Regions that overlap one another unexpectedly.
- Character regions extending beyond their decoded DTI size.
- Regions intersecting known material arrays, renderer command buffers, model
  resources, or other process-local ownership ranges.
- External regions whose owner slot no longer points to the captured allocation.

The release implementation should trim or omit an invalid region rather than
silently accepting an overlap.

### 4. Confirm the Writer

Use snapdump data to compare a material array immediately before and after
`Rollback_RestoreSnapshot()`. The root cause is confirmed only if a restore
changes the array without a native material operation occurring between the two
captures.

If restore does not write the array, the investigation must move to material
ownership and refcount transitions. In that case, capture the exact caller of
the final release and determine which second owner retained an uncounted alias.

## Preferred Root Fix

### Exact Region Sizes

Always use the decoded DTI allocation size when it is plausible. A nominal size
must only recover a missing or corrupt DTI size; it must never enlarge a valid
smaller derived class.

The fallback should also be bounded by the containing committed allocation when
that boundary can be identified safely.

### Explicit Character Restore Ranges

The stronger long-term fix is to stop restoring the entire character body and
then heuristically preserving values that look like pointers. Packed flags and
float pairs can resemble pointers, while renderer-owned scalar fields may not.

Character restore should instead copy an explicit manifest of deterministic
gameplay ranges:

- Action and status state.
- Position, velocity, hitbox, damage, input, and command matcher state.
- Verified concrete-character scalar tails.
- External gameplay allocations with validated owner slots.

It should explicitly omit:

- `uBaseModel` resource and material ownership.
- Material arrays and reference counts.
- Render command pages and dirty-command caches.
- Scene layers, shaders, particle models, and presentation-only transforms.
- Weak pointers whose targets are reconstructed separately.

This removes dependence on `IsLikelyPointer()` as the primary ownership policy
and makes accidental restoration of adjacent allocations much harder.

### Headless Presentation Boundary

Headless replay should advance gameplay effect lifetimes and deterministic
effect state without executing model/material presentation work. Newly created
resimulation effects may still require enough resource initialization for their
gameplay state, but renderer material cloning and particle-model transforms
should occur only on the live timeline or during an explicit post-resimulation
presentation rebuild.

The boundary should be defined by native function behavior rather than broad DTI
class exclusions. A function can be skipped only after confirming that it does
not mutate gameplay state, RNG, effect lifetime, collision, or ownership needed
by later deterministic code.

## Bandaid Removal Plan

Removal must be staged so a regression identifies which invariant remains
broken.

### Phase 1: Prove Snapshot Isolation

Keep all guards enabled while running the overlap validator. Require:

- No snapshot region intersects any material entry array.
- No valid DTI size is expanded past its allocation.
- Material arrays remain byte-identical across restore unless native live code
  changes them.
- No material object reaches the generic `MtObject` vtable while still cached.

### Phase 2: Remove Lazy Render Repair

Disable the hooks for:

- `uBaseModel_vfn22` at `0x14055FDC0`.
- `uToonModel__method_140564110` at `0x140564110`.

Retain the reset sanitizer temporarily. Stress test ordinary rollback and F9
roster restarts. Any stale traversal here means snapshot isolation or material
ownership is still incorrect.

### Phase 3: Remove Pending Material Repairs

Remove:

- `g_pendingMaterialRepairs`.
- `MarkMaterialRepairPending()` and related lookup/removal helpers.
- Deferred live validation from the render hooks.

At this stage, a suppressed headless reset must be unnecessary because the live
material cache remains valid and presentation state is rebuilt through a proven
live-only path.

### Phase 4: Remove the Reset Sanitizer

Remove stale-entry clearing from `hkResetMaterials` and restore the native reset
call for live execution. Keep headless suppression only if testing proves that
native character resets mutate renderer ownership during resimulation.

If no headless reset suppression is necessary after the snapshot contract is
fixed, remove the reset detour entirely.

### Phase 5: Reassess Adjacent Guards

The null guard for `uModel__getJointPosOrZeroByMaskedId` and the particle-model
filter were introduced from related headless presentation crashes, but they are
not strictly material-cache repairs. Remove them only after independently
proving that effect model tables are initialized and owned correctly during
resimulation.

## Stress-Test Matrix

Each removal phase should pass:

- Local F7 rollback tests at depths 1 through 15.
- Two-peer netplay at low latency and at approximately 300 ms RTT.
- Repeated F9 roster restarts across at least 20 battles.
- Felicia, Hulk, Hsien-Ko, Phoenix Wright, Firebrand, Shuma-Gorath, and effect-
  heavy teams.
- Assists, tag transitions, TAC, DHC, throws, supers, and KO transitions.
- Repeated rollback during action changes that normally call material reset.
- Checksum agreement and no growth in render-command, material, or effect
  allocations.

## Acceptance Criteria

The material bandaids can be removed when all of the following hold:

- Snapshot manifests show no process-local render ownership in restored regions.
- Material arrays do not change across restore.
- Native material resets never encounter null, recycled, or aliased entries.
- No material-related access violation occurs during extended stress testing.
- Render-command page use remains bounded across repeated battles.
- Effect and character visuals recover correctly after rollback.
- Both peers retain deterministic gameplay checksums.

Until these criteria are met, the runtime guards should be treated as temporary
containment with explicit compile-time switches, not as the final rollback
architecture.
