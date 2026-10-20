# uCharacter Inherited Model Member Audit

Status: **complete per-member policy inventory; targeted native verification,
with unresolved members explicitly retained below**.
Date: 2026-10-20. Target: Ghidra `ichneumon_sub/umvc3.exe.unpacked.exe`, port
8192, preferred image base `0x140000000`. No production changes.

## Scope And Evidence Limits

Scope is the inherited prefix `[0,0x1300)`: MtObject, cUnit, uCoord,
uBaseModel, uModel (including all eight embedded Motion objects), uToonModel
and its cToonParam. Gameplay/interpreter state beyond this prefix is outside
this audit. Heap pointees are ownership dependencies, not implicitly audited
by a row for their pointer. Vector/matrix/array rows cover their entire inline
storage, including vector fourth lanes. Contiguous unnamed bytes are recorded
as explicit ranges rather than omitted or assumed to be harmless padding.

Read `doc/README.md` and the exact permitted files
`UMVC3Hook/code/rollback_character_restore.cpp` and
`UMVC3Hook/code/rollback_restore.cpp`. No other source contents were used.
A Grep request whose path was the latter file unexpectedly returned broader
workspace matches; those other-file results were excluded from the evidence.
Subsequent source access used exact-file reads only. No home access or subagents.

Ghidra initially timed out and briefly failed to connect after returning the
layouts and two constructors. Later retries recovered access. Targeted native
writer/consumer verification then established parts-visibility mutation, toon
parameter mutation/reads, joint-dependency request consumption, allocation-bound
blend counts, and key-frame work dependencies. The evidence ledger below
distinguishes these from constructor-only and unresolved classifications.
`setupMotion` at `0x140529820` still failed decompilation at both short and
60-second timeouts. Constructor writes are evidence of representation/defaults,
not proof of runtime semantics or restore safety. Existing database names and
located function entry points are not substitutes for that proof.

## Policy Legend

| Policy | Meaning |
|---|---|
| S: restore scalar | Inline temporal state; existing implementation or candidate as qualified in the row. Does not authorize adjacent bytes. |
| L: live owner | Preserve live vtable, allocation, reference, topology, handle or infrastructure. Rebinding requires an explicit recipe. |
| R: resource matched | Restore only with matching, live resource/allocation and compatible bounds; identity alone does not prove lifetime. |
| D: derived | Rebuild coherently from authoritative state; never rewind a lone cache-validity bit. Existing copied derived state is noted. |
| P: presentation | Keep separate from gameplay scalar coverage; rewind only reviewed visual parameters, not renderer infrastructure. |
| U: unresolved | Leave live in a new policy pending native evidence. If current code copies it, that is a review concern, not an approval. |

Multiple policy letters describe dependencies, not permission for a broad memcpy.
Rows without a native evidence key remain conservative layout-based
classifications pending consumer verification.

Evidence keys:

| Key | Evidence actually inspected |
|---|---|
| T | Ghidra datatype layout, not native read/write proof. |
| B | `uBaseModel__ctor`, `0x14055B0F0`, typed decompilation; writes detailed below. |
| M | `uModel__ctor`, `0x1405644A0`, typed decompilation; writes detailed below. |
| CU | `cUnit__ctor 0x14050C890`; `setMoveLine 0x14050D040` and `setDrawMode 0x1401461E0` disassembly. |
| CO | `uCoord__composeWorldMatrix 0x140548730`, typed decompilation: parent/selector/flags/scale reads, world matrix writes, invalid-parent clear. |
| PV | Parts setter `0x14004D3E0`, reader `0x14004B070`, Arthur runtime writer `0x140064BB0`, cache comparison `0x140563FC0`, cache destruction/reset `0x14055B8D0`; decompiled. |
| AL | `uModel__setBlendNum 0x1405291E0` and `setModel 0x140568670`, typed decompilation of allocation/free and binding paths. |
| JD | Request setter `0x140569870`, consumer/clear `0x14056FBB0`, dependency rebuild `0x14056ADA0`, world-update readiness write `0x14056FD20`; decompiled. |
| MC | `Motion::ctor 0x140525EC0`, typed decompilation. |
| KF | `Motion::calcKeyFrame 0x1405260B0`, `calcSequence 0x140526460`, `uModel::updateMotionFrame 0x14052CB90`; decompiled. |
| TP | Toon constructor `0x140450EA0`, copy writer `0x140451410`, shader consumer `0x140451210`; helper reads/writes verified in disassembly, detailed below. |
| CE | `rollback_character_restore.cpp:25-36`, CommonEarly ranges for this prefix. |
| CM | `rollback_restore.cpp:1727-1766`, Motion inline state and roster resource-matched event bindings. |
| RP | `rollback_restore.cpp:1932-1955`, late visual flag/light-group/transparency repair. |
| OW | `rollback_restore.cpp:1888-1896`, external region owner mismatch repair; does not prove which model buffers were captured. |

All offsets and sizes below are hexadecimal. Unless explicitly relative, offsets
are from the character/model base. `live` in the coverage column means no write
found in the inspected character-body restore ranges, not a proof that no other
subsystem ever writes it. Capture implementation was intentionally not read.

## MtObject And cUnit

| Offset | Size | Member | Current coverage | Policy and evidence |
|---|---:|---|---|---|
| 000 | 08 | MtObject.vftable | live | L. T; B/M replace this during construction, not temporal state. |
| 008 | 04 | mRno | CE | S; native runtime consumer unverified. |
| 00C | 04 | mUnitParam | CE | S; native runtime consumer unverified. |
| 010 bits 0..2 | 3 bits | mBeFlag | CE roster only | L/S lifecycle split; no blanket helper rewind. Exact masks require consumer verification. |
| 010 bits 3..9 | 7 bits | mMoveLine | CE roster only | L registration/category selector; CU masks with 0xFFFFFC07 then ORs `(value & 0x7F)<<3`. |
| 011 packed | 6 bits | mUnitAttr | CE roster; RP visible bit | S/P/L mixed attribute word; retain mask-specific policy. |
| 012 packed | 10 bits | mDrawView | CE roster only | P; runtime semantics unverified. |
| 010..013 remaining | 6 bits | unnamed packed-word bits | CE roster only | U; the four named bitfield widths total 26 bits, not 32. Layout API did not expose individual bit offsets. |
| 014 | 04 | mDrawMode | live | P/U omitted candidate. B writes 7; M writes 0x1827; CU setter writes EDX at +14. Runtime callers/consumer still unverified. |
| 018 | 08 | mpNextUnit | live | L intrusive topology. |
| 020 | 08 | mpPrevUnit | live | L intrusive topology. |
| 028 | 04 | mDeltaTime | CE | S; CU sets 1, KF reads it in frame advancement. |
| 02C..02F | 04 | unnamed bytes | CE | U; copied by the 0x28/0x10 range. No evidence these are temporal scalars. |
| 030 | 08 | mUnitGroup | CE | S/L group mask; registration effects require native verification. |
| 038..03F | 08 | padding[2] in Ghidra | live | U/L. Source calls +0x38 a list link; do not trust the datatype's padding label or restore it. |

The bitfield display reports overlapping storage units. This table covers the
whole packed dword; CU verifies move-line bits 3..9. Remaining masks still need
verification beyond source's `0x4000` visible bit and CO's low-three-bit test.

## uCoord

The embedded cUnit occupies `[000,040)` and is expanded above.

| Offset | Size | Member | Current coverage | Policy and evidence |
|---|---:|---|---|---|
| 040 | 08 | mpParent | live | L/R live relationship; CO dereferences parent lifecycle and clears invalid parent. Any rewind needs identity/lifetime validation. |
| 048 | 04 | mParentNo | CE | S/R; CO chooses parent's world matrix versus virtual +A0 joint matrix according to selector sign. Must match retained parent. |
| 04C | 02 | mOrder | CE | S rotation-order scalar, T only. |
| 04E | 02 | mParentFlags | CE | S/R; CO selects inherited angle/scale handling. |
| 050 | 10 | mPos | CE | S; B initializes xyz and zeroes fourth lane. |
| 060 | 10 | mQuat | CE | S; B initializes quaternion. |
| 070 | 10 | mScale | CE | S; B initializes xyz=1 and fourth lane=0; CO consumes xyz. |
| 080 | 40 | mLmat | CE | D/S existing coherent snapshot copy; B initializes matrix; CO reads it. |
| 0C0 | 40 | mWmat | CE | D/S existing snapshot copy; CO derives it from local/parent state and scale. |

No unnamed interval exists between these members in the retrieved layout.

## uBaseModel

The embedded uCoord occupies `[000,100)` and is expanded above.

| Offset | Size | Member | Current coverage | Policy and evidence |
|---|---:|---|---|---|
| 100 | 08 | mpModel | live | L/R resource; B nulls it. Never restore an unpinned historical pointer. |
| 108 | 08 | mpMaterialData | live | L/R resource; B nulls it. |
| 110 | 08 | mpMaterials | live | L owned table; B nulls it. |
| 118 | 04 | mMaterialNum | live | L/R table bound; B zeroes it. Do not pair saved count with live storage. |
| 11C | 04 | mLightGroup | RP as drawLayerMask | P already restored separately; B sets 1. Naming disagreement does not justify a second blind write. |
| 120 | 04 | mLightingFlags | live | P/U mixed flags. B performs masks/ORs, including high-bit clears. No complete runtime bit audit. |
| 124 | 04 | mRenderFlags | live | P/D/U mixed flags. B masks/ORs multiple times. Do not restore whole word. |
| 128 | 04 | mDrawPriority | live | P omitted candidate; B sets 3. Writer/reader pair pending. |
| 12C | 40 | mPartsDisp[16] | live | P/R confirmed mutable omission, PV. Restore inline visibility for matching model/part layout; preserve cache owners and invalidate/reconcile live caches before draw. |
| 16C..16F | 04 | unnamed bytes | live | U, not proven padding. |
| 170 | 08 | mpPartsDrawMode | live | L table owner; B nulls it. Per-part contents need separate policy. |
| 178 | 04 | mZPrepassDist | live | P; B sets 2000. |
| 17C | 04 | mPriorityBias | live | P; B sets 0. |
| 180 | 04 | mTransparency | RP | P already restored separately; B sets 1. |
| 184 | 04 | mLODLowDist | live | P; B sets 1000. |
| 188 | 04 | mLODMidDist | live | P; B sets 3000. |
| 18C | 04 | mRenderFrameStamp | live | D/P; B zeroes it. Never rewind independently of renderer timeline. |
| 190 | 02 | mViewDraw | live | D/P; B zeroes it. |
| 192 | 02 | mViewDrawNew | live | D/P; B zeroes it. |
| 194 | 04 | mViewDrawControl | live | P/D/U; B clears low two bits only. Need bit-level input/output distinction. |
| 198 | 18 | mpMaterialStack[3] | live | L/U pointer stack. B shown decompilation does not initialize it; not permission to copy. |
| 1B0 | 10 | mBoundingSphere | live | D; B initializes position and radius 100. Refresh with restored transform/model. |
| 1C0 | 50 | mBoundingBox | live | D; B initializes coord and extent including extent fourth lane. |
| 210 | 10 | mLightReciver | live | D/P; spelling retained from database. B initializes position and radius 100. |
| 220 | 10 | mAmbientMask | live | P; B sets (1,0,0,0). |
| 230 | 04 | mCacheMode | live | L/D renderer cache policy. B's eight-byte store at AmbientMask.w also zeroes this field. |
| 234 | 40 | mPrevPartsDisp[16] | live | D; PV compares it against mPartsDisp, copies live bits on mismatch then clears caches. Never rewind mirror alone. |
| 274 | 04 | mPartsDispMax | live | R/D; PV uses this as byte count for comparison/copy, not a gameplay counter. Retain live resource-compatible bound. |
| 278 | 280 | mCommandCacheSlots[16] | live | L/D; expanded below, never raw copied. B zeroes array then initializes locks. |
| 4F8 | 04 | mCacheAllocationCursor | live | L/D; B zeroes it. |
| 4FC | 04 | mCacheFrameKeyMode | live | L/D; B zeroes it. |
| 500 | 01 | mIsRCNEnable | live | L/P/U capability/resource-dependent state; B false. Not a demonstrated gameplay latch. |
| 501 | 01 | mIsFromTextureCalcTangent | live | L/P/U pipeline choice; B false. |
| 502..50F | 0E | unnamed bytes | live | U; no observed accesses. Each byte remains excluded. |

### Command Cache Slot

For every `i=0..15`, base is `0x278 + i*0x28`. This expands every member and
unnamed byte of the array, not only slot zero.

| Relative offset | Size | Member | Policy and evidence |
|---|---:|---|---|
| 00 | 08 | mpCommandCache | L/D; B zeroes, retained live. |
| 08 | 08 | mpContext | L/D; B zeroes, retained live. |
| 10 | 04 | mKey | D; B zeroes, retained live. |
| 14..1B | 08 | unnamed bytes | U/L cache-internal storage; B memset is not scalar proof. |
| 1C | 04 | mLockWord | L synchronization; B writes 0xBEAF after memset. |
| 20 | 04 | mLastUsedFrame | D renderer epoch; B zeroes. |
| 24..27 | 04 | unnamed bytes | U/L cache-internal storage. |

## uModel

The embedded uBaseModel occupies `[000,510)` and is expanded above.

| Offset | Size | Member | Current coverage | Policy and evidence |
|---|---:|---|---|---|
| 510 | 20 | mModelFlags[32] | live | U. Name is not proof of flags or scalar representation; M does not explicitly initialize it. High-priority type/consumer audit. |
| 530 | 04 | mJointNum | live | L/R allocation bound; M zeroes it. |
| 534 | 04 | unknown_534 | live | U; no observed access. |
| 538 | 08 | mJoint | live | L owned joint storage; M nulls it. |
| 540 | 08 | mJointTable | live | L/R; M assigns static `DAT_140BAAB40`, not temporal payload. |
| 548 | 08 | mJointDepthTable | live | L/D; M nulls it. |
| 550 | 04 | mModelStatus | live | L/D/U mixed status. AL resets during rebinding; JD ORs S_JOINT_READY after matrix work. **Do not restore whole word.** Other bits unresolved. |
| 554 | 04 | mBlendNum | live | L/R allocation bound, AL; also KF evaluation-loop bound. Not an unconditional omitted scalar. |
| 558 | 04 | mMotionCalcFlag | CE | S/U existing copy; M zeroes it. Need flag-by-flag resource/derived audit. |
| 55C | 04 | mMotionInterpolateAttr | CE | S; M zeroes it. Runtime consumer not verified. |
| 560 | C00 | mMotion[8] | CM | Expanded below; no whole-object copy. |
| 1160 | 04 | mMotionBlurSetup | live | P/D; M zeroes it; AL and JD write 2 on resource/joint or lifecycle transitions. Keep coherent with live renderer history. |
| 1164 | 04 | mMotionBlurDist | live | P; M sets 2000. |
| 1168 | 04 | mMotionBlurLimite | live | P; database spelling retained. M sets 100. |
| 116C | 04 | mEnvelopeHandle | live | L renderer handle; AL releases/reserves palette handle and resets to 0xFFFFFFFF. Never restore as a scalar. |
| 1170 | 08 | unknown_1170 | live | U; representation and runtime accesses unverified. |
| 1178 | 08 | mpSkinMeshCollisionPtr | live | L/R model relationship; M sets self. Remap self on reconstructed objects. |
| 1180 | 10 | mNullTransScale | CE | S; M initializes xyz and zero fourth lane. |
| 1190 | 10 | mDeltaPos | CE | S temporal root-motion output; M initializes xyz and zero fourth lane. Native consumption timing unverified. |
| 11A0 | 10 | mDeltaQuat | CE | S temporal root-motion output; M initializes quaternion. |
| 11B0 | 08 | mpInterpolationJoint | live | L/R owned interpolation storage; AL frees/allocates jointCount*0x40. Separate contents audit needed. |
| 11B8 | 80 | mpMotionList[16] | live | L/R resources; M zeroes array. CM compares selected live/saved entry for borrowed event pointers. |
| 1238 | 04 | mKeyFrameNum | live | L/R capacity; AL sizes key-frame data as this*blendCount*0xC and slices by this*slot*0xC. Never rewind independently. |
| 123C | 01 | mShowJointNo | live | P debug display; M true. |
| 123D | 01 | mUpdateJointDependencyReq | live | D confirmed mutable omitted request: JD setter true; world-joint update rebuilds/sorts then clears. Request a rebuild for restored topology, not blind historical rewind. |
| 123E | 01 | mUpdateNormal | live | D/P/U; M false. Need texture/resource dependence. |
| 123F | 01 | unknown_123f | live | U. |
| 1240 | 10 | mMorphWeight[4] | CE | S/P; M zeroes four floats. Resource-compatible deformation inputs. |
| 1250 | 10 | mpShaderAttributes[2] | live | L/D; M nulls both. |
| 1260 | 10 | mBasePlane | CE | S; M constructs plane. Runtime IK consumer still unverified. |
| 1270 | 04 | mWaistOffset | CE | S; M zeroes. |
| 1274 | 04 | mWaistAdjustFactor | CE | S; M zeroes. |
| 1278 | 04 | mWaistAdjust | CE | S/D temporal IK state; M zeroes. |
| 127C | 08 | mWaistAdjustCurve | CE | S; two floats p1/p2 initialized from `QWORD_140D2B178` by M. |
| 1284 | 04 | mWaistAdjustSpeed | CE | S; M sets 2. |
| 1288 | 04 | mHeelHeight | CE | S; M zeroes. |
| 128C | 04 | mRotInverseWeight | CE | S; M sets 1. |
| 1290 | 04 | mFootAdjustType | CE | S; M sets 0xFFFFFFFF. |
| 1294 | 04 | mFootAdjustFilter | CE | S; M sets 0x3FFFFFFF. |
| 1298 | 04 | mFootAdjustGroupBit | CE | S; M sets 1. |
| 129C | 01 | mAnkleAdjust | CE | S; M true. |
| 129D | 01 | mFootAdjust | CE | S; M true. |
| 129E | 01 | mCalcBuiltInIKReq | CE | S/D request; M false; updateLocalMatrix 0x14056FCF0 tests it before calcBuiltInIK. Full producer/clear ordering still unresolved. |
| 129F | 01 | unknown_129f | live | U; deliberately outside CE's 0x3F-byte range. |
| 12A0 | 08 | mpPositionMap | live | L resource; M nulls. |
| 12A8 | 08 | mpNormalMap | live | L resource; M nulls. |
| 12B0 | 08 | unknown_12b0_texture_ptr | live | L/U pointer, M nulls; exact ownership unverified. |
| 12B8 | 08 | unknown_12b8_texture_ptr | live | L/U pointer, M nulls; exact ownership unverified. |

### Motion

This table applies member by member to **each** Motion slot. Slot bases are
`560, 6E0, 860, 9E0, B60, CE0, E60, FE0`; stride `180`; last end `1160`.
Offsets in this table are relative to that slot, not the character base.
M calls `uModel__Motion__ctor` (`0x140525EC0`) eight times. MC was subsequently
decompiled: it initializes playback defaults, event tables/cursors, mTransParam,
and work pointers/count. It does not prove unknown bytes are safe padding.

| Relative offset | Size | Member | Current coverage | Policy and evidence |
|---|---:|---|---|---|
| 000 | 08 | MtObject.vftable | live | L. |
| 008 | 02 | mMotionNo | CM | S/R saved motion selector requires compatible motion resource. |
| 00A | 02 | mState | CM | S/U playback state word, exact bit effects unverified. |
| 00C | 02 | mPrevState | CM | S/U previous playback state word. |
| 00E | 02 | unknown_e | CM | U; currently swept up by broad scalar range. |
| 010 | 04 | mAttr | CM | S/U attributes; exact runtime bit policy unverified. |
| 014 | 04 | mStartFrame | CM | S. |
| 018 | 04 | mCarryFrame | CM | S. |
| 01C | 04 | mPrevDeltaFrame | CM | S temporal history. |
| 020 | 10 | mCarryQuat | CM | S temporal history. |
| 030 | 10 | mCarryTrans | CM | S temporal history. |
| 040 | 04 | mInterFrame | CM | S interpolation state. |
| 044 | 04 | mInterCount | CM | S interpolation state. |
| 048 | 04 | mBlend | CM | S. |
| 04C | 04 | mFrame | CM | S. |
| 050 | 04 | mPrevFrame | CM | S temporal history. |
| 054 | 04 | mFrameMax | CM | S/R resource-compatible limit. |
| 058 | 04 | mLoopFrame | CM | S/R resource-compatible limit. |
| 05C | 04 | mSpeed | CM | S. |
| 060 | 04 | mInterRate | CM | S. |
| 064 | 04 | mNowFrame | CM | D/S; KF derives clamped current sample from frame/max and attribute 0x800. |
| 068 | 04 | mNextFrame | CM | D/S; KF derives clamped frame+speed*deltaTime. |
| 06C | 08 | mInterCurve | CM | S, p1/p2 as an eight-byte curve value. |
| 074 | 0C | unknown_74 | CM | U; cannot assert all 12 bytes are padding. |
| 080 | 10 | mPrevQuat | CM | S temporal history. |
| 090 | 10 | mBaseQuat | CM | S root transform state. |
| 0A0 | 10 | mNullQuat | CM | S root transform state. |
| 0B0 | 10 | mPrevTrans | CM | S temporal history. |
| 0C0 | 10 | mBaseTrans | CM | S root transform state. |
| 0D0 | 10 | mNullTrans | CM | S root transform state. |
| 0E0 | 20 | mTransParam[32] | CM | U/S existing byte-array copy; native representation/consumer proof still needed. |
| 100 | 20 | mQuatParam[32] | CM | U/S existing byte-array copy; native representation/consumer proof still needed. |
| 120 | 20 | mpSeqInfo[4] | CM conditional | R borrowed event bindings: roster only, saved motion != 0xFFFF, selected saved resource nonnull and equal to live. KF reads page table count +40 and records pointer +48. Binding writer/lifetime still unresolved. |
| 140 | 10 | mSequence[4] | CM | S/R event cursor state must agree with bindings. |
| 150 | 10 | mPrevSequence[4] | CM | S/R event history must agree with bindings. |
| 160 | 08 | mJoint | live | L owned work allocation; AL frees/reallocates jointCount*0x68 per active slot. |
| 168 | 04 | mKeyFrameNum | CM | R count; KF loop bound reads +170 records at stride 0x10 and writes +178 at stride 0xC. Current unconditional count copy needs binding/capacity compatibility. |
| 16C | 04 | unknown_16c | CM | U currently copied with count; not proven scalar/padding. |
| 170 | 08 | mpKeyFrameInfo | live | R key-frame descriptor binding; KF dereferences descriptor count and key-array pointers. Native binding writer/lifetime still unresolved. |
| 178 | 08 | mpKeyFrameData | live | L/R work slice; AL frees/allocates via slot 0 and sets remaining slices, KF reads cursor/writes cursor and value. Restore compatible contents, never historical owner pointer. |

The inline CM writes are exactly `[008,120)`, `[140,160)`, `[168,170)`.
They cover unknown bytes as well as known playback values. A rejected +120
binding copy does **not** reject the already-restored motion number, frames or
sequence indices. This is a source-observed consistency risk, not a proven
runtime failure. Non-roster helpers do not receive CM event-binding rewind.

OW only proves that mismatched captured external owners are preserved. It does
not by itself prove that inline key-frame counts, blend count, or selected
resources remain compatible. No claim of complete heap-buffer snapshot coverage
is made without inspecting capture and native allocation/consumer paths.

## uToonModel And cToonParam

uToonModel contains uModel at `[000,12C0)` and cToonParam at `[12C0,1300)`.
The following expands every cToonParam member. No extra uToonModel gap exists.

| Absolute offset | Relative offset | Size | Member | Current coverage | Policy and evidence |
|---|---|---:|---|---|---|
| 12C0 | 00 | 08 | MtObject.vftable | live | L. |
| 12C8 | 08 | 04 | mRimLightMinHeight | live | P safe inline addition; TP verifies writer +8 and shader-path reader +8. |
| 12CC | 0C | 04 | mRimLightMaxHeight | live | P safe inline addition; TP verifies writer/reader +C. |
| 12D0 | 10 | 10 | mRimLightColorFactor | live | P safe inline addition; TP verifies four-dword copy writer/reader +10. |
| 12E0 | 20 | 04 | mRimLightPower | live | P safe inline addition; TP verifies MOVSS writer/reader +20, despite misleading shared helper names. |
| 12E4..12EF | 24..2F | 0C | unnamed byte[12] | live | U; exclude even if neighboring visual scalars are later restored. |
| 12F0 | 30 | 10 | mCutInFactor | live | P safe inline addition; TP verifies four-dword copy writer/reader +30. |

## Findings And Safe Additions

**Safe pointer-free presentation additions:** `[12C8,12E4)` (0x1C bytes)
and `[12F0,1300)` (0x10 bytes), for a validated live uToonModel-derived
character. TP verifies writes and reads of each member. Apply after any native
reconstruction/lighting-copy work that would overwrite them, and before draw.
These contain neither the cToonParam vtable nor its unnamed +24..2F interval.
This establishes memory/ownership-safe visual rewind, not a gameplay determinism
benefit or a tested phase integration. No production change was made.

**Conditional addition:** `[12C,16C)` parts visibility, with matching live model
resource/part mapping and synchronized cache invalidation before drawing. Do not
restore the adjacent gap, parts-draw table pointer, historical mirror, locks,
contexts or cache keys. PV demonstrates a native comparison/rebuild path; use
it in the correct render-safe phase, rather than assuming the next draw will
automatically rebuild stale cached commands.

Confirmed source findings:

1. `mLightGroup/+11C` and `mTransparency/+180` are not total omissions: RP restores them. RP also restores the character visible bit and conditionally calls native material-mode repair at `0x140200DB0`. That call's native body was not inspected here.
2. Motion playback is not omitted: CM restores the three ranges above, and conditionally restores four event bindings. The broad ranges include four explicitly unknown areas/representations: +00E, +074, +16C and the byte-typed transform parameter blocks.
3. The broad CE +028 range copies the unnamed +02C dword. The +038 field is preserved and has conflicting layout/source descriptions. Do not promote either to a safe scalar from its layout label.
4. Confirmed omitted mutable fields include parts visibility (PV), the five toon members (TP), and the joint-dependency request (JD). The request requires coherent rebuild rather than historical byte-copy. Other visual scalars and the normal-update request remain less fully characterized.
5. `mModelStatus`, model/lighting/render flags, envelope handles and command cache state have no safe whole-word/block rewind justification. AL/JD demonstrate resource reset/readiness state, and PV demonstrates cache destruction/lock reset. Constructor zeros are not enough.
6. `mBlendNum/+554` is **not a safe scalar addition**. AL uses both old and new values in free/allocation loops and sizes shared key-frame storage with it. KF uses it as an evaluation-loop bound. Preserve it with the live graph or reconstruct the complete compatible graph.

### Verified Native Accesses

| Field/path | Writer evidence | Consumer / constraint evidence | Result |
|---|---|---|---|
| mPartsDisp | `0x14004D3E0` masks/sets one bit of `mPartsDisp[bitIndex>>5]`; runtime Arthur path `0x140064BB0` calls it for parts 1/2/3 based on mUsrF bits 0x20/0x40/0x80. Calls include `0x140064BD0`, `0x140064BFA`, `0x140064C13`. | `0x14004B070` shifts/tests same word; `0x140563FC0` compares against live mPrevPartsDisp using mPartsDispMax bytes, copies mismatch and clears commands. | Confirmed temporal visual omission; resource/cache-coherent restore. |
| command cache slots | `0x14055B8D0` dispatches cache virtual +28, nulls owners, memsets 0x280 bytes, initializes each lock to 0xBEAF. | `0x140563FC0` manipulates allocation cursor and checks context pointers. | Live infrastructure; never restore historical bytes/locks. |
| mBlendNum | `0x1405291E0` assigns new value, frees old slot work, allocates new jointCount*0x68 work, ORs S_SETUP into slot state. | Same function and `0x140568670` size/slice shared key-frame allocation by model key-frame capacity and blend count; `0x14052CB90` loops to blend count. | Live graph bound, not plain rewindable scalar. Setter clamps to 0x10 even though current datatype has eight inline slots; do not expand snapshot loops on that evidence. |
| mModelStatus / mEnvelopeHandle | `0x140568670` resets status, releases/reserves palette handle; `0x14056FD20` ORs S_JOINT_READY after joint world update. | State is tied to rebuilt joint/resource/render graph. Complete bit consumers not audited. | No whole-word restore. |
| mUpdateJointDependencyReq | `0x140569870` sets true. | `0x14056FBB0` tests, calls `0x14056ADA0` and clears; rebuild computes dependencies and sorts joint order. | Derived request: rebuild for restored graph if necessary, do not reintroduce stale sorted-table state. |
| Motion channel state | `0x1405260B0` writes +178 work cursor and sampled value. | Reads +168 count, +170 descriptor array (stride 0x10), +178 work (stride 0xC); AL proves slot-zero allocation and slices. | Match count, descriptors, resource and work storage as a unit. |
| Motion event tables | MC zeroes tables/cursors. | `0x140526460` dereferences selected +120 table's count +40 and records pointer +48. | Borrowed binding lifetime/producer still needs verification; current source gate is documented, not fully endorsed. |
| mNowFrame / mNextFrame | `0x14052CB90` writes clamped sample frames. | Reads motion number, frame, maximum, speed, attribute 0x800 and cUnit delta time. | Derived playback payload already copied. |
| Toon copy path | `0x140451410` computes destination model+12C0, copies five parameters via helpers (disassembly inspected). | `0x140451210` reads same members through helpers and submits shader parameter block before uModel drawModel. | Inline presentation values, no reference/handle restoration. |

Toon helper instruction evidence:

| cToonParam offset | Writer | Reader |
|---|---|---|
| +08 | `0x140451400`: MOVSS [RCX+8],XMM1 | `0x1402229F0`: MOVSS XMM0,[RCX+8] |
| +0C | `0x1404513F0`: MOVSS [RCX+C],XMM1 | `0x140451350`: MOVSS XMM0,[RCX+C] |
| +10..1F | `0x1404513E0`: destination +=0x10, jumps to four-dword copy | `0x140158ED9`: source = RCX+0x10, copy to output |
| +20 | `0x1400226E0`: MOVSS [RCX+20],XMM1 | `0x14054A250`: MOVSS XMM0,[RCX+20] |
| +30..3F | `0x14044B620`: destination +=0x30, jumps to four-dword copy | `0x14044A0C9`: source = RCX+0x30, copy to output |

The four-dword helper `0x1402F44C0` has loads/stores at offsets 0,4,8,C.
Disassembly of `0x140451410` confirms float return transfer XMM0 -> XMM1 at
`0x14045146D` before the +20 store call at `0x140451470`; the decompiler's
misleading shared Controller/cChrStatus helper types are not semantic evidence.
Existing typed uModel/uBaseModel/uCoord/cToonParam parameters were used. Shared
generic helpers were inspected as instructions instead of globally retyping
them to one call-site type.

## Native Verification Queue

These addresses form the follow-up queue. Entries already cited in the evidence
ledger were inspected; remaining addresses are **navigation targets only**, not
read/write evidence. Names remain database hypotheses unless verified above.

| Question | Ghidra entry points to inspect next |
|---|---|
| cUnit lifecycle, packed flags, +038 topology | Native add/remove callers `0x14051A6B0`, `0x14051B9F0` (addresses present in permitted source); identify cUnit constructor/consumers in Ghidra. |
| Parent selectors and local/world matrix dependence | `uCoord__setParent 0x1400376F0`, `uCoord__ctor 0x1405473A0`, `updateLocalMatrix 0x140548570`, `composeWorldMatrix 0x140548730`. |
| Parts visibility mutation and render/cache reads | `setPartsDisp 0x14004D3E0`, `setPartsDispAll 0x14004D420`, `isPartsDisp 0x14004B070`, `drawModel 0x14055ECB0`, `updateCommandCache 0x140563FC0`. |
| Mixed flags, draw-mode, visual parameters | `setCommonState 0x14055FDC0`, `updateViewDraw 0x1405641A0`, `setModel 0x140561CE0`, `fillPerPartDrawModeTable 0x140561E90`; follow native writers, not just property reflection. |
| Cache allocation, lock and key semantics | `clearCommandCache 0x14055B8D0`, `drawCommandCache 0x14055DDA0`, `getCommandCache 0x14055F8B0`, `updateCommandCache 0x140563FC0`. |
| +510 block, model status, blend count and channel counts | `uModel__setModel 0x140568670`, `setBlendNum 0x1405291E0`, `setupMotion 0x140529820`, `setMotionList 0x140529730`, `dtorInPlace 0x140564760`. |
| Motion root transforms, state flags, sequence cursor consumers | `Motion::ctor 0x140525EC0`, `dtorInPlace 0x140526000`, `calcKeyFrame 0x1405260B0`, `calcSequence 0x140526460`, `updateFrame 0x14052BBD0`, `updateMotionFrame 0x14052CB90`, `updateMotionNullParam 0x14052F260`, `updateMotionParam 0x140531A00`. |
| Joint/normal dirty request lifecycle | `updateJointDependency 0x140569870`, `calcJointDependency 0x14056ADA0`, `updateNormal 0x140569880`, `updatePosition 0x14056A1B0`, `updateShaderAttributes 0x14056A860`. |
| IK scalar versus derived state | `adjustFoot 0x14056FE10`, `calcBuiltInIK 0x140570820`; trace writes to request +129E and waist adjustment +1278. |
| Toon mutable visual payload and unnamed +24..2F | `cToonParam::ctor 0x140450EA0`, `createProperty 0x1404510A0`, `renderToonShading 0x140451210`; inspect external gameplay/cinematic writers through Ghidra. |

Remaining work: complete the resource-binding producer audit (especially
setupMotion), refine mixed flag masks, establish the mModelFlags byte-block
representation, and resolve or retain every U interval. A table row is a policy
decision, not a claim that all possible accesses have been exhaustively found.
No build/runtime test was performed
because this is a documentation-only investigation, and no deterministic-runtime
claim follows from the static inventory.
