# uCharacter Restore Member Audit

2026-10-20. Regression audit of the explicit-only restore. No heuristic or legacy
fallback was reintroduced. Full bodies are still captured.

## Inventory

The member inventories are split by class ownership, not by guessed scalar spans:

- [Model inheritance](uCharacter_model_member_audit.md): MtObject, cUnit, uCoord,
  uBaseModel/cache slots, uModel/Motion, and uToonModel/cToonParam through +1300.
- Actor/interpreter/share-model fields +1300..14AF are listed below.
- [Gameplay members](uCharacter_gameplay_member_audit.md): all 300 own components
  of uCharacter +14B0..68EF, including unknown gaps; expands status, ability,
  attack control, input, command/limit/charge/line, joint hold, messages, pause,
  RNG and the relevant container/value classes.
- [Embedded members](uCharacter_embedded_member_audit.md): eyes/material
  controllers/StringHandle, constraints/limits and collision geometry bases.

The inventories include explicit retain/unresolved dispositions. A complete
inventory does not mean every unknown byte has been proven irrelevant. Native
consumer verification was targeted, with timeouts recorded in the component
documents. The amendments below supersede their pre-fix source observations.

## Actor And Share Model

All offsets are relative to the character; endpoint ranges are inclusive.

| Bytes | Members | Disposition |
|---|---|---|
| 1300..1307 | mDeltaTimeCoef, mDeltaTimeLocal | Restore |
| 1308..130F | mpOwner | Keep live owner |
| 1310..1343 | mAnmNo, mAnmNoOld, mAnmNoReq, mAnmTimeReq, mAnmNoTakeon, mAnmStartFrameRequest, mAnmTime, mAnmTimePass, mAnmRate, mAnmWait, mAnmWaitTime, mAnmSerial, mWaitMotType | Restore |
| 1344..1347 | _pad_1344 | Retain; no demonstrated consumer |
| 1348..134F | mActorGameModelAnm.MtObject | Keep vtable |
| 1350..135F | mNowAnmNo, mOldAnmNo, mNextAnmNo, mCmdStop, mPause, mLoop, mbBypassAdvanceGateOnce | Restore |
| 1360..1377 | mpCommandResources[0..2] | Keep permanent resource bindings |
| 1378..137F | mpCommandResources[3] | Restore borrowed throw script binding, including null, before checking cursor compatibility. Native throw setup 140040754 calls pointer-only setter 140042460; the old 1350/98 copy restored this binding. |
| 1380..1387 | interpreter mpOwner | Keep live owner |
| 1388..138F | mAnmType, mKeyIdx | Restore with matching permanent command resources after rewinding throw slot 3; permanent resource replacement unresolved |
| 1390..13B7 | mpAnimationEntry, mpSimpleCommandDescriptor, mpSimpleCommandBlock, mpFrameKeyDescriptor, mpTimedCommandBlock | Borrowed cursors, restore only with matching command resources |
| 13B8..13BF | unnamed interpreter +70 | Retain unresolved; ctor clear alone is insufficient |
| 13C0 | mCngKey | Restore |
| 13C1..13C3 | unknown alignment bytes | Retain unresolved |
| 13C4..13D3 | mCmpS32, mCmpF32, mJmpCmdNum, mRetFlg | Restore |
| 13D4..13D7 | unknown bytes | Retain unresolved |
| 13D8..13DF | mEftRetU32 / borrowed object result | Saved null cleared; current roster owner validated; tracked shots/effects remapped by stable identity. Other results unresolved |
| 13E0..13E7 | mLoopNum, mLoopStartFrame | Restore |
| 13E8..13FF | mpAnmCmdCommon, mpAnmCmdCharacter, mpAnmCmdTdown | Keep resource owners |
| 1400..1407 | _unknown_1400 | Retain unresolved |
| 1408..143F | seven material-job array owners | Keep live allocations; their external contents have separate owner checks |
| 1440..1443 | mMaterialVjobCounts | Restore each count nibble only for matching allocation |
| 1444..144F | _unknown_1444 | Retain unresolved |
| 1450..146F | mCnsShareList (MtArray) | Keep native container/allocation ownership |
| 1470..1473 | mCnsJointNum | Keep bound with live constraint storage |
| 1474..1477 | _unknown_1474 | Retain unresolved |
| 1478..147F | mpConstraintJointWork | Keep live allocation |
| 1480..1487 | mHipRate, mHipRateOfs | Restore early and late |
| 1488..149F | mpConstraintExpressionCallback, mpExpressionJointAtCallback, mpExpressionJointCountCallback | Keep native callback infrastructure |
| 14A0..14A3 | mConstraintShareFlags | Restore |
| 14A4..14AF | _unknown_14A4 | Retain unresolved |

## Applied Corrections

1. `cChrCmd+1A0` is `mSavedSucWork`, not padding. Native `1400419C0` copies
   `mSucWork` there; action interpreter opcodes 3/4/7 consume it. Both matchers
   now restore it, at character +28D8 and +2AF0.
2. Every `cChrMessage` now restores its stop gate and complete +18/18 payload.
   Native writer `140030AB0` queues speed/acceleration/direction at +24/+28/+2C;
   dispatcher `1400303C0` applies these to recipient movement. The old +18/0C
   copy omitted them. Vtables, owners, type repair and unknown +15..17 stay separate.
3. Both constraint helpers restore the saved callback at relative `+128`,
   restricted to `14010BB40` (updateFromReference) and `14010BC20`
   (updateLockAngle), early and late. The previous setter-based repair was
   incorrect: constructor `14010B3B0` sets state zero with updateLockAngle,
   whereas setter `14010CBE0` sets state zero with updateFromReference.
   Thus mSelfState does not uniquely identify the historical callback.
   Native `14010B670` invokes the selected callback when applying constraints.
4. Native `1401079F0` writes interpreter-relative +90, or character +13D8.
   Shot capture and effect recipe capture now handle this actual object-result
   slot. Character +90 is inside uCoord::mLmat and is no longer inspected or
   cleared as a pointer by the reference-repair loop. Saved-null and roster
   owner results are handled explicitly; untracked results still need ownership
   analysis, not address-pattern inference.
5. `MtGeometry::mType` is four bytes, confirmed by ctor `14000DA80`.
   Capsule/sphere early copies and capsule late copies now use all four bytes.
   Relative +10 is MtGeomConvex::mMargin, not a kind field. Capsule +74 is
   still unapproved: its datatype name alone did not establish runtime state.
6. Eye material selectors/clocks now restore with unchanged target, model,
   material-array, controller parent and animation-handle bindings. Native
   `14054A430` and `14054A960` consume mControlAllSlot, mControlID, mFrame and
   mSlot. StringHandle/vtable/owner bytes remain live. Replacement bindings are
   not reconstructed by this change.
7. cToonParam rim-light parameters and cut-in factor restore as inline visual
   state; the vtable and unknown gap remain live. Verified writer/consumer are
   `140451410` / `140451210`. This is not claimed as a gameplay desync cause.

## Validation And Limits

### Motion Follow-Up

Fighters and weapons now call the same `Rollback_RestoreModelMotions` kernel.
It restores playback, both vector-track samplers (including their borrowed track
and segment pointers), event bindings/cursors, channel count, and the channel
descriptor at Motion +170 together. The +000 vtable, +160 joint-work owner and
+178 channel-work slice remain live. The weapon path no longer overwrites the
joint/blend allocation counts or model resource status as scalar playback state.

At the user's explicit request this is an unchecked copy routine: resource
lifetime, allocation compatibility and valid bodies are caller preconditions.
It does not validate or reconstruct changed resources. Consequently it fixes the
missing descriptor rewind but does not establish safety for resource replacement.
External buffer restoration retains its existing owner checks.

Captain America and Trish now capture empty shot-owner slots. A saved-null slot
cannot use the unique-owned-shot fallback; it must restore absence, even if a
different shot of the same class exists in the snapshot.

Tests exercise all eight Motion records through the actual shared kernel,
including repeated binding/count restoration and preservation of both live work
pointers. Native animation evaluation and resource replacement are not emulated.

The portable kernel test covers both command fields, all six message records,
toon ownership gaps, all 32 eye controller payloads, matching/replaced bindings,
and saved-null interpreter results in addition to existing tail/owner/bounds
tests. Native callback execution and actual shot/effect remapping require live
game validation; standalone copy tests cannot certify their temporal behavior.

Do not call the reported desync fixed until the reproducing replay passes. Known
open areas include resource replacement, non-roster generation-aware links,
untracked interpreter results, and the unnamed fields marked unresolved in the
inventories. No padding assumption or broad unknown-byte copy was added to hide
those gaps.
