#include "frame_pacing.h"

#include "d3d9ex.h"
#include "mk10utils.h"

#include <Windows.h>
#include <d3d9.h>
#include <cmath>

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
double g_qpcToMs = 0.0;
LONGLONG g_lastPresent = 0;
float g_intervals[kWindow] = {};
int g_intervalCount = 0;
int g_intervalNext = 0;

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

} // namespace

void FramePacing_OnPresent(IDirect3DDevice9* device)
{
    if (!IsGameDevice(device)) return;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    AcquireSRWLockExclusive(&g_lock);
    if (g_qpcToMs == 0.0) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        g_qpcToMs = 1000.0 / static_cast<double>(frequency.QuadPart);
    }
    if (g_lastPresent) {
        g_intervals[g_intervalNext] =
            static_cast<float>((counter.QuadPart - g_lastPresent) * g_qpcToMs);
        g_intervalNext = (g_intervalNext + 1) % kWindow;
        if (g_intervalCount < kWindow) ++g_intervalCount;
    }
    g_lastPresent = counter.QuadPart;
    UpdateDisplay(device, counter.QuadPart);
    ReleaseSRWLockExclusive(&g_lock);
}

FramePacingStats FramePacing_Get()
{
    FramePacingStats stats = {};
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    AcquireSRWLockShared(&g_lock);
    if (g_intervalCount >= 30) {
        double sum = 0.0, worst = 0.0;
        for (int i = 0; i < g_intervalCount; ++i) {
            sum += g_intervals[i];
            const double off = std::fabs(g_intervals[i] - kTargetMs);
            if (off > worst) worst = off;
            if (g_intervals[i] > kLateMs) ++stats.late;
        }
        stats.valid = true;
        stats.averageMs = static_cast<float>(sum / g_intervalCount);
        stats.worstMs = static_cast<float>(worst);
    }
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
