# uCharacter Gameplay Member Audit

Research ledger, 2026-10-20. Ghidra instance 8192,
`ichneumon_sub / umvc3.exe.unpacked.exe`, preferred base `0x140000000`.

## Scope And Evidence

This is a complete disposition inventory of the **own** `uCharacter` storage
`[0x14B0,0x68F0)`, plus the requested embedded gameplay types. It is not a
deterministic-support certification. `+0x68F0` is the exclusive common-body end,
not a universal scalar member. The 301-component Ghidra `uCharacter` structure
contains one inherited component and 300 own components (including explicitly
named unknown/padding arrays). All own components are accounted for below.

Source inspected in full: `UMVC3Hook/code/rollback_character_restore.cpp`
(344 lines at inspection), `UMVC3Hook/code/rollback_restore.cpp` (2104 lines),
`doc/README.md`, and `doc/chrfields.md`. No other source files were searched or
read. Consequently a call into an external reference/capture implementation is
recorded as a policy delegation, not proof that its capture recipe is complete.
Concurrent implementation of the already-known command fix can supersede the
baseline described here.

Layouts were retrieved directly with Ghidra `structs_get`. Native evidence below
comes from decompilation and, for the new message omission, exact instructions.
Names and types in the inventory are Ghidra database labels, **not independently
proven native semantics** unless backed by a cited consumer. Names beginning
`_pad` likewise do not prove that storage is unused. Script execution was
disabled; individual layout requests were used instead. No production code was
modified. One incorrect native function parameter type at `0x140030AB0` was
corrected to `cChrMessage*` for this analysis.

Inherited model state below `+0x14B0`, the internals of eyes/constraints and
collision types have separate auditors. Their own outer members and exact
restore dispositions are included here; their recursive member inventories are
not duplicated. All other requested nested types are expanded below.

## Confirmed Omissions

### Queued Message Impulse

**New confirmed omission:** `cChrMessage+0x24/0x0C` is three queued movement
floats, not tail padding. `rollback_restore.cpp:1707-1718` restores only
`+0x18/0x0C`, ending immediately before them. Neither common kernel phase writes
the message records. Thus historical flags/selectors are restored while their
queued impulse values remain from the live timeline.

| Message member | Base | Omitted speed | Omitted acceleration | Omitted direction |
|---|---:|---:|---:|---:|
| `mMsgMember` | `2C50` | `2C74/4` | `2C78/4` | `2C7C/4` |
| `mMsgParent` | `2C80` | `2CA4/4` | `2CA8/4` | `2CAC/4` |
| `mMsgChild` | `2CB0` | `2CD4/4` | `2CD8/4` | `2CDC/4` |
| `mMsgTarget` | `2CE0` | `2D04/4` | `2D08/4` | `2D0C/4` |
| `mMsgThrow` | `2D10` | `2D34/4` | `2D38/4` | `2D3C/4` |
| `mMsgDamage` | `2D40` | `2D64/4` | `2D68/4` | `2D6C/4` |

The shared layout and consumer apply to all six instances. **Direct producer
reachability in this pass is established specifically for `mMsgDamage`**; this
does not assert that all six slots receive impulse requests during ordinary play.

Native proof:

| Address | Observation |
|---|---|
| `140030AB0` | Function currently labeled `sMvc3Manager__requestPartnerAction`; actual receiver is `cChrMessage*`. It checks the byte at `+14`, ORs flags at `+18`, stores recipient selector at `+1C`, and queues three floats. Old manager/timing labels are misleading. |
| `140030AEA` | `MOVSS [RBX+28],XMM0`, queued acceleration. |
| `140030AEF` | `MOVSS [RBX+2C],XMM1`, queued direction. |
| `140030AF4` | `MOVSS [RBX+24],XMM6`, queued speed. |
| `1400FED09` | Animation interpreter sets receiver to `[R12+2D40]`, the damage message. |
| `1400FED11` | Sets message flags to `8`. |
| `1400FED2A` | Calls `140030AB0` with computed floating-point arguments. |
| `1400307FF` | Dispatcher tests `mMsgF & 8` before consuming the impulse. |
| `140030856..140030878` | Loads `+24/+28/+2C` and calls `140030B50/140030B30/140030B40` respectively. |
| `140030B50` | Stores float into recipient `+4118` (`mISpd`). |
| `140030B30` | Stores float into recipient `+411C` (`mIAcl`). |
| `140030B40` | Stores float into recipient `+4120` (`mIDir`). |
| `1400308A3..1400308C5` | Same impulse can also propagate to the recipient's parent when its tested flag permits it. |

This is a proven missing gameplay payload. A snapshot containing a pending flag-8
message can replay with a different impulse if these floats changed after
capture. No runtime replay reproducer or snapshot-boundary liveness proof was
run here; do not confuse the static omission with a measured desync frequency.

### Message Stop Byte

`cChrMessage+14/1` is also omitted and is **not padding**. Constructor
`1400301B0` writes zero at instruction `1400301D4`. Accessor `140030A10`
is exactly `MOVZX EAX,byte ptr [RCX+14]; RET`. Both action queue
`140030A60`, impulse queue `140030AB0`, and dispatcher `1400303C0` early-out
when it is nonzero. Its misleading accessor name
`cActorGameModelAnm__getCmdStop` does not change that receiver-relative access.

Absolute bytes: `2C64`, `2C94`, `2CC4`, `2CF4`, `2D24`, `2D54`.
This is confirmed **consumed but unrestored storage**, but this pass did not
establish a runtime writer beyond construction. Treat it as an unresolved
mutable-state risk, not a second proven dynamically changing desync cause.
The other three bytes at relative `+15..17` remain unknown.

### Already Reported Command Work

`cChrCmd+1A0/4 mSavedSucWork` maps to `uCharacter+28D8/4` and `+2AF0/4`.
The inspected kernel copies `+188/18`, then resumes at `+1A4`, omitting it.
This is the already-proven omission supplied by the main task, which is being
implemented separately; it is not counted as a new discovery here.

## Policy Legend

- **C**: explicit snapshot byte copy. A copy is not proof of native semantics.
- **R**: reference validation/remap/rebind, with conditions stated below.
- **L**: deliberately retained live ownership, resource, vtable or topology.
- **U**: unknown/unproven storage retained live. Not a claim of padding or safety.
- **M**: confirmed consumed gameplay bytes omitted by the inspected restore.
- **D**: delegated nested record policy; expand using the corresponding section.

All offsets/sizes below are hexadecimal. Intervals are inclusive unless written
`[start,end)`. Ordered lists with a start and stride assign one offset to **each**
listed member, in the displayed order. Array names cover every element of the
declared array; no element is silently excluded. This notation avoids hundreds
of identical policy rows without omitting names.

`C` normally means early phase. The late kernel repeats only `1518/18`,
`2AD8/18`, `3F84/18`, `4D10/100`, `6350/20`, `6430/2C`, each target-info
payload `+10/70`, `67F8/10`, `680D/1`, `6810/11`, `6828/9`, `6840/80`,
`68C4/2`, `68C8/14`, `68E0/10`, plus constraint state. Integration calls this
late phase only for matching current-team character regions. This is not a
promise of equivalent late reconstruction repair for non-roster helpers.

## Own Members: 14B0 Through 3F47

| Offset/size | Member(s) | Disposition |
|---|---|---|
| `14B0/40` | `mChrNode` | D: cChrNode; live topology |
| `14F0`, stride `4`, 16 fields | `mChrF`, `mAnmF`, `mPrgF`, `mNotF`, `mNo2F`, `mDmgF`, `mEtcF`, `mEt2F`, `mUsrF`, `mDbgF`, `mAnmFlagsBeforeActionChange`, `mPrgFlagsBeforeActionChange`, `mAnmFlagsBeforeMove`, `mPrgFlagsBeforeMove`, `mEtcFlagsBeforeMove`, `mMaterialEffectFlags` | C, entire `14F0..152F` |
| `1530/30` | `mStatus` | D: cChrStatus |
| `1560/318` | `mAbility` | D: cChrAbility |
| `1878/950` | `mAtkCtrl` | D: cAtkCtrl |
| `21C8/570` | `mInput` | D: cChrInput |
| `2738/218` | `mCmdSpatk` | D: cChrCmdSpatk contains only cChrCmd at `+0` |
| `2950/218` | `mCmdCombo` | D: cChrCmdCombo contains only cChrCmd at `+0` |
| `2B68/8` | `_pad_2b68` | U |
| `2B70/E0` | `mJointHold` | D: cChrJointHold |
| `2C50/30`, `2C80/30`, `2CB0/30`, `2CE0/30`, `2D10/30`, `2D40/30` | `mMsgMember`, `mMsgParent`, `mMsgChild`, `mMsgTarget`, `mMsgThrow`, `mMsgDamage` | D: cChrMessage; omissions above |
| `2D70/10` | `mPause` | D: cChrPause |
| `2D80/10` | `mRandom` | C: all four MtXorShift words |
| `2D90/1` | `mbRandomize` | U/live setup policy; constructor zero alone does not establish immutability |
| `2D91/3` | `_unknown_2d91` | U |
| `2D94/4` | `mMoveAfterCounter` | C |
| `2D98/4` | `mCharacterTypeId` | L by setup/type policy; not rewound |
| `2D9C`, stride `4`, 11 fields | `mId`, `mPlrId`, `mTeamId`, `mSettingId`, `mDefMemberId`, `mMemberId`, `mPreviousMemberId`, `mBody`, `mAssistType`, `mLanguage`, `mExCpu` | C, entire `2D9C..2DC7` |
| `2DC8/20` | `mpModelResources[4]` | L |
| `2DE8/28` | `mpMotionResources[5]` | L |
| `2E10/C0` | `mpWeaponModelResources[24]` | L |
| `2ED0/C0` | `mpWeaponMotionResources[24]` | L |
| `2F90/60` | `mpEffectPathLists[12]` | L |
| `2FF0/500` | `mpEffectResources[160]` | L |
| `34F0`, stride `8`, 15 pointers | `mpShotList`, `mpResAtkInfo`, `mpResAtkCollision`, `mpResBodyCollision`, `mpResBaseact`, `mpResCmdSpatk`, `mpResCmdCombo`, `mpResBaseactEasy`, `mpResCmdSpatkEasy`, `mpResCmdComboEasy`, `mpResStatus`, `mpResCpuCommon`, `mpResCpuCharacter`, `mpResProfile`, `mpResCamera` | L; last pointer at `3560` |
| `3568/40` | `mpCameraSchedulers[8]` | L |
| `35A8/10` | `mpHudNameTextures[2]` | L |
| `35B8/10` | `mpHudPortraitTextures[2]` | L |
| `35C8/10` | `mpHudAlternateFormPortraitTextures[2]` | L |
| `35D8/10` | `mpHudDamagedPortraitTextures[2]` | L |
| `35E8/8` | `mpResultPortraitTexture` | L |
| `35F0/8` | `mpCharacterSelectBodyTexture` | L |
| `35F8/8` | `mpChainSetup` | L |
| `3600/30` | `mResourceRefs[3]` | L; three cRef records, not plain scalars |
| `3630/5C0` | `mEyes` | D: outer policy below; separate eyes auditor |
| `3BF0/170` | `mCnsLook` | D: outer constraint policy below |
| `3D60/170` | `mCnsNeck` | D: same |
| `3ED0/4` | `mFaceParts` | C |
| `3ED4/4` | `_pad_3ed4` | U |
| `3ED8/20` | `mChainArray` | L: complete MtArray header |
| `3EF8/20` | `mChainColArray` | L: complete MtArray header |
| `3F18/20` | `mChainResArray` | L: complete MtArray header |
| `3F38/4` | `mWeaponNum` | C only if saved/live `mppWeapons` match |
| `3F3C/4` | `_unknown_3f3c` | U |
| `3F40/8` | `mppWeapons` | L; pointed-to weapons have separate region policies |

## Own Members: 3F48 Through 41FF

| Offset/size | Member(s) | Disposition |
|---|---|---|
| `3F48`, stride `4`, 14 fields | `mAnmNoReqAlive`, `mAnmNoReqStd`, `mAnmNoReqCmd`, `mTeamHyperActionRequestNo`, `mPartnerActionRequestNo`, `mDeltaRateAnm`, `mMotMode`, `mNextMotBlend`, `mThrowAnmNo`, `mTdownAnmNo`, `mThrowActionEventNo`, `mTdownActionEventNo`, `mThrowAnmNoEPLast`, `mTdownAnmNoEPLast` | C, through `3F7F` |
| `3F80/C` | `mEPCnt[3]` | C |
| `3F8C/4` | `mThrowFacingYaw` | C |
| `3F90/1` | `mbThrowEscapeAirVariant` | C |
| `3F91/1` | `mbRandomGuardEnabled` | C |
| `3F92/2` | `_unknown_3f92` | C, semantics unknown |
| `3F94/4` | `mPendingDhcActionNo` | C |
| `3F98/100` | `mAnmNoCombo[64]` | C, including last two entries at `4090/4094` |
| `4098/1` | `mbExplicitPushCollisionSet` | C |
| `4099/3` | `_unknown_4099` | C, semantics unknown |
| `409C/4` | `mRequestedReserveIndex` | C |
| `40A0/10` | `_unknown_40a0` | U; constructor clears are not scalar safety proof |
| `40B0/10`, `40C0/10`, `40D0/10` | `mPos`, `mMove`, `mMoveAux` | C, including MtVector3 fourth-word gap |
| `40E0`, stride `4`, 24 fields | `mFSpd`, `mFAcl`, `mYSpd`, `mYAcl`, `mOtherYSpd`, `mOtherYAcl`, `mDSpd`, `mDAcl`, `mBSpd`, `mBAcl`, `mWSpd`, `mWAcl`, `mJSpd`, `mJDir`, `mISpd`, `mIAcl`, `mIDir`, `mFlyHSpd`, `mFlyVSpd`, `mFlyAcl`, `mFlyDir`, `mFlySpdMax`, `mDeferredYAcl`, `mDeferredFAcl` | C, through `413F` |
| `4140`, stride `4`, 12 fields | `mPushDirectionOverride`, `mNDmgTimer`, `mNTDownTimer`, `mStiffWait`, `mStiffWaitRes`, `mStiffDmg`, `mStiffGuard`, `mStiffUkemi`, `mStiffDown`, `mHitCount`, `mHitAirCount`, `mHitAirCount2` | C, through `416F` |
| `4170`, stride `4`, 16 fields | `mHitAirCountAerial`, `mHitAnmCount`, `mHitAnmCount2`, `mHitAnmSerial`, `mHitTotal`, `mHitTimer`, `mHitAfterTimer`, `mHitAfterTimer2`, `mChainCount`, `mJCancelOrder`, `mJumpCount`, `mAirDash`, `mAirSpatk`, `mNoActTimer`, `mNAdvGTimer`, `mNAssistTimer` | C, through `41AF` |
| `41B0`, stride `4`, 15 fields | `mSpArmorColorCount`, `mBoneCount`, `mGravFreeCount`, `mWinWaitTime`, `mBlockNgTime`, `mBlockNgTimeMax`, `mBlockOkTime`, `mBlockOkTimeMax`, `mBlockOkTimeMaxDefault`, `mBlockOkTimeMaxN`, `mBlockOkTimeMaxGDefault`, `mBlockOkTimeMaxGN`, `mBlockingTimer`, `mBlockingTimerInit`, `mBlockingKind` | C, through `41EB` |
| `41EC/1` | `mbChangeBlockingTime` | C |
| `41ED/3` | `_unknown_41ed` | U |
| `41F0/4`, `41F4/4` | `mSAStopTimer`, `mSAStopTimerInit` | C |
| `41F8/1` | `mbReserveUsesGlobalActionScale` | C |
| `41F9/3` | `_unknown_41f9` | U |
| `41FC/4` | `mTeamMaterialEffectState` | C |

## Own Members: 4200 Through 68EF

| Offset/size | Member(s) | Disposition |
|---|---|---|
| `4200/8` | `mpCapsuleCollisionNode` | L; external clone/region policy delegated |
| `4208/8` | `_unknown_4208` | U |
| `4210/B00` | `mHitPrimCapsules[16]` | D: explicit primitive fields and parent reference policy below |
| `4D10/100` | `mCapsulePositionCache[16]` | C, early and late |
| `4E10/8` | `mpSphere0CollisionNode` | L; external clone/region policy delegated |
| `4E18/8` | `_unknown_4e18` | U |
| `4E20/700` | `mHitPrimSpheres0[16]` | D: sphere policy |
| `5520/8` | `mpSphere1CollisionNode` | L; external clone/region policy delegated |
| `5528/8` | `_unknown_5528` | U |
| `5530/700` | `mHitPrimSpheres1[16]` | D: sphere policy |
| `5C30/8` | `mpPushCollisionNode` | L; external clone/region policy delegated |
| `5C38/8` | `_unknown_5c38` | U |
| `5C40/700` | `mHitPrimSpheres2[16]` | D: sphere policy |
| `6340/10`, `6350/10` | `mPendingPushCorrection`, `mPreviousPushCorrection` | C |
| `6360/10` | `mCollisionSetIndices[4]` | C |
| `6370`, stride `4`, 6 fields | `mComboDmgRate`, `mComboGaugeRate`, `mHVRDmgRate`, `mDHCDmgRate`, `mLastHDmg`, `mLastHAnmNo` | C |
| `6388/C` | `mLastAttackFlags[3]` | C |
| `6394/4`, `6398/4`, `639C/4` | `mLastDDmg`, `mLastDComboRate`, `mLastDAnmNo` | C |
| `63A0/C` | `mLastDamageAttackFlags[3]` | C |
| `63AC/4` | `mTimeSinceLastDamageContact` | C |
| `63B0/1` | `mBaseActionChainLatch` | C |
| `63B1/3` | `_unknown_63b1` | C, semantics unknown |
| `63B4`, stride `4`, 29 fields | `mDmgSta`, `mDmgPowLv`, `mDmgPoint`, `mDmgMaterial`, `mDmgDir`, `mAirTime`, `mAirTimeOfs`, `mAirDist`, `mAirSpeed`, `mAirFallAcl`, `mDmgHitCount`, `mDmgAirHitCount`, `mDmgHeap`, `mPreviousDamageHeap`, `mDmgTime`, `mDmgAirTime`, `mComboDmgTotal`, `mStunCharge`, `mSameTdownNum`, `mSpArmorTimer`, `mSpArmorDmgRate`, `mSpArmorShield`, `mGuardType`, `mUkemiType`, `mDownEscType`, `mCHitType`, `mThrowEscType`, `mAdvGType`, `mVCounterType` | C, through `6427` |
| `6428/8` | `mpLastCreateShot` | R: shot reference restoration/remapping calls; fallback clears abandoned/invalid references, not scalar copied |
| `6430/4` | `mCurrentShotGroup` | C |
| `6434/20` | `mShotGroupAnmSerial[8]` | C |
| `6454/8` | `mbShotGroupHit[8]` | C |
| `645C/4` | `_unknown_645c` | U |
| `6460/28` | `mShotHistoryLock` | L: complete native critical section |
| `6488/8` | `_unknown_6488` | U |
| `6490/80`, `6510/80`, `6590/80`, `6610/80`, `6690/80` | `mTargetChrInfo`, `mAtkChrInfo`, `mDmgChrInfo`, `mThrowChrInfo`, `mParentChrInfo` | D: cChrTargetInfo |
| `6710/8`, `6718/8`, `6720/8`, `6728/8`, `6730/8` | `mpLastAttackTarget`, `mpLastDamageAttacker`, `mpThrowTarget`, `mpThrowSource`, `mpThrowPairChr` | R: saved equals live or current-team identity accepted; other saved identities become null; saved null becomes null |
| `6738/4` | `mSpecialCallbackCount` | L: paired with live callback storage |
| `673C/4` | `_unknown_673c` | U |
| `6740/8` | `mppSpecialCallbacks` | L |
| `6748/A8` | `mpAuxiliaryUnits[21]` | L in common kernel; delegated shot/effect reference recipes may repair selected slots; blanket completeness not established here |
| `67F0/8` | `mpCpuController` | L: CPU capture/restore removed at user request; controller remains live |
| `67F8/4`, `67FC/4` | `mShotDefenseType`, `mShotMutekiType` | C |
| `6800/4`, `6804/4`, `6808/4` | `mSoundVariantGroup2`, `mSoundVariantGroup3`, `mSoundVariantSpecial` | C; last selector early-only |
| `680C/1` | `mbPendingSpecialCommandVoiceReset` | C, early-only |
| `680D/1` | `mbJointTranslationWrittenThisMove` | C, early and late |
| `680E/2` | `_unknown_680e` | U |
| `6810/10` | `mThrowInputDamageCooldowns[4]` | C |
| `6820/1` | `mIsBoneDraw` | C |
| `6821/7` | `_unknown_6821` | U |
| `6828/4`, `682C/4` | `mProfileSelectionKind1`, `mProfileSelectionKind2` | C |
| `6830/1` | `mbUseLocalMatrixOverride` | C |
| `6831/F` | `_unknown_6831` | U |
| `6840/40`, `6880/40` | `mLocalMatrixOverride`, `mLocalMatrixPostTransform` | C: complete MtMatrix payloads |
| `68C0/4` | `_unknown_68c0` | U |
| `68C4/1`, `68C5/1` | `mbThrowLinkValidationPending`, `mbThrowVictimHeightAdjusted` | C |
| `68C6/2` | `_unknown_68c6` | U |
| `68C8`, stride `4`, 5 fields | `mThrowSourceInitialY`, `mThrowVictimInitialY`, `mShortChargeSPCRate`, `mShotLifetimeMultiplier`, `mStiffAdjustParam` | C |
| `68DC/4` | `_pad_68dc` | U |
| `68E0/10` | `mMaterialEffectVector68E0[4]` | C |

## cChrNode And Status

`cChrNode`, size `40`, at character `14B0`: every member is retained live.

| Relative offset/size | Member | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpChr` | L, embedded node's character identity |
| `10/8`, `18/8` | `mpNext`, `mpPrev` | L, topology |
| `20/8`, `28/8` | `mpFatherNext`, `mpFatherPrev` | L, topology |
| `30/8`, `38/8` | `mpParent`, `mpChild` | L, topology/relationships |

There are no layout gaps. `140030C90` traverses and repairs linked references,
supporting the no-raw-copy topology rule. That does not prove leaving historical
parent/child relationships live is deterministic. Message dispatcher
`1400303C0` case 3 walks child relationships; helper lifecycle reconstruction
is therefore a gameplay concern, not merely render ownership.

`cChrStatus`, size `30`, at `1530`:

| Relative offset/size | Member(s) | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpChr` | L |
| `10/8` | `mpModeResourceMap` | L |
| `18`, stride `4`, 6 fields | `mMode`, `mLifeMax`, `mLife`, `mLifeAdd`, `mRedLife`, `mRedLifeAdd` | C (`1548/18`) |

No gaps. The resource-map label does not by itself prove immutability or
resource-replacement equivalence.

## cChrAbility And Prim

`cChrAbility`, size `318`, at `1560`:

| Relative offset/size | Member | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpChr` | L |
| `10/300` | `mPrim[6]` | D: six inline records, stride `80`, bases `1570 + i*80` |
| `310/1` | `mbUpdatedThisMove` | C (`1870/1`) |
| `311/7` | `unknown_311` | U (`1871..1877`) |

`cChrAbility__Prim`, size `80`, every element:

| Relative offset/size | Member(s) | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08`, stride `4`, 19 fields | `mFlag`, `mType`, `mCount`, `mStartAnmNo`, `mAblF`, `mAb2F`, `mLifeAdd`, `mLifeSub`, `mGaugeAdd`, `mRLifeAdd`, `mLifeMaxRate`, `mGaugeAddRate`, `mRLifeAddRate`, `mAtkRate`, `mDmgRate`, `mActRate`, `mSpArmorDmgRate`, `mSpArmorShield`, `mSpArmorShieldAddChr` | C, through `53` |
| `54/10` | `mUserI[4]` | C |
| `64/10` | `mUserF[4]` | C |
| `74/4` | First four bytes of `_unknown_0074[12]` | C with payload, native purpose unknown |
| `78/8` | Last eight bytes of `_unknown_0074[12]` | L: native sound cookie, not copied |

These rows partition the entire Prim, including its apparently unnamed tail.
`rollback_restore.cpp:1658-1671` copies `+08/70`, not `+08/78`.
`140024BC0` reads the qword at `+78`, stops sound through the sound system,
then writes sentinel `0xFFFFFFFF`; restoring a historical cookie could stop an
unrelated live sound. `140024E90` consumes/decrements `mCount` and tests ability
flags/types before expiry/reset. The latter's character parameter has misleading
concrete subtype propagation; no subtype-specific semantics are inferred here.

## cAtkCtrl And TargetInfo

`cAtkCtrl`, size `950`, at `1878`:

| Relative offset/size | Member(s) | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `_unknown_0008` | U, constructor zero not enough |
| `10/8` | `mpUnit` | R: rebound to this live character |
| `18/8` | `_unknown_0018` | U |
| `20/8` | `mpResource` | L |
| `28`, stride `4`, 10 fields | `mAtkInfoId`, `mFlag`, `mTotalCombo`, `mDelayTime`, `mLifeTime`, `mCountTime`, `mEndTime`, `mTransitionElapsedTime`, `mAnmNo`, `mPendingAtkInfoId` | C (`18A0/28`) |
| `50/300` | `mTInfo[32]` | D: target bank at character `18C8` |
| `350/300` | `mTInfoShot[32]` | D: target bank at `1BC8` |
| `650/300` | `mTInfoEft[32]` | D: target bank at `1EC8` |

Each `cAtkCtrl__TargetInfo`, size `18`, in all 96 entries:

| Relative offset/size | Member | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpTarget` | R: copy saved null, saved-equals-live, or current-team character identity; otherwise retain live pointer |
| `10/4` | `mTotalHit` | C unconditionally within validated buffer |
| `14/4` | `mHitTime` | C unconditionally within validated buffer |

No entry gaps. Constructor `14000BCD0` creates three inline 32-entry banks,
not heap-backed MtArrays. Update `14000D6E0` consumes attack timers and visits
all three banks. `14000D2C0` compares exact `mpTarget` identity, enforces
repeat/hit-time limits, and increments counts; pointer/count coherence is
native gameplay behavior, not inferred from field names.

**Conditional reference gap:** in the character repair loop
(`rollback_restore.cpp:1844-1861`), an unmatched historical non-roster target
can be rejected while its historical count/time are still copied. The inspected
loop does not resolve reconstructed shot/effect identities. This is not a new
scalar omission, and external reference helpers were not audited here. It is a
priority question for reference-recipe coverage, especially `mTInfoShot` and
`mTInfoEft`; do not assert it is globally unhandled solely from this loop.

## cChrInput

Size `570`, at `21C8`. `00/8 MtObject.vftable` and `08/8 mpChr` are L.
Every byte in `10..56F` is C via character `21D8/560`:

| Relative offset/size | Member(s) |
|---|---|
| `10/4`, `14/4`, `18/4`, `1C/4`, `20/4`, `24/4` | `mPreviousRawInput`, `mRawInput`, `mPreviousDat`, `mDat`, `mTrg`, `mPop` |
| `28/80` | `mReleasedHoldTimes[32]` |
| `A8/4` | `mRep` |
| `AC/80` | `mButtonHoldTimes[32]` |
| `12C/4` | `mPre` |
| `130/80` | `mTriggerAges[32]` |
| `1B0/80` | `mPausedTriggerAges[32]` |
| `230/4` | `mSame` |
| `234/1` | `mbClearSameOnNextUpdate` |
| `235/3` | `_unknown_0235` (copied, semantics unknown) |
| `238/4` | `mBeat` |
| `23C/14` | `mBeatWindowMasks[5]` |
| `250/10` | `mConfiguredBeatMasks[4]` |
| `260/80` | `mBeatPressAges[32]` |
| `2E0/80` | `mRapidPressCounts[32]` |
| `360/200` | `mConfiguredRapidPressCounts`, all 128 ints (Ghidra display `int[4][32]`) |
| `560/4`, `564/4`, `568/4`, `56C/4` | `mOrg`, `mAct`, `mPause`, `mResolvedInputMask` |

There is no omitted input-history tail or pointer-shaped-value exception in the
current kernel. This layout inventory does not separately prove each input
label's native behavioral meaning.

## cChrCmd, Limit, BtnChrg And Line

Both subclasses have exactly one `cChrCmd` base at `+0`, size `218`, no extra
members. Apply the following to bases `2738` and `2950` independently.

| Relative offset/size | Member(s) | Policy |
|---|---|---|
| `000/8` | `MtObject.vftable` | L |
| `008/130` | `mLimit` | D: Limit below |
| `138/28` | `mBtnChrg` | D: BtnChrg below |
| `160/8` | `mpChr` | L |
| `168/8` | `mpResource` | L |
| `170/4` | `mLineCount` | C only if saved/live `mpLine` match |
| `174/4` | `_unknown_0174` | U |
| `178/8` | `mpLine` | L, current allocation retained |
| `180/8` | `mpSuccessfulEntry` | R: saved pointer copied only if saved/live resource at `168` matches |
| `188`, stride `4`, 6 fields | `mSucListId`, `mSucAnmNo`, `mSucThrowNo`, `mSucTdownNo`, `mSucDelay`, `mSucWork` | C |
| `1A0/4` | `mSavedSucWork` | M, already known; main implementation pending at baseline |
| `1A4/40` | `mLineUsageCount[64]` | C |
| `1E4/4` | `unknown_1e4` | U |
| `1E8/30` | `mLimitCallbacks[6]` | L |

`cChrCmdLimit`, size `130`, command-relative base `8`:

| Limit-relative offset/size | Member(s) | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpCommand` | L |
| `10`, stride `4`, 19 fields | `mLifeRateB`, `mLifeRateS`, `mGaugeRateB`, `mGaugeRateS`, `mRLifeRateB`, `mStatusNo`, `mStatusNoN`, `mAbility`, `mAbilityN`, `mAnmMark`, `mAnm`, `mAnmMarkOld`, `mAnmOld`, `mHeightB`, `mHeightS`, `mJumpCount`, `mAirSpatk`, `mAirSpatkEach`, `mHVRCount` | C, through `5B` |
| `5C/4` | unnamed byte array | C, semantics unknown |
| `60/4` | `mFlag` | C |
| `64`, stride `4`, 36 fields | `mChrFTst`, `mChrFNTst`, `mChrFEq`, `mAnmFTst`, `mAnmFNTst`, `mAnmFEq`, `mPrgFTst`, `mPrgFNTst`, `mPrgFEq`, `mNotFTst`, `mNotFNTst`, `mNotFEq`, `mNo2FTst`, `mNo2FNTst`, `mNo2FEq`, `mDmgFTst`, `mDmgFNTst`, `mDmgFEq`, `mEtcFTst`, `mEtcFNTst`, `mEtcFEq`, `mEt2FTst`, `mEt2FNTst`, `mEt2FEq`, `mProFTst`, `mProFNTst`, `mProFEq`, `mAblFTst`, `mAblFNTst`, `mAblFEq`, `mAb2FTst`, `mAb2FNTst`, `mAb2FEq`, `mUsrFTst`, `mUsrFNTst`, `mUsrFEq` | C, through `F3` |
| `F4/6` | `mbUserFunc[6]` | C |
| `FA/2` | `unknown_fa` | C, semantics unknown |
| `FC/18` | `mUserFuncArg0[6]` | C |
| `114/18` | `mUserFuncArg1[6]` | C |
| `12C/4` | `unknown_12c` | C, semantics unknown |

Thus command copy `+18/120` covers every Limit byte except its vtable/owner.

`cChrCmdBtnChrg`, size `28`, command-relative base `138`:

| Relative offset/size | Member | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpChr` | L |
| `10/4` | `mFlag` | C |
| `14/10` | `mButtonHoldTime[4]` | C |
| `24/4` | `unknown_24` | C, semantics unknown |

`cChrCmd__Line`, size `20`, external `mpLine[i]` allocation:

| Relative offset/size | Member | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | External-region copy if captured/accepted, not an inline command copy |
| `08/4` | `mCmdStep` | Same conditional external-region copy |
| `0C/4` | `mTime` | Same |
| `10/4` | `mChargeTime` | Same |
| `14/4` | `mPadBuf` | Same |
| `18/4` | unnamed byte array | Same, native semantics unknown |
| `1C/4` | `mFailTime` | Same |

The inspected integration records owner-pointer equality before restore,
skips replaced external regions, and preserves live line count on replacement
(`1357-1370`, `1408-1412`, `1871-1873`, `1889-1895`). There is no dedicated
line-payload case in this file: accepted ordinary regions use generic memcpy.
The **capture metadata** deciding whether every line is present and whether its
region is `skipVtable` was not inspected; these rows are conditional, not a
claim that line capture is complete. Do not report line progress as a confirmed
omission solely because it is absent from the inline kernel.

Native `1400280C0` frees/reallocates the line buffer, binds the resource/count,
constructs `20`-byte elements, then resets them. `140028D10` invokes each line
and updates success delay/charge timers. `140028240` consumes `mCmdStep`,
resets `mChargeTime`, and advances `mFailTime`. These establish the need for
external progress restoration and allocation matching; they do not prove that
pointer equality detects same-address allocation reuse.

## Joint Hold, Message And Pause

`cChrJointHold`, size `E0`, at `2B70`:

| Relative offset/size | Member(s) | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpUnit` | R: this live character |
| `10/4`, `14/4` | `mSelfJoint`, `mFlag` | C |
| `18/8` | `mpTarget` | R: saved equals live or current-team identity accepted; otherwise zero |
| `20/4`, `24/4`, `28/4`, `2C/4`, `30/4` | `mTargetJointP`, `mTargetJointA`, `mTargetJointOP`, `mTargetASymType`, `mTargetOPSymType` | C |
| `34/1`, `35/1` | `mSymmetrySelf`, `mSymmetryTarget` | C |
| `36/A` | `_unknown_0036` | C, semantics unknown |
| `40/10`, `50/10` | `mPosOfs`, `mAngleOfs` | C, including vector gaps |
| `60/40`, `A0/40` | `mTargetMtx`, `mTargetMtxOP` | C |

`cChrMessage`, size `30`, applies to all six bases listed above:

| Relative offset/size | Ghidra member / refined meaning | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpChr` | R: this live character |
| `10/4` | `mType` | Rebuilt as fixed `1..6` by instance index |
| `14/1` | First byte of unnamed `byte[4]`; message stop gate | M as consumed storage; runtime mutation unresolved |
| `15/3` | Remaining unnamed bytes | U |
| `18/4` | `mMsgF` | C |
| `1C/4` | `mChrId` | C |
| `20/4` | `mAnmNo` | C |
| `24/4` | First float of unnamed `byte[12]`; queued speed | M |
| `28/4` | Second float; queued acceleration | M |
| `2C/4` | Third float; queued direction | M |

`140056EC0` binds owner/type. `140030B10` clears only flags, selector and
animation number; it does **not** clear the impulse floats or stop gate.
`1400303C0` dispatches by type to member/parent/child/target/throw/damage
recipients and clears flags/selector after dispatch. This also disproves the
old source description of the entire `24..2F` range as a tail gap.

`cChrPause`, size `10`, at `2D70`: `00/8 MtObject.vftable` L;
`08/4 mMode` C; `0C/1 mbDisable` C; unnamed `0D/3` C with payload,
semantics unknown. No additional gaps.

## cChrTargetInfo

Size `80`. Apply to all five bases `6490 + i*80`, `i=0..4`.

| Relative offset/size | Member(s) | Policy |
|---|---|---|
| `00/8` | `MtObject.vftable` | L |
| `08/8` | `mpChr` | R: saved null, same as live, or current-team accepted; other identities remain live |
| `10/4`, `14/4`, `18/4` | `mId`, `mDefMemberId`, `mPlrId` | C subject to early reference acceptance; unconditional late copy |
| `1C/4` | unnamed byte array | Same C, semantics unknown |
| `20/10` | `mPos` | Same C, including vector gap |
| `30`, stride `4`, 16 fields | `mAngle`, `mAnmNo`, `mDmgSta`, `mChrF`, `mAnmF`, `mPrgF`, `mNotF`, `mNo2F`, `mDmgF`, `mEtcF`, `mEt2F`, `mUsrF`, `mAblF`, `mAb2F`, `mProF`, `mDbgF` | Same C, through `6F` |
| `70/4`, `74/4`, `78/4` | `mDist`, `mDistH`, `mDir` | Same C |
| `7C/1` | `mbDirNG` | Same C |
| `7D/3` | unnamed byte array | Same C, semantics unknown |

All bytes are covered by these policies. Early restore `1768-1788` gates the
payload together with `mpChr`; late kernel restores payload without that gate.
An invalid historical helper can therefore leave a live pointer paired with
historical payload. This is a reference-coherence risk, not an independently
confirmed new scalar omission.

## Containers And Math

| Type | Complete member/gap inventory | Context policy |
|---|---|---|
| `MtObject`, size `8` | `00/8 vftable` | L for inline gameplay records; external line region caveat above |
| `MtXorShift`, size `10` | `00/4 x`, `04/4 y`, `08/4 z`, `0C/4 w`; no gaps | C at `2D80`; reconstruction saves/restores owner RNG separately so factories do not consume simulation RNG |
| `MtArray`, size `20` | `00/8 MtObject.vftable`, `08/4 mLength`, `0C/4 mBufsiz`, `10/1 mAutoDelete`, `11/7 unknown_11`, `18/8 mpArray` | All L for the three chain headers. Unknown gap retained. Do not copy count/capacity separately from allocation ownership. Pointee completeness not established by this header audit. |
| `cRef`, size `10` | `00/8 vftable`, `08/8 mValue`; no gaps | Both L for all three resource refs. `ulonglong mValue` is not proof it is rewindable scalar state; native sound code also uses cRef wrappers. |
| `MtVector3`, size `10` | `00/4 x`, `04/4 y`, `08/4 z`, four unnamed bytes `0C..0F` | All C wherever the enclosing gameplay copy covers `10` bytes. Fourth word's semantics remain unknown, not universally a `w` coordinate. |
| `MtMatrix`, size `40` | `00/40 m`, 16 floats (Ghidra display `float[4][4]`); no gaps | C for the four gameplay matrices in joint hold/local overrides |
| `MtQuaternion`, size `10` | `00/4 x`, `04/4 y`, `08/4 z`, `0C/4 w`; no gaps | POD layout only; no direct own-member instance in this scope outside delegated types |
| Raw scalar arrays | `float[4]`, `int[4]`, etc. contain all declared scalar elements, no extra member gaps | Follow enclosing member policy, not a pointer scan |

`MtVector4` was not found under that name in the current Ghidra database; no
invented layout is substituted for it. The actual common material vector is a
`float[4]`, and the actual position vectors are `MtVector3`.

## Delegated Outer Records

These rows cover every byte of the own outer members assigned to other auditors.
They do not certify those auditors' native conclusions.

| Outer record | Exact policy observed in the two source files |
|---|---|
| `mEyes`, `3630..3BEF` | C `3638..369B`; L/U `3630..3637` and `369C..3BEF`. In particular material controller `36A0..3BEF` is not copied as a POD array. |
| Each constraint, bases `3BF0`, `3D60`, size `170` | C relative `68..77`, `80..8F`, `90..A7`, `B8..D7`, `E0..FF`, `108..117`, `120..127`, `130..16F`. R `A8/8` for null/current-team target; `100/8` for either local embedded limit address (null not explicitly accepted); `118/8` for null/either embedded helper. Remaining bytes L/U, including inherited/vtable/owner/callback intervals. No write beyond `+16F`. |
| Each capsule, base `4210+i*B0`, `i=0..15` | C relative `08/1`, `10/4`, `20/24`, `50/4`, `68/C`, `80/20`, early and late. Parent reference slots repaired using captured character/weapon identity by `RestoreCharacterCollisionParentReferences`; exact slots are supplied by capture metadata. All other bytes live unless that reference recipe covers them; vtables remain live, untyped gaps unresolved. |
| Each sphere, base `4E20+i*70`, `5530+i*70`, `5C40+i*70` | C relative `08/1`, `10/4`, `20/10`, `30/4`, `40/8`, `50/10`, early. Parent reference recipes as above. Other bytes L/U. The late sphere loop at `2012-2019` performs bounds checks but no scalar writes. |

For collision references, the restore routine accepts None, this Character, or a
validated current Weapon index and writes to the captured field offset; it does
not blindly restore historical parent addresses. No full collision-member
semantic claim is made by this outer partition.

## Remaining Desync Risks

The message impulse is the additional confirmed omission from this pass.
Other risks remain distinct from proof of an omitted mutable scalar:

- Message `+14` stop state is consumed but its runtime writer/lifetime remains unresolved.
- Attack target references and cChrTargetInfo payloads can be repaired under different acceptance conditions; non-roster reconstruction coverage needs a dedicated reference audit.
- `cChrNode` topology is live but native message routing consumes it. Preserving topology avoids corruption; it does not recreate historical helper relationships.
- Line-buffer or resource replacement deliberately suppresses restoration. This is a safe ownership policy, not deterministic reconstruction of historical command progress.
- Same-address/same-vtable allocation reuse is not excluded by equality guards.
- Outer own unknowns retained live are `2B68/8`, `2D91/3`, `3ED4/4`, `3F3C/4`, `40A0/10`, `41ED/3`, `41F9/3`, `4208/8`, `4E18/8`, `5528/8`, `5C38/8`, `645C/4`, `6488/8`, `673C/4`, `680E/2`, `6821/7`, `6831/F`, `68C0/4`, `68C6/2`, `68DC/4`; nested unknowns and copied unknowns are enumerated in their own rows.
- No native semantics were established merely from a scalar-looking name, constructor zero, or lack of a scalar-search hit. Scoped Ghidra searches for selected gaps returned no matches; this is not an exhaustive access proof because aliases, alternate function names and indexed addressing can hide accesses.

## Verification Boundary

The inventory is cross-checked against direct Ghidra structure sizes and all
explicit writes in the two inspected restore files. It accounts for the entire
own interval and each requested gameplay type, including retained and copied
unknown storage. Native evidence independently establishes the new message
payload issue. No build was needed or run for this documentation-only change;
no game replay or determinism test was performed. Capture helpers and external
reference implementations were intentionally outside this no-codebase-search
research pass.
