#pragma once

#include <cstddef>
#include <cstdint>

// Set true for a profiling build. The disabled API compiles entirely away.
inline constexpr bool ROLLBACK_PROFILING_ENABLED = true;

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
    KirigakureEffectUpdate,
    RenderEndFrame,
    SoundMove,
    GameUiMove,
    SchedulerRebuild,
    ChrCpuClassFunc,
    MainMove,
    Count
};

struct RollbackProfile {
    int32_t startFrame = 0;
    int32_t endFrame = 0;
    uint64_t total = 0;
    uint64_t patchEnable = 0;
    uint64_t inputHistorySave = 0;
    uint64_t diagSetup = 0;
    uint64_t preRestoreJson = 0;
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
    uint32_t restoreEffectVisits = 0;
    uint32_t restoreEffectSanitizeCalls = 0;
    uint32_t restoreEffectGeneratorNodes = 0;
    uint32_t restoreCurrentShots = 0;
    uint32_t restoreSavedShots = 0;
    uint64_t restoreUi = 0;
    uint64_t inputAvailability = 0;
    uint64_t frameInput = 0;
    uint64_t frameDiagPre = 0;
    uint64_t frameJson = 0;
    uint64_t frameSnapshot = 0;
    uint64_t frameNativeTick = 0;
    uint64_t frameNativeTickMax = 0;
    uint64_t frameInputEnd = 0;
    uint64_t frameDiagPost = 0;
    uint64_t frameTotal = 0;
    uint64_t frameTotalMax = 0;
    uint64_t inputHistoryRestore = 0;
    uint64_t timingResync = 0;
    uint64_t patchDisable = 0;
    uint64_t saveTotal = 0;
    uint64_t saveBuildRegions = 0;
    uint64_t saveRecycleAndAllocate = 0;
    uint64_t saveCopyRegions = 0;
    uint64_t saveMetadata = 0;
    uint64_t saveCollisionClones = 0;
    uint64_t saveShotRecipes = 0;
    uint64_t snapshotBytes = 0;
    uint64_t snapshotRegions = 0;
    uint32_t snapshotCount = 0;
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
uint64_t Profile_HookBegin();
void Profile_HookEnd(RollbackHookProfileId id, uint64_t start);
void Profile_RecordSnapshotEnabled(uint64_t profileStart, uint64_t buildRegionsEnd,
                                   uint64_t allocationEnd, uint64_t copyRegionsEnd,
                                   uint64_t snapshotMetadataEnd, uint64_t collisionClonesEnd,
                                   uint64_t shotRecipesEnd, size_t bytes, uint32_t regions);
void Profile_DumpEnabled(const RollbackProfile& profile, bool ndjsonEnabled,
                         bool multiResimEnabled);
void Profile_DumpPostResimEnabled(int32_t frame, uint64_t notifyTicks,
                                  uint64_t resolveTicks, uint64_t saveSnapshotTicks,
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

inline void Profile_Dump(const RollbackProfile& profile, bool ndjsonEnabled,
                         bool multiResimEnabled)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED)
        Profile_DumpEnabled(profile, ndjsonEnabled, multiResimEnabled);
}

inline void Profile_DumpPostResim(int32_t frame, uint64_t notifyTicks,
                                  uint64_t resolveTicks, uint64_t saveSnapshotTicks,
                                  uint64_t totalTicks)
{
    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        Profile_DumpPostResimEnabled(frame, notifyTicks, resolveTicks,
                                     saveSnapshotTicks, totalTicks);
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
