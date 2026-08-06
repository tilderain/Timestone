# cHitPrimCapsule

**Ghidra type:** `cHitPrimCapsule`
**Known size:** `0xB0` (176 bytes, DTI-registered)
**Base class:** `cHitPrim` / embeds `MtGeomCapsule` geometry at `+0x20`

Capsule-shaped hit/hurt/push collision primitive. Its vtable mirrors the
`MtGeomCapsule`/`MtGeomSphere` virtual table layout; slot indices match the
`cHitPrimSphere` sibling exactly. Slot semantics were recovered by comparing each
slot against the same-index `cHitPrimSphere` implementation and the underlying
`MtCollision__*` / `MtMath__*` resolvers.

The embedded `MtGeomCapsule` geometry at `+0x20` overlaps the documented
`mWorldEndpoint0`/`mWorldEndpoint1`/`mRadius` fields. The resolve virtuals read
endpoints at geometry `+0x0`/`+0x10` and radius at geometry `+0x20`.

## Confirmed Fields

| Offset | Size | Type | Name | Access | Evidence |
|---:|---:|---|---|---|---|
| `0x20` | `16` | `float[4]` | `mWorldEndpoint0` (capsule endpoint0) | read/write | `MtGeomCapsule__translate` adds delta to both endpoints; `resolveVs*` family reads endpoint0 here. |
| `0x30` | `16` | `float[4]` | `mWorldEndpoint1` (capsule endpoint1) | read/write | `MtGeomCapsule__translate`/`copy`; `resolveVs*` family reads endpoint1 here. |
| `0x40` | `4` | `float` | `mRadius` | read/write | `MtGeomCapsule__copy` stores radius here; all `resolveVs*` compare distance to radius at this offset. |
| `0x50` | `4` | `uint` | `mFlag` | read/write | ctor sets 0; shared primitive flag field. |
| `0x58` | `8` | `pointer64` | `mpParent0` | write | ctor/use. |
| `0x60` | `8` | `pointer64` | `mpParent1` | write | ctor/use. |
| `0x68` | `4` | `int` | `mParentNo0` | read/write | ctor sets -1. |
| `0x6C` | `4` | `int` | `mParentNo1` | read/write | ctor sets -1. |
| `0x70` | `4` | `float` | `mSizeRate` | read/write | ctor sets 1.0. |

## Candidate Fields

| Offset | Size | Proposed type/name | Confidence | Evidence needed |
|---:|---:|---|---|---|
| `0x74` | `4` | `uint mCapsuleFlags` | low | Layout suggests flags; purpose unresolved. |
| `0x80` | `16` | `float[4] mOfs0` | low | Likely per-parent offset vector; matches sphere `mOfs` pattern. |
| `0x90` | `16` | `float[4] mOfs1` | low | Likely per-parent offset vector. |

## Relevant Methods

| Address | Name | Relationship |
|---|---|---|
| `0x14000e7d0` | `cHitPrimCapsule__translate` | vfn56; adds delta to both world endpoints. |
| `0x14000ead0` | `cHitPrimCapsule__copyGeometry` | vfn11; copies embedded `MtGeomCapsule` from source primitive. |
| `0x14000f030` | `cHitPrimCapsule__resolveVsObb` | vfn28; `lineVsObbResolve`. |
| `0x14000f040` | `cHitPrimCapsule__resolveVsCapsule` | vfn29; `capsuleVsCapsuleResolve`. |
| `0x14000f050` | `cHitPrimCapsule__resolveVsRay` | vfn33; `lineRayClosestPoint`. |
| `0x14000f060` | `cHitPrimCapsule__resolveSweptVsLineSegment` | vfn31; `capsuleSweptResolve`. |
| `0x14000f070` | `cHitPrimCapsule__resolveVsObbInvTransform` | vfn27; `transformFrustumByInvMatrix`. |
| `0x14000f080` | `cHitPrimCapsule__resolveVsPlane` | vfn26. |
| `0x14000f090` | `cHitPrimCapsule__resolveVsSegment` | vfn32; `lineSegmentVsLineClosestPoints`. |
| `0x14000f0a0` | `cHitPrimCapsule__resolveVsSphere` | vfn30; `lineSegmentClosestPoint`. |
| `0x14000f0b0` | `cHitPrimCapsule__resolveVsAABB` | vfn24; `capsuleVsAABBResolve`. |
| `0x14000f0c0` | `cHitPrimCapsule__resolveVsPoint` | vfn34; `capsuleVsPointResolve`. |
| `0x14000f180` | `cHitPrimCapsule__resolveVsObbSwept` | vfn36. |
| `0x14000f190` | `cHitPrimCapsule__resolveVsCapsuleCollision` | vfn37; enhanced capsule-vs-capsule resolve. |
| `0x14000f1a0` | `cHitPrimCapsule__resolveObbCollision` | vfn35. |
| `0x14000f1b0` | `cHitPrimCapsule__obbVsObbResolve` | vfn38; generic `obbVsObbResolve_undec`. |
| `0x14000f280` | `cHitPrimCapsule__getCenter` | vfn52. |
| `0x14000f510` | `cHitPrimCapsule__getSupportPoint` | vfn50. |
| `0x14000f580` | `cHitPrimCapsule__testVsSweptSegment` | vfn12 dispatch target. |
| `0x14000f590` | `cHitPrimCapsule__testVsCapsule` | vfn13 dispatch target. |
| `0x14000f680` | `cHitPrimCapsule__resolveSweptVsSegment` | vfn46. |
| `0x14000f6b0` | `cHitPrimCapsule__resolveSweptVsObbOrSphere` | vfn47. |
| `0x14000f760` | `cHitPrimCapsule__resolveSweptVsObb` | vfn43. |
| `0x14000f790` | `cHitPrimCapsule__resolveSweptVsCapsule` | vfn44. |
| `0x14000f7c0` | `cHitPrimCapsule__capsuleSweptVsObb` | vfn42. |
| `0x14000f7f0` | `cHitPrimCapsule__resolveSphereSweptVsObb` | vfn45. |
| `0x14000f820` | `cHitPrimCapsule__resolveVsTriangle` | vfn39. |
| `0x14000f9a0` | `cHitPrimCapsule__testPointWithinRadius` | vfn17. |
| `0x14000f9b0` | `cHitPrimCapsule__testCapsuleOverlap` | vfn18. |
| `0x14000f9c0` | `cHitPrimCapsule__dispatchTestVsSweptSegment` | vfn12 slot. |
| `0x14000f9d0` | `cHitPrimCapsule__testVsPlane` | vfn22. |
| `0x14000f9e0` | `cHitPrimCapsule__testPlaneProjection` | vfn20. |
| `0x14000f9f0` | `cHitPrimCapsule__testVsObb` | vfn16. |
| `0x14000fa00` | `cHitPrimCapsule__overlapAxisTest` | vfn15. |
| `0x14000fa10` | `cHitPrimCapsule__testVsRay` | vfn21. |
| `0x14000fa20` | `cHitPrimCapsule__testVsSphere` | vfn19; `planeSphereVsSphere`. |
| `0x14000fa30` | `cHitPrimCapsule__dispatchTestVsCapsule` | vfn13 slot. |
| `0x14000fa40` | `cHitPrimCapsule__containsPoint` | vfn23; `capsuleVsPointFast`. |
| `0x14000fc70` | `cHitPrimCapsule__scale` | vfn55; `MtGeomCapsule__scale`. |
| `0x14000fd10` | `cHitPrimCapsule__updateWorldFromMatrix` | vfn10; transforms source geometry by matrix and copies as world bounds. |
| `0x14000fc90` | `cHitPrimCapsule__computeWorldBounds` | helper; world bounds from matrix (not a root). |
| `0x14000fd60` | `cHitPrimCapsule__updateWorldEndpointsFromMatrices` | helper (not a root). |
| `0x14000ffd0` | `cHitPrimCapsule__updateWorldEndpointsFromParents` | helper (not a root). |

## Open Questions

- Capsule vtable slots 37/38 do not match the same-index sphere slots exactly
  (`resolveVsCapsuleCollision` vs sphere `obbVsObbResolve`; `obbVsObbResolve` vs
  sphere `sphereVsSphereResolve`). The `MtGeomCapsule` and `MtGeomSphere` derived
  vtables appear to cross-over at these two slots; confirm the base-class virtual
  layout before relying on slot-index equivalence.
- Slots 40/41, 48/49, 51, 53, 57 are shared stub/base functions (`0x1407e4d40`,
  `0x1404c0960`, `0x1404c0890`, `0x1407cda60`, `0x140453d90`) not owned by this
  shard.
- Whether `mOfs0`/`mOfs1`/`mCapsuleFlags` participate in rollback snapshots is
  unresolved (see `doc/scollision_rollback_snapshot.md` geometry boundary).
