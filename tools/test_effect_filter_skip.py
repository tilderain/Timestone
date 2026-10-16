import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class EffectFilterSkipTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_headless_gate_and_hook_failures(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/rollback_runtime.cpp").read_text()
        constants = "\n".join(re.search(
            rf"static constexpr uintptr_t {name} = [^;]+;", source
        ).group() for name in ("FN_PARTICLE_FILTER_UPDATE",))
        start = source.index("    MH_STATUS particleFilterStatus =")
        end = source.index("    const MH_STATUS jointPosStatus =", start)
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#define __fastcall
#define _addr(address) (address)
using LPVOID = void*;
using MH_STATUS = int;
constexpr MH_STATUS MH_OK = 0;
''' + constants + r'''
using ParticleGeneratorUpdateFn = void(*)(uint64_t);
ParticleGeneratorUpdateFn g_originalParticleGeneratorFilterUpdate = nullptr;
bool g_isResimulating = false;
unsigned nativeCalls = 0, creates = 0, enables = 0;
int failCreate = 0, failEnable = 0;
bool installed = false;
uint64_t lastGenerator = 0;
void Native(uint64_t generator) { ++nativeCalls; lastGenerator = generator; }
void HookLog(const char*, ...) {}
MH_STATUS MH_CreateHook(LPVOID address, LPVOID replacement, LPVOID* original) {
    assert(address == (LPVOID)FN_PARTICLE_FILTER_UPDATE && replacement);
    ++creates;
    if (failCreate) return failCreate;
    *original = (LPVOID)Native;
    return MH_OK;
}
MH_STATUS MH_EnableHook(LPVOID address) {
    assert(address == (LPVOID)FN_PARTICLE_FILTER_UPDATE);
    ++enables;
    if (failEnable) return failEnable;
    installed = true;
    return MH_OK;
}
''' + block(source, "static void __fastcall hkParticleGeneratorFilterUpdate(") + r'''
void Install() {
''' + source[start:end] + r'''
}
int main() {
    Install();
    assert(installed && creates == 1 && enables == 1);
    alignas(16) std::array<uint8_t, 0x240> generator{};
    alignas(16) std::array<uint8_t, 0x2C0> owner{};
    generator.fill(0x5A);
    owner.fill(0xA5);
    *(uint64_t*)(generator.data() + 0x10) = (uint64_t)owner.data();
    const auto savedGenerator = generator;
    const auto savedOwner = owner;

    hkParticleGeneratorFilterUpdate((uint64_t)generator.data());
    assert(nativeCalls == 1 && lastGenerator == (uint64_t)generator.data());
    g_isResimulating = true;
    for (int frame = 0; frame < 100; ++frame)
        hkParticleGeneratorFilterUpdate((uint64_t)generator.data());
    assert(nativeCalls == 1 && generator == savedGenerator && owner == savedOwner);

    // Resimulation skips submission regardless of owner or generator validity.
    *(uint64_t*)owner.data() = 0x1234;
    hkParticleGeneratorFilterUpdate((uint64_t)generator.data());
    assert(nativeCalls == 1);
    *(uint64_t*)(generator.data() + 0x10) = 0;
    hkParticleGeneratorFilterUpdate((uint64_t)generator.data());
    assert(nativeCalls == 1);
    hkParticleGeneratorFilterUpdate(0);
    assert(nativeCalls == 1);

    generator = savedGenerator;
    owner = savedOwner;
    g_isResimulating = false;
    hkParticleGeneratorFilterUpdate((uint64_t)generator.data());
    assert(nativeCalls == 2); // First visible update resumes native submission.
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
        with tempfile.TemporaryDirectory(prefix="effect-filter-test-") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
