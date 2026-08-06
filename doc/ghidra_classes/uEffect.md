# uEffect

**Ghidra type:** `uEffect`
**Known size:** `0x250` (592 bytes)
**Base class:** `uBaseEffect` (via `SharedVirtual`)

Core effect base class. Instances are polymorphic and carry a large packed
field region. Most scalar properties are registered in
`uEffect__createProperty` (0x1405e2f00) with explicit property names that map
1:1 to the getter/setter methods in this reference.

Note: Ghidra currently models the type as `byte[592]`, so decompilations
address fields via `this->field0_0x0 + <offset>`, where `field0_0x0` is the
object base pointer at offset 0. All offsets below are class-relative.

## Confirmed Fields

| Offset | Size | Type | Name | Access | Evidence |
|---:|---:|---|---|---|---|
| `0x48` | `4` | `uint16_t/uint32_t` | `mParentNo` | read | Registered in `uEffect__createProperty` as "mParentNo" (+0x48). |
| `0x50` | `12` | `MtVector3` | `mPos` | read/write | "mPos" property registered at +0x50; world matrix region begins +0xc0. |
| `0x60` | `16` | `quaternion` | `mQuat` | read/write | "mQuat" property registered at +0x60. |
| `0x70` | `12` | `MtVector3` | `mScale` | read/write | "mScale" property registered at +0x70. |
| `0x100` | `4` | `uint32_t` (flags) | `mModeFlags` | read/write | Bit 0x17 = ParentSymmetryMode; 0x12 = ConstUpdateMode; 0xe = JointFixMode; 0xf = JointUpdateFlag; 0x16 = SeStopMode; 0x11 = UGInvalidMode; also bit 0x100000 touched by `uEffect__restart`; +0x1e4..0x1ef set to 0xff by `doClear`. |
| `0x12c` | `4` | `uint32_t` | `mRelationTypeBits` | read | `uEffect__getRelationType` reads `+0x12c >> 0x1c` (property "mRelationType"). |
| `0x12e` | `1` | `byte` | `mExclusionTrait` | read | `uEffect__getExclusionTrait` reads byte +0x12e (property "mExclusionTrait"). |
| `0x12f` | `1` | `byte` | `mAxisTypeBits` | read | `uEffect__getAxisType` reads `+0x12f & 0xf` (property "mAxisType"). |
| `0x130` | `4` | `uint32_t` | `mVolumeBits` | read/write | Nibbles: bits 0-3 = mEndType, 8-11 = mParticleVolume, 12-15 = mFilterVolume, 20-23 = mBoundaryType; bit 0x4000000 = parent-matrix valid (see `getGeneratorSubParentWorldMatrix`). |
| `0x132` | `2` | `ushort` | `mLifeFrameVolumeBits` | read | `uEffect__getLifeFrameVolume` reads `+0x132 & 0xf` (property "mLifeFrameVolume"). |
| `0x180` | `12` | `MtVector3` | `mOfs` | read/write | "mOfs" getter `uShot__copyVec3AtOff384` reads +0x180; setter `uEffect__setOffset` (0x1400db2b0) writes it. |
| `0x190` | `12` | `MtVector3` | `mDir` | read | "mDir" getter `uEffect__getDir` (0x1405e5180) copies +0x190/0x194/0x198. |
| `0x1a0` | `4` | `float` | `mTransparency` | read | `uEffect__getTransparency` reads +0x1a0 (property "mTransparency"). |
| `0x1c0` | `12` | `MtVector3` | `mParticle3DScale` | read/write | Getter 0x1405e5360 / setter 0x140107c60 (property "mParticle3DScale"). |
| `0x1d0` | `4` | `float` | `mParticleScale` | read/write | Getter 0x1405e5390 / setter 0x140107c70 (property "mParticleScale"). |
| `0x1d6` | `2` | `ushort` | `mLifeFrame` | read/write | `uEffect__getLifeFrame` reads +0x1d6; `uEffect__restart` copies +0x1d6 into +0x1d4 (property "mLifeFrame"). |
| `0x1d8` | `2` | `ushort` | `mWaitFrame` | read | `uEffect__getWaitFrame` reads +0x1d8 (property "mWaitFrame"). |
| `0x1da` | `1` | `byte` | `mLightPriority` | read | `uEffect__getLightPriority` reads +0x1da (property "mLightPriority"). |
| `0x1dc` | `4` | `float` | `mGravityCoef` | read | "mGravityCoef" property registered at +0x1dc. |
| `0x1e8` | `4` | `uint32_t` | `mGroupFlag` | read/write | "Group" getter `uEffect__getGroupFlag` bit-tests +0x1e8; "mGroupFlag" property points at +0x1e8; cleared by `doClear`. |
| `0x1ec` | `4` | `uint32_t` | `mMaterialFlag` | read/write | "Material" getter `uEffect__getMaterialFlag` bit-tests +0x1ec; "mMaterialFlag" property points at +0x1ec; cleared by `doClear`. |
| `0x200` | `2` | `ushort` | `mChildLifeFrame` | read | `uEffect__getChildLifeFrame` reads +0x200 (property "mChildLifeFrame"). |
| `0x202` | `2` | `ushort` | `mChildWaitFrame` | read | `uEffect__getChildWaitFrame` reads +0x202 (property "mChildWaitFrame"). |
| `0x204` | `2` | `ushort` | `mGeneratorNum` | read | `uEffect__getGeneratorNum` reads +0x204 (property "mGeneratorNum"). |
| `0x206` | `2` | `ushort` | `mGeneratorMoveNum` | read | `uEffect__getGeneratorMoveNum` reads +0x206 (property "mGeneratorMoveNum"); `doRestart` copies +0x204 into +0x206. |
| `0x208` | `2` | `ushort` | `mJointNum` | read | `doRestart` iterates +0x208 entries in the +0x220 array. |
| `0x20a` | `2` | `ushort` | `mJointMoveNum` | read/write | `doRestart` copies +0x208 into +0x20a. |
| `0x210` | `4` | `uint32_t` | `mGeneratorBuffSize` | read | `uEffect__getGeneratorBuffSize` reads +0x210 (property "mGeneratorBuffSize"). |
| `0x218` | `8` | `ptr` | `mGeneratorListHead` | read/write | Linked list of generator nodes (node+0x18 = next, node+0x24 = id, node+0x28 = matrix). Traversed by `getWorldMatrix`, `doKeepHoldOff`, `doRestart`. |
| `0x220` | `8` | `ptr` | `mJointArray` | read | `doRestart` iterates +0x208 entries of stride 0x150 starting at +0x220. |
| `0x230` | `4` | `float` | `mWaitFrameCoef` | read | `uEffect__getWaitFrameCoef` reads +0x230 (property "mWaitFrameCoef"). |
| `0x23c` | `4` | `int32_t` | `mDrawDepthBias` | read | "mDrawDepthBias" property registered at +0x23c. |

## Candidate Fields

| Offset | Size | Proposed type/name | Confidence | Evidence needed |
|---:|---:|---|---|---|
| `0x40` | `8` | `uEffect *mpParent` | medium | `getGeneratorSubParentWorldMatrix` dereferences +0x40 as parent effect. |
| `0x102` | `1` | `bool mChainResetFlag` | high | `uEffect__getChainResetFlag` reads `+0x102 & 1` (property "ChainResetFlag"). |
| `0x114` | `4` | `float mDeltaTimeRate` | medium | "mDeltaTimeRate" property registered at +0x114. |
| `0x11c` | `4` | `float mTimeInterpolationRate` | medium | "mTimeInterpolationRate" property registered at +0x11c. |
| `0x1e4` | `1` | `byte` (group flag spill) | medium | `doClear` sets +0x1e4..0x1ef to 0xff. |
| `0x238` | `4` | `uint32_t` | medium | `doKeepHoldOff` tests `(+0x238 & 0xf000) == 0x2000`. |

## Relevant Methods

| Address | Name | Relationship |
|---|---|---|
| `0x1405e2f00` | `uEffect__createProperty` | Registers every property listed above; source of the field names. |
| `0x1405e5470` | `uEffect__getWorldMatrix` | Uses +0x204 count, +0x218 list; default matrix +0xc0. |
| `0x1405e5210` | `uEffect__getGeneratorSubParentWorldMatrix` | Uses +0x40 parent, +0x130 flag 0x4000000, +0x100 bit 0x17. |
| `0x1405e4cd0` | `uEffect__doClear` | Clears +0x1e4..0x1ef, +0x1db. |
| `0x1405e4d60` | `uEffect__doKeepHoldOff` | Iterates +0x218 list; checks +0x238. |
| `0x1405e4db0` | `uEffect__doRestart` | Iterates +0x208/+0x220 joint array; copies +0x204->+0x206, +0x208->+0x20a. |
| `0x1405e6ab0` | `uEffect__restart` | Sets bit 0x100000 in +0x100; copies +0x1d6 into +0x1d4. |
| `0x1405e4fd0` | `uEffect__getDefaultGravityVector` | Writes a default quaternion into +0x0..0xf. |

## Open Questions

- Exact meaning of the packed nibble fields in `+0x130` and their enums.
- Whether `+0x1e4..0x1ef` byte region is an 8-bit-per-group flag array or
  byte-wide counterpart of the +0x1e8/+0x1ec dwords.
- Whether `mpParent` (+0x40) is owned or borrowed.
- Confirm `mDir`/`mParticle3DScale` getters return vec4 (w=0) vs vec3.
