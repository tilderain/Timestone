What It Is
cCnsHelper is the project’s recovered name for an embedded character constraint helper. Its actual runtime class appears to be uCnsLockChr, derived from uCnsShare.
Evidence:
- cCnsHelper__ctor @ 0x14010B3B0 first calls uCnsShare__ctor.
- It installs the uCnsLockChr vtable at 0x140A85720.
- Its reflected property creator is uCnsLockChr__createProperty_10C1B0.
- The complete object is 0x170 bytes.
- Every cChr embeds two instances:
  - cChr+0x3BF0: mCnsHelper
  - cChr+0x3D60: mCnsNeck
They are constructed consecutively by cChr__cChr @ 0x140054400.
Despite the names, these are not collision primitives themselves. They evaluate a constrained character joint orientation. The resulting joint/model transforms are subsequently used to place hit, hurt, and push spheres.
Purpose
The helper rotates a selected character joint so one axis points toward a target joint or target unit, subject to configurable angular limits.
Its reflected configuration includes:
- mEnable
- mPause
- mDbgDraw
- mJointNo
- mLockAxis
- mUpAxis
- mTgtJointNo
- mpTgtUnit
- mLimStand
- mLimDown
- mpLimCurrent
- mInterFrame
- mInterCurve
- mSelfState
- mpRefCnsLock
- mAngleWeight
- mAng
- mOldTgtAng
- mState
- mLength
- mOldAng
- mInterAng
- mSpeed
- mChrState
- mTgtAngle
- mPerceptionAngle
This strongly suggests the class was designed for joint-facing behavior such as torso/head tracking, with separate standing and lowered-state angular limits.
Recovered Layout
The most useful recovered portion is:
Helper offset	Character offsets	Meaning
+0x00..+0x8F	+0x3BF0, +0x3D60	Inherited uCnsShare state and ownership
+0x90	+0x3C80, +0x3DF0	mEnable
+0x91	+0x3C81, +0x3DF1	mPause, likely
+0x94	+0x3C84, +0x3DF4	mJointNo
+0x98	+0x3C88, +0x3DF8	mLockAxis
+0x9C	+0x3C8C, +0x3DFC	mUpAxis
+0xA0	+0x3C90, +0x3E00	mTgtJointNo
+0xA8	+0x3C98, +0x3E08	mpTgtUnit
+0xB0	+0x3CA0, +0x3E10	mLimStand, 0x28 bytes
+0xD8	+0x3CC8, +0x3E38	mLimDown, 0x28 bytes
+0x100	+0x3CF0, +0x3E60	mpLimCurrent
+0x108	+0x3CF8, +0x3E68	Interpolation-frame/configuration data
+0x114	+0x3D04, +0x3E74	Current output angle, reflected as mAng
+0x118	+0x3D08, +0x3E78	mpRefCnsLock
+0x120	+0x3D10, +0x3E80	mSelfState
+0x124	+0x3D14, +0x3E84	mState, interpolation state machine
+0x128	+0x3D18, +0x3E88	Native update callback
+0x130	+0x3D20, +0x3E90	Previous target/output angle
+0x134	+0x3D24, +0x3E94	Current interpolated angle
+0x140	+0x3D30, +0x3EA0	Normalized target/direction vector
+0x150	+0x3D40, +0x3EB0	mAngleWeight
+0x154	+0x3D44, +0x3EB4	mSpeed
+0x158	+0x3D48, +0x3EB8	Character-state selector
+0x15C	+0x3D4C, +0x3EBC	Target angle
+0x160	+0x3D50, +0x3EC0	Perception/threshold angle
+0x168	+0x3D58, +0x3EC8	mInterCurve, easing data
Some names in the latter part are inferred by combining the reflected property order with native reads and writes. The particularly important fields, mSelfState at +0x120 and mState at +0x124, are directly confirmed by code.
Limit Parameters
Each helper contains two uCnsLockChr::cLimitParam objects:
helper+0xB0  mLimStand
helper+0xD8  mLimDown
Each is 0x28 bytes and contains three angle-range-like records followed by:
+0x20 float mQuitLength
Constructor defaults include angular ranges corresponding approximately to:
- One range: -45 to 20
- One range: -20 to 20
- One range: -10 to 10
- mQuitLength = 1000.0
The exact semantic axis represented by each range depends on mLockAxis and mUpAxis.
Two Update Modes
mSelfState at helper +0x120 selects the update callback.
cCnsHelper__mSelfState__setter @ 0x14010CBE0 does:
helper->mSelfState = value;
helper->updateCallback =
    value == 0 ? referenceFollowUpdate : cCnsHelper__updateLockAngle;
Therefore mSelfState is not the interpolation phase. It selects the algorithm:
- mSelfState == 0: follow another uCnsLockChr through mpRefCnsLock.
- mSelfState != 0: independently calculate a target angle and interpolate it.
The callback pointer at +0x128 must remain a live native code pointer. That is why rollback does not blindly restore the entire helper.
Independent Update
cCnsHelper__updateLockAngle @ 0x14010BC20 is the main stateful algorithm.
At a high level, it:
1. Saves the previous output angle.
2. Calculates the direction from the constrained character joint to the selected target.
3. Normalizes target and reference axes.
4. Computes an angular target.
5. Chooses standing or lowered limits.
6. Advances a three-state interpolation machine.
7. Applies cUnit time scale and mSpeed.
8. Applies the configured easing curve.
9. Stores the new output angle.
10. Builds a transform matrix for the constrained joint.
The target calculation uses:
- The helper’s owning character.
- mpTgtUnit and mTgtJointNo.
- The selected target’s joint/model transform.
- The configured lock and up axes.
- Standing/down angular limits.
- Character state.
- Time scale.
- Interpolation speed and curve.
The mState Machine
The divergent field at helper +0x124 is reflected as mState.
The native function treats values 0, 1, and 2 distinctly:
- State 0: evaluates the standing-limit condition.
- State 1: actively interpolates between limit regimes.
- State 2: evaluates the alternate/down-limit condition and can begin transition back.
A simplified representation is:
switch (mState) {
case 0:
    if (targetOutsideStandingLimits)
        setState(1);
    break;
case 1:
    if (targetEnteredDownLimits || exceededQuitCondition) {
        setState(2);
        output = previousTargetAngle;
    } else {
        output = easeTowardTarget(output, target, speed, timeScale);
    }
    break;
case 2:
    if (targetEnteredStandingLimits || exceededQuitCondition)
        easeTowardStandingState();
    else
        setState(1);
    break;
}
The labels “standing” and “down” come directly from the reflected mLimStand and mLimDown fields. Exact gameplay interpretation can vary with character configuration.
Reference-Follow Update
The alternate callback at 0x14010BB40 uses mpRefCnsLock.
It:
1. Reads the referenced helper’s angle at referencedHelper+0x114.
2. Copies that angle into this helper’s current and interpolated angle fields.
3. Uses the referenced transform/direction.
4. Rebuilds this helper’s constrained joint matrix.
This lets one constraint mirror or inherit another rather than calculating an independent target.
Matrix Output
The final helper routine at 0x14010C920 constructs and writes a joint transform.
It combines:
- The calculated angle.
- A normalized direction vector.
- The selected axis configuration.
- The target or parent transform.
- A rotation matrix.
That output feeds the character’s model/joint evaluation. Collision primitives parented to those joints then obtain their world-space positions from the resulting matrices.
This is how a constraint-helper mismatch can become a gameplay collision mismatch even though cCnsHelper is not itself a collision object.
Connection To Pushboxes
The frame-866 desync exposed this chain for mCnsNeck:
cChr+0x3E84
  = mCnsNeck.mState
        |
        v
different cCnsHelper interpolation branch
        |
        v
different constrained joint transform
        |
        v
different active push-sphere world center
  cChr+0x5CD0
        |
        v
different push contact result
        |
        v
different correction vector
  cChr+0x6340
        |
        v
different character root position
  cChr+0x50
At frame 783:
Host cChr+0x3E84 = 2
Client cChr+0x3E84 = 1
The same frame showed differing sphere centers at:
cChr+0x4F20
cChr+0x5CD0
cChr+0x5C40 begins the third cHitPrimSphere bank, the bank processed by uCharacter__updatePushSpheresAndQueueCorrection. +0x5CD0 lies in an active sphere’s world-geometry payload. This is strong evidence that the affected primitive was the active airborne push sphere in that sequence, although “air pushbox” is an action/configuration role rather than a distinct C++ primitive type.
One frame later, the phase-0 push callback generated different values at cChr+0x6340, after which root position diverged.

The `20260804_040449` pair reproduced the same chain. At frame 6501, the
affected Chun-Li split at `mCnsNeck.mState` and the world centers at
`+0x4F20/+0x5CD0`, followed by push correction and root-position differences
at frame 6507. The four collision-set selectors at `cChr+0x6360..+0x636F` are:

- `+0x6360`: capsule-bank selector.
- `+0x6364`: sphere-bank-0 selector.
- `+0x6368`: sphere-bank-2 selector used by fighter push collision.
- `+0x636C`: sphere-bank-1 selector.

The affected airborne character had selector `9` at `+0x6368`; ordinary
grounded characters commonly had selector `1`. All four selectors matched
between peers from frame 6497 through frame 6507. That rules out a persistent
host/client selector difference in the captured frames, but not a transient
live-versus-restored difference before the first resimulated collision update.
The broad character restore already includes these fields; they are also
reapplied after reconstruction so native setup cannot leave bank 2 on the live
grounded/airborne set for part of the replayed tick. The first captured
pushbox-specific mismatch remains `mCnsNeck.mState` at frame 6501. This dump was
already divergent at its first common frame 6342, including different shot
topology, so an earlier dependency outside the available history may also have
driven that state transition.
Rollback Requirements
The helper cannot safely be restored with one raw memcpy because it contains:
- A vtable.
- Owner/self references inherited from uCnsShare.
- mpTgtUnit.
- mpLimCurrent, an interior pointer to one of its inline limits.
- mpRefCnsLock.
- A native update callback at +0x128.
Those fields must retain or reconstruct valid live identities.
The scalar and inline state that must rewind includes:
- Enable/pause and axis selectors.
- Both inline limit records.
- Interpolation configuration.
- mSelfState.
- mState.
- Current, old, target, and interpolated angles.
- Direction vectors.
- Speed, weight, and character-state selectors.
- Easing state.
The current restore policy therefore uses explicit pointer-free ranges and
validated pointer remapping while preserving callback and vtable fields.
The Restore Bug
Before the fix, these ranges restored only helper +0x120:
{0x3D10, 0x04} // mCnsHelper+0x120
{0x3E80, 0x04} // mCnsNeck+0x120
That restored mSelfState but omitted the immediately adjacent mState at +0x124.
They now restore both:
{0x3D10, 0x08}
{0x3E80, 0x08}
The `20260805_004907` pair showed that restoring only these interpolation
ranges was insufficient. Team 1 matched through frame 346, then split at frame
347 in `mCnsNeck.mState` (`cChr+0x3E84`, host 2/client 1) and both active sphere
centers (`+0x4F20/+0x5CD0`). The differing sphere geometry produced different
push correction at `+0x6340` on frame 348. Collision-bank selectors remained
identical.

`cCnsHelper__updateLockAngle` chooses state 1/2 from more than `mState`: it also
uses target/joint selectors, two inline `cLimitParam` records, `mpLimCurrent`,
`mpRefCnsLock`, angle history, direction vectors, and easing state. Several of
their packed scalar qwords satisfy the generic pointer heuristic, so broad
character restore retained prediction-timeline values. The old `+0x3CF0` and
`+0x3E60` four-byte copies were particularly unsafe because those offsets are
the low halves of each helper's `mpLimCurrent` pointer.

Restore now handles both embedded helpers as complete ownership-aware units:

- Explicitly restore pointer-free configuration, limit, interpolation, angle,
  direction, and easing ranges.
- Restore `mpTgtUnit` only when it is null or a current roster character.
- Remap `mpLimCurrent` only to one of the helper's two inline limit records.
- Restore `mpRefCnsLock` only when it is null or points to one of the two
  helpers embedded in the same character.
- Preserve helper/limit vtables, the update callback at `+0x128`, and unknown
  pointer identities.

The ownership-aware helper restore is reapplied after reconstruction because
model/effect repair can evaluate constraints and overwrite mutable state.
Relevant code is in:
- UMVC3Hook/code/rollback_restore.cpp:195
- UMVC3Hook/code/rollback_restore.cpp:248
Important Distinctions
- cCnsHelper is not the generic chain-collision solver.
- It is not a hitbox or pushbox object.
- It is a joint-orientation constraint whose output can move joint-parented collision primitives.
- mSelfState selects the update algorithm.
- mState is the internal standing/down interpolation phase.
- The callback pointer is process-local code identity and should not be restored from arbitrary snapshot bytes.
- The limit/current/reference pointers require ownership-aware handling rather than broad copying.
