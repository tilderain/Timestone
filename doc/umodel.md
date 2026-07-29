This is deepseek made so it may be garbage

Here's the complete uModel hierarchy layout:
Full uModel Inheritance Chain (Dragon's Dogma / MT Framework)
cUnit : MtObject                     (PPC: 48 / x86-64: 64)
  +4     u32              mBeFlag
  +4     u32              mMoveLine
  +4     u32              mUnitAttr
  +4     u32              mDrawView
  +8     u32              mDrawMode
  +12    cUnit *          mpNextUnit
  +16    cUnit *          mpPrevUnit
  +20    f32              mDeltaTime
  +24    u64              mUnitGroup
  +32    u32[]            padding
  +44    u32              mUnitParam
uCoord : cUnit                       (PPC: 240 / x86-64: 256)
  +48    uCoord *         mpParent
  +52    s32              mParentNo
  +56    u32              mOrder / mParentFlags
  +64    MtVector3        mPos
  +80    MtQuaternion     mQuat
  +96    MtVector3        mScale
  +112   MtMatrix         mLmat
  +176   MtMatrix         mWmat
uBaseModel : uCoord                  (PPC: 832 / x86-64: 1296)
  +240   rModel *         mpModel
  +244   rMaterial *      mpMaterialData
  +248   Material **      mpMaterials
  +252   u32              mMaterialNum
  +256   u32              mLightGroup
  +260   u32              mLightAttr
  +264   s32/u32          mVFCullLevel / mLODType / mDebugFlags / mShaderQuality /
                            mMaxLightNum / mShaderAttributes / mDissolveEnable / mFixPosition
                                 (union bits)
  +268   u32              mDrawPriority
  +272   u32[]            mPartsDisp          (16 × u32)
  +336   s32              mZPrepassDist
  +340   s32              mPriorityBias
  +344   f32              mTransparency
  +348   u32              mCacheMode
  +352   MtSphere          mBoundingSphere
  +368   MtOBB             mBoundingBox
  +448   MtSphere          mLightReciver
  +464   MtVector4        mAmbientMask
  +480   CommandCache *[]  mpCommandCacheDef     (4 ptrs)
  +496   u32[]            mCacheLODDef          (4 × u32)
  +512   u32[]            mCacheIdDef           (4 × u32)
  +528   CommandCache *[]  mpCommandCacheSC0     (4 ptrs)
  +544   u32[]            mCacheLODSC0
  +560   u32[]            mCacheIdSC0
  +576   CommandCache *[]  mpCommandCacheSC1     (4 ptrs)
  +592   u32[]            mCacheLODSC1
  +608   u32[]            mCacheIdSC1
  +624   CommandCache *[]  mpCommandCacheSR0     (4 ptrs)
  +640   u32[]            mCacheLODSR0
  +656   u32[]            mCacheIdSR0
  +672   CommandCache *[]  mpCommandCacheSR1     (4 ptrs)
  +688   u32[]            mCacheLODSR1
  +704   u32[]            mCacheIdSR1
  +720   s32[]            mCacheThread          (4 × s32)
  +736   u32[]            mPrevPartsDisp        (16 × u32)
  +800   u32              mPartsDispMax
  +804   bool             mbCacheInvalid
  +808   Material *[]     mpMaterialStack       (6 ptrs)
uModel : uBaseModel                  (PPC: 3664 / x86-64: 4800)
  +832   MtSphere[]       mPrevBoundingSphere
  +864   u32              mJointNum
  +868   Joint *          mJoint
  +872   const u8 *       mJointTable
  +876   u8 *             mJointDepthTable
  +880   u32              mModelStatus
  +884   u32              mBlendNum
  +888   u32              mMotionCalcFlag
  +892   u32              mMotionInterpolateAttr
  +896   Motion[]         mMotion              (8 × Motion = 2560 bytes)
  +3456  s32              mMotionBlurSetup
  +3460  f32              mMotionBlurDist
  +3464  f32              mMotionBlurLimite
  +3468  u32              mEnvelopeHandle
  +3472  u32              mEnvelopeNum
  +3476  uModel *         mpSkinMeshCollisionPtr
  +3488  MtVector3        mNullTransScale
  +3504  MtVector3        mDeltaPos
  +3520  MtQuaternion     mDeltaQuat
  +3536  InterpolationJoint * mpInterpolationJoint
  +3540  rMotionList *[]  mpMotionList         (16 ptrs = 64 bytes)
  +3604  u32              mKeyFrameNum
  +3608  bool             mShowJointNo
  +3609  bool             mUpdateJointDependencyReq
  +3610  bool             mUpdateNormal
  +3612  f32[]            mMorphWeight         (4 × f32)
  +3628  Texture *[]      mpShaderAttributes   (2 ptrs)
  +3636  Texture *        mpPositionMap
  +3640  Texture *        mpNormalMap
  +3644  Texture *        mpTangentMap
  +3648  Texture *        mpNormalSubMap
Key Differences: Dragon vs Devil4 vs Binary (umvc3)
Verification from umvc3 binary (confirmed via uModel__pickWaistAdjustCurve_10A6C0 at this+0x140):
The umvc3 binary has Devil4-style IK fields that Dragon's uModel lacks:
  +320   MtVector4/EaseCurve  mWaistAdjustCurve     (confirmed at this+0x140)
  +??    u32              mHeelHeight
  +??    bool             mAnkleAdjust
  +??    bool             mFootAdjust
  +??    f32              mWaistOffset
  +??    f32              mWaistAdjustFactor
  +??    f32              mWaistAdjustSpeed
  +??    f32              mRotInverseWeight
  +??    MtVector4        mBasePlane
  +??    u32              mLightGroup           (Devil4: +288 rel uCoord)
Architecture notes: Dragon/Devil4 are PPC (4-byte pointers, big-endian). The Ghidra binary (umvc3) is x86-64 (8-byte pointers, little-endian). Every * field grows from 4 to 8 bytes, shifting all subsequent offsets. The Ghidra struct at 4800 bytes suggests ~1300 bytes of pointer/alignment growth from the PPC 3664.