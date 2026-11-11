#pragma once

#include <cstdint>
#include <atomic>

// Backend timing detours (input, render, network, Present). They only record during a lag capture.
inline constexpr bool LAG_BACKEND_TIMING_ENABLED = true;
// Automatic battle-start capture and exit; suppresses desync handling during the test.
inline constexpr bool lagTest = false;
// Experimental: force the native SLI setting on; false respects game configuration.
inline constexpr bool FORCE_NATIVE_SLI_ON = true;

enum class LagMetric : unsigned {
    Frame, Native, Pacing, InputWait, BenchmarkUpdate, BenchmarkBegin,
    BenchmarkEnd, BenchmarkDraw, SteamRoute, EndScene, StatusHud, CharacterHud,
    EscapeHud, ImGuiSubmit, ReportQueue,
    ControllerStart, ControllerEnd, PacketDrain, PredictionWait, Checksum,
    MainHook, FrameTail, SaveLive, SaveResim, SaveRegions, SaveAllocate, SaveCopy,
    SaveClones, SaveParents, SaveRenderState, SaveLifetimeDrop, SaveLifetime, SaveEffects,
    SaveUnitMembers, SaveUnitLists, SaveUnitEffects, SaveUnitEffectCheck, SaveUnitEffectLeases,
    SaveUnitShotLeases, SaveUnitWorld, SaveUnitShotCopy, SaveUnitGenerations, SaveUnitConstraints,
    SnapDump, Restore, RestoreSetup, RestoreRegions, RestoreClones, RestoreFixups,
    RestoreWithdraw, RestoreLate, Correction, ResimFrame, ResimTick,
    RestoreEffects, RestoreShots, RestoreUnitRefs, RebuildSpawn, RebuildFinalize,
    OverlaySetup, OverlayLogic, ImGuiFinalize, HookLog, ProfileDump, RenderEnd, RenderBegin, WindowFocus,
    RenderMove, UnitMove, NativeInput, SoundMove,
    DrawCache, CameraDraw, UnitDraw, DrawFrameCache, Jobs, JobDispatch, UnitSync, UnitAfter,
    SceneSort, EffectMove, ParticlePre, ParticlePost, CameraMove, GraphicsUpdate, AiAfter,
    PadPoll, PadRead, PadDetect, XInputState, XInputCaps,
    XInputDiscoveryState, XInputDiscoveryCaps, DirectInputFind,
    DirectInputRead, InputWmi, RenderWorkerWait, RenderInlinePlayback,
    RenderPlayback, RenderGpuFence, BackendPresent, RenderRetire, RenderCapture,
    KeyboardMove, KeyboardState, MouseMove, MouseState,
    NetComponent, NativeNetwork, AdapterQuery, NetContextDestruct,
    ShaderMove, ShaderTable, GameConfigMove, GameUiMove, UiScreenCapture,
    CollisionBegin, CollisionMove, GameSoundMove, ShadowMove, CaptureMove, StageMove,
    OverlayAdmission, OverlayInit, OverlayCapture, OverlayKeyboardUpdate,
    OverlayDx9Frame, OverlayWin32Frame, OverlayRenderTarget, OverlayKeyboardPoll,
    OverlayImGuiFrame, OverlayWin32Size, OverlayWin32Clock, OverlayWin32Mouse,
    OverlayWin32Cursor, OverlayWin32Gamepad,
    KeyboardPoll, KeyboardAcquire, KeyboardAcquireRetry, KeyboardGetState,
    MousePoll, MouseAcquire, MouseAcquireRetry, MouseGetState,
    NativeMouseRect, NativeMouseCursor, NativeMouseClient,
    OverlayMouseWarp, OverlayMouseForeground, OverlayMouseChild, OverlayMouseCursorPos, OverlayMouseClient,
    OverlayMouseToScreen, Count
};
enum class LagEvent : unsigned {
    BenchmarkReset, LiveSample, CorrectionSample, BenchmarkClock,
    NativeOutsideFrame, FrameReentry, OverlayOrder, ScopeOverflow,
    RenderBatches, RenderJobs, RenderWorkers, SceneDrawables, Count
};

// Initially true for lazy configuration; false between opt-in captures.
extern std::atomic<bool> g_lagDiagnosticsMayBeEnabled;
uint64_t LagDiagnostics_BeginSlow(LagMetric metric, int32_t frame);
void LagDiagnostics_EventSlow(LagEvent event);
void LagDiagnostics_CountSlow(LagEvent event, uint64_t count);
inline void LagDiagnostics_Count(LagEvent event, uint64_t count) {
    if (g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed))
        LagDiagnostics_CountSlow(event, count);
}
void LagDiagnostics_RecordWaitSlow(LagMetric metric, int32_t required, int32_t remoteBefore,
                                  int32_t remoteAfter, int32_t delay, int32_t rollback);
inline void LagDiagnostics_RecordWait(LagMetric metric, int32_t required, int32_t remoteBefore,
                                     int32_t remoteAfter, int32_t delay, int32_t rollback) {
    if (g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed))
        LagDiagnostics_RecordWaitSlow(metric, required, remoteBefore, remoteAfter, delay, rollback);
}
inline uint64_t LagDiagnostics_Begin(LagMetric metric, int32_t frame = -1) {
    if (!g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed)) return 0;
    return LagDiagnostics_BeginSlow(metric, frame);
}
void LagDiagnostics_End(LagMetric metric, uint64_t start);
void LagDiagnostics_EndResult(LagMetric metric, uint64_t start, int32_t result);
inline void LagDiagnostics_Event(LagEvent event) {
    if (g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed))
        LagDiagnostics_EventSlow(event);
}
// Run on the existing initialization thread after its startup work completes.
void LagDiagnostics_RunWriter();
void LagDiagnostics_StopWriter();
enum class LagCaptureStatus : unsigned {
    Idle, Starting, Recording, Finishing, Complete, Failed, OpenFailed, Continuous
};
bool LagDiagnostics_StartCapture();
LagCaptureStatus LagDiagnostics_GetCaptureStatus();
unsigned LagDiagnostics_GetCaptureSecondsRemaining();

class LagDiagnosticScope {
    LagMetric metric;
    uint64_t start;
public:
    explicit LagDiagnosticScope(LagMetric value, int32_t frame = -1)
        : metric(value), start(LagDiagnostics_Begin(value, frame)) {}
    ~LagDiagnosticScope() { Finish(); }
    void Finish() {
        if (start) LagDiagnostics_End(metric, start);
        start = 0;
    }
    void Next(LagMetric value) {
        Finish();
        metric = value;
        start = LagDiagnostics_Begin(value);
    }
    LagDiagnosticScope(const LagDiagnosticScope&) = delete;
    LagDiagnosticScope& operator=(const LagDiagnosticScope&) = delete;
};
