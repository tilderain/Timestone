import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class NativeMaterialTimingTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_native_passthrough_and_replay_summary(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/rollback_runtime.cpp").read_text()
        declarations = source[source.index("struct NativeMaterialTiming {"):
                              source.index("using ParticleGeneratorUpdateFn")].split("#endif")[0]
        functions = source[source.index("static void TimeNativeMaterialUpdate("):
                           source.index("static void __fastcall hkCharacterModelPostMove(")].split("#endif")[0]
        harness = r'''
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#define __fastcall
using ToonMaterialUpdateFn = void(*)(uint64_t);
thread_local uint64_t now = 0;
thread_local unsigned postCalls = 0, jobsCalls = 0;
std::string logLine;
void Post(uint64_t model) { assert(model == 123); ++postCalls; now += 10; }
void Jobs(uint64_t model) { assert(model == 456); ++jobsCalls; now += 20; }
ToonMaterialUpdateFn g_originalMaterialPost = Post;
ToonMaterialUpdateFn g_originalMaterialJobs = Jobs;
uint64_t Profile_NowEnabled() { return now; }
double Profile_Ms(uint64_t ticks) { return ticks / 1000.0; }
void HookLog(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    logLine = buffer;
}
''' + declarations + functions + r'''
int main() {
    hkMaterialPostTiming(123);
    hkMaterialJobsTiming(456);
    assert(postCalls == 1 && jobsCalls == 1);
    assert(g_nativeMaterialTiming.calls[0] == 0);
    Rollback_BeginMaterialTiming();
    for (int i = 0; i < 7; ++i) {
        hkMaterialPostTiming(123);
        hkMaterialJobsTiming(456);
    }
    std::thread other([] {
        hkMaterialPostTiming(123);
        hkMaterialJobsTiming(456);
        assert(postCalls == 1 && jobsCalls == 1);
        assert(g_nativeMaterialTiming.calls[0] == 0);
    });
    other.join();
    Rollback_EndMaterialTiming(10, 17);
    assert(postCalls == 8 && jobsCalls == 8);
    assert(logLine == "MATCOST start=10 end=17 frames=7 "
        "post_calls=7 post_ms=0.070000 post_max_ms=0.010000 post_per7_ms=0.070000 "
        "jobs_calls=7 jobs_ms=0.140000 jobs_max_ms=0.020000 jobs_per7_ms=0.140000\n");
    hkMaterialPostTiming(123);
    assert(postCalls == 9 && g_nativeMaterialTiming.calls[0] == 7);
    Rollback_BeginMaterialTiming();
    hkMaterialJobsTiming(456);
    Rollback_EndMaterialTiming(20, 22);
    assert(logLine.find("post_calls=0 post_ms=0.000000") != std::string::npos);
    assert(logLine.find("jobs_per7_ms=0.070000") != std::string::npos);
    Rollback_BeginMaterialTiming();
    Rollback_EndMaterialTiming(22, 22);
    assert(logLine.find("jobs_per7_ms=0.000000") != std::string::npos);
}
'''
        with tempfile.TemporaryDirectory(prefix="material-timing-", dir="/tmp/opencode") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-pthread", str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_disabled_by_default(self):
        root = Path(__file__).resolve().parents[1]
        harness = r'''
#include "UMVC3Hook/code/rollback.h"
static_assert(UMVC3_NATIVE_MATERIAL_TIMING == 0);
int main() {
    Rollback_BeginMaterialTiming();
    Rollback_EndMaterialTiming(10, 17);
}
'''
        with tempfile.TemporaryDirectory(prefix="material-disabled-", dir="/tmp/opencode") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I", str(root), str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
