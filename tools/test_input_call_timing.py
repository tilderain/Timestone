"""Exercise production input wrappers and call-site patches with mocked Win32 APIs."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InputCallTimingTest(unittest.TestCase):
    def test_wrappers_and_patch_layout(self):
        source = (ROOT / "UMVC3Hook/code/rollback_hooks.cpp").read_text()
        code = source[source.index("template<LagMetric Metric, unsigned Slot"):
                      source.index("static void(__fastcall* oRenderSyncTiming)")]
        sites = re.findall(r'\{(0x[0-9a-f]+), "([^"\n]+)", ([68]),', code)
        self.assertEqual(len(sites), 11)
        fixtures = ",\n".join("{" + addr + ', "' + data + '", ' + size + "}"
                              for addr, data, size in sites)
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>
#include "lag_diagnostics.h"
#define __fastcall
#define WINAPI
using BOOL = int;
using DWORD = unsigned long;
using HWND = void*;
struct RECT { int left, top, right, bottom; };
struct POINT { int x, y; };
struct SYSTEM_INFO { uintptr_t dwAllocationGranularity; };
constexpr int MEM_RESERVE=1, MEM_COMMIT=2, PAGE_READWRITE=4, PAGE_EXECUTE_READWRITE=8;
uint8_t image[0x2000]{};
void* targetPage[32]{};
bool failProtect=false;
struct Patch { void* address; std::vector<uint8_t> before; };
std::vector<Patch> patches;
uintptr_t _addr(uintptr_t address) { return (uintptr_t)image + address - 0x1406aa000; }
void GetSystemInfo(SYSTEM_INFO* info) { info->dwAllocationGranularity=65536; }
void* VirtualAlloc(void*, size_t, int, int) { return targetPage; }
bool VirtualProtect(void*, size_t, DWORD, DWORD* old) { *old=PAGE_READWRITE; return !failProtect; }
void* GetCurrentProcess() { return nullptr; }
bool FlushInstructionCache(void*, void*, size_t) { return true; }
void HookLog(const char*, ...) {}
void GameplayHooks_RecordPatch(void* address, size_t size) {
    auto* bytes = (uint8_t*)address;
    patches.push_back({address, std::vector<uint8_t>(bytes, bytes+size)});
}
std::atomic<bool> g_lagDiagnosticsMayBeEnabled{true};
LagMetric lastMetric;
int32_t lastResult;
uint64_t LagDiagnostics_BeginSlow(LagMetric metric, int32_t) { lastMetric=metric; return 1; }
void LagDiagnostics_End(LagMetric, uint64_t) {}
void LagDiagnostics_EndResult(LagMetric metric, uint64_t start, int32_t result) {
    assert(start==1); lastMetric=metric; lastResult=result;
}
'''
        checks = r'''
void* expectedDevice;
void* expectedState;
uint32_t expectedSize;
int calls=0;
int32_t poll(void* device) { assert(device==expectedDevice); ++calls; return (int32_t)0x8007001e; }
int32_t acquire(void* device) { assert(device==expectedDevice); ++calls; return 1; }
int32_t state(void* device, uint32_t size, void* data) {
    assert(device==expectedDevice && size==expectedSize && data==expectedState);
    ++calls; *(uint8_t*)data=42; return (int32_t)0x80040209;
}
int main() {
    struct Fixture { uintptr_t address; const char* bytes; size_t size; };
    const Fixture fixtures[] = { FIXTURES };
    for (const auto& f : fixtures) std::memcpy((void*)_addr(f.address), f.bytes, f.size);
    failProtect=true;
    InstallInputCallTiming();
    assert(patches.empty());
    for (const auto& f : fixtures) assert(!std::memcmp((void*)_addr(f.address), f.bytes, f.size));
    failProtect=false;
    InstallInputCallTiming();
    assert(patches.size()==11);
    size_t patchIndex=0;
    for (size_t i=0; i<std::size(fixtures); ++i) {
        const auto& f=fixtures[i];
        const auto* bytes=(uint8_t*)_addr(f.address);
        assert(patches[patchIndex].address==bytes && patches[patchIndex].before.size()==f.size);
        assert(!std::memcmp(patches[patchIndex].before.data(), f.bytes, f.size));
        ++patchIndex;
        assert(bytes[0]==0xff && bytes[1]==0x15);
        int32_t displacement;
        std::memcpy(&displacement, bytes+2, 4);
        assert((uintptr_t)(bytes+6)+displacement==(uintptr_t)&targetPage[i]);
        assert(targetPage[i]);
        for (size_t j=6; j<f.size; ++j) assert(bytes[j]==0x90);
    }
    void* vtable[26]{};
    vtable[25]=(void*)&poll; vtable[7]=(void*)&acquire; vtable[9]=(void*)&state;
    void** device=vtable;
    expectedDevice=&device;
    uint8_t buffer[256]{};
    expectedState=buffer;
    using Call=int32_t(*)(void*,uint32_t,void*);
    const LagMetric metrics[]={LagMetric::KeyboardPoll, LagMetric::KeyboardAcquire,
        LagMetric::KeyboardAcquireRetry, LagMetric::KeyboardGetState,
        LagMetric::MousePoll, LagMetric::MouseAcquire,
        LagMetric::MouseAcquireRetry, LagMetric::MouseGetState};
    for (unsigned i=0; i<8; ++i) {
        expectedSize=i<4 ? 256 : 20;
        const auto result=((Call)targetPage[i])(&device, 999, buffer);
        assert(lastMetric==metrics[i] && result==lastResult);
        assert(result==(i%4==0 ? (int32_t)0x8007001e : i%4==3 ? (int32_t)0x80040209 : 1));
    }
    assert(calls==8 && buffer[0]==42);
    // Refuse unexpected/already-patched instructions rather than layering patches.
    InstallInputCallTiming();
    assert(patches.size()==11);
}
'''.replace("FIXTURES", fixtures)
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "input.cpp"
            exe = Path(tmp) / "input"
            cpp.write_text(harness + code + checks)
            subprocess.run(["g++", "-std=c++17", "-I", str(ROOT / "UMVC3Hook/code"),
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
