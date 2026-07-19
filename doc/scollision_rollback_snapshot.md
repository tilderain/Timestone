# sCollision Rollback Snapshot

The rollback snapshot captures the complete `sCollision` singleton at
`g_sCollision__mpInstance` (`0x140e18250`). Its corrected size is `0x239E0`:

- `+0x00000..+0x00037`: `cSystem` base
- `+0x00038..+0x239DF`: `sCollision__body`

## Restored frame state

Restoring the complete body covers every currently identified inline field:

| Singleton range | Body field |
|---|---|
| `+0x0038..+0x027F` | material/configuration fields, state flags, correction counters, and category ranges |
| `+0x0280..+0x43B7` | `mSbcArray` broadphase state |
| `+0x43B8..+0x685F` | `mSbcObject` state |
| `+0x6860..+0x733F` | `mSbcSkinMesh` state |
| `+0x7340..+0x7367` | `mSbcHeightField` state |
| `+0x7368..+0x73A7` | `mSbcMoveMatrixArray`, including frame-slot cursors and dispatch flags |
| `+0x73A8` | `mFlgRunColliderInSystem` |
| `+0x73B0..+0x2222F` | 246-entry cloned OBB, sphere, capsule, triangle, line-segment, and AABB geometry arrays |
| `+0x22230..+0x2396F` | six 41-entry node-cluster rows and their total counters |

This includes all scalar counts, flags, bounds, cloned geometry, queue lengths,
registration metadata, broadphase metadata, frame-slot state, and inline BVH
state represented inside the singleton.

## Preserved ownership state

Some bytes are process ownership and synchronization state rather than rewindable
gameplay state. Restore therefore retains their live values:

- `cSystem` base bytes at `+0x0000..+0x0037`, including synchronization state.
- `mPrivateAllocator` at `+0x23970..+0x239DF`, including its critical section,
  pool base, freelist, parent allocator, and bookkeeping.
- Every aligned 64-bit word whose current or saved value resolves to committed,
  accessible process memory. The live word is retained even when it is currently
  null. These are heap buffers, vtables, collision objects, BVH roots or leaf
  handles, manager arrays, and other ownership-bearing addresses.

The pointer rule intentionally restores the state stored around those addresses
without replacing live ownership with historical addresses. Flat-restoring those
pointers would require atomically snapshotting every referenced allocation and its
allocator topology, and can otherwise expose freed nodes or stale BVH handles.

## Runtime validation

The source build verifies the implementation, but it still requires a two-peer
rollback-heavy push-contact test. Compare the first push callback and resulting
fighter positions against the non-rollback peer. If a divergence remains, the next
step is deep capture of the specific external fighter `SbcObject` records and BVH
leaf allocations, not broader pointer restoration.

## Dragon reference audit

`/media/god/tempData/Lostplanet/dragon2012mar.native.json` is a PS3 Dragon's
Dogma build from the same MT Framework collision lineage. It does not contain
debug method records for the engine's full `sCollision` implementation, but it
does contain complete type layouts, vtables, mangled callback signatures, and
source names from `sCollision.h`. Those records correct several earlier UMVC3
ownership assumptions.

### Fighter collision nodes

The shared character methods access collision-object pointers at `+0x4200`,
`+0x4E10`, `+0x5520`, and `+0x5C30`. `cChr__cChr` confirms those same offsets
from the complete `cChr` base stored in the team list: the first pointer store
is encoded as `this + 0x4168 + 0x98`, which is `this + 0x4200`. These are not
instances of the singleton's inline `sCollision::SbcObject` manager.

The exact derived type remains uncertain. Dragon reports `sCollision::Node` as
`0xA8`, while UMVC3's move-state helper writes at object `+0xB0`; this requires
either a larger derived node, an incomplete recovered layout, or a corrected
owner for that helper. UMVC3's DTI registration evidence gives `0xE0` for the
runtime object currently captured by rollback, but it should be described as a
collision-node-like object until its complete layout is recovered.

The fighter push path establishes the role of the fourth pointer directly:

- `uCharacter__setup_base` resets all four nodes' move state.
- `uCharacter__movePost` queues the first collision-object pointer with the hit callback.
- `uZeroSh__actionTransition_B9D0` queues the fourth pointer with
  `uCharacter__applyCollisionPushCorrection`.
- `uCharacter__applyCollisionPushCorrection` writes node move state `0x20`,
  reads the contact normal/depth, and accumulates the resulting correction into
  the owning character.

The team-list pointer and constructor `this` use the same complete `cChr` base.
A proposed `-0x540` adjustment to `+0x3CC0`, `+0x48D0`, `+0x4FE0`, and `+0x56F0`
is incorrect: constructor arithmetic places the first pointer at `+0x4200`, and
runtime validation read scalar `0x43A00000` at `+0x3CC0`. Rollback therefore
uses the four constructor-confirmed offsets directly.

### Reference callback signature

Dragon exports the full object-contact callback shape through
`cObjCollision::callbackContactObjHit` and `callbackContactObjAdj`:

```text
(CALLBACK_MODE, sCollision::Node *thisNode, sCollision::Node *otherNode,
 MtContact *contact, uint param, sCollision::TriangleInfo *triangle,
 uint thisGeomId, uint otherGeomId, bool hit)
```

UMVC3 binds the owner separately when it enqueues a node, so the decompiler's
six visible parameters for `uCharacter__applyCollisionPushCorrection` are the
bound owner followed by the first callback arguments. The previous
`sCollision::SbcObject *` parameter type was therefore incorrect.

### State-bearing method families audited

The UMVC3 methods associated with the Dragon `sCollision` types divide into the
following rollback-relevant state owners:

| Method family | State read or mutated | Current coverage |
|---|---|---|
| `sCollision__move`, `drainNodeLists` | Singleton flags and six deferred cluster rows | Inline singleton image |
| `cSbcArrayBP` registration/refit/remove methods | Inline BVH headers, external `Sbc`/`Node` records, external BVH nodes | Headers only; fighter nodes now captured |
| `cSbcObject` dirty/merge/reactivate/retire methods | Manager arrays, external registration records, external BVH nodes | Headers/counts only |
| `cSbcMoveMatrix` enqueue/post-move methods | Frame cursor, per-thread NodeLists, callback records, particle-manager arrays | Cursor/headers only |
| `cDynamicBVH` insert/remove/refit methods | Root, leaf count, parent/children, bounds, reserved/free-node order | Inline root/header pointers only |
| `cBvBoxFlag` update methods | Per-node current/previous matrices and transformed bounds | Captured for fighter nodes |
| `Node` callback methods | Move state, cached transforms, owner/callback/contact metadata | Captured for four fighter nodes |
| `SbcInfo` query methods | Temporary hit records and pointers into resources/nodes | Recomputed or stack-local |
| `ScrCollisionInfo` query methods | Temporary traversal/contact output | Recomputed or stack-local |
| clone-geometry and reserve-cluster methods | Inline geometry and deferred operation records | Inline singleton image |

## Character push determinism audit

The character push routine is `uCollisionNode_processCharacterPush` at
`0x140725B70`. It is called from `uCnsGroup__stepSolver`, not directly from the
fighter callback. The relevant solver order is:

1. `uCollisionNode_updateWorldTransform` updates active node transforms.
2. `uCollisionBody_integrate` copies current positions to previous-position
   fields and applies integration state.
3. `uCollisionNode_processGeneralCollision` may adjust body positions.
4. `uCollisionNode_processCharacterPush` performs character separation.
5. The group may repeat collision passes according to iteration counters.

`uCollisionNode_processCharacterPush` reads the node body count at `+0x14` and
the body pointer array at `+0x78`. It iterates bodies from index 2 through
`bodyCount - 1`, skips candidates whose active radius at `+0x18` is not
positive, and selects sphere (`+0x16 == 1`) or capsule (`+0x16 == 2`) handling.

The sphere and capsule resolvers use these per-body fields:

| Offset | Role | Access |
|---|---|---|
| `+0x12` | immovable/static flag | Read; prevents position writes when nonzero |
| `+0x16` | shape kind | Read; selects sphere/capsule path |
| `+0x18` | radius/active extent | Read |
| `+0x60` | mass/weight | Read for weighted separation |
| `+0xB0..+0xB8` | world position | Read/write; primary push result |
| `+0xC0..+0xC8` | displacement/normal state | Read by later spring/orientation paths |
| `+0x160..+0x16C` | previous body position | Written by integration; consumed later |
| `+0x1B4` | contact flags | Bit 0 is set on contact |

The collision context supplies a scale at `context + 0x70 + 0x360`; each
candidate radius is multiplied by this value before intersection testing. On
contact, both bodies receive flag bit `+0x1B4`, then
`uCollisionPair__resolveByMassWeightedPush` applies the correction.

The mass-weighted resolver adjusts positions using each body's `+0x60` weight
unless `+0x12` marks it immovable. When a contact point is supplied, the weight
split also depends on squared distance from that point to each body's position.
Therefore deterministic push requires matching body positions, radii, mass,
static flags, contact point, and the context scale before the resolver runs.

This geometric separation path is distinct from
`uCharacter__applyCollisionPushCorrection`. The callback writes collision-node
move state `+0xB0 = 0x20` and accumulates the selected correction into the
character's execution state. Restoring only the callback wrapper does not
restore the geometric inputs or the solver iteration state.

### Snapshot coverage implemented

Rollback now captures the four `0xE0` collision wrappers referenced by each
`cChr` at `+0x4200`, `+0x4E10`, `+0x5520`, and `+0x5C30`. Restore copies the
wrapper's scalar frame state, including the move state at `+0xB0`, while
retaining live vtable, owner, callback, child-array, collision-context, child
list, and user-data pointers. Restore identifies these regions by matching them
against the current character-owned pointers rather than treating every
`0xE0` snapshot region as a collision wrapper.

The node child primitives are external reference-managed `MtArray` entries, not
the similarly named primitive arrays embedded in `cChr`. The node's live child
array and geometry ownership fields are therefore retained. For the fourth
character-push node, rollback separately captures the pointer-free value payload
at `MtGeom+0x20` for known fixed-size primitive types. Restore writes a saved
payload only when the same allocation and payload size remain reachable through
the current node, preserving the live vtable, type, reference count, geometry
pointer, and ownership flags.

`nCollision__testNodeGeometryPairs` (`0x1405AB9A0`) establishes this boundary:
child bounds at geometry
`+0x20..+0x38` prune candidate pairs, then the selected primitive intersection
function receives geometry `+0x10` directly. Virtual `+0x58` recomputes the
cached bounds from that `MtGeom` before the node aggregates its AABB. The shape
payload is therefore authoritative rewindable state; the cached bounds are
derived and need not be restored separately.

This does not deep-copy heap targets behind `sCollision` manager pointers. If a
fresh synchronized test still splits at the first contact callback, the likely
remaining carrier is a future-timeline external BVH/queue record used to build
the callback's contact normal and penetration depth, rather than one of the
four character wrapper scalars.

### Remaining deep-state gap

Capturing the fighter nodes closes the most direct omission, but the singleton
restore still preserves pointers without restoring their targets. The remaining
state-bearing targets are:

- Every `cDynamicBVHCollision::Node` (`0x40` bytes in UMVC3), including leaf
  bounds and parent/child links.
- The reserved-node pool, free-node pointer array, and no-use-node array of each
  dynamic BVH.
- Heap buffers behind the `SbcObject` active, dirty, merge, reactivate, and
  retire arrays.
- Heap buffers behind `cSbcMoveMatrix` per-thread `NodeList` arrays and their
  queued callback records.
- External `Sbc`, `Sbc::Parts`, and registration records reached from the
  singleton managers.

These are gameplay state, not merely ownership metadata. The current generic
pointer-preservation rule restores historical counts and flags around live
future-timeline buffers, which can produce a logically mixed collision graph.
If the fighter-node capture does not eliminate the frame-248 push split, the
next fix should deep-capture the active arrays and BVH node pools while retaining
their live allocation addresses, then restore contents and rebuild all internal
links to those allocations.

## 2026-07-18 slot-order finding

The `044007_host` / `044006_client` capture does not first diverge in collision
position. It exposes a character identity and team-slot problem after the host
rolls back `[431, 433)`.

The peers already have different Team 1 linked-list slot order from match startup:

- Host: slot 0 = mid 0, slot 1 = mid 1, slot 2 = mid 2.
- Client: slot 0 = mid 2, slot 1 = mid 1, slot 2 = mid 0.

Consequently, comparison by `(team, slot)` reports a persistent slot swap from
frame 10. Comparison by stable `(team, mid)` identity remains equal through frame
441, so this initial ordering mismatch does not immediately alter gameplay.

At frame 442, after the host-only rollback, Team 1 mids 0 and 1 disagree in `fl8`
and per-character RNG while position, action, and input still agree. One peer
advances mid 0's RNG while the other advances mid 1's RNG. This is an update or
activation ownership inversion, not an initial push-position difference.

Character snapshots currently walk the intrusive team list and restore bytes to
the saved addresses, while `cChrTeam` contains current/next character pointers and
index-based transition state. Any logic that treats traversal slot as character
identity can therefore select a different fighter on each peer. Future rollback
work must validate and, where necessary, canonicalize:

- `cChrTeam::mpCurrentChr` and related current/next/previous pointers.
- Member indexes used by tag, assist, and team transitions.
- Character-node links and traversal order.
- Slot-index-to-character lookup tables.

Until this identity/activation mismatch is fixed, the later position-only split at
frame 717 cannot cleanly prove a remaining `sCollision` snapshot gap because the
timeline has already diverged at frame 442.
