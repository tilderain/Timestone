#!/usr/bin/env python3
"""Exercise real lag aggregation with fake clocks; no performance stress loops."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / "UMVC3Hook/code"


class LagDiagnosticsTest(unittest.TestCase):
    def test_disabled_inline_gate_never_enters_implementation(self):
        source = r'''
#include "lag_diagnostics.h"
#include <cstdlib>
std::atomic<bool> g_lagDiagnosticsMayBeEnabled{false};
uint64_t LagDiagnostics_BeginSlow(LagMetric, int32_t) { std::abort(); }
void LagDiagnostics_EventSlow(LagEvent) { std::abort(); }
void LagDiagnostics_RecordWaitSlow(LagMetric, int32_t, int32_t, int32_t, int32_t, int32_t) { std::abort(); }
void LagDiagnostics_End(LagMetric, uint64_t) { std::abort(); }
int main() {
    LagDiagnosticScope scope(LagMetric::Frame, 42);
    scope.Next(LagMetric::Native);
    scope.Finish();
    LagDiagnostics_Event(LagEvent::BenchmarkClock);
    LagDiagnostics_RecordWait(LagMetric::PredictionWait, 10, 9, 10, 2, 7);
    return LagDiagnostics_Begin(LagMetric::Pacing) != 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="lag-disabled-", dir="/tmp/opencode") as out:
            cpp = Path(out) / "test.cpp"
            binary = Path(out) / "test"
            cpp.write_text(source)
            subprocess.run(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I", str(CODE), str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_aggregation_and_disabled_path(self):
        source = (CODE / "lag_diagnostics.cpp").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        prefix = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "lag_diagnostics.h"
#define MAX_PATH 260
using DWORD = uint32_t;
using HANDLE = void*;
using HMODULE = void*;
using LPCSTR = const char*;
using LPVOID = void*;
using HRESULT = int;
#define SUCCEEDED(hr) ((hr) >= 0)
#define WINAPI
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
constexpr unsigned GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4, GET_MODULE_HANDLE_EX_FLAG_PIN = 1;
constexpr unsigned FILE_APPEND_DATA = 4, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
constexpr unsigned GENERIC_WRITE = 0x40000000;
constexpr unsigned OPEN_ALWAYS = 4, CREATE_NEW = 1, FILE_ATTRIBUTE_NORMAL = 128;
constexpr int SW_SHOWNORMAL = 1;
constexpr unsigned COINIT_APARTMENTTHREADED = 2, COINIT_DISABLE_OLE1DDE = 4;
constexpr unsigned SEE_MASK_NOASYNC = 0x100, SEE_MASK_FLAG_NO_UI = 0x400;
struct SHELLEXECUTEINFOA {
    unsigned cbSize, fMask;
    const char* lpVerb;
    const char* lpFile;
    int nShow;
};
struct LARGE_INTEGER { long long QuadPart; };
thread_local uint64_t ticks = 100;
thread_local unsigned threadId = 1;
int qpcReads = 0, settingsReads = 0;
bool enabled = false;
std::mutex logMutex;
std::condition_variable logCondition;
bool blockWrites = false, writerBlocked = false, failOpen = false, failWrite = false;
std::vector<std::string> logs;
int fileOpens = 0;
std::atomic<uint64_t> wallMs{1000};
bool failThread = false, failShell = false;
bool fileClosed = false, fileFlushed = false;
int shellOpens = 0;
std::string openedPath, createdPath;
std::vector<std::thread> captureThreads;
uint64_t GetTickCount64() { return wallMs.load(); }
HANDLE CreateThread(void*, size_t, DWORD (*entry)(LPVOID), LPVOID arg, DWORD, DWORD*) {
    if (failThread) return nullptr;
    captureThreads.emplace_back([=] { threadId = 99; entry(arg); });
    return (HANDLE)2;
}
bool GetModuleHandleExA(unsigned, LPCSTR, HMODULE* module) { *module = (void*)1; return true; }
unsigned GetCurrentProcessId() { return 123; }
HANDLE CreateFileA(const char* path, unsigned access, unsigned, void*, unsigned mode, unsigned, void*) {
    assert(threadId == 99 && strstr(path, "Timestone_lag_123"));
    assert(mode == (strstr(path, "capture_") ? CREATE_NEW : OPEN_ALWAYS));
    assert(access == (mode == CREATE_NEW ? GENERIC_WRITE : FILE_APPEND_DATA));
    createdPath = path;
    fileClosed = fileFlushed = false;
    ++fileOpens;
    return failOpen ? INVALID_HANDLE_VALUE : (HANDLE)1;
}
bool CloseHandle(HANDLE handle) {
    if (handle != (HANDLE)2) { assert(threadId == 99); fileClosed = true; }
    return true;
}
bool FlushFileBuffers(HANDLE) { assert(threadId == 99); fileFlushed = true; return true; }
HRESULT CoInitializeEx(void*, unsigned) { assert(threadId == 99); return 0; }
void CoUninitialize() { assert(threadId == 99); }
bool ShellExecuteExA(SHELLEXECUTEINFOA* open) {
    assert(threadId == 99 && !strcmp(open->lpVerb, "open") && fileClosed && fileFlushed);
    assert(open->fMask == (SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI));
    openedPath = open->lpFile;
    ++shellOpens;
    return !failShell;
}
void Sleep(unsigned) { std::this_thread::yield(); }
bool QueryPerformanceCounter(LARGE_INTEGER* value) {
    value->QuadPart = ticks;
    if (threadId == 1) ++qpcReads;
    return true;
}
bool QueryPerformanceFrequency(LARGE_INTEGER* value) { value->QuadPart = 1000000; return true; }
unsigned GetCurrentThreadId() { return threadId; }
unsigned GetPrivateProfileIntA(const char* section, const char* key, int fallback, const char*) {
    assert(!strcmp(section, "Diagnostics") && !strcmp(key, "LagTiming"));
    assert(fallback == 0);
    ++settingsReads;
    return enabled;
}
namespace LaunchMenuUtils {
void GetSettingsPath(char* path, size_t) { strcpy(path, "fixture.ini"); }
void GetSettingsPath(const char* name, char* path, size_t) { strcpy(path, name); }
}
bool WriteFile(HANDLE, const void* data, DWORD size, DWORD* written, void*) {
    assert(threadId == 99); // Any producer-thread I/O fails the test.
    if (failWrite) { *written = 0; return false; }
    std::unique_lock<std::mutex> guard(logMutex);
    logs.emplace_back((const char*)data, size);
    if (blockWrites) {
        writerBlocked = true;
        logCondition.notify_all();
        logCondition.wait(guard, [] { return !blockWrites; });
    }
    *written = size;
    ticks += 2000;
    return true;
}
'''
        main = r'''
void Drain() {
    auto oldThread = threadId;
    auto oldTicks = ticks;
    threadId = 99;
    for (auto& slot : reports) {
        ReportData report;
        if (TakeReport(slot, report)) assert(WriteReport(report, (HANDLE)1, 2.0));
    }
    threadId = oldThread;
    ticks = oldTicks;
}
void WaitStatus(LagCaptureStatus expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (LagDiagnostics_GetCaptureStatus() != expected) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void FinishCapture() {
    wallMs = captureDeadline.load();
    WaitStatus(LagCaptureStatus::Finishing);
    // A new outer boundary flushes this thread's final partial window, but
    // must not start another measurement during the finishing period.
    assert(LagDiagnostics_Begin(LagMetric::Frame, 999) == 0);
    wallMs += 300;
    captureThreads.back().join();
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool timed = !strcmp(argv[1], "timed");
    const bool threadfail = !strcmp(argv[1], "threadfail");
    enabled = strcmp(argv[1], "off") != 0 && !timed && !threadfail;
    if (threadfail) {
        failThread = true;
        assert(!LagDiagnostics_StartCapture());
        assert(LagDiagnostics_GetCaptureStatus() == LagCaptureStatus::Failed);
        assert(!writerRunning && !g_lagDiagnosticsMayBeEnabled && fileOpens == 0);
        failThread = false;
        failOpen = true;
        assert(LagDiagnostics_StartCapture());
        WaitStatus(LagCaptureStatus::Failed);
        captureThreads.back().join();
        assert(fileOpens == 1 && shellOpens == 0 && !writerRunning);
        failOpen = false;
        assert(LagDiagnostics_StartCapture());
        WaitStatus(LagCaptureStatus::Recording);
        LagDiagnostics_StopWriter();
        captureThreads.back().join();
        assert(shellOpens == 0 && !g_lagDiagnosticsMayBeEnabled);
        assert(!LagDiagnostics_StartCapture());
        return 0;
    }
    if (timed) {
        LagDiagnostics_RunWriter();
        assert(fileOpens == 0 && !g_lagDiagnosticsMayBeEnabled);
        assert(LagDiagnostics_StartCapture());
        WaitStatus(LagCaptureStatus::Recording);
        assert(LagDiagnostics_GetCaptureSecondsRemaining() == 15);
        assert(!LagDiagnostics_StartCapture()); // No concurrent capture writers.
        { LagDiagnosticScope frame(LagMetric::Frame, 42); ticks += 1000; }
        wallMs += 14900;
        assert(LagDiagnostics_GetCaptureSecondsRemaining() == 1);
        assert(shellOpens == 0);
        FinishCapture();
        assert(LagDiagnostics_GetCaptureStatus() == LagCaptureStatus::Complete);
        assert(!writerRunning && !writerReady && !g_lagDiagnosticsMayBeEnabled);
        assert(shellOpens == 1 && openedPath == createdPath);
        assert(logs.size() == 3 && logs[1].find("net_frame=42") != std::string::npos);
        assert(logs.front().find("duration_ms=15000") != std::string::npos);
        assert(logs.back().find("LAGCAPTURE stopped") != std::string::npos);
        const auto firstPath = openedPath;
        const int readsBefore = qpcReads;
        { LagDiagnosticScope off(LagMetric::Frame); off.Next(LagMetric::Native); }
        LagDiagnostics_Event(LagEvent::BenchmarkClock);
        assert(qpcReads == readsBefore);

        // A frozen scope must not hold up completion or contaminate the next run.
        assert(LagDiagnostics_StartCapture());
        WaitStatus(LagCaptureStatus::Recording);
        const auto frozen = LagDiagnostics_Begin(LagMetric::Frame, 66);
        assert(frozen);
        FinishCapture();
        assert(openedPath != firstPath && shellOpens == 2);
        assert(LagDiagnostics_StartCapture());
        WaitStatus(LagCaptureStatus::Recording);
        assert(LagDiagnostics_Begin(LagMetric::Native) == 0);
        LagDiagnostics_End(LagMetric::Frame, frozen);
        { LagDiagnosticScope frame(LagMetric::Frame, 77); ticks += 2000; }
        failShell = true;
        FinishCapture();
        assert(LagDiagnostics_GetCaptureStatus() == LagCaptureStatus::OpenFailed);
        assert(logs.size() == 8 && logs[6].find("net_frame=77") != std::string::npos);
        assert(logs[6].find("frame=1/2.000/2.000/0") != std::string::npos);
        assert(logs[6].find("frame_gap=") == std::string::npos);
        assert(!g_lagDiagnosticsMayBeEnabled && shellOpens == 3);
        assert(settingsReads == 1);
        return 0;
    }
    if (!enabled) {
        for (int i = 0; i < 10; ++i) {
            LagDiagnosticScope scope(LagMetric::Frame, i);
            scope.Next(LagMetric::Native);
            scope.Finish();
            LagDiagnostics_Event(LagEvent::BenchmarkClock);
        }
        assert(settingsReads == 1 && qpcReads == 0 && logs.empty());
        assert(!g_lagDiagnosticsMayBeEnabled.load());
        assert(window.depth == 0 && window.started == 0 && droppedReports == 0);
        LagDiagnostics_RunWriter();
        assert(fileOpens == 0);
        return 0;
    }
    if (!strcmp(argv[1], "fail")) {
        failOpen = true;
        threadId = 99;
        LagDiagnostics_RunWriter();
        threadId = 1;
        { LagDiagnosticScope scope(LagMetric::Frame); ticks += 5000000; }
        assert(fileOpens == 1 && logs.empty() && droppedReports == 0 && !writerReady);
        assert(!g_lagDiagnosticsMayBeEnabled && !writerRunning);
        return 0;
    }
    if (!strcmp(argv[1], "writefail")) {
        failWrite = true;
        writerReady = true;
        { LagDiagnosticScope scope(LagMetric::Frame); ticks += 5000000; }
        threadId = 99;
        LagDiagnostics_RunWriter();
        threadId = 1;
        assert(!writerReady && logs.empty());
        { LagDiagnosticScope scope(LagMetric::Frame); ticks += 5000000; }
        assert(droppedReports == 0 && !g_lagDiagnosticsMayBeEnabled);
        return 0;
    }
    writerReady = true;
    {
        LagDiagnosticScope frame(LagMetric::Frame, 42);
        LagDiagnostics_Event(LagEvent::BenchmarkReset);
        { LagDiagnosticScope wait(LagMetric::InputWait); ticks += 30000; }
        { LagDiagnosticScope begin(LagMetric::BenchmarkBegin);
          LagDiagnostics_Event(LagEvent::LiveSample); ticks += 100; }
        { LagDiagnosticScope native(LagMetric::Native); ticks += 7000; }
        ticks += 5000000;
        assert(logs.empty());
    }
    assert(logs.empty());
    Drain();
    assert(logs.size() == 1 && logs[0].find("frame=42") != std::string::npos);
    assert(logs[0].find("input_wait=1/30.000/30.000/1") != std::string::npos);
    assert(logs[0].find("bench_begin=1/0.100/0.100/0") != std::string::npos);
    assert(logs[0].find("native_outside=0") != std::string::npos);
    assert(logs[0].find("resets=1 samples=1") != std::string::npos);
    assert(logs[0].find("frame=1/5037.100/5037.100/1") != std::string::npos);
    assert(logs[0].find("LAGWORST tid=1 net_frame=42 duration_ms=5037.100") != std::string::npos);
    assert(logs[0].find("frame=5037.100/5000.000") != std::string::npos);
    {
        LagDiagnosticScope frame(LagMetric::Frame, 43);
        ticks += 5000000;
    }
    assert(logs.size() == 1);
    Drain();
    assert(logs.size() == 2);
    assert(logs[1].find("diag_enqueue=1/0.000/0.000/0") != std::string::npos);
    assert(logs[1].find("writer_prev_ms=2.000") != std::string::npos);
    assert(logs[1].find("frame_gap=") != std::string::npos);
    assert(logs[1].find("resets=0 samples=0") != std::string::npos);
    std::thread renderer([] {
        threadId = 2;
        LagDiagnosticScope scene(LagMetric::EndScene);
        { LagDiagnosticScope status(LagMetric::StatusHud); ticks += 100; }
        { LagDiagnosticScope chars(LagMetric::CharacterHud); ticks += 200; }
        { LagDiagnosticScope esc(LagMetric::EscapeHud); ticks += 300; }
        { LagDiagnosticScope draw(LagMetric::BenchmarkDraw);
          { LagDiagnosticScope steam(LagMetric::SteamRoute); ticks += 25000; }
          ticks += 400; }
        ticks += 5000000;
    });
    renderer.join();
    assert(logs.size() == 2);
    Drain();
    assert(logs.size() == 3 && logs[2].find("tid=2") != std::string::npos);
    assert(logs[2].find("frame=-1") != std::string::npos);
    assert(logs[2].find("overlay_order=0") != std::string::npos);
    assert(logs[2].find("steam_route=1/25.000/25.000/0") != std::string::npos);
    {
        LagDiagnosticScope scene(LagMetric::EndScene);
        { LagDiagnosticScope wrongOrder(LagMetric::EscapeHud); }
        ticks += 5000000;
    }
    Drain();
    assert(logs.back().find("overlay_order=1") != std::string::npos);
    assert(settingsReads == 1);

    // Attribute repeated replay work to one specific slow frame, with exclusive
    // time partitioned rather than double-counted across nested stages.
    {
        LagDiagnosticScope frame(LagMetric::Frame, 777);
        { LagDiagnosticScope input(LagMetric::ControllerStart);
          ticks += 1000;
          { LagDiagnosticScope wait(LagMetric::InputWait); ticks += 2000; }
          const int beforeContext = qpcReads;
          LagDiagnostics_RecordWait(LagMetric::InputWait, 775, 774, 775, 2, 7);
          assert(qpcReads == beforeContext); }
        { LagDiagnosticScope correction(LagMetric::Correction);
          ticks += 100;
          for (int i = 0; i < 2; ++i) {
              { LagDiagnosticScope save(LagMetric::SaveResim); ticks += 300; }
              { LagDiagnosticScope tick(LagMetric::ResimTick); ticks += 500; }
          } }
        { LagDiagnosticScope save(LagMetric::SaveLive);
          LagDiagnosticScope stage(LagMetric::SaveAllocate);
          ticks += 100;
          stage.Next(LagMetric::SaveCopy); ticks += 200;
          stage.Finish(); ticks += 50; }
        ticks += 300;
    }
    assert(window.worstFrame == 777 && window.worstDuration == 5350);
    assert(window.worstEnd - window.worstStart == window.worstDuration);
    uint64_t partition = 0;
    for (auto part : window.worstSelf) partition += part;
    assert(partition == 5350);
    assert(window.worstInclusive[(unsigned)LagMetric::SaveResim] == 600);
    assert(window.worstSelf[(unsigned)LagMetric::Correction] == 100);
    ticks += 5000000;
    { LagDiagnosticScope frame(LagMetric::Frame, 778); ++ticks; }
    Drain();
    assert(logs.back().find("net_frame=777 duration_ms=5.350") != std::string::npos);
    assert(logs.back().find("correction=1.700/0.100") != std::string::npos);
    assert(logs.back().find("qpc_hz=1000000") != std::string::npos);
    assert(logs.back().find("input_wait_context{required=775 remote_before=774 remote_after=775 delay=2 rollback=7}") != std::string::npos);
    assert(frameWaits[0].required == -1); // The following frame did not inherit the wait.

    const size_t beforeConcurrent = logs.size();
    std::vector<std::thread> producers;
    for (unsigned i = 0; i < 8; ++i) producers.emplace_back([i] {
        threadId = 100 + i;
        LagDiagnosticScope scope(LagMetric::Frame, 1000 + i);
        ticks += 5000000 + i;
    });
    for (auto& producer : producers) producer.join();
    assert(droppedReports == 0);
    Drain();
    assert(logs.size() == beforeConcurrent + 8);
    for (unsigned i = 0; i < 8; ++i) {
        const auto identity = "tid=" + std::to_string(100 + i) + " net_frame=" + std::to_string(1000 + i);
        assert(std::count_if(logs.begin(), logs.end(), [&](const auto& text) {
            return text.find(identity) != std::string::npos;
        }) == 1);
    }

    uint64_t nested[65];
    for (auto& start : nested) start = LagDiagnostics_Begin(LagMetric::BenchmarkBegin);
    assert(nested[64] == 0 && window.events[(unsigned)LagEvent::ScopeOverflow] == 1);
    for (int i = 64; i >= 0; --i) LagDiagnostics_End(LagMetric::BenchmarkBegin, nested[i]);
    assert(window.depth == 0);

    // Actual writer loop blocked inside its output API. Producers must still
    // finish, fill only eight slots and drop the rest; shutdown must not join.
    { LagDiagnosticScope frame(LagMetric::Frame, 100); ticks += 5000000; }
    blockWrites = true;
    std::thread worker([] { threadId = 99; LagDiagnostics_RunWriter(); });
    {
        std::unique_lock<std::mutex> guard(logMutex);
        logCondition.wait(guard, [] { return writerBlocked; });
    }
    const uint64_t beforeDrops = droppedReports;
    for (int frame = 101; frame < 121; ++frame) {
        LagDiagnosticScope scope(LagMetric::Frame, frame);
        ticks += 5000000;
    }
    assert(droppedReports - beforeDrops == 12);
    for (auto& slot : reports) assert(slot.state == 2);
    LagDiagnostics_StopWriter();
    {
        std::lock_guard<std::mutex> guard(logMutex);
        blockWrites = false;
    }
    logCondition.notify_all();
    worker.join();
    assert(!writerReady && fileOpens == 1);
    assert(!g_lagDiagnosticsMayBeEnabled);
}
'''
        with tempfile.TemporaryDirectory(prefix="lag-diagnostics-", dir="/tmp/opencode") as out:
            cpp = Path(out) / "test.cpp"
            binary = Path(out) / "test"
            cpp.write_text(prefix + source + main)
            subprocess.run(["g++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                            "-pthread", "-I", str(CODE), str(cpp), "-o", str(binary)], check=True)
            for mode in ("off", "on", "fail", "writefail", "timed", "threadfail"):
                subprocess.run([str(binary), mode], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
