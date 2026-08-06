# cHitPrimSphere

**Ghidra type:** `cHitPrimSphere`
**Known size:** `0x70` (112 bytes, DTI-registered)
**Base class:** `cHitPrim` (via `cHitPrim__baseCtor @ 0x14000da20`; base ctor passes primKind=5)

Collision sphere primitive. Embeds an `MtGeomSphere` geometry object at
complete-object offset `+0x20`. Virtual family shared with the sibling
`cHitPrimCapsule`. A broad virtual table (slots 10..60) implements narrow-phase
`resolve*` / `test*` / `closestPoint*` methods plus geometry accessors and a
factory.

## Confirmed Fields

| Offset | Size | Type | Name | Access | Evidence |
|---:|---:|---|---|---|---|
| `0x00` | `8` | `void **` | vtable | read/write | `cHitPrim__baseCtor` installs `cHitPrim__vtable`; `cHitPrimSphere__ctor @ 0x14000dd40` installs `cHitPrimSphere__vtable`. |
| `0x10` | `4` | `uint32_t` | mPrimKind | read/write | Written with `g_sCollision__defaultPrimKind` by `cHitPrim__baseCtor @ 0x14000da20`; read by `cHitPrimSphere__getPrimKind @ 0x14000f350` (vfn54); written by `cHitPrimSphere__setPrimKind @ 0x14000fc30` (vfn53). |
| `0x20` | `48` | `MtGeomSphere` | mGeom (embedded) | read/write | All geometry virtuals operate on `param_1+0x20`: `translate @ 0x14000e7e0`, `getCenter @ 0x14000f2a0`, `getSupportPoint @ 0x14000f530`. |
| `0x30` | `4` | `int` | mFlag | read/write | Struct field; zeroed by `cHitPrimSphere__ctor @ 0x14000dd40`. |
| `0x38` | `8` | `void *` | mpParent | read/write | Struct field; zeroed by ctor. |
| `0x40` | `4` | `int` | mParentNo | read/write | Struct field; initialized to `-1` by ctor (mirrors capsule `mParentNo0/1` sentinel). |
| `0x44` | `4` | `float` | mSizeRate | read/write | Struct field; initialized to `1.0` by ctor. |
| `0x50` | `16` | `MtVector3` | mOfs (local offset) | read/write | Struct field; zeroed via `MtVector3__op_assign(DAT_140d76030)` by ctor. |

## Candidate Fields

| Offset | Size | Proposed type/name | Confidence | Evidence needed |
|---:|---:|---|---|---|
| `0x10` as float in world-support-point virtuals | `4` | length/scale source read by `computeWorldSupportPoint_*` virtuals | medium | Confirm whether `mPrimKind` doubles as a length in `computeWorldSupportPoint_1404c0890/0960`, which call `this->vfn54()` for a scale. |

## Relevant Methods

| Address | Name | Relationship |
|---|---|---|
| `0x14000da50` | `cHitPrimSphere__base_ctor` | Installs parent vtable `0x140A6AA60`, calls `cHitPrim__baseCtor(5)`. |
| `0x14000dd40` | `cHitPrimSphere__ctor` | Installs `cHitPrimSphere__vtable`, inits mParentNo/-1, mSizeRate=1.0, mFlag=0, mpParent=0, mOfs. |
| `0x14000eae0` | `cHitPrimSphere__copyGeometry` | vfn11; copies embedded geometry (16-byte center copy). |
| `0x14000e7e0` | `cHitPrimSphere__translate` | vfn56; translates embedded geometry. |
| `0x14000f2a0` | `cHitPrimSphere__getCenter` | vfn52; `MtGeomSphere__getCenter`. |
| `0x14000f530` | `cHitPrimSphere__getSupportPoint` | vfn50; `MtGeomSphere__getSupportPoint`. |
| `0x14000f350` | `cHitPrimSphere__getPrimKind` | vfn54; reads `mPrimKind` at `+0x10`. |
| `0x14000fc30` | `cHitPrimSphere__setPrimKind` | vfn53; writes `mPrimKind` at `+0x10`. |
| `0x14000fbd0` | `cHitPrimSphere__createCapsule` | vfn60; factory allocating a `cHitPrimCapsule`. |

## Virtual Slots (recovered)

Slots 24..38 are narrow-phase `resolve*`/`closestPoint*` methods (plane, OBB,
Y-plane, ray, line segment, sphere, point, triangle). Slots 12..23 are boolean
`test*`/`overlap*` methods plus `dispatchTest*` shims that rebase `this` to the
embedded geometry at `+0x20`. Slots 39..47 are swept-resolve variants. See the
individual renamed functions for details.

## Open Questions

- The struct field `byte[48]` at `+0x00` in Ghidra is misleading: the embedded
  `MtGeomSphere` lives at `+0x20`, and the preceding `0x20` bytes are the
  `cHitPrim` base (vtable + `mPrimKind`). The struct should be corrected so
  `+0x20` is typed as `MtGeomSphere`.
- `cHitPrimSphere_vfn60 @ 0x14000fbd0` creates a `cHitPrimCapsule` while the
  capsule's vfn58 `@ 0x14000fc00` creates a sphere — confirm whether this is a
  shared base-class clone/factory virtual or a cross-type conversion helper.
- `computeWorldSupportPoint_1404c0890/0960` and `forwardVirtual_1407cda60` are
  shared base virtuals across 12 primitive vtables; exact per-class semantics
  of the delegated virtuals (slots 50/51/54) need the owner vtable layout.
