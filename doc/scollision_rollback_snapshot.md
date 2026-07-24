# `sCollision` Rollback Snapshot

**Status: Current focused reference. Verified against Ghidra and `rollback.cpp`
on 2026-07-23.** Collision restore combines an ownership-aware singleton copy,
discovered external buffers, and deep clones of character attachment nodes.

## Native Singleton

| Item | Value |
|---|---|
| Singleton pointer storage | `0x140E18250` |
| Captured size | `0x239E0` |
| `cSystem` base | `+0x0000..+0x0037` |
| Collision body | `+0x0038..+0x239DF` |

Major inline regions recovered in the singleton include:

| Range | Native state |
|---|---|
| `+0x0280..+0x43B7` | Broadphase-array state and inline BVH headers |
| `+0x43B8..+0x685F` | `SbcObject` manager state and inline BVH headers |
| `+0x6860..+0x733F` | Skin-mesh manager state |
| `+0x7340..+0x7367` | Height-field manager state |
| `+0x7368..+0x73A7` | Move-matrix manager header and parallel flags |
| `+0x73B0..+0x2222F` | Inline cloned collision-geometry arrays |
| `+0x22230..+0x2396F` | Inline deferred cluster rows/counters |
| `+0x23970..+0x239DF` | Private allocator and ownership state |

The exact semantic name of every inline word is not required by restore. The
critical distinction is rewindable scalar/container state versus live allocator
and synchronization ownership.

## Singleton Restore Policy

`RestoreSCollisionBody` does not flat-copy all `0x239E0` bytes. It preserves:

- The `cSystem` base and its critical section.
- The private allocator tail at `+0x23970..+0x239DF`.
- Live values for pointer-bearing aligned words according to the collision
  restore policy.

Other inline state is restored from the selected snapshot. This avoids replacing
current allocation ownership with stale historical addresses.

## External Discovery

`BuildRegions` follows verified ownership headers and adds external regions only
when pointers, counts, capacities, and address ranges pass safety checks. Each
region records its owner pointer slot. Restore skips it if that slot no longer
points to the captured allocation.

### Dynamic BVHs

The helper visits five broadphase-array BVH banks and two `SbcObject` BVH banks,
31 BVHs per bank. For each reachable BVH it can capture:

- Fixed `0x40`-byte node pool payloads while leaving the preceding allocator
  header live.
- Free-node pointer arrays at BVH `+0x20`.
- No-use-node pointer arrays at BVH `+0x48`.
- Individually allocated `0x40`-byte nodes reachable from the root but outside
  the fixed pool.

Standalone tree traversal is bounded and deduplicated. Parent/root or child
pointer slots serve as ownership anchors for separately allocated nodes.

### `SbcObject` arrays

Discovery captures:

- Active registration pointer array from the `SbcObject+0x40` `MtArray`.
- Forty-one dirty queue arrays beginning at `SbcObject+0x70`.
- Forty-one merge queue arrays beginning at `SbcObject+0x590`.
- Reachable `0x28`-byte queue records referenced by active queue entries.

### Move-matrix queues

The move-matrix header at `sCollision+0x7368` owns a category pointer array.
Each category can point to a per-thread/frame array of `NodeList*`. Discovery
captures:

- Category and per-thread pointer arrays.
- Each reachable `0x2550`-byte `NodeList` body.
- Seven groups of 41 external pointer arrays in each `NodeList`.
- Four additional singleton arrays.
- Two embedded dynamic BVHs and their pools/pointer arrays.

This supersedes older notes saying only move-matrix headers were captured.

## Character Collision Attachments

Each live `cChr` exposes four external collision attachment pointers at:

```text
cChr+0x4200
cChr+0x4E10
cChr+0x5520
cChr+0x5C30
```

Constructor arithmetic and native users establish these offsets. An older
proposed `-0x540` adjustment was incorrect.

Snapshot handling has two layers:

- The external wrapper's common `0xE0` scalar image is captured with the owner
  slot and expected vtable.
- `CaptureCollisionNodeClones` asks the native collision-node factory/copy path
  for a deep clone of each reachable attachment graph.

Restore applies wrapper scalar policy only if the owner still matches, then
copies the cloned node/child graph back through the native copy function. If an
external owner changed, the live owner pointer is restored rather than writing
to abandoned memory.

The fourth attachment participates in native fighter push callback setup:
`uCharacter__applyCollisionPushCorrection` consumes contact information and
accumulates a correction into the owning character. Its known geometry payload
is treated separately from vtables, references, and ownership metadata.

## Geometry Boundary

`nCollision__testNodeGeometryPairs` at `0x1405AB9A0` establishes the native
narrow-phase boundary:

- Child cached bounds prune candidate pairs.
- The selected primitive intersection receives each heap `MtGeom` pointer.
- Contact output is passed to callback dispatch.

Current character-node cloning and dedicated geometry handling preserve fresh
object ownership while rewinding supported shape payload. Cached bounds can be
recomputed by native geometry methods.

## Generic Chain Solver Caveat

`uCnsGroup__stepSolver` at `0x14071C170` and
`uCollisionNode_processCharacterPush` at `0x140725B70` belong to the generic
`uCnsTinyChain` secondary-motion constraint system. The latter iterates generic
child bodies and resolves sphere/capsule separation using fields such as shape,
radius, mass, position, previous position, and contact flags.

The word “CharacterPush” in a recovered function name does not prove these are
MVC3 fighter pushboxes. Runtime owner and pointer provenance is required before
using this solver to explain fighter root-position divergence.

## Current Limits

The discovery is broad but still not a heap checkpoint:

- Unsupported or malformed ownership headers are skipped.
- An allocation replaced after capture is retained rather than overwritten.
- External objects not reachable through the audited headers are not captured.
- Concrete collision subclasses can own additional unrecognized state.
- Queue and worker interaction still requires deterministic execution policy.

Two-peer rollback-heavy contact tests remain required. A future divergence
should first identify the exact callback, object owner, and changed region,
rather than broadening pointer restoration indiscriminately.

## Historical Capture Note

The July 18 `044007_host`/`044006_client` capture first exposed a roster identity
problem, not a proven collision-state omission. Team 1 intrusive-list traversal
order differed from startup, while stable `(team, mid)` identity remained equal
until a host-only rollback. Later RNG/flag divergence followed different member
activation ownership.

Current restore handles `cChrTeam` active-character state separately while
retaining live list topology. Historical `(team, slot)` comparisons should not
be treated as stable character identity; diagnostics should prefer a stable
member identifier.

## Invariants

- Do not restore historical allocator or critical-section bytes.
- Restore an external region only while its live owner slot still references
  the captured allocation.
- Keep pointer-array capacities/counts validated and bounded.
- Clone polymorphic character collision graphs through native constructors/copy
  logic rather than raw heap-body allocation.
- Treat generic chain collision and fighter gameplay collision as separate until
  runtime provenance connects them.
