"""Run the production headless walkers against mock native objects and jobs."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>
#define __fastcall
using ObjectFn = void(*)(uint64_t);
enum class RollbackUnitProfilePhase { Move, SyncJob, SyncSave };
constexpr size_t ROLLBACK_UNIT_CATEGORY_COUNT = 64;
int g_resimulateCurrentFrame = 0;
std::vector<int> calls;
int mutateAt = 0;
uint32_t replacementFlags = 0;
uint64_t firstUnit = 0;
int cpuShortcuts = 0;
void HookLog(const char*, ...) {}
uint64_t Profile_NowEnabled() { return 0; }
void Profile_RegisterUnitObjectType(RollbackUnitProfilePhase, uint32_t, uint64_t) {}
void Profile_RecordUnitObject(RollbackUnitProfilePhase, uint32_t, uint64_t, uint64_t) {}
void Profile_RecordUnitCategory(RollbackUnitProfilePhase, uint32_t, uint64_t) {}
void Rollback_FreezeWatchdogUnit(const char*, uint32_t, uint64_t, uint64_t, int) {}
const char* WatchdogPhaseName(RollbackUnitProfilePhase) { return "test"; }
uint64_t GetObjectDti(uint64_t unit) { return *(uint64_t*)(unit + 8); }
bool Effect_IsExAtkSpaceTransitionEffect(void*, bool) { return false; }
bool Effect_IsExAtkSpaceUnit(void*) { return false; }
bool RunDisabledCpuPostMove(uint64_t unit) {
    ++cpuShortcuts;
    calls.push_back(unit == firstUnit ? 99 : 199);
    return true;
}
void VTableCall(uint64_t unit, uint32_t offset) {
    (*(ObjectFn*)(*(uint64_t*)unit + offset))(unit);
}
void VTableCall(uint64_t unit, uint32_t offset, int value) {
    (*(void(**)(uint64_t, int))(*(uint64_t*)unit + offset))(unit, value);
}
void DisabledCpuPostMove(uint64_t unit) {
    if (!RunDisabledCpuPostMove(unit)) VTableCall(unit, 0x48);
}
void FenceBegin(uint32_t*, int) {}
void FenceEnd(uint32_t*) {}
void Remove(uint64_t, uint64_t) { assert(false); }
void ExecuteJobs(uint64_t main, int) {
    auto& count = *(uint32_t*)(main + 0x98);
    for (uint32_t i = 0; i < count; ++i) {
        auto arg = *(uint64_t*)(main + 0xc8 + i * 0x10);
        auto fn = *(ObjectFn*)(main + 0xd0 + i * 0x10);
        fn(arg);
    }
    count = 0;
}
uintptr_t _addr(uintptr_t address) {
    switch (address) {
    case 0x14079a020: return (uintptr_t)FenceBegin;
    case 0x14079a050: return (uintptr_t)FenceEnd;
    case 0x140521230: return (uintptr_t)ExecuteJobs;
    case 0x14051baf0: case 0x14051b9f0: return (uintptr_t)Remove;
    default: return address;
    }
}
void Record(uint64_t unit, int slot) {
    calls.push_back(slot + (unit == firstUnit ? 0 : 100));
    if (unit == firstUnit && slot == mutateAt)
        *(uint32_t*)(unit + 0x10) = replacementFlags;
}
void Init(uint64_t u) { Record(u, 28); }
void Move(uint64_t u) { Record(u, 40); }
void Sync(uint64_t u) { Record(u, 50); }
void Late(uint64_t u) { Record(u, 48); }
void CategoryBegin(uint64_t, int) {}
void NativeCategory(uint64_t manager, int category) {
    assert(category != 13);
    if (*(uint64_t*)(manager + 0x50 + 0x30 * category))
        calls.push_back(1000 + category);
}
'''

CASES = r'''
template<bool Profiling>
std::pair<std::vector<int>, uint32_t> Run(
    RollbackUnitProfilePhase phase, uint32_t category, bool jobs,
    uint32_t initialFlags, int mutation, uint32_t newFlags, uint64_t dti = 1)
{
    alignas(16) uint8_t mainData[0x1000] = {};
    alignas(16) uint8_t unitManager[0x2000] = {};
    alignas(16) uint8_t units[2][0x80] = {};
    uint64_t vtable[16] = {};
    uint64_t managerVtable[16] = {};
    vtable[0x28 / 8] = (uint64_t)Init;
    vtable[0x40 / 8] = (uint64_t)Move;
    vtable[0x50 / 8] = (uint64_t)Sync;
    vtable[0x48 / 8] = (uint64_t)Late;
    managerVtable[0x78 / 8] = (uint64_t)CategoryBegin;
    for (int slot : {0x48, 0x50, 0x60})
        managerVtable[slot / 8] = (uint64_t)NativeCategory;
    const uint64_t main = (uint64_t)mainData;
    const uint64_t manager = (uint64_t)unitManager;
    *(uint64_t*)manager = (uint64_t)managerVtable;
    *(uint32_t*)(manager + 0xc38) = 22;
    *(uint64_t*)(manager + 0x1848) = ~uint64_t(0);
    *(float*)(main + 0x80) = 1.0f;
    const uint64_t categoryBase = manager + 0x50 + 0x30 * category;
    *(uint32_t*)(categoryBase - 8) = jobs ? 1 : 0;
    *(float*)(categoryBase + 0x10) = 1.0f;
    firstUnit = (uint64_t)units[0];
    *(uint64_t*)categoryBase = firstUnit;
    for (int i = 0; i < 2; ++i) {
        const uint64_t u = (uint64_t)units[i];
        *(uint64_t*)u = (uint64_t)vtable;
        *(uint64_t*)(u + 8) = dti;
        *(uint32_t*)(u + 0x10) = i == 0 ? initialFlags : 0x402;
        *(uint64_t*)(u + 0x18) = i == 0 ? (uint64_t)units[1] : 0;
    }
    calls.clear();
    cpuShortcuts = 0;
    mutateAt = mutation;
    replacementFlags = newFlags;
    const uint32_t categorySlot = phase == RollbackUnitProfilePhase::Move
        ? SUNIT_MOVE_CATEGORY_VTABLE : phase == RollbackUnitProfilePhase::SyncJob
        ? SUNIT_SYNC_JOB_VTABLE : SUNIT_SYNC_SAVE_VTABLE;
    HeadlessUnitPass<Profiling>(main, manager, phase, 3, categorySlot);
    assert(*(uint32_t*)(main + 0x98) == 0);
    return {calls, *(uint32_t*)(firstUnit + 0x10)};
}

int main() {
    using Phase = RollbackUnitProfilePhase;
    for (bool jobs : {false, true}) {
        for (auto phase : {Phase::SyncJob, Phase::SyncSave}) {
            for (uint32_t category : {13u}) {
                for (int mutation : {0, 28, 40, 50}) {
                    for (uint32_t replacement : {2u, 0x403u, 0x404u}) {
                        auto normal = Run<false>(phase, category, jobs, 0x401,
                                                  mutation, replacement);
                        auto profiled = Run<true>(phase, category, jobs, 0x401,
                                                  mutation, replacement);
                        assert(normal == profiled);
                        std::vector<int> first;
                        for (int call : normal.first) if (call < 100) first.push_back(call);
                        std::vector<int> expected{28};
                        if (mutation != 28) {
                            expected.push_back(40);
                            // Native phase 3 always completes this initial pair,
                            // even if Move disables the unit.
                            if (phase == Phase::SyncSave) expected.push_back(50);
                            if (mutation != 40 &&
                                !(phase == Phase::SyncSave && mutation == 50))
                                expected.push_back(phase == Phase::SyncJob ? 50 : 48);
                        }
                        assert(first == expected);
                        // A skipped first unit must not skip the next list node.
                        assert(std::count(normal.first.begin(), normal.first.end(),
                            phase == Phase::SyncJob ? 150 : 148) == 1);
                    }
                }
                for (uint32_t flags : {0x402u, 2u, 0x403u, 0x404u}) {
                    auto a = Run<false>(phase, category, jobs, flags, 0, 0);
                    auto b = Run<true>(phase, category, jobs, flags, 0, 0);
                    assert(a == b);
                    assert(a.first.size() == (flags == 0x402 ? 2u : 1u));
                }
            }
        }
        // Preserve native dispatch outside category 13 in nonprofiling builds.
        auto a = Run<false>(Phase::SyncSave, 21, jobs, 0x402, 0, 0, DTI_UCHRCPU);
        assert(cpuShortcuts == 0 && a.first == std::vector<int>{1021});
        auto b = Run<true>(Phase::SyncSave, 21, jobs, 0x402, 0, 0, DTI_UCHRCPU);
        assert(cpuShortcuts == 2);
        for (auto phase : {Phase::Move, Phase::SyncJob, Phase::SyncSave}) {
            a = Run<false>(phase, 13, jobs, 0x402, 0, 0, DTI_UCHRCHAIN);
            b = Run<true>(phase, 13, jobs, 0x402, 0, 0, DTI_UCHRCHAIN);
            assert(a == b && a.first.empty());
            a = Run<false>(phase, 8, jobs, 0x402, 0, 0);
            assert(a.first == std::vector<int>{1008});
        }
    }
}
'''


class HeadlessUnitPassTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_lifecycle_and_category_dispatch(self):
        source = (ROOT / "UMVC3Hook/code/rollback_tick.cpp").read_text()
        constants = "\n".join(re.findall(
            r"static constexpr (?:uintptr_t|uint32_t) [A-Z_]+\s*=\s*[^;]+;", source))
        filters = source[source.index("static bool IsHeadlessGameplayUnitCategory("):
                         source.index("static constexpr uintptr_t SLEEP_CALL_FIXED")]
        walkers = source[source.index("template <size_t Category>"):
                         source.index("void Rollback_ResyncMainTiming(")]
        with tempfile.TemporaryDirectory(prefix="headless-pass-", dir="/tmp/opencode") as tmp:
            cpp = Path(tmp) / "test.cpp"
            executable = Path(tmp) / "test"
            cpp.write_text(MOCKS + constants + filters + walkers + CASES)
            subprocess.run(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra",
                            "-Werror", str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
