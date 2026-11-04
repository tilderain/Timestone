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
std::map<void*, bool> queued;
bool failApply = false, failProtect = false, failFlush = false, failQueueEnable = false;
int applyCount = 0;
void HookLog(const char*, ...) {}
MH_STATUS MH_CreateHook(void* target, void*, void**) {
    if (hooks.count(target)) return MH_ERROR_ALREADY_CREATED;
    hooks[target] = false;
    return MH_OK;
}
MH_STATUS MH_QueueDisableHook(void* target) {
    assert(hooks.count(target));
    queued[target] = false;
    return MH_OK;
}
MH_STATUS MH_QueueEnableHook(void* target) {
    if (failQueueEnable || !hooks.count(target)) return 99;
    queued[target] = true;
    return MH_OK;
}
MH_STATUS MH_ApplyQueued() {
    ++applyCount;
    if (failApply) return 99;
    for (const auto& [target, enabled] : queued) hooks[target] = enabled;
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
    g_gameplayActive = false; g_gameplayDisabled = g_gameplayEnableFailed = false;
    hooks.clear(); queued.clear(); applyCount = 0;
    failApply = failProtect = failFlush = failQueueEnable = false;
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
    hooks[&graphics] = true; // Graphics hooks use their own installation path.
    assert(GameplayHooks_Create(&diagnostic, nullptr, nullptr) == MH_OK);
    assert(GameplayHooks_QueueEnable(&gameplay) == MH_OK);
    assert(!hooks[&gameplay] && !hooks[&diagnostic]);
    assert(applyCount == 0);
    assert(GameplayHooks_Apply());
    assert(applyCount == 1 && hooks[&gameplay] && !hooks[&diagnostic]);
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
    assert(applyCount == 2);
    assert(GameplayHooks_Disable() && applyCount == 2);
    GameplayHooks_Activate();
    assert(!GameplayHooks_IsActive());
    assert(GameplayHooks_Create(patch, nullptr, nullptr) == MH_ERROR_DISABLED);
    assert(GameplayHooks_QueueEnable(&gameplay) == MH_ERROR_DISABLED);
    assert(!GameplayHooks_Apply() && applyCount == 2);

    reset();
    GameplayHooks_Create(&gameplay, nullptr, nullptr);
    GameplayHooks_Create(&diagnostic, nullptr, nullptr);
    GameplayHooks_QueueEnable(&gameplay);
    failQueueEnable = true;
    assert(GameplayHooks_QueueEnable(&diagnostic) != MH_OK);
    assert(!GameplayHooks_Apply() && applyCount == 0);
    assert(!hooks[&gameplay] && !hooks[&diagnostic]);

    reset();
    GameplayHooks_Create(&gameplay, nullptr, nullptr);
    GameplayHooks_QueueEnable(&gameplay);
    failApply = true;
    assert(!GameplayHooks_Apply() && !hooks[&gameplay]);

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
    def test_native_network_frame_patch(self):
        source = (ROOT / "UMVC3Hook/code/rollback_hooks.cpp").read_text()
        patch = source[source.index("static bool DisableNativeNetworkFrameUpdates()"):
                       source.index("void Rollback_InitMainHooks(")]
        lifecycle_source = (ROOT / "UMVC3Hook/code/rollback.cpp").read_text()
        lifecycle = lifecycle_source[lifecycle_source.index("namespace {"):
                                     lifecycle_source.index("void Rollback_InitHooks()")]
        unit = MOCKS + lifecycle + r'''
uint8_t image[0x80];
uintptr_t _addr(uintptr_t va) {
    assert(va >= 0x140259800 && va < 0x140259880);
    return reinterpret_cast<uintptr_t>(image + (va - 0x140259800));
}
''' + patch + r'''
int main() {
    constexpr size_t offsets[] = {0x13, 0x20, 0x43, 0x59};
    std::memset(image, 0xCC, sizeof(image));
    for (size_t offset : offsets) {
        image[offset] = 0xFF;
        image[offset + 1] = offset < 0x43 ? 0x50 : 0x52;
        image[offset + 2] = 0x30;
    }
    uint8_t original[sizeof(image)];
    std::memcpy(original, image, sizeof(image));
    // Even a mismatch at the final site must reject before the first write.
    image[0x5B] = 0x31;
    assert(!DisableNativeNetworkFrameUpdates());
    assert(image[0x13] == 0xFF && g_gameplayPatches.empty());
    image[0x5B] = 0x30;
    failProtect = true;
    assert(!DisableNativeNetworkFrameUpdates());
    assert(std::memcmp(image, original, sizeof(image)) == 0);
    assert(g_gameplayPatches.empty());
    failProtect = false;
    assert(DisableNativeNetworkFrameUpdates());
    assert(g_gameplayPatches.size() == 4);
    for (size_t i = 0; i < sizeof(image); ++i) {
        bool patched = false;
        for (size_t offset : offsets) patched |= i >= offset && i < offset + 3;
        assert(image[i] == (patched ? 0x90 : original[i]));
    }
    // Duplicate installation must not capture NOPs as the original bytes.
    assert(!DisableNativeNetworkFrameUpdates());
    assert(g_gameplayPatches.size() == 4);
    GameplayHooks_Activate();
    assert(GameplayHooks_Disable());
    assert(std::memcmp(image, original, sizeof(image)) == 0);
}
'''
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="native-net-") as tmp:
            cpp = Path(tmp) / "test.cpp"
            exe = Path(tmp) / "test"
            cpp.write_text(unit)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
        installer = source[source.index("void Rollback_InitMainHooks("):]
        self.assertLess(installer.index("DisableNativeNetworkFrameUpdates()"),
                        installer.index("if constexpr (LAG_BACKEND_TIMING_ENABLED)"))

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
                 "rollback_runtime", "rollback_rng"]
        for name in files:
            with self.subTest(file=name):
                source = (ROOT / f"UMVC3Hook/code/{name}.cpp").read_text()
                self.assertNotIn("MH_CreateHook(", source)
                self.assertIn("GameplayHooks_Create(", source)
                self.assertNotIn("MH_EnableHook(", source)
                self.assertIn("GameplayHooks_QueueEnable(", source)

    def test_native_sli_toggle(self):
        source = (ROOT / "UMVC3Hook/code/rollback_hooks.cpp").read_text()
        hook = source[source.index("static void(__fastcall* oRenderGpuFence)"):
                      source.index("using RetireTiming")]
        for enabled in (False, True):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory() as tmp:
                cpp = Path(tmp) / "fence.cpp"
                exe = Path(tmp) / "fence"
                cpp.write_text(r'''
#include <cassert>
#include <cstdint>
#define __fastcall
enum class LagMetric { RenderGpuFence };
struct LagDiagnosticScope { explicit LagDiagnosticScope(LagMetric) {} };
constexpr bool FORCE_NATIVE_SLI_ON = ''' + str(enabled).lower() + ";\n" + hook + r'''
int calls = 0;
void original(uint64_t render) { assert(render == 123); ++calls; }
uint8_t renderer[0x867731]{};
void create(uint64_t render, uint64_t a2, uint64_t a3, uint64_t a4) {
    assert(render == (uint64_t)renderer && a2 == 2 && a3 == 3 && a4 == 4);
    renderer[0x867730] = 0;
}
void setSli(uint64_t render, uint8_t enabled) {
    assert(render == (uint64_t)renderer);
    renderer[0x867730] = enabled;
}
int main() {
    oRenderGpuFence = original;
    hkRenderGpuFence(123);
    assert(calls == 1); // Never bypass the original fence function directly.
    oRenderCreateDevice = create;
    oRenderSetSli = setSli;
    hkRenderCreateDevice((uint64_t)renderer, 2, 3, 4);
    assert(renderer[0x867730] == (FORCE_NATIVE_SLI_ON ? 1 : 0));
    hkRenderSetSli((uint64_t)renderer, 0);
    assert(renderer[0x867730] == (FORCE_NATIVE_SLI_ON ? 1 : 0));
    hkRenderSetSli((uint64_t)renderer, 1);
    assert(renderer[0x867730] == 1);
}
''')
                subprocess.run(["g++", "-std=c++17", str(cpp), "-o", str(exe)], check=True)
                subprocess.run([str(exe)], check=True)

    def test_launch_order_and_network_handoff(self):
        source = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        installers = source[source.index("static void InitializeGameplayHooks()\n{"):source.index("void __fastcall Hook_titleLogoUpdate")]
        for installer in ["FrameStepper_Init();", "Controller_InitHooks();", "Diag_InitHooks();"]:
            self.assertIn(installer, installers)
            self.assertLess(installers.index(installer), installers.index("GameplayHooks_Apply()"))
        self.assertEqual(installers.count("GameplayHooks_Apply()"), 1)
        rng = (ROOT / "UMVC3Hook/code/rollback_rng.cpp").read_text()
        for diagnostic in ["FN_RNG_RANDOM_INT", "FN_HSIENKO_SELECT_ANKI", "FN_SET_RESERVED_ANKI"]:
            self.assertNotIn(f"GameplayHooks_QueueEnable((LPVOID)_addr({diagnostic}))", rng)
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
