import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class MaterialPostSkipTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_exact_classes_live_passthrough_and_installation(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/rollback_runtime.cpp").read_text()
        constants = "\n".join(re.search(
            rf"static constexpr uintptr_t {name} = [^;]+;", source
        ).group() for name in ("FN_BASE_MODEL_MATERIAL_POST", "VTABLE_UCHRWEAPON",
                              "VTABLE_UKOBUN", "VTABLE_ULINLIN"))
        hook = source[source.index("static void __fastcall hkBaseModelMaterialPost("):
                      source.index("static void __fastcall hkParticleGeneratorModelUpdate(")]
        install = source[source.index("    MH_STATUS materialPostSkipStatus ="):
                         source.index("    const MH_STATUS particleModelStatus =")]
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#define __fastcall
// Exercise relocated vtables as well as native-address installation.
#define _addr(address) ((address) + 0x100000)
using LPVOID = void*;
using MH_STATUS = int;
constexpr MH_STATUS MH_OK = 0;
using ToonMaterialUpdateFn = void(*)(uint64_t);
''' + constants + r'''
ToonMaterialUpdateFn g_originalBaseModelMaterialPost = nullptr;
bool g_isResimulating = false, installed = false;
unsigned nativeCalls = 0, creates = 0, enables = 0, lightingCalls = 0, jobCalls = 0;
uint64_t lastModel = 0;
MH_STATUS failCreate = 0, failEnable = 0;
void Native(uint64_t model) { ++nativeCalls; lastModel = model; }
void HookLog(const char*, ...) {}
MH_STATUS MH_CreateHook(LPVOID address, LPVOID replacement, LPVOID* original) {
    assert(address == (LPVOID)_addr(FN_BASE_MODEL_MATERIAL_POST) && replacement);
    ++creates;
    if (failCreate) return failCreate;
    *original = (LPVOID)Native;
    return MH_OK;
}
MH_STATUS MH_EnableHook(LPVOID address) {
    assert(address == (LPVOID)_addr(FN_BASE_MODEL_MATERIAL_POST));
    ++enables;
    if (failEnable) return failEnable;
    installed = true;
    return MH_OK;
}
''' + hook + r'''
void Install() {
''' + install + r'''
}
int main() {
    Install();
    assert(installed && creates == 1 && enables == 1);
    alignas(16) std::array<uint64_t, 16> model{};
    const uint64_t address = (uint64_t)model.data();
    for (uint64_t vtable : {VTABLE_UCHRWEAPON, VTABLE_UKOBUN, VTABLE_ULINLIN}) {
        model.fill(0x5A5A5A5A5A5A5A5A);
        model[0] = _addr(vtable);
        const auto saved = model;
        const unsigned before = nativeCalls;
        g_isResimulating = false;
        hkBaseModelMaterialPost(address);
        assert(nativeCalls == before + 1 && lastModel == address);
        g_isResimulating = true;
        for (int frame = 0; frame < 7; ++frame) {
            ++jobCalls; // Separate native move phase is not intercepted.
            hkBaseModelMaterialPost(address);
            ++lightingCalls; // Shared post continues after the material call.
        }
        assert(nativeCalls == before + 1 && model == saved);
        g_isResimulating = false;
        hkBaseModelMaterialPost(address);
        assert(nativeCalls == before + 2);
    }
    assert(jobCalls == 21 && lightingCalls == 21);
    g_isResimulating = true;
    // Character, thrown-weapon shots, effect models and unknown/derived tables stay native.
    for (uint64_t vtable : {0x140AB1460ULL, 0x140AB18B0ULL, 0x140A79488ULL,
                            0x140A70CD0ULL, 0x140A6A510ULL}) {
        model[0] = _addr(vtable);
        const unsigned before = nativeCalls;
        hkBaseModelMaterialPost(address);
        assert(nativeCalls == before + 1 && lastModel == address);
    }
    const unsigned before = nativeCalls;
    hkBaseModelMaterialPost(0);
    assert(nativeCalls == before + 1 && lastModel == 0);
    installed = false;
    creates = enables = 0;
    failCreate = 7;
    Install();
    assert(!installed && creates == 1 && enables == 0);
    creates = enables = 0;
    failCreate = 0;
    failEnable = 8;
    Install();
    assert(!installed && creates == 1 && enables == 1);
}
'''
        with tempfile.TemporaryDirectory(prefix="material-post-skip-", dir="/tmp/opencode") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
