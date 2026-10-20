# uCharacter Embedded Member Audit

Date: 2026-10-20. **Partial native verification; not implementation approval.**

## Scope and Evidence

Read in full, without codebase search: `doc/README.md`, `doc/ccnshelper.md`,
`UMVC3Hook/code/rollback_character_restore.cpp` and
`UMVC3Hook/code/rollback_restore.cpp`. No production changes were made.

Ghidra: `instances_list`, then `instances_use(8192)`, project
`ichneumon_sub / umvc3.exe.unpacked.exe`. Addresses below use preferred image
base `0x140000000`.

Native evidence obtained in this session is **existing datatype layouts and
function listings**, not newly verified instruction semantics. Script execution
was disabled. Subsequent decompilation requests, a single retry with an explicit
20-second timeout, and a disassembly request all timed out. Consequently this
document does not claim the requested constructor/consumer audit is complete.
Existing names, including `mCapsuleFlags`, remain hypotheses pending instruction
verification. The unresolved entries are intentional, not endorsements to copy
unknown bytes.

All ranges below are inclusive hexadecimal offsets relative to the named object.
Every returned field and gap is included. Repeated arrays use an explicit index
formula; the disposition applies independently to every element. Nested base
types whose layouts could not be retrieved are explicitly marked unresolved,
rather than inventing their members.

Disposition vocabulary:

- **Restore**: already explicitly copied in the two inspected source files.
- **Reference**: identity/ownership requires validation or reconstruction, not raw copy.
- **Keep**: preserve native identity such as a vtable.
- **Candidate**: omitted typed scalar; native semantics still need confirmation.
- **Unknown**: do not add copying without constructor AND consumer evidence.

## Source Coverage

| Evidence | Exact source location | Observation |
|---|---|---|
| S1 | `rollback_character_restore.cpp:63-69` | Eye `+08..6B` copied early; material controller excluded; constraint `+68..77`, `+80..8F` copied early. |
| S2 | `rollback_character_restore.cpp:109-115` | Same inherited constraint ranges copied late; no eye range reapplied here. |
| S3 | `rollback_restore.cpp:1178-1234` | Constraint derived scalar ranges and three validated references. |
| S4 | `rollback_restore.cpp:1646-1647,1983-1984` | Derived constraint repair early and late; late loop is restricted to matching team characters. |
| S5 | `rollback_restore.cpp:1790-1834` | Capsule and sphere explicit early ranges. |
| S6 | `rollback_restore.cpp:1988-2019` | Capsule ranges repeated late; sphere loop has no writes. |
| S7 | `rollback_restore.cpp:123-154,1876,2046` | Saved collision-parent references applied early and late; weapon resolution checks current array/count/index. Capture completeness was not inspected. |

There is no broad character fallback that can be assumed to cover these omissions:
the character branch calls the explicit kernel at lines 1570-1591 and reports
incomplete coverage. Comments referring to a broad pointer guard are historical
context, not evidence of another current copy.

## cFollowEyes

Character base `0x3630`, size `0x5C0`, ending before `0x3BF0`.
Datatype returned by `structs_get(cFollowEyes)`.

| Relative bytes | Member | Disposition / evidence |
|---|---|---|
| 00..07 | MtObject base/vtable | Keep; no explicit restore. |
| 08 | mUseFollowEye | Restore early, S1. |
| 09 | mUseLimitEye | Restore early, S1. |
| 0A | mIsFollowEye | Restore early, S1. |
| 0B | mIsForceStopEye | Restore early, S1. |
| 0C..13 | mLEyeAngH, RangeF | Restore early; range endpoints' names not retrieved. |
| 14..1B | mLEyeAngV, RangeF | Restore early. |
| 1C..1F | mLMatId | Restore early; verify binding stability before adding controller restoration. |
| 20..27 | mLEyeLimU, RangeF | Restore early. |
| 28..2F | mLEyeLimV, RangeF | Restore early. |
| 30..33 | mLAngleY | Restore early. |
| 34..37 | mLAngleZ | Restore early. |
| 38..3F | mREyeAngH, RangeF | Restore early. |
| 40..47 | mREyeAngV, RangeF | Restore early. |
| 48..4B | mRMatId | Restore early; same binding caveat. |
| 4C..53 | mREyeLimU, RangeF | Restore early. |
| 54..5B | mREyeLimV, RangeF | Restore early. |
| 5C..5F | mRAngleY | Restore early. |
| 60..63 | mRAngleZ | Restore early. |
| 64..67 | mEyeSpeed | Restore early. |
| 68..6B | mEyeLength | Restore early. |
| 6C..6F | unnamed byte[4] | Unknown; omitted. Do not call padding from layout alone. |
| 70..5BF | mEyeMatCtrl | Expanded below; no explicit restore in inspected files. |

### uMaterialControl

Character base `0x36A0`, size `0x550`.

| Relative bytes | Member | Disposition / evidence |
|---|---|---|
| 00..3F | cUnit base | Keep live under current policy; internal member expansion unresolved because base-layout query timed out. Do not bulk-copy lifecycle/list links. |
| 40..47 | mpTarget, uBaseModel pointer | Reference; preserve live until owner/binding protocol verified. Character `+36E0`. |
| 48 | mControlAllSlot | Candidate scalar, omitted. Character `+36E8`; target/material-slot relationship not verified. |
| 49..4F | unknown_49[7] | Unknown; omitted. |
| 50..54F | mControllers[32] | Each 0x28-byte Controller expanded below. |

### Controller and StringHandle

For every `i = 0..31`, Controller base is character `0x36F0 + 0x28*i`.
This enumerates all 32 records, including unused slots; no assumption is made
that all slots participate in eye tracking.

| Controller bytes | Member | Disposition / evidence |
|---|---|---|
| 00..07 | MtObject base/vtable | Keep. |
| 08..0F | mpParent, uMaterialControl pointer | Reference; expected embedded owner needs constructor confirmation. Reconstruct from live eye controller if confirmed, not snapshot address. |
| 10..13 | mControlID | Candidate binding selector; omitted. Not approved independently of target/slot/animation. |
| 14..17 | unknown_14[4] | Unknown; omitted. |
| 18..1F | mAnimation, StringHandle | Reference; one member `StringHandle::mpData` at handle `+00..07`. Ownership/refcount/interning unresolved; never treat handle as scalar. |
| 20..23 | mFrame | Candidate clock; omitted. Character `0x3710 + 0x28*i`. Must establish initialization, advancement, wrap/reset and animation binding before restoring. |
| 24..27 | mSlot | Candidate binding selector; omitted. Character `0x3714 + 0x28*i`. |

Eye-clock investigation is unresolved, not a finding of gameplay divergence.
The layout alone cannot establish whether `mFrame` is accumulated, overwritten
from the eye angles, or unused for these particular embedded controllers.

## uCnsLockChr and uConstraint

Two instances: character `0x3BF0` and `0x3D60`, each size `0x170`.
Ghidra flattens the first `0x90` bytes as `uConstraint`; the existing native note
describes a `uCnsShare` constructor. That inheritance representation discrepancy
does not authorize inventing another payload.

### Inherited uConstraint

| Relative bytes | Member | Disposition / evidence |
|---|---|---|
| 00..3F | cUnit base | Current policy leaves live. Internal members not enumerated natively in this session; layout query timed out. |
| 40..4F | mConstraint, uConstraint__cConstraint | Reference/Unknown; inline 0x10-byte subobject. Internal layout query timed out. No scalar approval. |
| 50..57 | mpPrevCns | Keep live intrusive topology. |
| 58..5F | mpNextCns | Keep live intrusive topology. |
| 60..67 | mpParentJnt | Reference; borrowed live joint identity, omitted. Joint/model replacement semantics unresolved. |
| 68..6B | mBlendWeight | Restore early/late, S1/S2. |
| 6C..6F | mID | Restore early/late. |
| 70..73 | mOrder | Restore early/late. |
| 74..77 | mPri | Restore early/late. |
| 78..7F | mpModel | Reference; retain live model identity. |
| 80..83 | mBlendSpeed | Restore early/late. |
| 84..87 | mBlend | Restore early/late. |
| 88 | mIgnoreRate | Restore early/late. |
| 89 | mEnable | Restore early/late. This is the datatype's enable location, not helper +90. |
| 8A..8F | unknown_8a[6] | Already copied by S1/S2; unknown semantics, not newly proven scalar padding. |

### Derived Members

| Relative bytes | Member | Disposition / evidence |
|---|---|---|
| 90 | mPause | Restore early/late, S3/S4. Conflicts with older note calling +90 mEnable. |
| 91..93 | unnamed byte[3] | Already copied; meaning unresolved. In particular, do not assume +91 is pause from the older note. |
| 94..97 | mJointNo | Restore early/late. |
| 98..9B | mLockAxis | Restore early/late. |
| 9C..9F | mUpAxis | Restore early/late. |
| A0..A3 | mTgtJointNo | Restore early/late. |
| A4..A7 | unknown_a4[4] | Already copied; semantics unresolved. |
| A8..AF | mpTgtUnit | Reference: S3 restores null or current team-list character; other saved identities leave live value. Broader native target domain unresolved. |
| B0..D7 | mLimStand | Expanded below. |
| D8..FF | mLimDown | Expanded below. |
| 100..107 | mpLimCurrent | Reference: S3 accepts either inline limit address only; saved null/other value leaves live pointer. Need native invariant before accepting null. |
| 108..10B | mInterFrame | Restore early/late. S3's comment is not an exact member naming reference. |
| 10C..10F | mLength | Restore early/late. |
| 110 | mDbgDraw | Restore early/late. |
| 111..113 | unknown_111[3] | Already copied; unresolved. |
| 114..117 | mAng | Restore early/late. |
| 118..11F | mpRefCnsLock | Reference: S3 restores null or either same-character helper only; unknown relationships remain live. |
| 120..123 | mSelfState | Restore early/late. Existing native note says this selects callback algorithm. |
| 124..127 | mState | Restore early/late; interpolation phase per existing note. |
| 128..12F | mpUpdateCallback | Keep live currently. Potential coherence issue with rewound mSelfState; reconstruct through verified native selector, not arbitrary snapshot callback. |
| 130..133 | mOldAng | Restore early/late. |
| 134..137 | mInterAng | Restore early/late. |
| 138..13F | first 8 bytes of unnamed byte[24] | Already copied; unresolved. |
| 140..14F | last 16 bytes of unnamed byte[24] | Already copied; existing note identifies direction vector here, not independently reverified. |
| 150..153 | mAngleWeight | Restore early/late. |
| 154..157 | mSpeed | Restore early/late. |
| 158..15B | mChrState | Restore early/late. |
| 15C..15F | mTgtAngle | Restore early/late. |
| 160..163 | mPerceptionAngle | Restore early/late. |
| 164..167 | mOldTgtAng | Restore early/late. |
| 168..16F | mInterCurve, EaseCurve | Restore early/late; internal EaseCurve members not retrieved. |

### cLimitParam

Apply the following independently to helper `+B0` and `+D8`.

| Limit bytes | Member | Disposition / evidence |
|---|---|---|
| 00..07 | MtObject base/vtable | Keep. |
| 08..0F | mLockAng, RangeF | Restore early/late. |
| 10..17 | mLooseAng, RangeF | Restore early/late. |
| 18..1F | mQuitAng, RangeF | Restore early/late. |
| 20..23 | mQuitLength | Restore early/late. |
| 24 | unnamed undefined | Already copied; unresolved. |
| 25 | unnamed undefined | Already copied; unresolved. |
| 26 | unnamed undefined | Already copied; unresolved. |
| 27 | unnamed undefined | Already copied; unresolved. |

No newly identified omitted scalar in the returned derived helper/limit layouts.
This does not close the inherited-base or reference audit.

## Geometry Bases

The returned datatypes have a significant unresolved discrepancy with source
comments. `MtGeometry` has `mType` at +08, whereas production calls byte +08 an
inherited active flag and copies a primitive kind at +10. The intermediate
`MtGeomConvex` layout could not be retrieved. Do not silently reconcile these
as if the layouts had been verified against constructors.

| MtGeometry bytes | Member | Disposition |
|---|---|---|
| 00..07 | MtObject base/vtable | Keep. |
| 08..0B | mType, Type enum (4 bytes) | Unknown conflict: production copies only byte +08. If this layout is correct for the embedded object, bytes +09..0B are omitted enum bytes, not proven padding. |
| 0C..0F | unnamed byte[4] | Unknown; omitted. |

`MtGeomCapsule` size `0x50`: `MtGeomConvex` at `+00..1F`,
`MtCapsule mCapsule` at `+20..4F`.
`MtGeomSphere` size `0x30`: `MtGeomConvex` at `+00..1F`,
`MtSphere mSphere` at `+20..2F`.
The nested vector/shape member names were not retrieved; the payload intervals
below distinguish source comments from verified native members.

## cHitPrimCapsule

For every `i = 0..15`, base `0x4210 + 0xB0*i`; object size `0xB0`.

| Capsule bytes | Member or gap | Disposition / evidence |
|---|---|---|
| 00..07 | inherited vtable | Keep. |
| 08 | inherited field, source calls active | Restore early/late; type conflict above. |
| 09..0F | inherited unresolved bytes | Unknown; omitted. Split into +09..0B and +0C..0F if MtGeometry layout applies. |
| 10..13 | source calls primitive kind | Restore early/late; intermediate base definition unresolved. |
| 14..1F | remaining MtGeomConvex bytes | Unknown; omitted. |
| 20..2F | mCapsule first endpoint region | Restore early/late; source identifies endpoints/radius at +20..43. |
| 30..3F | mCapsule second endpoint region | Restore early/late. |
| 40..43 | mCapsule radius region | Restore early/late. |
| 44..4F | remaining MtCapsule bytes | Unknown; omitted; shape layout/consumer proof required. |
| 50..53 | mFlag | Restore early/late. |
| 54..57 | _pad54[4] | Unknown; omitted; existing name is not padding proof. |
| 58..5F | mpParent0 | Reference, S7. Verify capture covers each parent field. |
| 60..67 | mpParent1 | Reference, S7; same caveat. |
| 68..6B | mParentNo0 | Restore early/late. |
| 6C..6F | mParentNo1 | Restore early/late. |
| 70..73 | mSizeRate | Restore early/late. |
| 74..77 | mCapsuleFlags, uint | Candidate: definitely omitted by both explicit range lists. Native consumer verification blocked. |
| 78..7F | _pad78[8] | Unknown; omitted. |
| 80..8F | mOfs0, float[4] | Restore early/late, all four lanes. |
| 90..9F | mOfs1, float[4] | Restore early/late, all four lanes. |
| A0..AF | _tailA0[16] | Unknown; omitted. Constructor initialization alone would not establish padding. |

## cHitPrimSphere

For each bank base `B = 0x4E20, 0x5530, 0x5C40` and every `i = 0..15`,
object base is `B + 0x70*i`, size `0x70`. Dispositions apply to all 48 objects.

| Sphere bytes | Member or gap | Disposition / evidence |
|---|---|---|
| 00..07 | inherited vtable | Keep. |
| 08 | inherited field, source calls active | Restore early only; same geometry type conflict. |
| 09..0F | inherited unresolved bytes | Unknown; omitted. |
| 10..13 | source calls primitive kind | Restore early only; intermediate base unresolved. |
| 14..1F | remaining MtGeomConvex bytes | Unknown; omitted. |
| 20..2F | mSphere, MtSphere | Restore early only; source calls center/radius. Native component layout not retrieved. |
| 30..33 | mFlag | Restore early only. |
| 34..37 | _unknown_0034[4] | Unknown; omitted. |
| 38..3F | mpParent | Reference, S7; early/late application, capture not reviewed. |
| 40..43 | mParentNo | Restore early only. |
| 44..47 | mSizeRate | Restore early only. |
| 48..4F | unnamed byte[8] | Unknown; omitted. |
| 50..5F | mOfs, MtVector3 | Restore early only, full 16 bytes including fourth lane. |
| 60..6F | _unknown_0060[16] | Unknown; omitted. Copy-constructor and consumers must distinguish cached geometry, references and padding. |

The bank spacing is `0x710`, not `16*0x70 = 0x700`. Inter-bank bytes
`0x5520..552F` and `0x5C30..5C3F`, and post-bank bytes `0x6340..634F`,
are **not sphere tails**. The last range is included in the existing push-vector
copy; the first two gaps require enclosing-character layout evidence and must
not be covered by enlarging a sphere stride or tail copy.

## Proposed Additions and Limits

**No new range is fully constructor/consumer-verified in this session.** Exact
candidate changes are listed to avoid converting an omission into an unsupported
safety claim:

1. Capsule `+74..77`: add `{0x74, 0x04}` or widen `{0x68, 0x0C}` to
   `{0x68, 0x10}` in BOTH capsule loops. Applies at character
   `0x4284 + 0xB0*i`, `i=0..15`. Existing datatype makes this the strongest
   scalar candidate. Approve only after native writes/reads confirm its width
   and that it is value state, not an ownership-dependent cache.
2. Sphere late pass: the loop at S6 is a no-op. Reapplying the existing six early
   scalar ranges would not introduce new pointer bytes, but whether it is needed
   and correctly ordered depends on which reconstruction paths mutate spheres.
   Do not describe current sphere restoration as early-and-late.
3. Eye clocks: `mFrame` at `0x3710 + 0x28*i` is omitted. A proposed four-byte
   copy needs verified target, control ID, slot and animation binding semantics.
   Pointer equality alone does not establish StringHandle lifetime or material
   state coherence. No unconditional clock/selector/handle copy is approved.
4. Helper callback: existing notes identify a deterministic mapping from
   `mSelfState` to native callback. Reconstructing that mapping may be preferable
   to preserving a prediction-timeline callback, but its setter and callers must
   be reverified. Do not raw-copy `+128`.

There is no evidence here supporting raw copying of sphere `+60..6F`, sphere
`+48..4F`, capsule `+A0..AF`, eye `+6C..6F`, controller gaps, or entire embedded
objects.

## Native Follow-up Ledger

These exact addresses were returned by Ghidra function listings unless marked
as coming from the existing native note. They are research targets, **not
functions successfully decompiled during this audit**.

| Question | Constructor / writer / consumer targets |
|---|---|
| Eye defaults and whether material clocks are used | `cFollowEyes__ctor 0x140033520`, `createProperty 0x140034300`, `updateMaterialControl 0x140034F00`; inspect callers and angle-update paths. |
| Material/controller identity and slot lifecycle | `uMaterialControl__ctor 0x1405496F0`, `configureController 0x140141FF0`, `Controller__local_1400226e0 0x1400226E0`, `setEnable 0x14054A7A0`. |
| Eye clock advancement/reset and animation handle consumption | `uMaterialControl__trans 0x14054A430`, `Controller__applyPatternAnimation 0x14054A960`, `Controller__dtor 0x140549810`; follow handle creation/release calls. |
| Capsule +74, geometry header and tails | `cHitPrimCapsule__base_ctor 0x14000D9F0`, `ctor 0x14000DCB0`, `createProperty 0x14000EAF0`, `testTransformFlag 0x14000FD40`, `updateWorldEndpointsFromMatrices 0x14000FD60`, `updateWorldEndpointsFromParents 0x14000FFD0`. |
| Sphere +48/+60 tails, base widths | `cHitPrimSphere__base_ctor 0x14000DA50`, `ctor 0x14000DD40`, `copyCtor 0x14011ED10`, `createProperty 0x14000EC90`, `updateWorldFromOwnerWithBase 0x140010210`, `updateWorldFromOwner 0x140010380`, `setPrimKind 0x14000FC30`. |
| Primitive base field conflict | `cHitPrim__baseCtor 0x14000DA20`; inspect actual stores at +08/+10 and recover MtGeomConvex before changing types. |
| Helper callback/state and references | Existing note: ctor `0x14010B3B0`, property creator `0x14010C1B0`, self-state setter `0x14010CBE0`, reference-follow `0x14010BB40`, independent update `0x14010BC20`, output matrix `0x14010C920`. |

For each unknown gap, require access-width evidence and follow aliases/copy
operations; absence from reflected properties is not proof of padding. For each
reference, determine ownership, nullability and target lifetime. Apply recovered
struct types to suitable unknown function parameters when native signatures can
be validated. No Ghidra type/signature changes were made on unverified evidence.

## Completion Status

- Completed: four exact requested file reads; existing named layout inventory;
  byte-range coverage ledger for returned layouts; concrete source omissions;
  research target addresses; documentation-only output.
- Blocked: instruction/decompiler constructor and consumer verification, including
  capsule flags, eye clocks, sphere tails and reference state.
- Not fully enumerated internally: cUnit, uConstraint__cConstraint, MtGeomConvex,
  MtCapsule, MtSphere, RangeF, EaseCurve and vector submembers. Their enclosing
  bytes are accounted for, but this is not a substitute for a full member audit.
- No runtime testing/build performed: no production edits, and static evidence
  does not establish rollback determinism.
