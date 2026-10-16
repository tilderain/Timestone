#pragma once

#include <cstddef>
#include <cstdint>

// Override to 0 at compile time for a profiling-free build.
#ifndef UMVC3_ROLLBACK_PROFILING
#define UMVC3_ROLLBACK_PROFILING 0
#endif
inline constexpr bool ROLLBACK_PROFILING_ENABLED = UMVC3_ROLLBACK_PROFILING != 0;
inline constexpr size_t ROLLBACK_UNIT_CATEGORY_COUNT = 64;
inline constexpr size_t ROLLBACK_UNIT_OBJECT_TYPE_COUNT = 512;

enum class RollbackUnitProfilePhase : uint8_t {
    Move,
    SyncJob,
    SyncSave,
    Count
};

enum class RollbackHookProfileId : uint8_t {
    ButtonRemap,
    GetCurrentInput,
    StepIntro,
    StepFighting,
    ProcessFrameInput,
    PadSlotRead,
    PadPollAll,
    XInputToMask,
    PadPostRead,
    InputAutoRepeat,
    ShotSpawn,
    ShotSpawnEx,
    EffectCreate,
    BuildAttachMatrix,
    ShotUpdateHitboxes,
    KingKobunMove,
    UnitRemove,
    UnitRemoveRecipeLookup,
    UnitRemoveShotDetach,
    UnitRemoveEffectCheck,
    UnitRemoveEffectSanitize,
    UnitRemoveEffectResource,
    UnitRemoveEffectGenerator,
    UnitRemoveEffectSubBuffers,
    UnitRemoveNative,
    UnitRemoveRecipeCleanup,
    GameEffectAttachmentUpdate,
    RenderEndFrame,
    SoundMove,
    GameUiMove,
    ChrCpuClassFunc,
    MainMove,
    ModelBoundsFromJoints,
    CollisionMove,
    HitSolverMove,
    Count
};

struct RollbackUnitObjectProfile {
    uint64_t dti = 0;
    uint32_t category = 0;
    RollbackUnitProfilePhase phase = RollbackUnitProfilePhase::Move;
    uint32_t visits = 0;
    uint64_t ticks = 0;
    uint64_t maxTicks = 0;
    uint32_t calls = 0;
};

struct RollbackProfile {
    int32_t startFrame = 0;
    int32_t endFrame = 0;
    uint64_t total = 0;
    uint64_t patchEnable = 0;
    uint64_t inputHistorySave = 0;
    uint64_t restoreTotal = 0;
    uint64_t restoreSetup = 0;
    uint64_t restoreRegions = 0;
    uint64_t restoreCollisionClones = 0;
    uint64_t restoreOwnerPointers = 0;
    uint64_t restoreHitSolver = 0;
    uint64_t restoreStalePointers = 0;
    uint64_t restoreEffectSlots = 0;
    uint64_t restoreShots = 0;
    uint64_t restoreShotPinResources = 0;
    uint64_t restoreShotPreSanitize = 0;
    uint64_t restoreShotRetireEffects = 0;
    uint64_t restoreShotCollect = 0;
    uint64_t restoreShotDestroy = 0;
    uint64_t restoreShotPostSanitize = 0;
    uint64_t restoreShotSanitizeEffects = 0;
    uint64_t restoreShotRespawnSetup = 0;
    uint64_t restoreShotReferenceCache = 0;
    uint64_t restoreShotRespawn = 0;
    uint64_t restoreShotRespawnPrepare = 0;
    uint64_t restoreShotRespawnFactory = 0;
    uint64_t restoreShotRespawnRegister = 0;
    uint64_t restoreShotRespawnReleaseEffects = 0;
    uint64_t restoreShotRespawnConcrete = 0;
    uint64_t restoreConcreteValidate = 0;
    uint64_t restoreConcreteCopyBody = 0;
    uint64_t restoreConcretePointerScan = 0;
    uint64_t restoreConcreteEmbeddedLive = 0;
    uint64_t restoreConcreteEmbeddedSaved = 0;
    uint64_t restoreConcreteApplyBody = 0;
    uint64_t restoreConcreteTargets = 0;
    uint64_t restoreConcreteCollision = 0;
    uint64_t restoreShotRespawnOverlay = 0;
    uint64_t restoreShotFinalize = 0;
    uint64_t restoreCharacterShotReferencesEarly = 0;
    uint64_t restoreCharacterShotReferencesLate = 0;
    uint32_t restoreCharacterShotReferenceCount = 0;
    uint64_t restoreCharacterEffectReferencesEarly = 0;
    uint64_t restoreCharacterEffectReferencesLate = 0;
    uint32_t restoreCharacterEffectReferenceCount = 0;
    uint32_t restoreEffectVisits = 0;
    uint32_t restoreEffectSanitizeCalls = 0;
    uint32_t restoreEffectGeneratorNodes = 0;
    uint32_t restoreCurrentShots = 0;
    uint32_t restoreSavedShots = 0;
    uint64_t restoreUi = 0;
    uint64_t frameInput = 0;
    uint64_t frameSnapshot = 0;
    uint64_t frameNativeTick = 0;
    uint64_t frameNativeTickMax = 0;
    uint64_t frameInputEnd = 0;
    uint64_t frameTotal = 0;
    uint64_t frameTotalMax = 0;
    uint64_t headlessTimingInput = 0;
    uint64_t headlessTimingInputMax = 0;
    uint64_t headlessEarlyDrawCollision = 0;
    uint64_t headlessEarlyDrawCollisionMax = 0;
    uint64_t headlessViewportSetup = 0;
    uint64_t headlessViewportSetupMax = 0;
    uint64_t headlessRenderBeginArea = 0;
    uint64_t headlessRenderBeginAreaMax = 0;
    uint64_t headlessAction = 0;
    uint64_t headlessActionMax = 0;
    uint64_t headlessInputDevices = 0;
    uint64_t headlessInputDevicesMax = 0;
    uint64_t headlessGameplaySetup = 0;
    uint64_t headlessGameplaySetupMax = 0;
    uint64_t headlessUnitMove = 0;
    uint64_t headlessUnitMoveMax = 0;
    uint64_t headlessCollisionHit = 0;
    uint64_t headlessCollisionHitMax = 0;
    uint64_t hitSolverInputNodes = 0;
    uint64_t hitSolverOutputNodes = 0;
    uint32_t hitSolverInputNodesMax = 0;
    uint32_t hitSolverOutputNodesMax = 0;
    uint32_t hitSolverFrames = 0;
    uint64_t headlessUnitSync = 0;
    uint64_t headlessUnitSyncMax = 0;
    uint64_t headlessSimulationTail = 0;
    uint64_t headlessSimulationTailMax = 0;
    uint64_t headlessManagerCore = 0;
    uint64_t headlessManagerCoreMax = 0;
    uint64_t headlessNetworkServices = 0;
    uint64_t headlessNetworkServicesMax = 0;
    uint64_t headlessRender = 0;
    uint64_t headlessRenderMax = 0;
    uint64_t headlessPostFrame = 0;
    uint64_t headlessPostFrameMax = 0;
    uint64_t headlessAltF4 = 0;
    uint64_t headlessAltF4Max = 0;
    uint64_t headlessTotal = 0;
    uint64_t headlessTotalMax = 0;
    uint64_t inputHistoryRestore = 0;
    uint64_t timingResync = 0;
    uint64_t cutinRepair = 0;
    uint64_t patchDisable = 0;
    uint64_t saveTotal = 0;
    uint64_t saveBuildRegions = 0;
    uint64_t saveRecycleAndAllocate = 0;
    uint64_t saveCopyRegions = 0;
    uint64_t saveMetadata = 0;
    uint64_t saveCollisionClones = 0;
    uint64_t saveShotRecipes = 0;
    uint64_t saveCharacterShotReferences = 0;
    uint64_t saveCharacterEffectReferences = 0;
    uint64_t snapshotBytes = 0;
    uint64_t snapshotRegions = 0;
    uint32_t snapshotCount = 0;
    uint32_t snapshotCloneWrappers = 0;
    uint32_t snapshotCloneChildren = 0;
    uint32_t snapshotCloneCopies = 0;
    uint32_t snapshotCloneEmptyCopies = 0;
    uint32_t snapshotCloneCreates = 0;
    uint32_t snapshotCloneDestroys = 0;
    uint32_t snapshotShotRecipeCount = 0;
    uint32_t snapshotCharacterShotReferenceCount = 0;
    uint32_t snapshotCharacterCollisionParentReferenceCount = 0;
    uint32_t snapshotCharacterCollisionParentNoneCount = 0;
    uint32_t snapshotCharacterCollisionParentCharacterCount = 0;
    uint32_t snapshotCharacterCollisionParentWeaponCount = 0;
    uint32_t snapshotCharacterCollisionParentUnclassifiedCount = 0;
    uint32_t snapshotCharacterEffectReferenceCount = 0;
    uint32_t snapshotCharacterEffectReferenceNoneCount = 0;
    uint32_t snapshotCharacterEffectReferenceReconstructedCount = 0;
    uint32_t snapshotCharacterEffectReferenceRetainedCount = 0;
    uint64_t unitCategoryTicks[(size_t)RollbackUnitProfilePhase::Count]
                              [ROLLBACK_UNIT_CATEGORY_COUNT] = {};
    uint64_t unitCategoryMaxTicks[(size_t)RollbackUnitProfilePhase::Count]
                                 [ROLLBACK_UNIT_CATEGORY_COUNT] = {};
    uint32_t unitCategoryCalls[(size_t)RollbackUnitProfilePhase::Count]
                               [ROLLBACK_UNIT_CATEGORY_COUNT] = {};
    RollbackUnitObjectProfile unitObjectTypes[ROLLBACK_UNIT_OBJECT_TYPE_COUNT] = {};
    uint32_t unitObjectTypeCount = 0;
    uint64_t hookTicks[(size_t)RollbackHookProfileId::Count] = {};
    uint64_t hookMaxTicks[(size_t)RollbackHookProfileId::Count] = {};
    uint32_t hookCalls[(size_t)RollbackHookProfileId::Count] = {};
};

extern RollbackProfile* g_activeRollbackProfile;

uint64_t Profile_NowEnabled();
double Profile_Ms(uint64_t ticks);
void Profile_BeginRollbackEnabled(RollbackProfile& profile, int32_t startFrame,
                                  int32_t endFrame);
void Profile_EndRollbackEnabled(RollbackProfile& profile, uint64_t profileStart);
void Profile_AddTicksEnabled(uint64_t RollbackProfile::*field, uint64_t ticks);
void Profile_AddElapsedEnabled(uint64_t RollbackProfile::*field, uint64_t start);
void Profile_AddFramePhaseEnabled(uint64_t RollbackProfile::*field,
                                  uint64_t& checkpoint);
void Profile_AddFramePhaseMaxEnabled(uint64_t RollbackProfile::*field,
                                     uint64_t RollbackProfile::*maximum,
                                     uint64_t& checkpoint);
void Profile_FinishFrameEnabled(uint64_t start);
void Profile_AddHeadlessPhaseEnabled(uint64_t RollbackProfile::*field,
                                     uint64_t RollbackProfile::*maximum,
                                     uint64_t& checkpoint);
void Profile_FinishHeadlessEnabled(uint64_t start);
uint64_t Profile_HookBegin();
void Profile_HookEnd(RollbackHookProfileId id, uint64_t start);
void Profile_RecordUnitCategory(RollbackUnitProfilePhase phase, uint32_t category,
                                 uint64_t ticks);
void Profile_RegisterUnitObjectType(RollbackUnitProfilePhase phase, uint32_t category,
                                    uint64_t dti);
void Profile_RecordUnitObject(RollbackUnitProfilePhase phase, uint32_t category,
                               uint64_t dti, uint64_t ticks);
void Profile_RecordSnapshotEnabled(uint64_t profileStart, uint64_t buildRegionsEnd,
                                   uint64_t allocationEnd, uint64_t copyRegionsEnd,
                                   uint64_t snapshotMetadataEnd, uint64_t collisionClonesEnd,
                                   uint64_t shotRecipesEnd, size_t bytes, uint32_t regions);
void Profile_DumpEnabled(const RollbackProfile& profile, bool multiResimEnabled);
void Profile_DumpPostResimEnabled(int32_t frame, uint64_t resolveTicks,
                                   uint64_t saveSnapshotTicks,
                                   uint64_t totalTicks);

inline constexpr bool Profile_Enabled()
{
    return ROLLBACK_PROFILING_ENABLED;
}

inline uint64_t Profile_Now()
{
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        return Profile_NowEnabled();
    return 0;
}

inline void Profile_BeginRollback(RollbackProfile& profile, int32_t startFrame,
                                  int32_t endFrame)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        Profile_BeginRollbackEnabled(profile, startFrame, endFrame);
}

inline void Profile_EndRollback(RollbackProfile& profile, uint64_t profileStart)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        Profile_EndRollbackEnabled(profile, profileStart);
}

inline void Profile_AddTicks(uint64_t RollbackProfile::*field, uint64_t ticks)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        Profile_AddTicksEnabled(field, ticks);
}

inline void Profile_AddElapsed(uint64_t RollbackProfile::*field, uint64_t start)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        Profile_AddElapsedEnabled(field, start);
}

inline void Profile_RecordSnapshot(uint64_t profileStart, uint64_t buildRegionsEnd,
                                   uint64_t allocationEnd, uint64_t copyRegionsEnd,
                                   uint64_t snapshotMetadataEnd, uint64_t collisionClonesEnd,
                                   uint64_t shotRecipesEnd, size_t bytes, uint32_t regions)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        Profile_RecordSnapshotEnabled(profileStart, buildRegionsEnd, allocationEnd,
                                      copyRegionsEnd, snapshotMetadataEnd,
                                      collisionClonesEnd, shotRecipesEnd, bytes, regions);
    }
}

inline void Profile_Dump(const RollbackProfile& profile, bool multiResimEnabled)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        Profile_DumpEnabled(profile, multiResimEnabled);
}

inline void Profile_DumpPostResim(int32_t frame, uint64_t resolveTicks,
                                   uint64_t saveSnapshotTicks,
                                   uint64_t totalTicks)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        Profile_DumpPostResimEnabled(frame, resolveTicks, saveSnapshotTicks,
                                      totalTicks);
    }
}

template <bool Enabled>
class RollbackHookProfileScopeImpl;

template <>
class RollbackHookProfileScopeImpl<true> {
public:
    explicit RollbackHookProfileScopeImpl(RollbackHookProfileId id)
        : id_(id), start_(Profile_HookBegin()) {}
    ~RollbackHookProfileScopeImpl() { Profile_HookEnd(id_, start_); }

    RollbackHookProfileScopeImpl(const RollbackHookProfileScopeImpl&) = delete;
    RollbackHookProfileScopeImpl& operator=(const RollbackHookProfileScopeImpl&) = delete;

private:
    RollbackHookProfileId id_;
    uint64_t start_;
};

template <>
class RollbackHookProfileScopeImpl<false> {
public:
    explicit RollbackHookProfileScopeImpl(RollbackHookProfileId) {}
};

using RollbackHookProfileScope =
    RollbackHookProfileScopeImpl<ROLLBACK_PROFILING_ENABLED>;

template <bool Enabled>
class RollbackProfileTimerImpl;

template <>
class RollbackProfileTimerImpl<true> {
public:
    RollbackProfileTimerImpl() : start_(Profile_NowEnabled()), checkpoint_(start_) {}

    void Add(uint64_t RollbackProfile::*field) {
        Profile_AddFramePhaseEnabled(field, checkpoint_);
    }

    void Add(uint64_t RollbackProfile::*field,
             uint64_t RollbackProfile::*maximum) {
        Profile_AddFramePhaseMaxEnabled(field, maximum, checkpoint_);
    }

    void Finish() const {
        Profile_FinishFrameEnabled(start_);
    }

private:
    uint64_t start_;
    uint64_t checkpoint_;
};

template <>
class RollbackProfileTimerImpl<false> {
public:
    void Add(uint64_t RollbackProfile::*) {}
    void Add(uint64_t RollbackProfile::*, uint64_t RollbackProfile::*) {}
    void Finish() const {}
};

using RollbackProfileTimer = RollbackProfileTimerImpl<ROLLBACK_PROFILING_ENABLED>;

template <bool Enabled>
class RollbackHeadlessProfileTimerImpl;

template <>
class RollbackHeadlessProfileTimerImpl<true> {
public:
    RollbackHeadlessProfileTimerImpl() : start_(Profile_NowEnabled()), checkpoint_(start_) {}

    void Add(uint64_t RollbackProfile::*field, uint64_t RollbackProfile::*maximum) {
        Profile_AddHeadlessPhaseEnabled(field, maximum, checkpoint_);
    }

    void Finish() const {
        Profile_FinishHeadlessEnabled(start_);
    }

private:
    uint64_t start_;
    uint64_t checkpoint_;
};

template <>
class RollbackHeadlessProfileTimerImpl<false> {
public:
    void Add(uint64_t RollbackProfile::*, uint64_t RollbackProfile::*) {}
    void Finish() const {}
};

using RollbackHeadlessProfileTimer =
    RollbackHeadlessProfileTimerImpl<ROLLBACK_PROFILING_ENABLED>;
