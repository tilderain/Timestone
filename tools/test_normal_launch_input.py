"""Normal launches must preserve native controller detection and input routing."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class NormalLaunchInputTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_only_dual_training_requires_controller_focus(self):
        source = (ROOT / "UMVC3Hook/code/controller.cpp").read_text()
        harness = r'''
#include <cassert>
#include <cstdint>
#define __fastcall
enum LaunchOption { LAUNCH_NORMAL, LAUNCH_TRAINING, LAUNCH_DUAL_TRAINING };
enum class RollbackHookProfileId { XInputToMask };
struct RollbackHookProfileScope {
    explicit RollbackHookProfileScope(RollbackHookProfileId) {}
};
struct XInputState { uint32_t buttons; };
LaunchOption option;
bool foreground;
const XInputState* received;
unsigned calls;
LaunchOption GetLaunchOption() { return option; }
bool IsGameProcessForeground() { return foreground; }
void oxinputToGameMask(uint64_t pad, uint64_t slot, const XInputState* state) {
    assert(pad == 1 && slot == 2);
    received = state;
    ++calls;
}
''' + block(source, "static void __fastcall hkxinputToGameMask(") + r'''
int main() {
    const XInputState input{0x1234};
    for (int mode = LAUNCH_NORMAL; mode <= LAUNCH_DUAL_TRAINING; ++mode) {
        option = static_cast<LaunchOption>(mode);
        for (int focused = 0; focused <= 1; ++focused) {
            foreground = focused != 0;
            calls = 0;
            hkxinputToGameMask(1, 2, &input);
            assert(calls == 1);
            if (option == LAUNCH_DUAL_TRAINING && !foreground)
                assert(received != &input && received->buttons == 0);
            else
                assert(received == &input);
        }
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="controller-focus-") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_native_input_is_untouched_only_for_normal_launch(self):
        source = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        training = block(source, "\n\tif (opt == LAUNCH_TRAINING || opt == LAUNCH_DUAL_TRAINING)")
        hooks = block(source, "\n\tif (opt != LAUNCH_NORMAL) {\n\t\t// Frame stepper")
        for call in ("FrameStepper_Init();", "Controller_InitHooks();", "Diag_InitHooks();"):
            self.assertEqual(source.count(call), 1)
            self.assertIn(call, hooks)
        for address in ("0x1402B4140", "0x1402B4141", "0x1402B4142", "0x1402E57EE"):
            self.assertEqual(source.count(f"_addr({address})"), 1)
            self.assertIn(f"_addr({address})", training)

        harness = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
enum LaunchOption { LAUNCH_NORMAL, LAUNCH_TRAINING, LAUNCH_DUAL_TRAINING };
std::vector<uint64_t> patches;
unsigned frameStepper = 0, controllers = 0, diagnostics = 0, trainingBoot = 0;
uint64_t _addr(uint64_t address) { return address; }
namespace Memory::VP {
template<class T> void Patch(uint64_t address, T) { patches.push_back(address); }
void Nop(uint64_t address, size_t) { patches.push_back(address); }
}
void HookLog(const char*, ...) {}
void FrameStepper_Init() { ++frameStepper; }
void Controller_InitHooks() { ++controllers; }
void Diag_InitHooks() { ++diagnostics; }
void TrainingBoot_InitHooks() { ++trainingBoot; }
void initialize(LaunchOption opt) {
''' + training + hooks + r'''
}
int main() {
    for (auto opt : {LAUNCH_NORMAL, LAUNCH_TRAINING, LAUNCH_DUAL_TRAINING}) {
        patches.clear();
        frameStepper = controllers = diagnostics = trainingBoot = 0;
        initialize(opt);
        const bool customized = opt != LAUNCH_NORMAL;
        assert(frameStepper == customized && controllers == customized);
        assert(diagnostics == customized && trainingBoot == customized);
        assert(patches.size() == (customized ? 5u : 0u));
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="normal-launch-input-") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
