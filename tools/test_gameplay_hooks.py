"""Exercise production gameplay-hook ownership and restoration with mocked APIs."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
using LPVOID = void*;
using DWORD = unsigned long;
using MH_STATUS = int;
constexpr int MH_OK = 0, MH_ERROR_DISABLED = 1, MH_ERROR_ALREADY_CREATED = 2;
constexpr int PAGE_EXECUTE_READWRITE = 64;
std::map<void*, bool> hooks;
std::vector<void*> queued;
bool failApply = false, failProtect = false, failFlush = false;
int applyCount = 0;
void HookLog(const char*, ...) {}
MH_STATUS MH_CreateHook(void* target, void*, void**) {
    if (hooks.count(target)) return MH_ERROR_ALREADY_CREATED;
    hooks[target] = true;
    return MH_OK;
}
MH_STATUS MH_QueueDisableHook(void* target) {
    assert(hooks.count(target));
    if (!hooks[target]) return MH_ERROR_DISABLED;
    queued.push_back(target);
    return MH_OK;
}
MH_STATUS MH_ApplyQueued() {
    ++applyCount;
    if (failApply) return 99;
    for (void* target : queued) hooks[target] = false;
    queued.clear();
    return MH_OK;
}
bool VirtualProtect(void*, size_t, DWORD, DWORD* old) {
    *old = 32;
    return !failProtect;
}
void* GetCurrentProcess() { return nullptr; }
bool FlushInstructionCache(void*, void*, size_t) { return !failFlush; }
'''

CHECKS = r'''
void reset() {
    g_gameplayTargets.clear(); g_gameplayPatches.clear();
    g_gameplayActive = false; g_gameplayDisabled = false;
    hooks.clear(); queued.clear(); applyCount = 0;
    failApply = failProtect = failFlush = false;
}
int main() {
    int gameplay = 0, graphics = 0, diagnostic = 0;
    uint8_t patch[] = {0x74, 0x0f, 0xcc};
    uintptr_t vtable = 123;
    assert(!GameplayHooks_IsActive());
    assert(GameplayHooks_Create(&gameplay, nullptr, nullptr) == MH_OK);
    assert(GameplayHooks_Create(&gameplay, nullptr, nullptr) == MH_ERROR_ALREADY_CREATED);
    assert(g_gameplayTargets.size() == 1);
    assert(MH_CreateHook(&graphics, nullptr, nullptr) == MH_OK);
    assert(GameplayHooks_Create(&diagnostic, nullptr, nullptr) == MH_OK);
    hooks[&diagnostic] = false; // Diagnostic detours can be intentionally disabled.
    GameplayHooks_RecordPatch(patch, 2);
    patch[1] = 0x4b;
    GameplayHooks_RecordPatch(patch, 2); // Never replace the original capture.
    GameplayHooks_RecordPatch(&vtable, sizeof(vtable));
    vtable = 456;
    GameplayHooks_Activate();
    assert(GameplayHooks_IsActive());
    assert(GameplayHooks_Disable());
    assert(!GameplayHooks_IsActive());
    assert(!hooks[&gameplay] && !hooks[&diagnostic] && hooks[&graphics]);
    assert(patch[0] == 0x74 && patch[1] == 0x0f && patch[2] == 0xcc);
    assert(vtable == 123);
    assert(applyCount == 1);
    assert(GameplayHooks_Disable() && applyCount == 1);
    GameplayHooks_Activate();
    assert(!GameplayHooks_IsActive());
    assert(GameplayHooks_Create(patch, nullptr, nullptr) == MH_ERROR_DISABLED);

    reset();
    GameplayHooks_Create(&gameplay, nullptr, nullptr);
    GameplayHooks_RecordPatch(patch, 2);
    patch[1] = 0x4b;
    failApply = true;
    assert(!GameplayHooks_Disable());
    assert(patch[1] == 0x4b && !g_gameplayDisabled);
    failApply = false;
    assert(GameplayHooks_Disable() && patch[1] == 0x0f);

    reset();
    GameplayHooks_RecordPatch(patch, 2);
    patch[1] = 0x4b;
    failProtect = true;
    assert(!GameplayHooks_Disable());
    assert(patch[1] == 0x4b && !g_gameplayDisabled);
    failProtect = false;
    failFlush = true;
    assert(!GameplayHooks_Disable() && !g_gameplayDisabled);
    failFlush = false;
    assert(GameplayHooks_Disable() && patch[1] == 0x0f);
}
'''


class GameplayHooksTests(unittest.TestCase):
    def test_production_lifecycle(self):
        source = (ROOT / "UMVC3Hook/code/rollback.cpp").read_text()
        lifecycle = source[source.index("namespace {"):source.index("void Rollback_InitHooks()")]
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="gameplay-hooks-") as directory:
            cpp = Path(directory) / "test.cpp"
            exe = Path(directory) / "test"
            cpp.write_text(MOCKS + lifecycle + CHECKS)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_gameplay_installers_are_owned(self):
        files = ["controller", "diag", "autotest", "framestepper", "rollback_hooks",
                 "rollback_runtime", "rollback_rng", "rollback_effect", "rollback_shot",
                 "rollback_model_policy"]
        for name in files:
            with self.subTest(file=name):
                source = (ROOT / f"UMVC3Hook/code/{name}.cpp").read_text()
                self.assertNotIn("MH_CreateHook(", source)
                self.assertIn("GameplayHooks_Create(", source)

    def test_launch_order_and_network_handoff(self):
        source = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        installers = source[source.index("static void InitializeGameplayHooks()\n{"):source.index("void __fastcall Hook_titleLogoUpdate")]
        for installer in ["FrameStepper_Init();", "Controller_InitHooks();", "Diag_InitHooks();"]:
            self.assertIn(installer, installers)
        startup = source[source.index("void OnInitializeHook()"):source.index("static void CompleteLaunchInitialization(LaunchOption opt)\n{")]
        self.assertLess(startup.index("CreateThread(nullptr, 0, LaunchMenuThread"), startup.index("WaitForSingleObject(g_launchWindowShownEvent, INFINITE)"))
        self.assertLess(startup.index("WaitForSingleObject(g_launchWindowShownEvent, INFINITE)"), startup.index("InitializeGameplayHooks();"))
        menu = (ROOT / "UMVC3Hook/code/LaunchMenu.cpp").read_text()
        shown = menu.index("LaunchMenu_NotifyWindowShown();")
        self.assertLess(menu.rindex("UpdateWindow(hWnd);", 0, shown), shown)
        completion = source[source.index("static void CompleteLaunchInitialization(LaunchOption opt)\n{"):]
        self.assertIn("opt == LAUNCH_NORMAL && !GameplayHooks_Disable()", completion)
        self.assertNotIn("FrameStepper_Init();", completion)
        self.assertLess(completion.index("FrameStepper_InitNetplay();"), completion.index("GameplayHooks_Activate();"))
        frame = (ROOT / "UMVC3Hook/code/framestepper.cpp").read_text()
        init = frame[frame.index("void FrameStepper_Init()\n{"):frame.index("// All overlay consumers")]
        self.assertNotIn("FrameStepper_InitNetplay();", init)
        self.assertEqual(init.count("GameplayHooks_RecordPatch("), 2)


if __name__ == "__main__":
    unittest.main()
