// Temporary diagnostics for slow asynchronous loads (2026-11-19: a character
// on the character select taking about five seconds to appear on one Windows
// machine, with texture creation already shown to be quick).
//
// Two measurements, both written to the log only when something was slow:
//  - the loader as the game thread sees it, sampled once a frame from
//    sResource's own state: how long it stayed busy, how many items passed
//    through, how deep the queue got, which item was in progress longest, and
//    how long the loader sat paused;
//  - each archive decode, timed exactly on the loader thread.
//
// Layout (Ghidra 8192, sResource__loaderThreadProc 0x140519040 and
// sResource__loadAsync_enqueue 0x1405189A0):
//   sResource+0x2A0A0  uint32  resources queued
//   sResource+0x2A0A4  char[0x40]  path of the item in progress, "" when none
//   sResource+0x2A0E8  uint8   an item was taken on the loader's last pass
//   sResource+0x4A1CD  uint8   loader paused (it sleeps 10 ms at a time)
//   cResource+0x0C     char[0x40]  path

#include "load_diag.h"
#include "d3d9reset.h"

#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr uintptr_t FN_RESOURCE_GET_INSTANCE = 0x140001B10;
constexpr uintptr_t FN_RESOURCE_EXECUTE_DECODE = 0x1405181F0;
constexpr size_t RESOURCE_QUEUED = 0x2A0A0;
constexpr size_t RESOURCE_CURRENT_PATH = 0x2A0A4;
constexpr size_t RESOURCE_BUSY = 0x2A0E8;
constexpr size_t RESOURCE_PAUSED = 0x4A1CD;
constexpr size_t CRESOURCE_PATH = 0x0C;
constexpr size_t kPathSize = 0x40;

// A busy stretch ends after this many idle frames in a row, so the short gaps
// between the items of one load do not split it.
constexpr int kIdleFramesToEnd = 30;
constexpr uint32_t kReportBusyMs = 250;
constexpr double kReportDecodeMs = 50.0;

double NowMs()
{
    static double toMs = 0.0;
    LARGE_INTEGER value;
    if (toMs == 0.0) {
        QueryPerformanceFrequency(&value);
        toMs = 1000.0 / static_cast<double>(value.QuadPart);
    }
    QueryPerformanceCounter(&value);
    return static_cast<double>(value.QuadPart) * toMs;
}

// The other thread may be writing the path: copy what is there and make sure
// it ends.
void CopyPath(char* out, const char* in)
{
    memcpy(out, in, kPathSize);
    out[kPathSize - 1] = '\0';
}

using ExecuteDecodeFn = uint64_t(__fastcall*)(void* resource, void* archive);
ExecuteDecodeFn g_originalExecuteDecode = nullptr;

uint64_t __fastcall Hook_ExecuteDecode(void* resource, void* archive)
{
    const double start = NowMs();
    const uint64_t result = g_originalExecuteDecode(resource, archive);
    const double took = NowMs() - start;
    if (took >= kReportDecodeMs && archive) {
        char path[kPathSize];
        CopyPath(path, static_cast<const char*>(archive) + CRESOURCE_PATH);
        HookLog("LoadDiag: archive %s decoded in %.0f ms (result %u) t=%lu\n", path, took,
                static_cast<unsigned>(result & 0xFF), GetTickCount());
    }
    return result;
}

struct BusyStretch {
    bool active = false;
    double start = 0, lastActive = 0, itemStart = 0, pausedMs = 0, lastTick = 0;
    int idleFrames = 0;
    uint32_t items = 0, peakQueue = 0;
    char current[kPathSize] = {};
    char longest[kPathSize] = {};
    double longestMs = 0;
};
BusyStretch g_stretch;

void CloseItem(double now)
{
    if (!g_stretch.current[0]) return;
    const double took = now - g_stretch.itemStart;
    if (took > g_stretch.longestMs) {
        g_stretch.longestMs = took;
        memcpy(g_stretch.longest, g_stretch.current, kPathSize);
    }
    g_stretch.current[0] = '\0';
}

} // namespace

void LoadDiag_Install()
{
    void* target = reinterpret_cast<void*>(_addr(FN_RESOURCE_EXECUTE_DECODE));
    if (MH_CreateHook(target, reinterpret_cast<void*>(&Hook_ExecuteDecode),
                      reinterpret_cast<void**>(&g_originalExecuteDecode)) == MH_OK &&
        MH_EnableHook(target) == MH_OK)
        HookLog("LoadDiag: timing archive decodes and the resource loader\n");
    else
        HookLog("LoadDiag: FAILED to hook the archive decode\n");
}

void LoadDiag_Tick()
{
    using GetInstance = uint8_t*(__fastcall*)();
    const uint8_t* resource = reinterpret_cast<GetInstance>(_addr(FN_RESOURCE_GET_INSTANCE))();
    if (!resource) return;
    const uint32_t queued = *reinterpret_cast<const uint32_t*>(resource + RESOURCE_QUEUED);
    const bool busy = resource[RESOURCE_BUSY] != 0;
    const bool paused = resource[RESOURCE_PAUSED] != 0;
    char path[kPathSize];
    CopyPath(path, reinterpret_cast<const char*>(resource + RESOURCE_CURRENT_PATH));

    const double now = NowMs();
    // A loader that has work and gets nowhere for 20 s while frames are still
    // being drawn (2026-11-21: a black screen for good at a menu change on a
    // Windows NVIDIA machine, 0.6.4 only). Every thread is listed once, and
    // the Ex backend steps back for the next start as it does for a failed
    // call. A paused loader is listed too but not blamed on the backend.
    {
        static char stuckPath[kPathSize] = {};
        static uint32_t stuckQueued = 0;
        static double stuckSince = 0;
        static bool stuckReported = false;
        if (!(queued != 0 || busy || path[0]) || queued != stuckQueued ||
            strcmp(path, stuckPath) != 0 || !stuckSince) {
            memcpy(stuckPath, path, kPathSize);
            stuckQueued = queued;
            stuckSince = now;
            stuckReported = false;
        } else if (!stuckReported && now - stuckSince >= 20000.0) {
            stuckReported = true;
            HookLog("LoadDiag: loader has made no progress for 20 s: item %s, queued %u, "
                    "busy %d, paused %d\n", path[0] ? path : "-", queued, busy ? 1 : 0, paused ? 1 : 0);
            D3D9ResetDiagnostics_LogAllThreads("loader stuck");
        }
    }
    BusyStretch& s = g_stretch;
    const bool working = queued != 0 || busy || path[0] != '\0';
    if (!s.active) {
        if (!working) return;
        s = {};
        s.active = true;
        s.start = s.lastTick = now;
    }
    if (paused && working) s.pausedMs += now - s.lastTick;
    s.lastTick = now;
    if (queued > s.peakQueue) s.peakQueue = queued;
    if (strcmp(path, s.current) != 0) {
        CloseItem(now);
        if (path[0]) {
            memcpy(s.current, path, kPathSize);
            s.itemStart = now;
            ++s.items;
        }
    }
    if (working) {
        s.lastActive = now;
        s.idleFrames = 0;
        return;
    }
    if (++s.idleFrames < kIdleFramesToEnd) return;
    CloseItem(s.lastActive);
    const double total = s.lastActive - s.start;
    if (total >= kReportBusyMs)
        HookLog("LoadDiag: loader busy %.0f ms: %u items seen, peak queue %u, paused %.0f ms, "
                "longest %s (%.0f ms) t=%lu\n",
                total, s.items, s.peakQueue, s.pausedMs, s.longest[0] ? s.longest : "-",
                s.longestMs, GetTickCount());
    s.active = false;
}
