"""Exercise the production CRT-abort bridge against a minimal Windows context mock."""

from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ExceptionTests(unittest.TestCase):
    def test_lifetime_failure_persists_before_trying_normal_logger(self):
        source = (ROOT / "UMVC3Hook/code/lifetime_failure.inl").read_text()
        begin = source.index("void WriteLifetimeFailureRecord(")
        end = source.index("\ntemplate<class T>", begin)
        writer = source[begin:end]
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define _WIN32 1
using HANDLE = void*;
using DWORD = uint32_t;
constexpr int MAX_PATH = 260;
constexpr DWORD FILE_APPEND_DATA = 4, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, FILE_SHARE_DELETE = 4;
constexpr DWORD OPEN_ALWAYS = 4, FILE_ATTRIBUTE_NORMAL = 0x80, FILE_FLAG_WRITE_THROUGH = 0x80000000;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
bool fileUnavailable = false, loggerBusy = true;
int flushes = 0, closes = 0;
std::string persisted, normalLog, debugger;
std::vector<std::string> operations;
int Rollback_ResolveDllPath(char* out, size_t size, const char* relative) {
    return std::snprintf(out, size, "/plugin/%s", relative);
}
HANDLE CreateFileA(const char* path, DWORD access, DWORD share, void*, DWORD mode, DWORD flags, void*) {
    assert(!std::strcmp(path, "/plugin/lifetime_failure.log"));
    assert(access == FILE_APPEND_DATA && share == (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE));
    assert(mode == OPEN_ALWAYS && (flags & FILE_FLAG_WRITE_THROUGH)); operations.push_back("open");
    return fileUnavailable ? INVALID_HANDLE_VALUE : (void*)1;
}
bool WriteFile(HANDLE, const char* text, DWORD size, DWORD* written, void*) {
    *written = std::min(size, DWORD(7)); persisted.append(text, *written); operations.push_back("write"); return true;
}
bool FlushFileBuffers(HANDLE) { ++flushes; operations.push_back("flush"); return true; }
void CloseHandle(HANDLE) { ++closes; operations.push_back("close"); }
void OutputDebugStringA(const char* text) { debugger = text; }
void HookLogTryWriteFatal(const char* text) {
    if (!fileUnavailable) assert(flushes == 1 && closes == 1);
    operations.push_back("normal-log"); if (!loggerBusy) normalLog = text;
}
'''
        main = r'''
int main() {
    const char* message = "reason=ownership mismatch\nexpected=0x0 actual=0x1234\n";
    WriteLifetimeFailureRecord(message);
    assert(persisted == message && debugger == message && normalLog.empty());
    assert(operations.back() == "normal-log");
    assert(std::find(operations.begin(), operations.end(), "flush") <
           std::find(operations.begin(), operations.end(), "normal-log"));
    fileUnavailable = true; loggerBusy = false;
    WriteLifetimeFailureRecord(message); assert(normalLog == message);
}
'''
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="fatal-writer-") as temporary:
            cpp, exe = Path(temporary) / "test.cpp", Path(temporary) / "test"
            cpp.write_text(harness + writer + main)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        logger = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        logger = logger[logger.index("void HookLogTryWriteFatal("):
                        logger.index("static void HookLogWriteConsoleLocked(")]
        self.assertIn("TryAcquireSRWLockExclusive", logger)
        self.assertNotIn("\tAcquireSRWLockExclusive", logger)

    def test_run_log_survives_abort_flushes(self):
        source = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        logger = source[source.index("static SRWLOCK g_hookLogLock"):
                        source.index("static void HookLogWriteConsoleLocked(")]
        harness = r"""
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
using DWORD = unsigned long; using BOOL = int; using HMODULE = void*;
constexpr int MAX_PATH = 260; constexpr BOOL FALSE = 0, TRUE = 1;
constexpr DWORD INVALID_FILE_ATTRIBUTES = DWORD(-1);
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8;
struct SRWLOCK { bool held; };
#define SRWLOCK_INIT {false}
void AcquireSRWLockExclusive(SRWLOCK* lock) { assert(!lock->held); lock->held = true; }
bool TryAcquireSRWLockExclusive(SRWLOCK* lock) { if (lock->held) return false; return lock->held = true; }
void ReleaseSRWLockExclusive(SRWLOCK* lock) { assert(lock->held); lock->held = false; }
struct SYSTEMTIME { unsigned wYear, wMonth, wDay, wHour, wMinute, wSecond, wMilliseconds; };
void GetLocalTime(SYSTEMTIME* time) { *time = {2026, 11, 10, 1, 11, 26, 33}; }
DWORD GetFileAttributesA(const char* path) { struct stat st; return stat(path, &st) ? INVALID_FILE_ATTRIBUTES : 0; }
BOOL MoveFileExA(const char* from, const char* to, DWORD) { return std::rename(from, to) == 0; }
BOOL CopyFileA(const char* from, const char* to, BOOL failIfExists) {
    if (failIfExists && GetFileAttributesA(to) != INVALID_FILE_ATTRIBUTES) return FALSE;
    std::ifstream in(from, std::ios::binary); std::ofstream out(to, std::ios::binary);
    out << in.rdbuf(); return bool(out);
}
DWORD GetModuleFileNameA(HMODULE, char*, DWORD) { return 0; }
DWORD GetPrivateProfileStringA(const char*, const char*, const char*, char*, DWORD, const char*) { return 0; }
int GetPrivateProfileIntA(const char*, const char*, int, const char*) { return 0; }
void IniSafe_WriteString(const char*, const char*, const char*, const char*) {}
"""
        main = r"""
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary); std::stringstream text; text << in.rdbuf(); return text.str();
}
int main(int, char** argv) {
    (void)HookLogConsoleEnabledByConfig; (void)g_asiModule; (void)g_hookConsoleBuffer;
    const std::string dir = argv[1], mainLog = dir + "/Timestone_client.log", runLog = dir + "/run/Timestone_client.log";
    HookLogSetFile(mainLog.c_str());
    HookLogTryWriteFatal("early session line\n");          // flushed before the run log exists
    AcquireSRWLockExclusive(&g_hookLogLock);
    HookLogAppendLocked("pending line\n", 13);
    ReleaseSRWLockExclusive(&g_hookLogLock);
    assert(HookLogSetSnapshotFile(runLog.c_str()));
    assert(slurp(runLog) == slurp(mainLog));
    assert(slurp(runLog).find("early session line\npending line\n") != std::string::npos);
    assert(HookLogSetSnapshotFile(runLog.c_str()));         // re-registration does not duplicate
    // Abort path: the lifetime record flushes, then the crash handler appends and flushes again.
    HookLogTryWriteFatal("=== LIFETIME INVARIANT FAILURE ===\n");
    AcquireSRWLockExclusive(&g_hookLogLock);
    HookLogAppendLocked("=== UMVC3Hook crash ===\n", 24);
    ReleaseSRWLockExclusive(&g_hookLogLock);
    HookLogTryFlush();
    const std::string run = slurp(runLog);
    assert(run == slurp(mainLog));
    assert(run.find("early session line") < run.find("LIFETIME INVARIANT FAILURE"));
    assert(run.find("LIFETIME INVARIANT FAILURE") < run.find("UMVC3Hook crash"));
    assert(std::count(run.begin(), run.end(), '\n') == 5);
    // A held logger lock (abort inside HookLog) neither blocks nor rewrites the run log.
    AcquireSRWLockExclusive(&g_hookLogLock);
    HookLogTryWriteFatal("lost\n"); HookLogTryFlush();
    ReleaseSRWLockExclusive(&g_hookLogLock);
    assert(slurp(runLog) == run);
}
"""
        with tempfile.TemporaryDirectory(prefix="run-log-") as temporary:
            cpp, exe = Path(temporary) / "test.cpp", Path(temporary) / "test"
            (Path(temporary) / "run").mkdir()
            cpp.write_text(harness + logger + main)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-diagnostics-color",
                            str(cpp), "-o", str(exe)], check=True)
            # This GCC's ASan intermittently faults at startup under high-entropy ASLR.
            subprocess.run(["setarch", platform.machine(), "-R", str(exe), temporary], check=True)

    def test_abort_signal_reaches_crash_reporter(self):
        source = (ROOT / "UMVC3Hook/code/exception.cpp").read_text()
        begin = source.index("void AbortSignalHandler(")
        end = source.index("\nLONG CALLBACK VectoredExceptionHandler", begin)
        bridge = source[begin:end]
        harness = r'''
#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using ULONG_PTR = uintptr_t;
struct CONTEXT { uintptr_t Rip, Rsp; };
struct EXCEPTION_RECORD {
    uint32_t ExceptionCode, ExceptionFlags;
    void* ExceptionAddress;
    uint32_t NumberParameters;
    ULONG_PTR ExceptionInformation[15];
};
struct EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; CONTEXT* ContextRecord; };
constexpr uint32_t EXCEPTION_NONCONTINUABLE = 1;
bool fatalMode = false;
int reports = 0;
void RtlCaptureContext(CONTEXT* context) { context->Rip = 0x1234; context->Rsp = 0x5678; }
int HandleException(EXCEPTION_POINTERS* info, bool catastrophicOnly, const char* reason) {
    assert(!catastrophicOnly && !std::strcmp(reason, "SIGABRT (std::abort/CRT termination)"));
    assert(info->ExceptionRecord->ExceptionCode == 0x40000015);
    assert(info->ExceptionRecord->ExceptionFlags == EXCEPTION_NONCONTINUABLE);
    assert(info->ExceptionRecord->ExceptionAddress == (void*)0x1234);
    assert(info->ExceptionRecord->NumberParameters == 1);
    assert(info->ExceptionRecord->ExceptionInformation[0] == SIGABRT);
    assert(info->ContextRecord->Rip == 0x1234 && info->ContextRecord->Rsp == 0x5678);
    ++reports;
    if (fatalMode) {
        std::puts("abort reached crash reporter"); std::fflush(stdout);
        std::_Exit(23); // Avoid a native core dump after verifying real SIGABRT dispatch.
    }
    return 0;
}
'''
        main = r'''
int main(int argc, char**) {
    if (argc > 1) {
        fatalMode = true;
        assert(std::signal(SIGABRT, AbortSignalHandler) != SIG_ERR);
        std::abort();
    }
    AbortSignalHandler(SIGABRT);
    assert(reports == 1);
}
'''
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="exception-") as temporary:
            cpp = Path(temporary) / "test.cpp"
            exe = Path(temporary) / "test"
            cpp.write_text(harness + bridge + main)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            result = subprocess.run([str(exe), "abort"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 23)
            self.assertIn("abort reached crash reporter", result.stdout)

    def test_abort_handler_lifecycle_and_safe_reporting(self):
        source = (ROOT / "UMVC3Hook/code/exception.cpp").read_text()
        self.assertIn("std::signal(SIGABRT, AbortSignalHandler)", source)
        self.assertIn("std::signal(SIGABRT, g_previousAbortHandler)", source)
        self.assertIn("g_previousAbortHandler != SIG_ERR", source)
        bridge = source[source.index("void AbortSignalHandler("):
                        source.index("\nLONG CALLBACK VectoredExceptionHandler")]
        self.assertNotIn("PrepareForGameExit(", bridge)
        self.assertNotIn("RaiseException(", bridge)
        self.assertIn('reason ? reason : "Windows exception"', source)


if __name__ == "__main__":
    unittest.main()
