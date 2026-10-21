#pragma once

#include <cstdint>
#include <atomic>

enum class LagMetric : unsigned {
    Frame, Native, Pacing, InputWait, BenchmarkUpdate, BenchmarkBegin,
    BenchmarkEnd, BenchmarkDraw, SteamRoute, EndScene, StatusHud, CharacterHud,
    EscapeHud, ImGuiSubmit, ReportQueue,
    ControllerStart, ControllerEnd, PacketDrain, PredictionWait, Checksum,
    MainHook, FrameTail, SaveLive, SaveResim, SaveRegions, SaveAllocate, SaveCopy,
    SaveClones, SaveParents, SaveRenderState, SaveShots, SaveShotRefs, SaveEffects,
    SnapDump, Restore, RestoreSetup, RestoreRegions, RestoreClones, RestoreFixups,
    RestoreReconstruct, RestoreLate, Correction, ResimFrame, ResimTick,
    RebuildPin, RebuildRetire, RebuildDestroy, RebuildSpawn, RebuildFinalize,
    OverlaySetup, OverlayLogic, ImGuiFinalize, HookLog, Count
};
enum class LagEvent : unsigned {
    BenchmarkReset, LiveSample, CorrectionSample, BenchmarkClock,
    NativeOutsideFrame, FrameReentry, OverlayOrder, ScopeOverflow, Count
};

// Initially true to allow lazy configuration; immutable after configuration loads.
extern std::atomic<bool> g_lagDiagnosticsMayBeEnabled;
uint64_t LagDiagnostics_BeginSlow(LagMetric metric, int32_t frame);
void LagDiagnostics_EventSlow(LagEvent event);
inline uint64_t LagDiagnostics_Begin(LagMetric metric, int32_t frame = -1) {
    if (!g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed)) return 0;
    return LagDiagnostics_BeginSlow(metric, frame);
}
void LagDiagnostics_End(LagMetric metric, uint64_t start);
inline void LagDiagnostics_Event(LagEvent event) {
    if (g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed))
        LagDiagnostics_EventSlow(event);
}
// Run on the existing initialization thread after its startup work completes.
void LagDiagnostics_RunWriter();
void LagDiagnostics_StopWriter();

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
