"""Run production reset/overlay admission functions against a mocked D3D device.

These tests cover the plugin contract, not native renderer/driver compatibility.
"""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"^(?:\[\[noreturn\]\] )?[\w *&]+\b" + name + r"\([^;]*?\)\n\{", source, re.M)
    if match is None:
        raise AssertionError(f"Missing production function: {name}")
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    start = match.start()
    previous = source.rfind("\n", 0, start - 1) + 1
    if source[previous:start].startswith("template<"):
        start = previous
    return source[start:end] + "\n"


MOCKS = r'''
#include <atomic>
#include <array>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <vector>
#include <stdexcept>
#define WINAPI
#define __fastcall
using HRESULT = int32_t;
using DWORD = unsigned long;
using HWND = void*;
using LONG = int32_t;
using D3DDEVTYPE = uint32_t;
using ULONGLONG = unsigned long long;
constexpr HRESULT D3D_OK = 0;
constexpr HRESULT D3DERR_INVALIDCALL = HRESULT(0x8876086C);
constexpr HRESULT D3DERR_DEVICELOST = HRESULT(0x88760868);
constexpr HRESULT D3DERR_DEVICENOTRESET = HRESULT(0x88760869);
constexpr HRESULT E_UNEXPECTED = HRESULT(0x8000FFFF), E_FAIL = HRESULT(0x80004005);
constexpr unsigned D3DFMT_UNKNOWN = 0, D3DMULTISAMPLE_NONE = 0;
constexpr unsigned D3DSWAPEFFECT_DISCARD = 1, D3DSWAPEFFECT_FLIP = 2, D3DSWAPEFFECT_COPY = 3;
constexpr unsigned ERROR_TIMEOUT = 1460;
constexpr unsigned ERROR_CANCELLED = 1223;
#define HRESULT_FROM_WIN32(x) HRESULT(0x80070000 | (x))
#define FAILED(x) ((x) < 0)
#define SUCCEEDED(x) ((x) >= 0)
struct D3DPRESENT_PARAMETERS {
    unsigned BackBufferWidth = 0, BackBufferHeight = 0, BackBufferFormat = 0;
    unsigned BackBufferCount = 0, MultiSampleType = 0, MultiSampleQuality = 0;
    unsigned SwapEffect = 0;
    void* hDeviceWindow = nullptr;
    unsigned Windowed = 0, EnableAutoDepthStencil = 0, AutoDepthStencilFormat = 0;
    unsigned Flags = 0, FullScreen_RefreshRateInHz = 0, PresentationInterval = 0;
};
std::vector<HRESULT> cooperativeResults;
struct D3DDEVICE_CREATION_PARAMETERS { HWND hFocusWindow = nullptr; };
struct D3DCAPS9 { unsigned MaxTextureWidth = 16384, MaxTextureHeight = 16384;
                 unsigned PresentationIntervals = 0; };
struct IDirect3DDevice9 {
    HRESULT cooperative = D3DERR_DEVICELOST;
    HWND focusWindow = reinterpret_cast<HWND>(123);
    unsigned maxWidth = 16384;
    HRESULT GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) {
        p->hFocusWindow = focusWindow; return D3D_OK;
    }
    HRESULT GetDeviceCaps(D3DCAPS9* p) {
        *p = {}; p->MaxTextureWidth = maxWidth; return D3D_OK;
    }
    HRESULT TestCooperativeLevel() {
        if (cooperativeResults.empty()) return cooperative;
        const HRESULT result = cooperativeResults.front();
        cooperativeResults.erase(cooperativeResults.begin());
        return result;
    }
    HRESULT Reset(D3DPRESENT_PARAMETERS*);
};
bool g_deviceLock = false;
void AcquireSRWLockExclusive(bool* lock) { assert(!*lock); *lock = true; }
bool TryAcquireSRWLockExclusive(bool* lock) {
    if (*lock) return false;
    *lock = true;
    return true;
}
void ReleaseSRWLockExclusive(bool* lock) { assert(*lock); *lock = false; }
void AcquireSRWLockShared(bool* lock) { AcquireSRWLockExclusive(lock); }
void ReleaseSRWLockShared(bool* lock) { ReleaseSRWLockExclusive(lock); }
LONG InterlockedExchange(LONG* value, LONG next) { LONG old = *value; *value = next; return old; }
std::atomic<unsigned> g_transitions { 0 };
std::atomic<bool> g_nativeReady { false };
IDirect3DDevice9* g_device = nullptr;
void* g_resetTarget = reinterpret_cast<void*>(1);
bool g_deviceOperational = true, g_pluginInvalidated = false;
bool g_managedReset = false;
bool g_creatingNativeDevice = false;
DWORD g_createThread = 7, currentThread = 7;
ULONGLONG tick = 10, g_nextPluginRetry = 0;
int releasePause = 0, releaseImGui = 0, recreateImGui = 0;
int resetCalls = 0, remembers = 0;
bool contextExists = true, recreateSucceeds = true;
HRESULT resetResult = D3D_OK;
D3DPRESENT_PARAMETERS remembered = {}, seen = {};
D3DPRESENT_PARAMETERS g_effective = {};
bool g_haveActive = false;
void* g_creationFocusWindow = nullptr;
D3DCAPS9 g_caps;
DWORD GetCurrentThreadId() { return currentThread; }
ULONGLONG GetTickCount64() { return tick; }
void HookLog(const char*, ...) {}
void D3D9ResetDiagnostics_RecordDeviceReset() {}
void FrameStepper_InvalidateDeviceObjects() { ++releasePause; }
void FrameStepper_SetDevice(IDirect3DDevice9*) {}
namespace ImGui { bool GetCurrentContext() { return contextExists; } }
void ImGui_ImplDX9_InvalidateDeviceObjects() { ++releaseImGui; }
bool ImGui_ImplDX9_CreateDeviceObjects() {
    assert(g_deviceOperational && g_nativeReady);
    ++recreateImGui;
    return recreateSucceeds;
}
void PrepareD3D9PresentationParameters(D3DPRESENT_PARAMETERS* p, const char*) {
    p->Windowed = 1;
    p->FullScreen_RefreshRateInHz = 0;
}
void RememberActive(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& p) {
    assert(g_deviceLock);
    g_device = device;
    ++remembers;
    remembered = p;
    g_effective = p;
    g_haveActive = true;
    g_deviceOperational = true;
}
std::vector<HRESULT> resetResults;
unsigned transitionsInDriver = 0;
HRESULT FakeReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* p) {
    assert(!g_deviceLock); // Never hold the plugin lock across external code.
    if (device == g_device) assert(g_transitions > 0 && g_managedReset);
    transitionsInDriver = g_transitions;
    ++resetCalls;
    seen = *p;
    p->BackBufferWidth = p->BackBufferHeight = p->BackBufferCount = 0;
    HRESULT result = resetResult;
    if (!resetResults.empty()) {
        result = resetResults.front();
        resetResults.erase(resetResults.begin());
    }
    device->cooperative = result == D3D_OK ? D3D_OK : D3DERR_DEVICENOTRESET;
    return result;
}
auto g_originalReset = FakeReset;
'''


OWNERSHIP_MOCKS = r'''
struct IDirect3D9 {
    IDirect3DDevice9* device;
    bool nestedHelper = false;
    HRESULT CreateDevice(uint32_t, D3DDEVTYPE, HWND, uint32_t,
                         D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
};
void* nativeRenderer = nullptr;
void* nativeApp = nullptr;
uintptr_t _addr(uintptr_t address) {
    assert(address == 0x140E179A8);
    return reinterpret_cast<uintptr_t>(&nativeRenderer);
}
unsigned prepared = 0, windowsApplied = 0, installed = 0, factoryCalls = 0;
unsigned relaunchDisabled = 0;
bool installFails = false;
HWND policyWindow = nullptr;
void DisableReturnToLaunchMenuOnExit() { ++relaunchDisabled; }
[[noreturn]] void FailTransition(HRESULT, const char*, bool = true) {
    throw std::runtime_error("stopped");
}
void PrepareD3D9NativeCreate(HWND window, D3DPRESENT_PARAMETERS* p) {
    ++prepared; policyWindow = window;
    PrepareD3D9PresentationParameters(p, "native");
}
void ApplyD3D9BorderlessWindow() { ++windowsApplied; }
bool InstallDeviceHook(IDirect3DDevice9*);
void RecordNativeDevice(IDirect3DDevice9*, const D3DPRESENT_PARAMETERS&);
[[noreturn]] void D3D9Reset_AbortStartup(const char*);
'''


OWNERSHIP_CASES = r'''
bool InstallDeviceHook(IDirect3DDevice9* device) {
    ++installed;
    assert(device == g_device && Field<uint32_t>(nativeApp, 0x2AC4) == 3);
    return !installFails;
}
HRESULT IDirect3D9::CreateDevice(uint32_t, D3DDEVTYPE, HWND, uint32_t,
                                D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** output) {
    ++factoryCalls;
    if (nestedHelper) {
        IDirect3DDevice9 helper;
        IDirect3D9 helperCore { &helper };
        D3DPRESENT_PARAMETERS helperRequest = {};
        IDirect3DDevice9* helperOutput = nullptr;
        helperCore.CreateDevice(0, 1, nullptr, 0, &helperRequest, &helperOutput);
        assert(helperOutput == &helper && g_device != &helper && prepared == 1);
    }
    // An overlay may use a temporary output before returning a wrapped device.
    IDirect3DDevice9* temporary = device;
    *output = temporary;
    p->BackBufferWidth = p->BackBufferHeight = p->BackBufferCount = 0;
    return D3D_OK;
}
int main() {
    std::array<uint64_t, 0xD0 / 8> render = {};
    std::array<uint64_t, 0x2B00 / 8> app = {};
    nativeRenderer = render.data(); nativeApp = app.data();
    IDirect3DDevice9 primary, helper;
    primary.maxWidth = 8192;
    helper.maxWidth = 2048;
    IDirect3D9 core { &primary, true }, otherCore { &helper };
    D3DPRESENT_PARAMETERS p = {};
    p.BackBufferWidth = 2560; p.BackBufferHeight = 1440; p.BackBufferCount = 1;
    const auto initial = p;
    auto** output = &Field<IDirect3DDevice9*>(nativeRenderer, 0xB0);
    NativeCreateParameters request { 0, 1, primary.focusWindow, 0x46, &p, output };
    Field<IDirect3D9*>(nativeRenderer, 0xC0) = &core;
    Field<uint32_t>(nativeApp, 0x2AC4) = 3;
    Field<IDirect3D9*>(nativeApp, 0x2AD8) = &core;
    Field<NativeCreateParameters*>(nativeApp, 0x2AE0) = &request;
    currentThread = 77;
    assert(CreateRendererDevice(nativeApp));
    assert(Field<uint32_t>(nativeApp, 0x2AC4) == 0 && installed == 1);
    assert(g_device == &primary && g_createThread == 77 && g_caps.MaxTextureWidth == 8192);
    assert(remembered.BackBufferWidth == 2560 && remembered.BackBufferHeight == 1440);
    assert(remembered.Windowed == 1 && p.BackBufferWidth == 0);
    assert(prepared == 1 && factoryCalls == 2 && windowsApplied == 1);
    assert(policyWindow == primary.focusWindow && !g_nativeReady && !g_creatingNativeDevice);

    // An unrelated job with the same D3D core must not capture ownership/policy.
    g_nativeReady = true;
    IDirect3DDevice9* helperOutput = nullptr;
    request.output = &helperOutput;
    Field<uint32_t>(nativeApp, 0x2AC4) = 3;
    assert(!CreateRendererDevice(nativeApp));
    assert(prepared == 1 && installed == 1 && g_nativeReady);
    assert(g_device == &primary && g_caps.MaxTextureWidth == 8192);

    // A different core cannot claim the renderer output slot either.
    request.output = output;
    Field<IDirect3D9*>(nativeApp, 0x2AD8) = &otherCore;
    assert(!CreateRendererDevice(nativeApp) && g_device == &primary);

    // Device-hook failure stops startup before publishing native completion.
    Field<IDirect3D9*>(nativeApp, 0x2AD8) = &core;
    core.nestedHelper = false;
    g_device = nullptr; *output = nullptr; p = initial;
    installFails = true;
    bool stopped = false;
    try { CreateRendererDevice(nativeApp); }
    catch (const std::runtime_error&) { stopped = true; }
    assert(stopped && relaunchDisabled == 1 && !g_nativeReady);
    assert(Field<uint32_t>(nativeApp, 0x2AC4) == 3);
}
'''


INSTALL_MOCKS = r'''
#include <set>
using MH_STATUS = int;
constexpr int MH_OK = 0, MH_ERROR_UNSUPPORTED_FUNCTION = 8;
enum class Failure { None, Create, Queue, Apply, Enable } failure = Failure::None;
unsigned failAt = 1, createCalls = 0, queueCalls = 0, removed = 0;
const auto unrelated = reinterpret_cast<void*>(0xABCD);
std::set<void*> created, enabled;
uintptr_t _addr(uintptr_t address) { return address; }
int MH_CreateHook(void* target, void*, void** original) {
    if (++createCalls == failAt && failure == Failure::Create) return MH_ERROR_UNSUPPORTED_FUNCTION;
    created.insert(target); *original = target; return MH_OK;
}
int MH_QueueEnableHook(void*) {
    if (++queueCalls == failAt && failure == Failure::Queue) return MH_ERROR_UNSUPPORTED_FUNCTION;
    return MH_OK;
}
int MH_ApplyQueued() {
    enabled = created; // Also exercise rollback of partially enabled hooks.
    return failure == Failure::Apply ? MH_ERROR_UNSUPPORTED_FUNCTION : MH_OK;
}
int MH_EnableHook(void* target) {
    if (failure == Failure::Enable) return MH_ERROR_UNSUPPORTED_FUNCTION;
    enabled.insert(target); return MH_OK;
}
int MH_DisableHook(void* target) { assert(target != unrelated); enabled.erase(target); return MH_OK; }
int MH_RemoveHook(void* target) {
    assert(target != unrelated); created.erase(target); enabled.erase(target); ++removed; return MH_OK;
}
#define PLACEHOLDER(name, original) void name() {} void* original = nullptr;
PLACEHOLDER(HookBeginFrame, g_originalBeginFrame)
int D3D9Reset_GetRenderingThreadOverride() { return -1; }
PLACEHOLDER(HookResultHandler, g_originalResultHandler)
PLACEHOLDER(HookApplyGraphics, g_originalApplyGraphics)
PLACEHOLDER(HookRelease, g_originalRelease)
PLACEHOLDER(HookAppReset, g_originalAppReset)
PLACEHOLDER(HookCooperative, g_originalCooperative)
PLACEHOLDER(HookUIUpdate, g_originalUIUpdate)
PLACEHOLDER(HookResolution, g_originalResolution)
PLACEHOLDER(HookFullscreen, g_originalFullscreen)
PLACEHOLDER(HookVsync, g_originalVsync)
PLACEHOLDER(HookResize, g_originalResize)
PLACEHOLDER(HookSyncSettings, g_originalSync)
PLACEHOLDER(HookWindowRedraw, g_originalWindowRedraw)
void HookResetDevice() {}
'''


INSTALL_CASES = r'''
void ResetTest(Failure next, unsigned at = 1) {
    failure = next; failAt = at;
    createCalls = queueCalls = removed = 0;
    created = enabled = {unrelated};
    g_resetTarget = nullptr;
}
int main() {
    for (unsigned i = 1; i <= 12; ++i) {
        ResetTest(Failure::Create, i);
        assert(!D3D9Reset_InstallNativeHooks() && removed == i - 1);
        assert(created == std::set<void*>{unrelated} && enabled == created);
        ResetTest(Failure::Queue, i);
        assert(!D3D9Reset_InstallNativeHooks() && removed == i);
        assert(created == std::set<void*>{unrelated} && enabled == created);
    }
    ResetTest(Failure::Apply);
    assert(!D3D9Reset_InstallNativeHooks() && removed == 12);
    assert(created == std::set<void*>{unrelated} && enabled == created);
    ResetTest(Failure::None);
    assert(D3D9Reset_InstallNativeHooks() && created.size() == 13);
    assert(enabled.count(reinterpret_cast<void*>(0x140537080)) == 1);

    void* vtable[17] = {};
    vtable[16] = reinterpret_cast<void*>(0x1234);
    void** object = vtable;
    auto* device = reinterpret_cast<IDirect3DDevice9*>(&object);
    ResetTest(Failure::Enable);
    assert(!InstallDeviceHook(device) && !g_resetTarget && removed == 1);
    assert(created == std::set<void*>{unrelated} && enabled == created);
    ResetTest(Failure::None);
    assert(InstallDeviceHook(device) && g_resetTarget == vtable[16]);
    assert(!g_deviceLock);
}
'''


RELEASE_MOCKS = r'''
using RenderMethod = void(*)(void*);
struct CRITICAL_SECTION {};
int criticalDepth = 0;
void EnterCriticalSection(CRITICAL_SECTION*) { ++criticalDepth; }
void LeaveCriticalSection(CRITICAL_SECTION*) { assert(criticalDepth > 0); --criticalDepth; }
[[noreturn]] void FailTransition(HRESULT, const char*) { throw std::runtime_error("stopped"); }
std::vector<int> sequence;
uint64_t textureType = 0x123456;
std::array<uint64_t, 5> textureDTI;
void Flush(void*) { sequence.push_back(1); }
void ReleaseWrapper(void*) { sequence.push_back(2); }
void ReleaseArray(void*) { sequence.push_back(3); }
void ReleaseCOM(void*) { sequence.push_back(4); }
void LoseTexture(void*) { assert(criticalDepth == 1); sequence.push_back(5); }
void* GetDTI(void*) { return textureDTI.data(); }
uintptr_t _addr(uintptr_t address) {
    if (address == 0x14053D230) return reinterpret_cast<uintptr_t>(&ReleaseArray);
    assert(address == 0x140E1C908);
    return reinterpret_cast<uintptr_t>(&textureType);
}
'''


RELEASE_CASES = r'''
int main() {
    std::vector<uint64_t> storage(0x889810 / 8 + 1);
    void* render = storage.data();
    void* rendererVtable[7] = {};
    rendererVtable[5] = reinterpret_cast<void*>(&Flush);
    Field<void**>(render, 0) = rendererVtable;
    void* resourceVtable[7] = {};
    resourceVtable[2] = reinterpret_cast<void*>(&ReleaseCOM);
    resourceVtable[4] = reinterpret_cast<void*>(&GetDTI);
    resourceVtable[5] = reinterpret_cast<void*>(&ReleaseWrapper);
    resourceVtable[6] = reinterpret_cast<void*>(&LoseTexture);
    std::array<uint64_t, 8> resource = {};
    Field<void**>(resource.data(), 0) = resourceVtable;
    for (size_t offset : {0x8676E8, 0x8676F8, 0x8676F0, 0x8897C8, 0x8897C0})
        Field<void*>(render, offset) = resource.data();
    Field<uint32_t>(render, 0x889808) = 1;
    Field<uint32_t>(render, 0x8897B0) = 1;
    Field<void*>(render, 0x8817B0) = resource.data();
    textureDTI[1] = textureType;
    Field<uint32_t>(render, 0x867718) = 42;
    ReleaseLostResources(render);
    assert((sequence == std::vector<int>{1, 2, 2, 3, 2, 4, 2, 1, 5, 1}));
    for (size_t offset : {0x8676E8, 0x8676F8, 0x8676F0, 0x8897C8, 0x8897C0})
        assert(!Field<void*>(render, offset));
    assert(Field<uint32_t>(render, 0x867718) == 42);
    assert(Field<uint8_t>(resource.data(), 0x29) == 1 && criticalDepth == 0);
    Field<uint8_t>(render, 0x38) = 1;
    bool stopped = false;
    try { ReleaseLostResources(render); }
    catch (const std::runtime_error&) { stopped = true; }
    assert(stopped && sequence.size() == 10);
}
'''


QUEUE_MOCKS = r'''
using SRWLOCK = bool;
#define SRWLOCK_INIT false
enum class Phase { Startup, Settings };
std::atomic<Phase> g_phase { Phase::Settings };
bool g_applyingSettings = false;
std::vector<int> applied;
void Resolution(void*, uint32_t index) { assert(g_applyingSettings && index == 0); applied.push_back(1); }
void Fullscreen(void*, bool) {
    assert(g_applyingSettings || g_phase == Phase::Startup); applied.push_back(2);
}
void Vsync(void*, bool enabled) { assert(g_applyingSettings && !enabled); applied.push_back(3); }
void Resize(void*, bool, const uint64_t* size, uint64_t) {
    assert(g_applyingSettings && *size == (uint64_t(600) << 32 | 800));
    applied.push_back(4);
}
auto g_originalResolution = Resolution;
auto g_originalFullscreen = Fullscreen;
auto g_originalVsync = Vsync;
auto g_originalResize = Resize;
bool Sync(void*);
auto g_originalSync = Sync;
'''


QUEUE_CASES = r'''
void* render;
unsigned syncStep = 0;
bool Sync(void*) {
    switch (syncStep++) {
    case 0: HookFullscreen(render, false); return true;
    case 1: HookResolution(render, 0); return true;
    case 2: HookFullscreen(render, true); return true;
    default: return false;
    }
}
int main() {
    std::vector<uint64_t> renderer(0x889810 / 8 + 1), mainState(0x40300 / 8);
    render = renderer.data();
    Field<uint32_t>(render, 0x8804E8) = 1;
    Field<uintptr_t>(render, 0x8804F0) = reinterpret_cast<uintptr_t>(render) + 0x87A8E8;
    Field<uint32_t>(render, 0x87A8E8 + 0x50) = 1280;
    Field<uint32_t>(render, 0x87A8E8 + 0x54) = 720;
    g_phase = Phase::Startup;
    HookFullscreen(render, true); // Constructor write must precede native config save.
    assert((applied == std::vector<int>{2}) && !g_pendingChanges);
    applied.clear();
    g_phase = Phase::Settings;
    HookVsync(render, true);
    HookVsync(render, false);
    uint64_t size = uint64_t(480) << 32 | 640;
    HookResize(render, false, &size, 0);
    HookFullscreen(render, true);
    size = uint64_t(600) << 32 | 800;
    HookResize(render, false, &size, 0);
    size = 0; // Queued request must own the dimensions, not this stack pointer.
    assert(applied.empty() && g_pendingChanges);
    ApplyQueuedSettings(render);
    assert((applied == std::vector<int>{3, 2, 4}));
    assert(!g_pendingChanges && !g_applyingSettings);
    applied.clear();
    HookResolution(render, 256); // Reject sentinel and unpopulated slots.
    ApplyQueuedSettings(render);
    assert(applied.empty());
    assert(!HookSyncSettings(mainState.data()));
    Field<uint32_t>(mainState.data(), 0x4028C) = 999; // Saved selection must survive.
    ApplyQueuedSettings(render);
    assert((applied == std::vector<int>{2, 1, 2}));
    assert(syncStep == 4 && Field<uint32_t>(mainState.data(), 0x4028C) == 0);
    assert(!g_pendingChanges && !g_applyingSettings);
}
'''


CONTROLLER_MOCKS = r'''
enum class Phase { Startup, Ready, Resetting, Rebuilding };
std::atomic<Phase> g_phase { Phase::Resetting };
bool g_inTransition = true, g_released = true;
bool g_applyingSettings = false;
bool stallUI = false;
void* g_transitionRender = nullptr;
bool ShutdownRequested() { return false; }
void* rendererForProbe = nullptr;
void* NativeRenderer() { return rendererForProbe; }
int offers = 0;
[[noreturn]] void FailTransition(HRESULT, const char*, bool = true) {
    throw std::runtime_error("stopped");
}
bool OfferRetry(HRESULT) { ++offers; return false; }
void Sleep(unsigned);
void D3D9ResetDiagnostics_BeginAppReset(bool) {}
void D3D9ResetDiagnostics_EndAppReset(uint64_t, uint64_t) {}
uint64_t NativeReset(void*, IDirect3DDevice9*, D3DPRESENT_PARAMETERS*) {
    assert(false); return 0;
}
auto g_originalAppReset = NativeReset;
unsigned legacyProbes = 0;
HRESULT NativeProbe(void*, IDirect3DDevice9* device) { ++legacyProbes; return device->TestCooperativeLevel(); }
auto g_originalCooperative = NativeProbe;
'''


CONTROLLER_CASES = r'''
HRESULT IDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS* p) {
    return HookResetDevice(this, p);
}
void Sleep(unsigned ms) {
    tick += stallUI ? 10001 : ms;
    if (!stallUI) {
        const DWORD saved = currentThread;
        currentThread = g_createThread;
        ServiceUIJob();
        currentThread = saved;
    }
}
int main() {
    IDirect3DDevice9 device;
    std::vector<uint64_t> render(0x889810 / 8 + 1);
    g_transitionRender = render.data();
    rendererForProbe = render.data();
    Field<IDirect3DDevice9*>(g_transitionRender, 0xB0) = &device;
    Field<uint32_t>(g_transitionRender, 0x8676E0) = 1280;
    Field<uint32_t>(g_transitionRender, 0x8676E4) = 720;
    g_device = &device;
    currentThread = 99; // Controller thread, different from creation/UI thread.
    device.cooperative = D3DERR_DEVICENOTRESET;
    D3DPRESENT_PARAMETERS p = {};
    p.BackBufferWidth = 1280; p.BackBufferHeight = 720;
    p.BackBufferCount = 1; p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    p.PresentationInterval = 1;
    resetResults = {D3DERR_DEVICELOST, D3D_OK};
    assert(HookAppReset(nullptr, &device, &p) == 1);
    assert(resetCalls == 2 && remembers == 1 && releasePause == 1);
    assert(p.BackBufferWidth == 1280 && p.BackBufferHeight == 720);
    assert(seen.BackBufferWidth == 1280 && seen.BackBufferHeight == 720);
    assert(g_phase == Phase::Rebuilding && g_job.state == JobState::Idle);
    assert(!g_deviceLock && !g_transitions && currentThread == 99);

    // The second native submission gets the original input, not API output zeros.
    assert(HookAppReset(nullptr, &device, &p) == 1);
    assert(g_job.skipped && resetCalls == 2);

    // Changed VSync must not be mistaken for a redundant reset.
    p.PresentationInterval = 0x80000000;
    assert(HookAppReset(nullptr, &device, &p) == 1);
    assert(resetCalls == 3 && remembered.PresentationInterval == 0x80000000);

    // Real INVALIDCALL never returns to native resource rebuilding.
    p.BackBufferWidth = 1920;
    resetResult = D3DERR_INVALIDCALL;
    bool stopped = false;
    try { HookAppReset(nullptr, &device, &p); }
    catch (const std::runtime_error&) { stopped = true; }
    assert(stopped && offers == 1 && resetCalls == 4 && remembers == 2);
    assert(p.BackBufferWidth == 1920 && g_phase == Phase::Resetting);
    assert(!g_deviceOperational && g_job.state == JobState::Idle);

    // Invalid request is rejected without entering the driver.
    p.BackBufferWidth = 999999;
    stopped = false;
    try { HookAppReset(nullptr, &device, &p); }
    catch (const std::runtime_error&) { stopped = true; }
    assert(stopped && resetCalls == 4);

    // A native property setter must not discard its queued change on loss.
    g_applyingSettings = true;
    cooperativeResults = {D3DERR_DEVICELOST, D3DERR_DEVICELOST, D3DERR_DEVICENOTRESET};
    const auto before = tick;
    assert(HookCooperative(nullptr, &device) == D3DERR_DEVICENOTRESET);
    assert(tick - before >= 200 && cooperativeResults.empty());
    g_applyingSettings = false;

    // Constructor writes use bounded UI probes too, without deferring config save.
    g_inTransition = false;
    g_phase = Phase::Startup;
    cooperativeResults = {D3DERR_DEVICELOST, D3DERR_DEVICENOTRESET};
    assert(HookCooperative(nullptr, &device) == D3DERR_DEVICENOTRESET);
    assert(legacyProbes == 0);
    IDirect3DDevice9 helper;
    helper.cooperative = D3D_OK;
    assert(HookCooperative(nullptr, &helper) == D3D_OK && legacyProbes == 1);

    // Even outside g_inTransition, a stalled startup probe cannot wait forever.
    // Timeout does not destroy or re-use an unconsumed request.
    stallUI = true;
    stopped = false;
    try { HookCooperative(nullptr, &device); }
    catch (const std::runtime_error&) { stopped = true; }
    assert(stopped && g_job.state == JobState::Queued && g_job.device == &device);
}
'''


CASES = r'''
int main() {
    D3DPRESENT_PARAMETERS a = {}, b = {};
    assert(SamePresentation(a, b));
    unsigned D3DPRESENT_PARAMETERS::* fields[] = {
        &D3DPRESENT_PARAMETERS::BackBufferWidth,
        &D3DPRESENT_PARAMETERS::BackBufferHeight,
        &D3DPRESENT_PARAMETERS::BackBufferFormat,
        &D3DPRESENT_PARAMETERS::BackBufferCount,
        &D3DPRESENT_PARAMETERS::MultiSampleType,
        &D3DPRESENT_PARAMETERS::MultiSampleQuality,
        &D3DPRESENT_PARAMETERS::SwapEffect,
        &D3DPRESENT_PARAMETERS::Windowed,
        &D3DPRESENT_PARAMETERS::EnableAutoDepthStencil,
        &D3DPRESENT_PARAMETERS::AutoDepthStencilFormat,
        &D3DPRESENT_PARAMETERS::Flags,
        &D3DPRESENT_PARAMETERS::FullScreen_RefreshRateInHz,
        &D3DPRESENT_PARAMETERS::PresentationInterval
    };
    for (auto field : fields) {
        b = a; b.*field = 1;
        assert(!SamePresentation(a, b));
    }
    b = a; b.hDeviceWindow = &a;
    assert(!SamePresentation(a, b));

    IDirect3DDevice9 device, other;
    g_device = &device;
    a.BackBufferWidth = 1280; a.BackBufferHeight = 720;
    a.BackBufferCount = 1; a.PresentationInterval = 1;

    assert(HookResetDevice(&device, nullptr) == D3DERR_INVALIDCALL);
    // An unmanaged Reset must not alter a ready renderer, even if the driver
    // would succeed. Do not hide this with a manual nativeReady=false in setup.
    g_nativeReady = true;
    b = a;
    assert(HookResetDevice(&device, &b) == D3DERR_INVALIDCALL);
    assert(resetCalls == 0 && releasePause == 0 && releaseImGui == 0);
    assert(g_nativeReady && g_deviceOperational && SamePresentation(a, b));
    assert(D3D9Reset_BeginOverlay(&device));
    D3D9Reset_EndOverlay();

    g_managedReset = true;
    a.Windowed = 1; // Managed submission already normalized its request.
    currentThread = 8;
    b = a;
    assert(HookResetDevice(&device, &b) == D3DERR_INVALIDCALL);
    assert(resetCalls == 0 && !g_transitions && !g_deviceLock);
    currentThread = 7;

    resetResult = D3DERR_INVALIDCALL;
    assert(HookResetDevice(&device, &b) == D3DERR_INVALIDCALL);
    assert(resetCalls == 1 && remembers == 0 && recreateImGui == 0);
    assert(!g_deviceOperational && releasePause == 1 && releaseImGui == 1);
    assert(b.BackBufferWidth == 0); // Actual API output, not fake old state.
    g_nativeReady = true;
    assert(!D3D9Reset_BeginOverlay(&device));
    assert(!g_deviceLock && !g_transitions);

    resetResult = D3DERR_DEVICELOST;
    b = a;
    assert(HookResetDevice(&device, &b) == D3DERR_DEVICELOST);
    assert(releasePause == 1 && releaseImGui == 1 && remembers == 0);

    resetResult = D3D_OK;
    g_nativeReady = true; // The production device hook must close admission.
    b = a;
    assert(HookResetDevice(&device, &b) == D3D_OK);
    assert(!g_nativeReady && remembers == 0 && !g_deviceOperational);
    // The outer UI job publishes success after the entire overlay chain returns.
    AcquireSRWLockExclusive(&g_deviceLock);
    RememberActive(&device, a);
    ReleaseSRWLockExclusive(&g_deviceLock);
    assert(remembers == 1 && remembered.BackBufferWidth == 1280);
    assert(remembered.Windowed == 1 && remembered.PresentationInterval == 1);
    assert(recreateImGui == 0); // Wait for native rebuild, not just Reset.
    assert(!D3D9Reset_BeginOverlay(&device));
    g_nativeReady = true;
    g_transitions = 1;
    assert(!D3D9Reset_BeginOverlay(&device));
    g_transitions = 0;

    recreateSucceeds = false;
    assert(!D3D9Reset_BeginOverlay(&device));
    assert(recreateImGui == 1 && !g_deviceLock && g_deviceOperational);
    assert(!D3D9Reset_BeginOverlay(&device));
    assert(recreateImGui == 1); // Throttled retry, not every EndScene.
    tick += 1000;
    recreateSucceeds = true;
    assert(D3D9Reset_BeginOverlay(&device));
    assert(g_deviceLock && !g_pluginInvalidated && recreateImGui == 2);
    D3D9Reset_EndOverlay();

    const int oldReleases = releasePause;
    g_managedReset = false;
    currentThread = 88; // The game's creation-thread restriction is not the helper's.
    b = a;
    b.Windowed = 0;
    assert(HookResetDevice(&other, &b) == D3D_OK);
    assert(remembers == 1 && releasePause == oldReleases && g_deviceOperational);
    assert(seen.Windowed == 0); // Do not impose our policy on helper devices.
    assert(transitionsInDriver == 0 && g_device == &device && g_nativeReady);
    assert(g_createThread == 7);
    assert(!g_deviceLock && !g_transitions);
}
'''


class D3D9ResetTests(unittest.TestCase):
    def test_rendering_thread_changes_only_between_frames(self):
        source = (ROOT / "UMVC3Hook/code/d3d9reset.cpp").read_text()
        unit = MOCKS + r'''
std::atomic<int> g_renderingThreadOverride {-1};
std::atomic<bool> g_renderingThreadEnabled {false};
int beginCalls = 0;
void Begin(void*) { ++beginCalls; }
auto g_originalBeginFrame = Begin;
'''
        unit += function(source, "Field") + function(source, "HookBeginFrame")
        unit += r'''
int main() {
    uint8_t render[0x40] = {};
    render[0x3D] = 1;
    render[0x3E] = 7; // Only native beginFrame may latch this byte.
    HookBeginFrame(render);
    assert(render[0x3D] == 1);
    render[0x38] = 1;
    g_renderingThreadOverride = 0;
    HookBeginFrame(render);
    assert(render[0x3D] == 1); // Defer while a frame is in progress.
    render[0x38] = 0;
    HookBeginFrame(render);
    assert(render[0x3D] == 0 && !g_renderingThreadEnabled.load());
    g_renderingThreadOverride = 1;
    HookBeginFrame(render);
    assert(render[0x3D] == 1 && g_renderingThreadEnabled.load());
    g_renderingThreadOverride = 0;
    HookBeginFrame(render);
    g_renderingThreadOverride = -1;
    HookBeginFrame(render);
    assert(render[0x3D] == 1 && render[0x3E] == 7 && beginCalls == 6);
    uint8_t other[0x40] = {};
    g_renderingThreadOverride = 1;
    HookBeginFrame(other);
    assert(other[0x3D] == 1);
    g_renderingThreadOverride = -1;
    HookBeginFrame(other);
    assert(other[0x3D] == 0); // Restore this renderer's own native setting.
}
'''
        self.run_cpp(unit)

    def run_cpp(self, unit):
        with tempfile.TemporaryDirectory(prefix="d3d9reset-", dir="/tmp/opencode") as tmp:
            cpp = Path(tmp) / "test.cpp"
            exe = Path(tmp) / "test"
            cpp.write_text(unit)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_production_reset_and_overlay_contract(self):
        source = (ROOT / "UMVC3Hook/code/d3d9reset.cpp").read_text()
        names = ["SamePresentation", "InvalidatePlugin", "HookResetDevice",
                 "D3D9Reset_BeginOverlay", "D3D9Reset_EndOverlay"]
        unit = MOCKS + "\n".join(function(source, name) for name in names) + CASES
        self.run_cpp(unit)

    def test_controller_retry_and_native_continuation(self):
        source = (ROOT / "UMVC3Hook/code/d3d9reset.cpp").read_text()
        job = source[source.index("enum class JobState"):source.index("enum class ChangeKind")]
        names = ["Field", "IsRendererDevice", "SamePresentation", "MatchesEffective", "ValidRequest", "InvalidatePlugin",
                 "HookResetDevice", "ServiceUIJob", "RunUIJob", "HookAppReset", "HookCooperative"]
        unit = MOCKS + CONTROLLER_MOCKS + job
        unit += "\n".join(function(source, name) for name in names) + CONTROLLER_CASES
        self.run_cpp(unit)

    def test_lost_device_release_order(self):
        source = (ROOT / "UMVC3Hook/code/d3d9reset.cpp").read_text()
        names = ["Field", "Virtual", "ReleaseField", "ReleaseLostResources"]
        unit = MOCKS + RELEASE_MOCKS
        unit += "\n".join(function(source, name) for name in names) + RELEASE_CASES
        self.run_cpp(unit)

    def test_settings_queue_ownership_and_coalescing(self):
        source = (ROOT / "UMVC3Hook/code/d3d9reset.cpp").read_text()
        changes = source[source.index("enum class ChangeKind"):source.index("[[noreturn]]")]
        names = ["Field", "QueueChange", "ValidResolutionChoice", "ApplyPropertyNow", "HookResolution",
                 "HookFullscreen", "HookVsync", "HookResize", "HookSyncSettings", "ApplyQueuedSettings"]
        unit = MOCKS + QUEUE_MOCKS + changes
        unit += "\n".join(function(source, name) for name in names) + QUEUE_CASES
        self.run_cpp(unit)

    def test_native_creation_ownership_and_startup_failure(self):
        source = (ROOT / "UMVC3Hook/code/d3d9reset.cpp").read_text()
        layout = source[source.index("struct NativeCreateParameters"):source.index("bool CreateRendererDevice")]
        names = ["Field", "NativeRenderer", "D3D9Reset_AbortStartup", "RecordNativeDevice",
                 "CreateRendererDevice"]
        unit = MOCKS + OWNERSHIP_MOCKS + layout
        unit += "\n".join(function(source, name) for name in names) + OWNERSHIP_CASES
        self.run_cpp(unit)

    def test_hook_installation_failures(self):
        source = (ROOT / "UMVC3Hook/code/d3d9reset.cpp").read_text()
        unit = MOCKS + INSTALL_MOCKS
        unit += function(source, "D3D9Reset_InstallNativeHooks")
        unit += function(source, "InstallDeviceHook") + INSTALL_CASES
        self.run_cpp(unit)
        dll = (ROOT / "UMVC3Hook/dllmain.cpp").read_text()
        self.assertRegex(dll, r"if \(!D3D9Reset_InstallNativeHooks\(\)\)\s+D3D9Reset_AbortStartup\(")
        self.assertNotIn("D3D9Reset_RecordCreate", dll)
        self.assertNotIn("D3D9Reset_InstallDeviceHook", dll)


if __name__ == "__main__":
    unittest.main()
