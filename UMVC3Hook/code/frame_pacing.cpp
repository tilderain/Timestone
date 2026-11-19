#include "frame_pacing.h"

#include "d3d9ex.h"
#include "mk10utils.h"

#include <Windows.h>
#include <d3d9.h>
#include <atomic>
#include <cmath>

extern void HookLog(const char* fmt, ...);
extern bool IsLaunchVsyncEnabled();

namespace {

constexpr double kTargetMs = 1000.0 / 60.0;
constexpr double kLateMs = kTargetMs + 1.0;
constexpr int kWindow = 120;          // frames of pacing history (two seconds)
constexpr int kDisplaySeconds = 10;   // seconds of repeat/drop history
constexpr uintptr_t PTR_SRENDER = 0x140E179A8;
constexpr size_t SRENDER_DEVICE = 0xB0;
// IID_IDirect3DSwapChain9Ex; the import libraries here do not carry it.
const GUID kSwapChainExId = {0x91886caf, 0x1c3d, 0x4d2e, {0xa0, 0xab, 0x3e, 0x4c, 0x7d, 0x8d, 0x33, 0x03}};

SRWLOCK g_lock = SRWLOCK_INIT;
const double g_qpcToMs = [] {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return 1000.0 / static_cast<double>(frequency.QuadPart);
}();
LONGLONG g_lastPresent = 0;
// Two seconds of: time between presents, and the same frames split up into
// when each started, how long its work took before Present was called, and
// how long Present held the thread.
struct Ring {
    float values[kWindow] = {};
    int count = 0, next = 0;
    void Add(float value) {
        values[next] = value;
        next = (next + 1) % kWindow;
        if (count < kWindow) ++count;
    }
};
Ring g_intervals, g_starts, g_work, g_presentTime;
LONGLONG g_lastStart = 0;   // previous frame start
LONGLONG g_frameStart = 0;  // this frame's, until its Present

// Even pacing (FramePacing_BeforePresent).
// The hold point is set from five seconds of work times: the third longest
// of them (about the 99th percentile) plus a margin. One exceptional frame, a
// rollback say, is late once and does not hold every frame after it back.
constexpr int kHoldWindow = 300;
constexpr float kHoldMarginMs = 0.5f;
float g_holdWork[kHoldWindow] = {};
int g_holdWorkCount = 0, g_holdWorkNext = 0;
constexpr float kHoldLimitMs = 10.0f;  // never held further into the frame than this
constexpr float kHoldEaseMs = 0.03f;   // most the hold comes down by in one frame
float g_holdMs = 0.0f;                 // the hold in use, for display

// TEMPORARY (2026-11-19): one summary line every two seconds while even
// pacing is being tuned. A line costs about 0.3 ms in the frame it is written
// from. Remove with the PACING block in FramePacing_OnPresent.
constexpr bool kPacingLog = true;
struct PacingLogWindow {
    int frames = 0, held = 0, exceeded = 0;
    float holdMin = 0.0f, holdMax = 0.0f, overMax = 0.0f;
} g_log;
// How late the two waits returned at worst (ms past their target): the hold
// before Present, and whatever else used FramePacing_WaitUntil (frame start).
float g_logHoldLate = 0.0f;
std::atomic<float> g_logWaitLate{0.0f};

constexpr DWORD kHighResolutionTimer = 0x00000002; // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
// 1 once a high-resolution timer has been created, 0 when one cannot be (or
// one stopped working), -1 before the first try.
std::atomic<int> g_preciseTimer{-1};

bool PreciseTimerAvailable()
{
    int value = g_preciseTimer.load(std::memory_order_relaxed);
    if (value < 0) {
        HANDLE probe = CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer,
                                              TIMER_ALL_ACCESS);
        value = probe ? 1 : 0;
        if (probe) CloseHandle(probe);
        g_preciseTimer.store(value, std::memory_order_relaxed);
        HookLog("FramePacing: high-resolution timer %s\n",
                value ? "available" : "unavailable; waits use Sleep(1) and a spin of up to 2 ms");
    }
    return value != 0;
}

// Present statistics. The counts are running totals kept by the system.
bool g_displayValid = false;
bool g_havePrevious = false;
D3DPRESENTSTATS g_previous = {};
LONGLONG g_rateFromQpc = 0;   // where the refresh rate is measured from
UINT g_rateFromRefresh = 0;
double g_refreshHz = 0.0;
// One bucket per second, so the totals shown cover a fixed stretch of time.
struct Bucket { LONGLONG second; uint32_t repeated, dropped; };
Bucket g_buckets[kDisplaySeconds] = {};
std::atomic<LONGLONG> g_lastReadMs{-100000}; // when the panel last asked for the figures
LONGLONG g_displayRetryMs = 0;

bool IsGameDevice(IDirect3DDevice9* device)
{
    const uint64_t render = *reinterpret_cast<uint64_t*>(_addr(PTR_SRENDER));
    return render && device &&
        *reinterpret_cast<IDirect3DDevice9**>(render + SRENDER_DEVICE) == device;
}

Bucket& BucketFor(LONGLONG nowMs)
{
    const LONGLONG second = nowMs / 1000;
    Bucket& bucket = g_buckets[second % kDisplaySeconds];
    if (bucket.second != second) bucket = {second, 0, 0};
    return bucket;
}

// The implicit swapchain is looked up and released every time: a reference
// kept across frames would make the next Reset fail.
bool ReadPresentStats(IDirect3DDevice9* device, D3DPRESENTSTATS* stats)
{
    if (!D3D9Ex_IsDevice(device)) return false;
    IDirect3DSwapChain9* chain = nullptr;
    if (FAILED(device->GetSwapChain(0, &chain)) || !chain) return false;
    IDirect3DSwapChain9Ex* chainEx = nullptr;
    HRESULT result = chain->QueryInterface(kSwapChainExId,
                                           reinterpret_cast<void**>(&chainEx));
    chain->Release();
    if (FAILED(result) || !chainEx) return false;
    *stats = {};
    result = chainEx->GetPresentStats(stats);
    chainEx->Release();
    // A backend that does not implement them answers with zeros.
    return SUCCEEDED(result) && stats->SyncQPCTime.QuadPart != 0;
}

void UpdateDisplay(IDirect3DDevice9* device, LONGLONG now)
{
    D3DPRESENTSTATS stats;
    if (!ReadPresentStats(device, &stats)) {
        g_displayValid = false;
        g_havePrevious = false;
        g_refreshHz = 0.0;
        return;
    }
    if (!g_havePrevious || stats.PresentCount < g_previous.PresentCount ||
        stats.SyncRefreshCount < g_previous.SyncRefreshCount) {
        // First sample, or the counters started again (a reset).
        g_previous = stats;
        g_havePrevious = true;
        g_rateFromQpc = stats.SyncQPCTime.QuadPart;
        g_rateFromRefresh = stats.SyncRefreshCount;
        return;
    }
    // Refresh rate, from how many refreshes the system counted over time.
    const double spanMs = (stats.SyncQPCTime.QuadPart - g_rateFromQpc) * g_qpcToMs;
    if (spanMs >= 500.0) {
        const double hz = (stats.SyncRefreshCount - g_rateFromRefresh) * 1000.0 / spanMs;
        if (hz > 20.0 && hz < 1000.0) g_refreshHz = hz;
        g_rateFromQpc = stats.SyncQPCTime.QuadPart;
        g_rateFromRefresh = stats.SyncRefreshCount;
    }
    const UINT shown = stats.PresentCount - g_previous.PresentCount;
    const UINT refreshes = stats.PresentRefreshCount - g_previous.PresentRefreshCount;
    g_previous = stats;
    if (g_refreshHz <= 0.0) return;
    g_displayValid = true;
    if (!shown || shown > 30 || refreshes > 600) return; // nothing new, or a long stall
    // How many refreshes one 60 fps frame should stay up for on this display.
    const long perFrame = std::lround(g_refreshHz / 60.0) < 1 ? 1 : std::lround(g_refreshHz / 60.0);
    const long excess = static_cast<long>(refreshes) - static_cast<long>(shown) * perFrame;
    Bucket& bucket = BucketFor(static_cast<LONGLONG>(now * g_qpcToMs));
    if (excess > 0) bucket.repeated += static_cast<uint32_t>(excess);
    else if (excess < 0) bucket.dropped += static_cast<uint32_t>((-excess + perFrame - 1) / perFrame);
}

// Registers the calling thread with the multimedia class scheduler as a game
// thread (avrt.dll, loaded on demand), or failing that raises its priority a
// step. Once per thread; it stays that way.
bool BoostWaitingThread()
{
    using SetCharacteristics = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    bool registered = false;
    if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
        if (auto set = reinterpret_cast<SetCharacteristics>(
                GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))) {
            DWORD task = 0;
            registered = set(L"Games", &task) != nullptr;
        }
    }
    if (!registered) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HookLog("FramePacing: thread %lu %s\n", GetCurrentThreadId(),
            registered ? "registered with the multimedia scheduler (Games)"
                       : "priority raised (multimedia scheduler unavailable)");
    return registered;
}

} // namespace

void FramePacing_OnFrameStart()
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    AcquireSRWLockExclusive(&g_lock);
    if (g_lastStart)
        g_starts.Add(static_cast<float>((counter.QuadPart - g_lastStart) * g_qpcToMs));
    g_lastStart = g_frameStart = counter.QuadPart;
    ReleaseSRWLockExclusive(&g_lock);
}

static void NoteWaitLate(LONGLONG now, LONGLONG target)
{
    if (!kPacingLog) return;
    const float late = static_cast<float>((now - target) * g_qpcToMs);
    if (late > g_logWaitLate.load(std::memory_order_relaxed))
        g_logWaitLate.store(late, std::memory_order_relaxed);
}

void FramePacing_WaitUntil(int64_t targetQpc)
{
    const double ticksPerMs = 1.0 / g_qpcToMs;
    // A timer that can wake within a fraction of a millisecond (Windows 10
    // 1803 and later); without one, Sleep(1), which can run a millisecond over.
    thread_local HANDLE timer = PreciseTimerAvailable()
        ? CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer, TIMER_ALL_ACCESS)
        : nullptr;
    // The sleep is aimed kSpinMs short of the target and the rest is spun.
    // A sleep alone was tried: it usually lands within 0.2 ms, but about
    // once a minute it woke 1 to 2 ms late (once 4 ms), and each of those was
    // a frame shown that late. One millisecond of spin brings all of those
    // but the rarest under a millisecond late, for half the processor time
    // two would take. `overshootMs` (how late sleeps have landed lately)
    // only widens the spin on a system whose sleeps are worse than that.
    thread_local double overshootMs = 0.0;
    constexpr double kSpinMs = 1.0, kMostSpinMs = 4.0;
    // A thread the system treats as a game's is woken more promptly, which is
    // what lets the spin go.
    thread_local const bool boosted = BoostWaitingThread();
    (void)boosted;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    for (;;) {
        const double remainingMs = (targetQpc - now.QuadPart) / ticksPerMs;
        if (remainingMs <= 0.0) {
            NoteWaitLate(now.QuadPart, targetQpc);
            return;
        }
        double spinMs = overshootMs * 1.25;
        if (spinMs < kSpinMs) spinMs = kSpinMs;
        if (spinMs > kMostSpinMs) spinMs = kMostSpinMs;
        const double sleepMs = remainingMs - spinMs;
        if (sleepMs < 0.05 || (!timer && sleepMs < 1.0)) break; // the rest is spun
        const LONGLONG before = now.QuadPart;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(sleepMs * 10000.0); // relative, 100 ns units
            if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) ||
                WaitForSingleObject(timer, 50) != WAIT_OBJECT_0) {
                CloseHandle(timer);
                timer = nullptr; // not usable here: Sleep from now on
                if (g_preciseTimer.exchange(0, std::memory_order_relaxed) != 0)
                    HookLog("FramePacing: high-resolution timer stopped working; "
                            "waits use Sleep(1) and a spin of up to 2 ms\n");
                continue;
            }
        } else {
            Sleep(1);
        }
        QueryPerformanceCounter(&now);
        const double asked = timer ? sleepMs : 1.0;
        const double over = (now.QuadPart - before) / ticksPerMs - asked;
        // A typical figure, not the worst: it moves a tenth of the way to
        // each new sample, so one late wake does not widen the spin.
        overshootMs += ((over > 0.0 ? over : 0.0) - overshootMs) * 0.1;
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < targetQpc);
    NoteWaitLate(now.QuadPart, targetQpc);
}

int64_t FramePacing_BeforePresent(IDirect3DDevice9* device)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (!IsGameDevice(device)) return counter.QuadPart;
    AcquireSRWLockExclusive(&g_lock);
    const LONGLONG start = g_frameStart;
    g_frameStart = 0; // one Present per frame start; work is recorded here
    LONGLONG target = 0;
    g_holdMs = 0.0f;
    if (start && counter.QuadPart >= start) {
        const float work = static_cast<float>((counter.QuadPart - start) * g_qpcToMs);
        // VSync paces the frame itself; with it on there is nothing to even out.
        if (!IsLaunchVsyncEnabled() && g_holdWorkCount >= 30) {
            float top[3] = {}; // the three longest, longest first
            for (int i = 0; i < g_holdWorkCount; ++i) {
                const float value = g_holdWork[i];
                if (value > top[0]) { top[2] = top[1]; top[1] = top[0]; top[0] = value; }
                else if (value > top[1]) { top[2] = top[1]; top[1] = value; }
                else if (value > top[2]) top[2] = value;
            }
            float hold = top[2] + kHoldMarginMs;
            if (hold > kHoldLimitMs) hold = kHoldLimitMs;
            // The hold point moving is itself an uneven frame, so it only
            // moves gradually on the way down (1.8 ms a second); it rises at
            // once, since the frames that raised it were late anyway.
            static float eased = 0.0f;
            if (hold >= eased) eased = hold;
            else eased = eased - hold > kHoldEaseMs ? eased - kHoldEaseMs : hold;
            hold = eased;
            if (kPacingLog) {
                if (!g_log.held || hold < g_log.holdMin) g_log.holdMin = hold;
                if (hold > g_log.holdMax) g_log.holdMax = hold;
                ++g_log.held;
                if (work > hold) {
                    ++g_log.exceeded;
                    if (work - hold > g_log.overMax) g_log.overMax = work - hold;
                }
            }
            if (hold > work) {
                g_holdMs = hold;
                target = start + static_cast<LONGLONG>(hold / g_qpcToMs);
            }
        }
        g_work.Add(work);
        g_holdWork[g_holdWorkNext] = work;
        g_holdWorkNext = (g_holdWorkNext + 1) % kHoldWindow;
        if (g_holdWorkCount < kHoldWindow) ++g_holdWorkCount;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!target) return counter.QuadPart;
    FramePacing_WaitUntil(target);
    QueryPerformanceCounter(&counter);
    if (kPacingLog) {
        const float late = static_cast<float>((counter.QuadPart - target) * g_qpcToMs);
        if (late > g_logHoldLate) g_logHoldLate = late;
    }
    return counter.QuadPart;
}

void FramePacing_OnPresent(IDirect3DDevice9* device, int64_t beforeToken)
{
    if (!IsGameDevice(device)) return;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    AcquireSRWLockExclusive(&g_lock);
    if (beforeToken)
        g_presentTime.Add(static_cast<float>((counter.QuadPart - beforeToken) * g_qpcToMs));
    if (g_lastPresent)
        g_intervals.Add(static_cast<float>((counter.QuadPart - g_lastPresent) * g_qpcToMs));
    g_lastPresent = counter.QuadPart;
    // Present statistics cost several COM calls: only while the panel that
    // shows them is being drawn, and a backend without them is asked again
    // only every couple of seconds.
    const LONGLONG nowMs = static_cast<LONGLONG>(counter.QuadPart * g_qpcToMs);
    if (nowMs - g_lastReadMs.load(std::memory_order_relaxed) < 3000 && nowMs >= g_displayRetryMs) {
        UpdateDisplay(device, counter.QuadPart);
        if (!g_havePrevious) g_displayRetryMs = nowMs + 2000;
    }
    PacingLogWindow log;
    const bool report = kPacingLog && ++g_log.frames >= kWindow;
    if (report) {
        log = g_log;
        g_log = {};
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (report) {
        // early/late: frames more than 1 ms short of / over 16.67 ms.
        // exceeded: frames whose work ran past the hold point, and by how much.
        const LONGLONG saved = g_lastReadMs.load(std::memory_order_relaxed);
        const FramePacingStats stats = FramePacing_Get();
        g_lastReadMs.store(saved, std::memory_order_relaxed); // the log is not the panel
        int early = 0;
        AcquireSRWLockShared(&g_lock);
        for (int i = 0; i < g_intervals.count; ++i)
            if (g_intervals.values[i] < kTargetMs - 1.0) ++early;
        ReleaseSRWLockShared(&g_lock);
        // wait_late / hold_late: the latest any wait, and the hold wait,
        // returned past its target (a first wait entered late counts too).
        const float waitLate = g_logWaitLate.exchange(0.0f, std::memory_order_relaxed);
        const float holdLate = g_logHoldLate;
        g_logHoldLate = 0.0f;
        HookLog("PACING 2s avg=%.2f worst=%.2f late=%u early=%d start_worst=%.2f work=%.2f-%.2f "
                "present_max=%.2f hold=%.2f-%.2f held=%d exceeded=%d over_max=%.2f "
                "wait_late=%.2f hold_late=%.2f vsync=%d even=%d timer=%d backend=\"%s\"\n",
                stats.averageMs, stats.worstMs, stats.late, early, stats.startWorstMs,
                stats.workMinMs, stats.workMaxMs, stats.presentMaxMs, log.holdMin, log.holdMax,
                log.held, log.exceeded, log.overMax, waitLate, holdLate,
                IsLaunchVsyncEnabled() ? 1 : 0,
                IsLaunchVsyncEnabled() ? 0 : 1, PreciseTimerAvailable() ? 1 : 0, D3D9Ex_BackendName());
    }
}

FramePacingStats FramePacing_Get()
{
    FramePacingStats stats = {};
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    g_lastReadMs.store(static_cast<LONGLONG>(counter.QuadPart * g_qpcToMs),
                       std::memory_order_relaxed);
    AcquireSRWLockShared(&g_lock);
    if (g_intervals.count >= 30) {
        double sum = 0.0, worst = 0.0;
        for (int i = 0; i < g_intervals.count; ++i) {
            sum += g_intervals.values[i];
            const double off = std::fabs(g_intervals.values[i] - kTargetMs);
            if (off > worst) worst = off;
            if (g_intervals.values[i] > kLateMs) ++stats.late;
        }
        stats.valid = true;
        stats.averageMs = static_cast<float>(sum / g_intervals.count);
        stats.worstMs = static_cast<float>(worst);
    }
    for (int i = 0; i < g_starts.count; ++i) {
        const float off = std::fabs(g_starts.values[i] - static_cast<float>(kTargetMs));
        if (off > stats.startWorstMs) stats.startWorstMs = off;
    }
    for (int i = 0; i < g_work.count; ++i) {
        if (!i || g_work.values[i] < stats.workMinMs) stats.workMinMs = g_work.values[i];
        if (g_work.values[i] > stats.workMaxMs) stats.workMaxMs = g_work.values[i];
    }
    for (int i = 0; i < g_presentTime.count; ++i)
        if (g_presentTime.values[i] > stats.presentMaxMs) stats.presentMaxMs = g_presentTime.values[i];
    stats.holdMs = g_holdMs;
    stats.displayValid = g_displayValid;
    stats.refreshHz = static_cast<float>(g_refreshHz);
    const LONGLONG second = static_cast<LONGLONG>(counter.QuadPart * g_qpcToMs) / 1000;
    for (const Bucket& bucket : g_buckets) {
        if (second - bucket.second >= kDisplaySeconds) continue;
        stats.repeated += bucket.repeated;
        stats.dropped += bucket.dropped;
    }
    ReleaseSRWLockShared(&g_lock);
    return stats;
}
