"""Exercise production live-threading functions with mocked native state."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "UMVC3Hook/code/LaunchMenu_settings.cpp"


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <algorithm>
#define __fastcall
constexpr int MAX_PATH = 260;
alignas(8) uint8_t mainState[0x40090] = {};
uint64_t singleton = reinterpret_cast<uint64_t>(mainState);
bool g_threadingHooksReady = true;
bool writable = true;
int setterCalls = 0, groupCalls = 0;
uint8_t expectedFlag = 0;
void LoadSettings() {}
void GetSettingsPath(char*, size_t) {}
void HookLog(const char*, ...) {}
bool IniSafe_WriteString(const char*, const char*, const char*, const char*) {
    return writable;
}
void SetCount(uint64_t state, uint32_t count) {
    ++setterCalls;
    *reinterpret_cast<uint32_t*>(state + 0xA8) = std::clamp(count, 1u, 16u);
}
uintptr_t _addr(uintptr_t address) {
    if (address == 0x140e177e8) return reinterpret_cast<uintptr_t>(&singleton);
    assert(address == 0x140522450);
    return reinterpret_cast<uintptr_t>(&SetCount);
}
void OriginalMove(void* group) {
    ++groupCalls;
    assert(*(static_cast<uint8_t*>(group) + 0xB0) == expectedFlag);
}
using GeometryGroupMoveFn = void(*)(void*);
GeometryGroupMoveFn g_geometryGroupMove = OriginalMove;
'''

CASES = r'''
int main() {
    auto& requested = *reinterpret_cast<uint32_t*>(mainState + 0xA8);
    auto& active = *reinterpret_cast<uint32_t*>(mainState + 0xA4);
    requested = active = 8;
    assert(ApplyAdvancedLaunchConfig());
    assert(requested == 1);
    assert(active == requested);
    for (uint32_t count : {4u, 16u, 8u, 2u}) {
        requested = count;
        assert(IsGameSingleThreaded());
        assert(ApplyAdvancedLaunchConfig());
        assert(requested == 1);
        assert(active == requested);
        int previousCalls = setterCalls;
        assert(ApplyAdvancedLaunchConfig());
        assert(setterCalls == previousCalls);
        for (uint8_t flag : {0, 1}) {
            uint8_t group[0xB8] = {};
            group[0xB0] = flag;
            expectedFlag = 0;
            HookGeometryGroupMove(group);
            assert(group[0xB0] == flag);
        }
    }
    assert(groupCalls == 8);
    active = 8;
    assert(ApplyAdvancedLaunchConfig());
    assert(requested == 1 && active == 1);
    singleton = 0;
    assert(!ApplyAdvancedLaunchConfig());
    singleton = reinterpret_cast<uint64_t>(mainState);
    g_threadingHooksReady = false;
    assert(!ApplyAdvancedLaunchConfig());
}
'''


class LiveSingleThreadTests(unittest.TestCase):
    def test_production_transitions(self):
        source = SOURCE.read_text()
        signatures = [
            "void __fastcall HookGeometryGroupMove(",
            "bool ApplyAdvancedLaunchConfig(",
            "bool IsGameSingleThreaded(",
        ]
        code = MOCKS + "\n".join(function(source, s) for s in signatures) + CASES
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            cpp = path / "test.cpp"
            cpp.write_text(code)
            executable = path / "test"
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_workers_and_frame_polling(self):
        source = SOURCE.read_text()
        self.assertNotIn("0x14051FC6D", source)
        self.assertNotIn("PatchBytes", source)
        frame = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        self.assertIn("    ApplyAdvancedLaunchConfig();", frame)
        self.assertNotIn("advancedConfigApplied", frame)
        for name in ("LaunchMenu_settings.cpp", "LaunchMenu_settings.h", "umvc3menu.cpp", "escmenu_imgui.cpp"):
            text = (ROOT / "UMVC3Hook/code" / name).read_text()
            self.assertNotIn("SetGameSingleThreaded", text)
            self.assertNotIn("EntireGameSingleThreaded", text)
            self.assertNotIn("ID_SINGLE_THREAD", text)


if __name__ == "__main__":
    unittest.main()
