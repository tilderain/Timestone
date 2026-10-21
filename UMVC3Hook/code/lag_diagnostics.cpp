#include "../pch.h"
#include "lag_diagnostics.h"
#include "launchmenu_utils.h"
#include <algorithm>
#include <atomic>
#include <cstdio>

#ifndef UMVC3HOOK_GIT_COMMIT
#define UMVC3HOOK_GIT_COMMIT "unknown"
#endif
#ifndef UMVC3HOOK_GIT_DIRTY
#define UMVC3HOOK_GIT_DIRTY 0
#endif

std::atomic<bool> g_lagDiagnosticsMayBeEnabled{true};
static_assert(std::atomic<bool>::is_always_lock_free);

namespace {
struct Settings { bool enabled = false; uint64_t frequency = 0; };
const Settings& Config() {
    static const Settings config = [] {
        Settings result;
        char path[MAX_PATH] = {};
        LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
        if (path[0] && GetPrivateProfileIntA("Diagnostics", "LagTiming", 1, path)) {
            LARGE_INTEGER frequency;
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
                result.enabled = true;
                result.frequency = (uint64_t)frequency.QuadPart;
            }
        }
        g_lagDiagnosticsMayBeEnabled.store(result.enabled, std::memory_order_relaxed);
        return result;
    }();
    return config;
}
uint64_t Counter() {
    LARGE_INTEGER value;
    if (!QueryPerformanceCounter(&value)) return 0;
    return (uint64_t)value.QuadPart;
}
struct Cost {
    uint64_t count = 0, ticks = 0, maximum = 0, over25 = 0;
    void Add(uint64_t elapsed, uint64_t frequency) {
        ++count;
        ticks += elapsed;
        maximum = std::max(maximum, elapsed);
        over25 += (double)elapsed * 1000.0 / frequency > 25.0;
    }
};
struct Window {
    Cost costs[(unsigned)LagMetric::Count];
    uint64_t selfTicks[(unsigned)LagMetric::Count] = {};
    uint64_t worstInclusive[(unsigned)LagMetric::Count] = {};
    uint64_t worstSelf[(unsigned)LagMetric::Count] = {};
    uint64_t worstDuration = 0;
    int32_t worstFrame = -1;
    Cost gaps[2];
    uint64_t events[(unsigned)LagEvent::Count] = {};
    uint64_t started = 0, previousFrame = 0, previousScene = 0;
    unsigned depth = 0, frameDepth = 0, sceneDepth = 0, overlayStage = 0;
    int32_t frame = -1;
};
thread_local Window window;
struct ActiveScope { uint64_t children = 0; };
thread_local ActiveScope activeScopes[64];
thread_local uint64_t frameInclusive[(unsigned)LagMetric::Count] = {};
thread_local uint64_t frameSelf[(unsigned)LagMetric::Count] = {};
struct ReportData {
    Window snapshot;
    uint64_t now = 0, frequency = 0;
    DWORD threadId = 0;
};
struct ReportSlot {
    // 0 = free, 1 = exclusively claimed, 2 = ready for the writer.
    std::atomic<unsigned> state{0};
    ReportData report;
};
ReportSlot reports[8];
std::atomic<bool> writerReady{false}, writerStop{false};
std::atomic<uint64_t> droppedReports{0};
static_assert(std::atomic<unsigned>::is_always_lock_free &&
              std::atomic<uint64_t>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free);
constexpr const char* names[] = {
    "frame", "native", "pacing", "input_wait", "bench_update", "bench_begin",
    "bench_end", "bench_draw", "steam_route", "endscene", "status_hud", "chrsel_hud",
    "esc_hud", "imgui_submit", "diag_enqueue",
    "controller_start", "controller_end", "packet_drain", "prediction_wait", "checksum",
    "main_hook", "frame_tail", "save_live", "save_resim", "save_regions", "save_alloc", "save_copy",
    "save_clones", "save_parents", "save_render", "save_shots", "save_shot_refs", "save_effects",
    "snapdump", "restore", "restore_setup", "restore_regions", "restore_clones", "restore_fixups",
    "restore_reconstruct", "restore_late", "correction", "resim_frame", "resim_tick",
    "rebuild_pin", "rebuild_retire", "rebuild_destroy", "rebuild_spawn", "rebuild_finalize",
    "overlay_setup", "overlay_logic", "imgui_finalize", "hooklog"
};
static_assert(sizeof(names) / sizeof(*names) == (unsigned)LagMetric::Count);

bool WriteReport(const ReportData& report, HANDLE file, double previousWriteMs) {
    const auto& window = report.snapshot;
    const uint64_t now = report.now, frequency = report.frequency;
    char text[16384] = {};
    size_t used = 0;
    auto append = [&](const char* format, auto... args) {
        if (used >= sizeof(text) - 1) return;
        const int n = std::snprintf(text + used, sizeof(text) - used, format, args...);
        if (n > 0) used += std::min((size_t)n, sizeof(text) - used - 1);
    };
    const double ms = 1000.0 / frequency;
    append("LAGDIAG tid=%lu net_frame=%d window_ms=%.0f build=%s dirty=%d units=count/avg_ms/max_ms/over25",
           (unsigned long)report.threadId, window.frame,
           (now - window.started) * ms, UMVC3HOOK_GIT_COMMIT, UMVC3HOOK_GIT_DIRTY);
    for (unsigned i = 0; i < (unsigned)LagMetric::Count; ++i) {
        const auto& c = window.costs[i];
        if (c.count) append(" %s=%llu/%.3f/%.3f/%llu", names[i],
            (unsigned long long)c.count, c.ticks * ms / c.count,
            c.maximum * ms, (unsigned long long)c.over25);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const auto& c = window.gaps[i];
        if (c.count) append(" %s_gap=%llu/%.3f/%.3f/%llu", i ? "scene" : "frame",
            (unsigned long long)c.count, c.ticks * ms / c.count,
            c.maximum * ms, (unsigned long long)c.over25);
    }
    append(" resets=%llu samples=%llu corrections=%llu bench_qpc=%llu native_outside=%llu reentry=%llu overlay_order=%llu",
        (unsigned long long)window.events[(unsigned)LagEvent::BenchmarkReset],
        (unsigned long long)window.events[(unsigned)LagEvent::LiveSample],
        (unsigned long long)window.events[(unsigned)LagEvent::CorrectionSample],
        (unsigned long long)window.events[(unsigned)LagEvent::BenchmarkClock],
        (unsigned long long)window.events[(unsigned)LagEvent::NativeOutsideFrame],
        (unsigned long long)window.events[(unsigned)LagEvent::FrameReentry],
        (unsigned long long)window.events[(unsigned)LagEvent::OverlayOrder]);
    append(" scope_overflow=%llu report_qpc=%llu dropped=%llu writer_prev_ms=%.3f",
        (unsigned long long)window.events[(unsigned)LagEvent::ScopeOverflow],
        (unsigned long long)now,
        (unsigned long long)droppedReports.load(std::memory_order_relaxed), previousWriteMs);
    append("%s", " self_avg_ms{");
    for (unsigned i = 0; i < (unsigned)LagMetric::Count; ++i)
        if (window.costs[i].count && window.selfTicks[i])
            append(" %s=%.3f", names[i], window.selfTicks[i] * ms / window.costs[i].count);
    append("%s", " }\n");
    if (window.worstDuration) {
        append("LAGWORST tid=%lu net_frame=%d duration_ms=%.3f report_qpc=%llu units=inclusive_ms/self_ms",
            (unsigned long)report.threadId, window.worstFrame, window.worstDuration * ms,
            (unsigned long long)now);
        for (unsigned i = 0; i < (unsigned)LagMetric::Count; ++i)
            if (window.worstInclusive[i]) append(" %s=%.3f/%.3f", names[i],
                window.worstInclusive[i] * ms, window.worstSelf[i] * ms);
        append("%s", "\n");
    }
    DWORD written = 0;
    return WriteFile(file, text, (DWORD)used, &written, nullptr) && written == used;
}

void Report(uint64_t now, uint64_t frequency) {
    const uint64_t beforeQueue = Counter();
    bool queued = false;
    if (writerReady.load(std::memory_order_acquire)) {
        for (auto& slot : reports) {
            unsigned expected = 0;
            if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire))
                continue;
            slot.report = {window, now, frequency, GetCurrentThreadId()};
            slot.state.store(2, std::memory_order_release);
            queued = true;
            break;
        }
    }
    if (!queued) droppedReports.fetch_add(1, std::memory_order_relaxed);
    for (auto& c : window.costs) c = {};
    for (auto& c : window.selfTicks) c = 0;
    window.worstDuration = 0;
    for (auto& c : window.gaps) c = {};
    for (auto& e : window.events) e = 0;
    window.started = now;
    const uint64_t afterQueue = Counter();
    if (beforeQueue && afterQueue >= beforeQueue)
        window.costs[(unsigned)LagMetric::ReportQueue].Add(afterQueue - beforeQueue, frequency);
}

bool TakeReport(ReportSlot& slot, ReportData& report) {
    unsigned expected = 2;
    if (!slot.state.compare_exchange_strong(expected, 1, std::memory_order_acquire))
        return false;
    report = slot.report;
    // Release the slot before formatting or I/O. Producers never wait for disk.
    slot.state.store(0, std::memory_order_release);
    return true;
}
} // namespace

void LagDiagnostics_RunWriter() {
    if (!Config().enabled || writerStop.load(std::memory_order_acquire)) return;
    // This process-lifetime worker must not execute from an unloaded ASI.
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCSTR>(&LagDiagnostics_RunWriter), &module)) return;
    char filename[64], path[MAX_PATH] = {};
    std::snprintf(filename, sizeof(filename), "Timestone_lag_%lu.log",
                  (unsigned long)GetCurrentProcessId());
    LaunchMenuUtils::GetSettingsPath(filename, path, sizeof(path));
    if (!path[0]) return;
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    writerReady.store(true, std::memory_order_release);
    double previousWriteMs = 0;
    while (!writerStop.load(std::memory_order_acquire)) {
        for (auto& slot : reports) {
            if (writerStop.load(std::memory_order_acquire)) break;
            ReportData report;
            if (!TakeReport(slot, report)) continue;
            const uint64_t start = Counter();
            if (!WriteReport(report, file, previousWriteMs)) {
                writerReady.store(false, std::memory_order_release);
                CloseHandle(file);
                return;
            }
            const uint64_t end = Counter();
            previousWriteMs = start && end >= start
                ? (end - start) * 1000.0 / report.frequency : 0;
        }
        Sleep(100);
    }
    writerReady.store(false, std::memory_order_release);
    CloseHandle(file);
}

void LagDiagnostics_StopWriter() {
    // Never join a worker under DllMain's loader lock. Queued diagnostics may be lost.
    writerReady.store(false, std::memory_order_release);
    writerStop.store(true, std::memory_order_release);
}

void LagDiagnostics_EventSlow(LagEvent event) {
    if (Config().enabled) ++window.events[(unsigned)event];
}

uint64_t LagDiagnostics_BeginSlow(LagMetric metric, int32_t frame) {
    if (!Config().enabled) return 0;
    const uint64_t now = Counter();
    if (!now) return 0;
    if (window.depth == std::size(activeScopes)) {
        ++window.events[(unsigned)LagEvent::ScopeOverflow];
        return 0;
    }
    if (!window.started) window.started = now;
    activeScopes[window.depth++].children = 0;
    if (metric == LagMetric::Frame) {
        if (!window.frameDepth) {
            for (auto& c : frameInclusive) c = 0;
            for (auto& c : frameSelf) c = 0;
        }
        if (window.frameDepth++) ++window.events[(unsigned)LagEvent::FrameReentry];
        window.frame = frame;
    }
    if (metric == LagMetric::Native && !window.frameDepth)
        ++window.events[(unsigned)LagEvent::NativeOutsideFrame];
    if (metric == LagMetric::EndScene) {
        ++window.sceneDepth;
        window.overlayStage = 0;
    }
    if (window.sceneDepth) {
        unsigned expected = 0;
        switch (metric) {
        case LagMetric::StatusHud: expected = 1; break;
        case LagMetric::CharacterHud: expected = 2; break;
        case LagMetric::EscapeHud: expected = 3; break;
        case LagMetric::BenchmarkDraw: expected = 4; break;
        default: break;
        }
        if (expected) {
            if (window.overlayStage + 1 != expected)
                ++window.events[(unsigned)LagEvent::OverlayOrder];
            window.overlayStage = expected;
        }
    }
    if (metric == LagMetric::Frame || metric == LagMetric::EndScene) {
        const unsigned i = metric == LagMetric::Frame ? 0 : 1;
        auto& previous = i ? window.previousScene : window.previousFrame;
        if (previous && now >= previous) window.gaps[i].Add(now - previous, Config().frequency);
        previous = now;
    }
    return now;
}

void LagDiagnostics_End(LagMetric metric, uint64_t start) {
    if (!start) return;
    const uint64_t now = Counter();
    const uint64_t elapsed = now >= start ? now - start : 0;
    const uint64_t children = activeScopes[window.depth - 1].children;
    const uint64_t self = elapsed - std::min(elapsed, children);
    window.costs[(unsigned)metric].Add(elapsed, Config().frequency);
    window.selfTicks[(unsigned)metric] += self;
    if (window.depth > 1) activeScopes[window.depth - 2].children += elapsed;
    if (window.frameDepth) {
        frameInclusive[(unsigned)metric] += elapsed;
        frameSelf[(unsigned)metric] += self;
        if (metric == LagMetric::Frame && window.frameDepth == 1 && elapsed > window.worstDuration) {
            window.worstDuration = elapsed;
            window.worstFrame = window.frame;
            std::copy(std::begin(frameInclusive), std::end(frameInclusive), window.worstInclusive);
            std::copy(std::begin(frameSelf), std::end(frameSelf), window.worstSelf);
        }
    }
    if (metric == LagMetric::Frame) --window.frameDepth;
    if (metric == LagMetric::EndScene) --window.sceneDepth;
    if (--window.depth == 0 && now >= window.started &&
        now - window.started >= Config().frequency * 5)
        Report(now, Config().frequency);
}
