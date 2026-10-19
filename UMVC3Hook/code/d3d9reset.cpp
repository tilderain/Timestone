#include "netplay.h" // ENet includes winsock2 before the Windows/D3D headers.
#include "d3d9reset.h"

#include "mk10utils.h"
#include "framestepper.h"
#include "LaunchMenu.h"
#include "launchmenu_utils.h"
#include "../IniSafe.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_impl_dx9.h"
#include "../kiero/minhook/include/MinHook.h"

#include <Windows.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstddef>

extern void HookLog(const char* fmt, ...);
extern void HookLogFlush();
// Window policy remains with the window-management code in dllmain.cpp.
extern void PrepareD3D9PresentationParameters(D3DPRESENT_PARAMETERS*, const char*);
extern void PrepareD3D9NativeCreate(HWND, D3DPRESENT_PARAMETERS*);
extern void ApplyD3D9BorderlessWindow();

namespace {

using ResetDevice = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using AppReset = uint64_t(__fastcall*)(void*, IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using ApplyGraphics = void(__fastcall*)(void*, uintptr_t, uintptr_t, uintptr_t);
using ResultHandler = void(__fastcall*)(void*, int32_t, const char*, uintptr_t);
using RenderMethod = void(__fastcall*)(void*);
using TestCooperative = HRESULT(__fastcall*)(void*, IDirect3DDevice9*);
using SetChoice = void(__fastcall*)(void*, uint32_t);
using SetBool = void(__fastcall*)(void*, bool);
using Resize = void(__fastcall*)(void*, bool, const uint64_t*, uint64_t);
using SyncSettings = bool(__fastcall*)(void*);

ResetDevice g_originalReset = nullptr;
void* g_resetTarget = nullptr;
AppReset g_originalAppReset = nullptr;
ApplyGraphics g_originalApplyGraphics = nullptr;
ResultHandler g_originalResultHandler = nullptr;
RenderMethod g_originalUIUpdate = nullptr;
RenderMethod g_originalRelease = nullptr;
RenderMethod g_originalBeginFrame = nullptr;
std::atomic<int> g_renderingThreadOverride { 0 };
std::atomic<bool> g_renderingThreadEnabled { false };
TestCooperative g_originalCooperative = nullptr;
SetChoice g_originalResolution = nullptr;
SetBool g_originalFullscreen = nullptr;
SetBool g_originalVsync = nullptr;
Resize g_originalResize = nullptr;
SyncSettings g_originalSync = nullptr;

SRWLOCK g_deviceLock = SRWLOCK_INIT;
std::atomic<unsigned> g_transitions { 0 };
std::atomic<bool> g_nativeReady { false };
IDirect3DDevice9* g_device = nullptr; // Borrowed; native renderer owns lifetime.
D3DPRESENT_PARAMETERS g_active = {};
D3DPRESENT_PARAMETERS g_effective = {};
D3DCAPS9 g_caps = {};
DWORD g_createThread = 0;
HWND g_creationFocusWindow = nullptr;
bool g_haveActive = false;
bool g_deviceOperational = false;
bool g_pluginInvalidated = false;
uint64_t g_generation = 0;
ULONGLONG g_nextPluginRetry = 0;

enum class Phase { Startup, Ready, Settings, Releasing, Resetting, Rebuilding, Failed };
std::atomic<Phase> g_phase { Phase::Startup };
thread_local bool g_inTransition = false;
thread_local bool g_applyingSettings = false;
thread_local bool g_managedReset = false;
thread_local bool g_creatingNativeDevice = false;
thread_local bool g_released = false;
thread_local void* g_transitionRender = nullptr;

void RecordNativeDevice(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& requested);
bool InstallDeviceHook(IDirect3DDevice9* device);

// One immutable request, owned until the UI thread publishes Done. Native
// gameplay remains suspended on the submitting stack, with no device lock held.
enum class JobState { Idle, Queued, Running, Done };
enum class JobKind { Probe, Reset };
struct UIJob {
    std::atomic<JobState> state { JobState::Idle };
    JobKind kind = JobKind::Probe;
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS request = {};
    HRESULT result = E_FAIL;
    bool skipped = false;
} g_job;

enum class ChangeKind { Resolution, Fullscreen, Vsync, Resize, Sync, Count };
struct Change {
    ChangeKind kind = ChangeKind::Resolution;
    void* object = nullptr;
    uint64_t order = 0;
    uint64_t dimensions = 0;
    uint32_t value = 0;
    bool fullscreen = false;
    bool force = false;
    uint32_t options[5] = {};
};
SRWLOCK g_settingsLock = SRWLOCK_INIT;
std::array<Change, static_cast<size_t>(ChangeKind::Count)> g_changes = {};
uint64_t g_changeOrder = 0;
std::atomic<bool> g_pendingChanges { false };

[[noreturn]] void FailTransition(HRESULT result, const char* operation, bool showMessage = true)
{
    g_nativeReady.store(false);
    const Phase failedPhase = g_phase.exchange(Phase::Failed);
    HookLog("D3D9: recovery stopped phase=%u HRESULT=0x%08X operation=%s\n",
        static_cast<unsigned>(failedPhase), (uint32_t)result, operation);
    Netplay_SendPlayerDisconnect(100);
    HookLogFlush();
    char text[512];
    snprintf(text, sizeof(text),
        "Graphics recovery could not complete (0x%08X).\n%s\n\n"
        "The game must close: rendering will not resume on a failed device.\n"
        "Please keep the Timestone log. If an overlay is enabled, try disabling it.",
        (uint32_t)result, operation);
    // No owner HWND: the device/UI thread may itself be stuck in a driver.
    if (showMessage)
        MessageBoxA(nullptr, text, "Timestone graphics recovery", MB_OK | MB_ICONERROR);
    const JobState state = g_job.state.load();
    if (state == JobState::Running || state == JobState::Queued) {
        // Never unwind/free a live request or run DLL detach while a driver is
        // stuck. This is process termination after explicit acknowledgement.
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
    }
    ExitProcess(static_cast<UINT>(result));
}

template<typename T>
T& Field(void* object, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<unsigned char*>(object) + offset);
}

void* NativeRenderer()
{
    return *reinterpret_cast<void**>(_addr(0x140E179A8));
}

void __fastcall HookBeginFrame(void* render)
{
    // EndFrame uses +0x3D as well as the latched +0x3E. Never change either
    // during a live frame (including a frame retained by native frame skipping).
    static void* nativeRenderer = nullptr;
    static uint8_t nativeMode = 0;
    if (D3D9Reset_RenderingThreadOverrideAvailable && !Field<uint8_t>(render, 0x38)) {
        if (nativeRenderer != render) {
            nativeRenderer = render;
            nativeMode = Field<uint8_t>(render, 0x3D);
        }
        const int mode = g_renderingThreadOverride.load();
        const uint8_t enabled = mode < 0 ? nativeMode : static_cast<uint8_t>(mode);
        if (Field<uint8_t>(render, 0x3D) != enabled) {
            Field<uint8_t>(render, 0x3D) = enabled;
            HookLog("D3D9: RenderingThread %s (override=%d)\n", enabled ? "ON" : "OFF", mode);
        }
    }
    g_renderingThreadEnabled.store(Field<uint8_t>(render, 0x3D) != 0, std::memory_order_relaxed);
    g_originalBeginFrame(render);
}

bool IsRendererDevice(IDirect3DDevice9* device)
{
    void* render = NativeRenderer();
    return device && render && Field<IDirect3DDevice9* volatile>(render, 0xB0) == device;
}

// sApp UI job 3, verified at 0x14051F930. Capturing at this outer boundary
// also handles overlays that wrap CreateDevice using a temporary output slot.
struct NativeCreateParameters {
    uint32_t adapter;
    D3DDEVTYPE type;
    HWND focusWindow;
    uint32_t behavior;
    D3DPRESENT_PARAMETERS* presentation;
    IDirect3DDevice9** output;
};
static_assert(offsetof(NativeCreateParameters, presentation) == 0x18);
static_assert(offsetof(NativeCreateParameters, output) == 0x20);

bool CreateRendererDevice(void* app)
{
    if (Field<uint32_t>(app, 0x2AC4) != 3)
        return false;
    void* render = NativeRenderer();
    auto* parameters = Field<NativeCreateParameters*>(app, 0x2AE0);
    IDirect3D9* core = Field<IDirect3D9*>(app, 0x2AD8);
    if (!render || !parameters || !core || core != Field<IDirect3D9*>(render, 0xC0) ||
        parameters->output != &Field<IDirect3DDevice9*>(render, 0xB0))
        return false;
    if (!parameters->presentation)
        D3D9Reset_AbortStartup("Native CreateDevice request has no presentation parameters");

    g_creatingNativeDevice = true;
    PrepareD3D9NativeCreate(parameters->focusWindow, parameters->presentation);
    const D3DPRESENT_PARAMETERS requested = *parameters->presentation;
    const HRESULT result = core->CreateDevice(parameters->adapter, parameters->type,
        parameters->focusWindow, parameters->behavior, parameters->presentation, parameters->output);
    if (result != D3D_OK || !*parameters->output)
        FailTransition(result != D3D_OK ? result : E_UNEXPECTED, "native renderer CreateDevice failed");
    RecordNativeDevice(*parameters->output, requested);
    ApplyD3D9BorderlessWindow();
    // Publish completion ONLY after ownership and the Reset hook are ready.
    // The original dispatcher still handles its remaining window/cursor work.
    InterlockedExchange(&Field<LONG>(app, 0x2AC4), 0);
    g_creatingNativeDevice = false;
    return true;
}

bool ShutdownRequested()
{
    void* app = *reinterpret_cast<void**>(_addr(0x140E17758));
    return app && Field<volatile uint8_t>(app, 0x2AA0) != 0;
}

bool SamePresentation(const D3DPRESENT_PARAMETERS& a,
                      const D3DPRESENT_PARAMETERS& b)
{
    return a.BackBufferWidth == b.BackBufferWidth &&
        a.BackBufferHeight == b.BackBufferHeight &&
        a.BackBufferFormat == b.BackBufferFormat &&
        a.BackBufferCount == b.BackBufferCount &&
        a.MultiSampleType == b.MultiSampleType &&
        a.MultiSampleQuality == b.MultiSampleQuality &&
        a.SwapEffect == b.SwapEffect && a.hDeviceWindow == b.hDeviceWindow &&
        a.Windowed == b.Windowed &&
        a.EnableAutoDepthStencil == b.EnableAutoDepthStencil &&
        a.AutoDepthStencilFormat == b.AutoDepthStencilFormat &&
        a.Flags == b.Flags &&
        a.FullScreen_RefreshRateInHz == b.FullScreen_RefreshRateInHz &&
        a.PresentationInterval == b.PresentationInterval;
}

// Called only on the device thread, after a successful creation/reset. Keep
// immutable input for fields Reset mutates; resolve automatic dimensions and
// format using the actual swapchain rather than caching Reset's output zeros.
void RememberActive(IDirect3DDevice9* device,
                    const D3DPRESENT_PARAMETERS& requested)
{
    g_device = device;
    g_effective = requested;
    g_haveActive = false;
    IDirect3DSwapChain9* chain = nullptr;
    HRESULT result = device->GetSwapChain(0, &chain);
    if (SUCCEEDED(result) && chain) {
        D3DPRESENT_PARAMETERS effective = {};
        result = chain->GetPresentParameters(&effective);
        chain->Release();
        if (SUCCEEDED(result)) {
            g_effective = effective;
            g_haveActive = true;
        }
    }
    g_deviceOperational = true;
    HookLog("D3D9: effective device=%p bb=%ux%u interval=0x%08X "
        "queried=%d result=0x%08X\n", device,
        g_effective.BackBufferWidth, g_effective.BackBufferHeight,
        g_effective.PresentationInterval, g_haveActive, (uint32_t)result);
}

bool MatchesEffective(D3DPRESENT_PARAMETERS request)
{
    if (!request.hDeviceWindow)
        request.hDeviceWindow = g_creationFocusWindow;
    if (!request.BackBufferCount)
        request.BackBufferCount = 1;
    if (request.Windowed && request.BackBufferFormat == D3DFMT_UNKNOWN)
        request.BackBufferFormat = g_effective.BackBufferFormat;
    return g_haveActive && SamePresentation(request, g_effective);
}

bool ValidRequest(const D3DPRESENT_PARAMETERS& p)
{
    return (!g_caps.MaxTextureWidth || p.BackBufferWidth <= g_caps.MaxTextureWidth) &&
        (!g_caps.MaxTextureHeight || p.BackBufferHeight <= g_caps.MaxTextureHeight) &&
        (!p.Windowed || !p.FullScreen_RefreshRateInHz) &&
        (p.Windowed || (p.BackBufferWidth && p.BackBufferHeight)) &&
        (p.SwapEffect == D3DSWAPEFFECT_DISCARD || p.SwapEffect == D3DSWAPEFFECT_FLIP ||
         p.SwapEffect == D3DSWAPEFFECT_COPY) &&
        p.BackBufferCount <= 3 &&
        (p.SwapEffect != D3DSWAPEFFECT_COPY || p.BackBufferCount <= 1) &&
        (p.MultiSampleType == D3DMULTISAMPLE_NONE || p.SwapEffect == D3DSWAPEFFECT_DISCARD) &&
        (!p.PresentationInterval ||
            (!(p.PresentationInterval & (p.PresentationInterval - 1)) &&
             (!g_caps.PresentationIntervals ||
              (p.PresentationInterval & g_caps.PresentationIntervals))));
}

// Called only by the native UI dispatcher, after its current native job has
// completed. Use the device's current vtable so other overlays retain their
// Reset cleanup. Bypassing their hook by calling our trampoline here is unsafe.
void ServiceUIJob()
{
    JobState expected = JobState::Queued;
    if (!g_job.state.compare_exchange_strong(expected, JobState::Running))
        return;
    HRESULT result = D3DERR_INVALIDCALL;
    g_job.skipped = false;
    if (GetCurrentThreadId() == g_createThread && g_job.device == g_device) {
        result = g_job.device->TestCooperativeLevel();
        if (g_job.kind == JobKind::Reset &&
            (result == D3D_OK || result == D3DERR_DEVICENOTRESET)) {
            AcquireSRWLockExclusive(&g_deviceLock);
            const bool redundant = result == D3D_OK && g_deviceOperational &&
                MatchesEffective(g_job.request);
            const bool valid = ValidRequest(g_job.request);
            if (!redundant)
                g_deviceOperational = false;
            ReleaseSRWLockExclusive(&g_deviceLock);
            if (redundant) {
                g_job.skipped = true;
                result = D3D_OK;
            } else if (!valid) {
                result = D3DERR_INVALIDCALL;
            } else {
                D3DPRESENT_PARAMETERS working = g_job.request;
                g_managedReset = true;
                result = g_job.device->Reset(&working);
                g_managedReset = false;
                // The device detour may be below an overlay which changes the
                // final result. Publish success only after the entire chain.
                AcquireSRWLockExclusive(&g_deviceLock);
                if (result == D3D_OK)
                    RememberActive(g_job.device, g_job.request);
                else
                    g_deviceOperational = false;
                ReleaseSRWLockExclusive(&g_deviceLock);
            }
        }
    }
    g_job.result = result;
    g_job.state.store(JobState::Done);
}

void __fastcall HookUIUpdate(void* app)
{
    // Reset may run a nested window-message loop. Leave native jobs pending
    // rather than dispatching another D3D call from inside that Reset.
    if (g_managedReset || g_creatingNativeDevice)
        return;
    CreateRendererDevice(app);
    g_originalUIUpdate(app);
    if (Field<uint32_t>(app, 0x2AC4) == 0)
        ServiceUIJob();
}

HRESULT RunUIJob(JobKind kind, IDirect3DDevice9* device,
                 const D3DPRESENT_PARAMETERS* request = nullptr)
{
    if (g_job.state.load() != JobState::Idle)
        FailTransition(E_UNEXPECTED, "overlapping device-thread requests");
    g_job.kind = kind;
    g_job.device = device;
    g_job.request = request ? *request : D3DPRESENT_PARAMETERS{};
    g_job.state.store(JobState::Queued);
    if (GetCurrentThreadId() == g_createThread)
        ServiceUIJob();
    const ULONGLONG start = GetTickCount64();
    while (g_job.state.load() != JobState::Done) {
        if (ShutdownRequested())
            FailTransition(HRESULT_FROM_WIN32(ERROR_CANCELLED), "game window closed during recovery", false);
        if (GetTickCount64() - start >= 30000)
            FailTransition(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "device/UI thread stopped responding");
        Sleep(1);
    }
    const HRESULT result = g_job.result;
    g_job.state.store(JobState::Idle);
    return result;
}

bool OfferRetry(HRESULT result)
{
    char text[384];
    snprintf(text, sizeof(text),
        "Graphics recovery is paused (0x%08X).\n\n"
        "Restore the game window or disable conflicting overlays, then Retry.\n"
        "Cancel closes the game safely. No rendering will resume until recovery succeeds.",
        (uint32_t)result);
    HookLog("D3D9: recovery awaiting user decision HRESULT=0x%08X\n", (uint32_t)result);
    HookLogFlush();
    return MessageBoxA(nullptr, text, "Timestone graphics recovery",
        MB_RETRYCANCEL | MB_ICONWARNING) == IDRETRY;
}

HRESULT __fastcall HookCooperative(void* app, IDirect3DDevice9* device)
{
    // Constructor properties must still precede the native config save, but
    // that does not require using sApp's unbounded legacy job wait.
    const bool startup = g_phase.load() == Phase::Startup && IsRendererDevice(device);
    if (!g_inTransition && !startup)
        return g_originalCooperative(app, device);
    ULONGLONG start = GetTickCount64();
    for (;;) {
        const HRESULT result = RunUIJob(JobKind::Probe, device);
        if ((!g_applyingSettings && !startup) || result != D3DERR_DEVICELOST)
            return result;
        // Native property setters silently discard changes on DEVICELOST.
        // Keep their queued request alive until they can actually apply it.
        if (GetTickCount64() - start >= 30000) {
            if (!OfferRetry(result))
                FailTransition(result, "graphics settings application cancelled", false);
            start = GetTickCount64();
        }
        Sleep(100);
    }
}

void InvalidatePlugin()
{
    if (g_pluginInvalidated)
        return;
    FrameStepper_InvalidateDeviceObjects();
    if (ImGui::GetCurrentContext())
        ImGui_ImplDX9_InvalidateDeviceObjects();
    g_pluginInvalidated = true;
}

HRESULT WINAPI HookResetDevice(IDirect3DDevice9* device,
                               D3DPRESENT_PARAMETERS* parameters)
{
    D3D9ResetDiagnostics_RecordDeviceReset();
    if (!device || !parameters)
        return D3DERR_INVALIDCALL;

    AcquireSRWLockShared(&g_deviceLock);
    const bool tracked = device == g_device;
    ReleaseSRWLockShared(&g_deviceLock);
    // Shared implementation, unrelated device: no admission gate, invalidation,
    // thread restriction, or presentation policy belongs on this call.
    if (!tracked)
        return g_originalReset(device, parameters);
    if (!g_managedReset) {
        // Reject BEFORE changing device or renderer state. A successful raw
        // Reset cannot prove native resources were rebuilt; only our managed
        // submission may reset the tracked device and later reopen admission.
        HookLog("D3D9: rejected unmanaged Reset of renderer device; request a native graphics transition\n");
        return D3DERR_INVALIDCALL;
    }

    g_transitions.fetch_add(1);
    AcquireSRWLockExclusive(&g_deviceLock);
    if (GetCurrentThreadId() != g_createThread) {
        HookLog("D3D9: rejected Reset on thread=%lu createThread=%lu\n",
            GetCurrentThreadId(), g_createThread);
        ReleaseSRWLockExclusive(&g_deviceLock);
        g_transitions.fetch_sub(1);
        return D3DERR_INVALIDCALL;
    }
    g_nativeReady.store(false);
    g_deviceOperational = false;
    InvalidatePlugin();
    ReleaseSRWLockExclusive(&g_deviceLock);

    D3DPRESENT_PARAMETERS requested = *parameters;
    D3DPRESENT_PARAMETERS working = requested;
    const ULONGLONG start = GetTickCount64();
    const HRESULT result = g_originalReset(device, &working);
    *parameters = working; // Preserve the D3D9 API's actual in/out contract.

    HookLog("D3D9: Reset result=0x%08X elapsed=%llums thread=%lu "
        "requested=%ux%u windowed=%d interval=0x%08X\n", (uint32_t)result,
        (unsigned long long)(GetTickCount64() - start), GetCurrentThreadId(),
        requested.BackBufferWidth, requested.BackBufferHeight,
        requested.Windowed, requested.PresentationInterval);

    AcquireSRWLockExclusive(&g_deviceLock);
    if (FAILED(result)) {
        // TestCooperativeLevel is legal after failed Reset; resource/display
        // probes are not. Never claim success or rebuild plugin objects here.
        HookLog("D3D9: Reset failed cooperative=0x%08X; retaining failure\n",
            (uint32_t)device->TestCooperativeLevel());
    }
    ReleaseSRWLockExclusive(&g_deviceLock);
    g_transitions.fetch_sub(1);
    return result;
}

uint64_t __fastcall HookAppReset(void* app, IDirect3DDevice9* device,
                                D3DPRESENT_PARAMETERS* parameters)
{
    if (!g_inTransition)
        return g_originalAppReset(app, device, parameters);
    if (!device || !parameters || !g_released)
        FailTransition(E_UNEXPECTED, "Reset outside the resource-release boundary");
    D3DPRESENT_PARAMETERS requested = *parameters;
    PrepareD3D9PresentationParameters(&requested, "native Reset request");
    g_phase.store(Phase::Resetting);
    const ULONGLONG start = GetTickCount64();
    ULONGLONG retryStart = start;
    for (;;) {
        const HRESULT result = RunUIJob(JobKind::Reset, device, &requested);
        D3D9ResetDiagnostics_BeginAppReset(!g_job.skipped);
        if (result == D3D_OK) {
            AcquireSRWLockExclusive(&g_deviceLock);
            const bool dimensionsMatch = g_haveActive &&
                g_effective.BackBufferWidth == Field<uint32_t>(g_transitionRender, 0x8676E0) &&
                g_effective.BackBufferHeight == Field<uint32_t>(g_transitionRender, 0x8676E4);
            ReleaseSRWLockExclusive(&g_deviceLock);
            if (!dimensionsMatch)
                FailTransition(E_UNEXPECTED, "effective backbuffer does not match native render dimensions");
            HookLog("D3D9: native Reset completed skipped=%d requested=%ux%u\n",
                g_job.skipped, requested.BackBufferWidth, requested.BackBufferHeight);
            // This is NOT the COM interface: leave the native input intact.
            // Its optional second submission reuses this same stack object.
            // Passing back Reset's output zeros would corrupt the next request.
            g_phase.store(Phase::Rebuilding);
            D3D9ResetDiagnostics_EndAppReset(1, GetTickCount64() - start);
            return 1;
        }
        if ((result == D3DERR_DEVICELOST || result == D3DERR_DEVICENOTRESET) &&
            GetTickCount64() - retryStart < 30000) {
            Sleep(100);
            continue;
        }
        if (result == D3DERR_INVALIDCALL || result == D3DERR_DEVICELOST ||
            result == D3DERR_DEVICENOTRESET) {
            if (OfferRetry(result)) {
                retryStart = GetTickCount64();
                continue;
            }
            FailTransition(result, "graphics recovery cancelled", false);
        }
        FailTransition(result, "Reset did not succeed; native rebuild was not resumed");
    }
}

template<typename T = void>
T Virtual(void* object, size_t slot)
{
    return reinterpret_cast<T(__fastcall*)(void*)>(Field<void**>(object, 0)[slot])(object);
}

void ReleaseField(void* render, size_t offset, size_t slot)
{
    void*& object = Field<void*>(render, offset);
    if (object) {
        Virtual(object, slot);
        object = nullptr;
    }
}

// Release-only transcription of 0x140540DB0. The native apply routine has
// already drained playback and cleared +0x38. Retain native lifetime callbacks
// and their ordering, but omit gamma, SetRenderTarget, Clear, Present and fences.
void ReleaseLostResources(void* render)
{
    if (Field<uint8_t>(render, 0x38))
        FailTransition(E_UNEXPECTED, "render worker not drained before lost-device cleanup");
    const uint32_t displays = Field<uint32_t>(render, 0x889808);
    if (displays > 2)
        FailTransition(E_UNEXPECTED, "invalid native display count");
    Field<uint8_t>(render, 0x10C) = 1;
    Virtual(render, 5); // sRender__tickObjectArray_helper; no Present with +0x38=0.
    ReleaseField(render, 0x8676E8, 5);
    ReleaseField(render, 0x8676F8, 5);
    const uint32_t saved = Field<uint32_t>(render, 0x867718);
    reinterpret_cast<RenderMethod>(_addr(0x14053D230))(render);
    Field<uint32_t>(render, 0x867718) = saved;
    for (uint32_t i = 0; i < displays; ++i) {
        ReleaseField(render, 0x8897C8 + i * 0x28, 5);
        ReleaseField(render, 0x8897C0 + i * 0x28, 2);
    }
    ReleaseField(render, 0x8676F0, 5);
    Virtual(render, 5);

    auto* critical = &Field<CRITICAL_SECTION>(render, 8);
    EnterCriticalSection(critical);
    const uint32_t count = Field<uint32_t>(render, 0x8897B0);
    if (count > 4096) {
        LeaveCriticalSection(critical);
        FailTransition(E_UNEXPECTED, "invalid native graphics-resource count");
    }
    const uintptr_t textureType = *reinterpret_cast<uintptr_t*>(_addr(0x140E1C908));
    for (uint32_t i = 0; i < count; ++i) {
        void* object = Field<void*>(render, 0x8817B0 + i * 8);
        for (void* dti = Virtual<void*>(object, 4); dti; dti = Field<void*>(dti, 0x20)) {
            if (Field<uintptr_t>(dti, 8) == textureType) {
                Virtual(object, 6);
                Field<uint8_t>(object, 0x29) = 1;
                break;
            }
        }
    }
    LeaveCriticalSection(critical);
    Virtual(render, 5);
}

void __fastcall HookRelease(void* render)
{
    if (!g_inTransition) {
        g_originalRelease(render);
        return;
    }
    if (g_released)
        FailTransition(E_UNEXPECTED, "duplicate native resource teardown");
    g_phase.store(Phase::Releasing);
    const HRESULT cooperative = RunUIJob(JobKind::Probe, Field<IDirect3DDevice9*>(render, 0xB0));
    if (cooperative != D3D_OK && cooperative != D3DERR_DEVICELOST &&
        cooperative != D3DERR_DEVICENOTRESET)
        FailTransition(cooperative, "device unavailable before resource release");
    AcquireSRWLockExclusive(&g_deviceLock);
    InvalidatePlugin();
    ReleaseSRWLockExclusive(&g_deviceLock);
    g_released = true;
    if (cooperative == D3D_OK)
        g_originalRelease(render);
    else
        ReleaseLostResources(render);
}

void QueueChange(Change change)
{
    AcquireSRWLockExclusive(&g_settingsLock);
    change.order = ++g_changeOrder;
    g_changes[static_cast<size_t>(change.kind)] = change;
    g_pendingChanges.store(true);
    ReleaseSRWLockExclusive(&g_settingsLock);
}

bool ValidResolutionChoice(void* render, uint32_t index)
{
    const uint32_t count = Field<uint32_t>(render, 0x8804E8);
    if (index >= 256 || !count || count > 256)
        return false;
    const uintptr_t entry = Field<uintptr_t>(render, 0x8804F0 + index * 0x10);
    const uintptr_t base = reinterpret_cast<uintptr_t>(render) + 0x87A8E8;
    if (entry < base || entry >= base + count * 0x5C || (entry - base) % 0x5C != 0)
        return false;
    const auto* dimensions = reinterpret_cast<const uint32_t*>(entry + 0x50);
    AcquireSRWLockExclusive(&g_deviceLock);
    const bool valid = dimensions[0] && dimensions[1] &&
        (!g_caps.MaxTextureWidth || dimensions[0] <= g_caps.MaxTextureWidth) &&
        (!g_caps.MaxTextureHeight || dimensions[1] <= g_caps.MaxTextureHeight);
    ReleaseSRWLockExclusive(&g_deviceLock);
    return valid;
}

bool ApplyPropertyNow()
{
    // Before the first apply boundary, native construction sets logical display
    // fields and then sApp saves them. Preserve those property-only writes:
    // queuing them past that save would persist stale startup settings. No
    // rendering or plugin resources are admitted yet; dirty bits still coalesce
    // into the first controller-owned apply/Reset transaction.
    return g_applyingSettings || g_phase.load() == Phase::Startup;
}

void __fastcall HookResolution(void* render, uint32_t index)
{
    if (ApplyPropertyNow()) {
        if (ValidResolutionChoice(render, index))
            g_originalResolution(render, index);
        else
            HookLog("D3D9: rejected invalid resolution choice=%u before teardown\n", index);
        return;
    }
    Change change;
    change.kind = ChangeKind::Resolution;
    change.object = render;
    change.value = index;
    QueueChange(change);
}

void __fastcall HookFullscreen(void* render, bool enabled)
{
    if (ApplyPropertyNow()) {
        g_originalFullscreen(render, enabled);
        return;
    }
    Change change;
    change.kind = ChangeKind::Fullscreen;
    change.object = render;
    change.fullscreen = enabled;
    QueueChange(change);
}

void __fastcall HookVsync(void* render, bool enabled)
{
    if (ApplyPropertyNow()) {
        g_originalVsync(render, enabled);
        return;
    }
    Change change;
    change.kind = ChangeKind::Vsync;
    change.object = render;
    change.value = enabled;
    QueueChange(change);
}

void __fastcall HookResize(void* render, bool fullscreen, const uint64_t* dimensions, uint64_t force)
{
    if (!dimensions)
        return;
    if (ApplyPropertyNow()) {
        const uint32_t width = static_cast<uint32_t>(*dimensions);
        const uint32_t height = static_cast<uint32_t>(*dimensions >> 32);
        AcquireSRWLockExclusive(&g_deviceLock);
        const bool valid = width && height &&
            (!g_caps.MaxTextureWidth || width <= g_caps.MaxTextureWidth) &&
            (!g_caps.MaxTextureHeight || height <= g_caps.MaxTextureHeight);
        ReleaseSRWLockExclusive(&g_deviceLock);
        if (valid)
            g_originalResize(render, fullscreen, dimensions, force);
        else
            HookLog("D3D9: rejected invalid resize=%ux%u before teardown\n", width, height);
        return;
    }
    Change change;
    change.kind = ChangeKind::Resize;
    change.object = render;
    change.dimensions = *dimensions;
    change.fullscreen = fullscreen;
    change.force = static_cast<uint8_t>(force) != 0;
    QueueChange(change);
}

bool __fastcall HookSyncSettings(void* main)
{
    Change change;
    change.kind = ChangeKind::Sync;
    change.object = main;
    memcpy(change.options, &Field<uint32_t>(main, 0x4028C), sizeof(change.options));
    QueueChange(change);
    // Clear the native per-frame retry flag: the controller now owns a copy of
    // the entire selection, not just the first fullscreen/resolution callback.
    return false;
}

void ApplyQueuedSettings(void* render)
{
    decltype(g_changes) changes;
    AcquireSRWLockExclusive(&g_settingsLock);
    changes = g_changes;
    g_changes = {};
    g_pendingChanges.store(false);
    ReleaseSRWLockExclusive(&g_settingsLock);
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) {
        return a.order < b.order;
    });
    g_phase.store(Phase::Settings);
    g_applyingSettings = true;
    for (const Change& change : changes) {
        if (!change.order)
            continue;
        switch (change.kind) {
        case ChangeKind::Resolution: HookResolution(change.object, change.value); break;
        case ChangeKind::Fullscreen: HookFullscreen(change.object, change.fullscreen); break;
        case ChangeKind::Vsync: HookVsync(change.object, change.value != 0); break;
        case ChangeKind::Resize:
            HookResize(change.object, change.fullscreen, &change.dimensions, change.force);
            break;
        case ChangeKind::Sync:
            if (!ValidResolutionChoice(render, change.options[0])) {
                HookLog("D3D9: rejected unavailable graphics resolution selection=%u\n",
                    change.options[0]);
                break;
            }
            memcpy(&Field<uint32_t>(change.object, 0x4028C), change.options, sizeof(change.options));
            // Native sync changes at most fullscreen-off, resolution,
            // fullscreen-on, then remaining options. Apply these property-only
            // steps together BEFORE one native teardown, not on separate frames.
            for (unsigned step = 0; step < 5; ++step) {
                if (!g_originalSync(change.object))
                    break;
                if (step == 4)
                    HookLog("D3D9: graphics selections did not converge; retained effective settings\n");
            }
            break;
        default: break;
        }
    }
    g_applyingSettings = false;
}

void __fastcall HookApplyGraphics(void* render, uintptr_t a2, uintptr_t a3, uintptr_t a4)
{
    constexpr size_t dirtyOffset = 0x867734;
    if (!Field<uint32_t>(render, dirtyOffset) && !g_pendingChanges.load()) {
        // Preserve normal parallel rendering; do not drain EndScene every tick.
        g_originalApplyGraphics(render, a2, a3, a4);
        return;
    }
    if (g_inTransition)
        FailTransition(E_UNEXPECTED, "reentrant native graphics transition");
    g_transitions.fetch_add(1);
    g_inTransition = true;
    g_transitionRender = render;
    // Drain plugin work, but release the lock before native worker/UI waits.
    AcquireSRWLockExclusive(&g_deviceLock);
    ReleaseSRWLockExclusive(&g_deviceLock);
    ApplyQueuedSettings(render);
    const uint32_t dirty = Field<uint32_t>(render, dirtyOffset);
    if (!dirty) {
        g_inTransition = false;
        g_transitionRender = nullptr;
        g_phase.store(g_nativeReady.load() ? Phase::Ready : Phase::Startup);
        g_transitions.fetch_sub(1);
        g_originalApplyGraphics(render, a2, a3, a4);
        return;
    }
    D3D9ResetDiagnostics_RecordGraphicsSync();
    g_nativeReady.store(false);
    g_released = false;
    const ULONGLONG start = GetTickCount64();
    ULONGLONG retryStart = start;
    HookLog("D3D9: native graphics transition dirty=0x%X requested=%ux%u\n",
        dirty, Field<uint32_t>(render, 0x8676E0), Field<uint32_t>(render, 0x8676E4));
    for (;;) {
        g_originalApplyGraphics(render, a2, a3, a4);
        if (Field<uint32_t>(render, dirtyOffset) == 0)
            break;
        // Native DEVICELOST returns before teardown. This is the only point
        // where calling the apply routine again is safe. No gameplay slips
        // through while waiting for device readiness.
        if (g_released)
            FailTransition(E_FAIL, "native graphics rebuild left dirty state");
        if (GetTickCount64() - retryStart >= 30000) {
            if (!OfferRetry(D3DERR_DEVICELOST))
                FailTransition(D3DERR_DEVICELOST, "device did not become ready for teardown", false);
            retryStart = GetTickCount64();
        }
        Sleep(100);
    }
    if (!Field<void*>(render, 0x8676E8) || !Field<void*>(render, 0x8897C8))
        FailTransition(E_OUTOFMEMORY, "native rendering resources were not rebuilt");
    const uint32_t displays = Field<uint32_t>(render, 0x889808);
    if (!displays || displays > 2)
        FailTransition(E_UNEXPECTED, "invalid native display count after rebuild");
    for (uint32_t i = 0; i < displays; ++i) {
        if (!Field<void*>(render, 0x8897C8 + i * 0x28))
            FailTransition(E_OUTOFMEMORY, "native backbuffer wrapper was not rebuilt");
    }
    AcquireSRWLockExclusive(&g_deviceLock);
    g_active = g_effective;
    ++g_generation;
    ReleaseSRWLockExclusive(&g_deviceLock);
    ApplyD3D9BorderlessWindow();
    g_nativeReady.store(true);
    g_phase.store(Phase::Ready);
    g_inTransition = false;
    g_transitionRender = nullptr;
    g_transitions.fetch_sub(1);
    HookLog("D3D9: committed graphics generation=%llu bb=%ux%u interval=0x%08X elapsed=%llums\n",
        (unsigned long long)g_generation, g_active.BackBufferWidth, g_active.BackBufferHeight,
        g_active.PresentationInterval, (unsigned long long)(GetTickCount64() - start));
}

void __fastcall HookResultHandler(void* app, int32_t result,
                                 const char* operation, uintptr_t context)
{
    if (result != 0) {
        HookLog("D3D9: failure HRESULT=0x%08X operation=%s caller=%p thread=%lu context=%p\n",
            (uint32_t)result, operation ? operation : "<null>",
            __builtin_return_address(0), GetCurrentThreadId(), (void*)context);
        HookLogFlush();
    }
    if (FAILED(result) && g_inTransition)
        FailTransition(result, operation ? operation : "native graphics operation");
    g_originalResultHandler(app, result, operation, context);
}

volatile LONG g_graphicsSyncCalls = 0;
volatile LONG g_appResetCalls = 0;
volatile LONG g_forwardedAppResetCalls = 0;
volatile LONG g_deviceResetCalls = 0;
volatile LONG g_watchdogStarted = 0;
volatile LONG g_startupComplete = 0;
ULONGLONG g_watchdogStartTick = 0;

LONG ReadCounter(volatile LONG* counter)
{
    return InterlockedCompareExchange(counter, 0, 0);
}

DWORD WINAPI StartupWatchdogThread(LPVOID)
{
    unsigned elapsedSeconds = 0;
    while (InterlockedCompareExchange(&g_startupComplete, 0, 0) == 0) {
        Sleep(1000);
        ++elapsedSeconds;
        if (elapsedSeconds % 5 != 0 ||
            InterlockedCompareExchange(&g_startupComplete, 0, 0) != 0) {
            continue;
        }

        const uintptr_t app = *(uintptr_t*)_addr(0x140E17758);
        const LONG jobType = app ? *(volatile LONG*)(app + 0x2AC4) : -1;
        const LONG resetResult = app ? *(volatile LONG*)(app + 0x2AF0) : -1;
        const LONG retryAck = app ? *(volatile LONG*)(app + 0x2AF4) : -1;
        HookLog("D3D9: startup watchdog elapsed=%us app=%p "
            "jobType=%ld resetResult=0x%08lX retryAck=%ld "
            "syncCalls=%ld appResetCalls=%ld forwardedAppResets=%ld "
            "deviceResetCalls=%ld phase=%u ready=%d uiJob=%u transitions=%u\n",
            elapsedSeconds, (void*)app, jobType,
            (unsigned long)resetResult, retryAck,
            ReadCounter(&g_graphicsSyncCalls), ReadCounter(&g_appResetCalls),
            ReadCounter(&g_forwardedAppResetCalls),
            ReadCounter(&g_deviceResetCalls), static_cast<unsigned>(g_phase.load()),
            g_nativeReady.load(), static_cast<unsigned>(g_job.state.load()), g_transitions.load());
    }
    return 0;
}

} // namespace

bool D3D9Reset_InstallNativeHooks()
{
    D3D9Reset_GetRenderingThreadOverride();
    struct Hook { uintptr_t address; void* detour; void** original; };
#define RESET_HOOK(address, name, original) \
    { address, reinterpret_cast<void*>(&name), reinterpret_cast<void**>(&original) }
    const Hook hooks[] = {
        RESET_HOOK(0x140537080, HookBeginFrame, g_originalBeginFrame),
        RESET_HOOK(0x1405371F0, HookResultHandler, g_originalResultHandler),
        RESET_HOOK(0x14053D690, HookApplyGraphics, g_originalApplyGraphics),
        RESET_HOOK(0x140540DB0, HookRelease, g_originalRelease),
        RESET_HOOK(0x14051F6A0, HookAppReset, g_originalAppReset),
        RESET_HOOK(0x14051F8C0, HookCooperative, g_originalCooperative),
        RESET_HOOK(0x14051F930, HookUIUpdate, g_originalUIUpdate),
        RESET_HOOK(0x14053F620, HookResolution, g_originalResolution),
        RESET_HOOK(0x14053EB00, HookFullscreen, g_originalFullscreen),
        RESET_HOOK(0x140540D80, HookVsync, g_originalVsync),
        RESET_HOOK(0x14053EDE0, HookResize, g_originalResize),
        RESET_HOOK(0x140259BC0, HookSyncSettings, g_originalSync),
    };
#undef RESET_HOOK
    // Never enable half a controller: property hooks need the apply boundary,
    // and Reset submission needs the UI dispatcher and release hook.
    size_t created = 0;
    MH_STATUS status = MH_OK;
    for (const Hook& hook : hooks) {
        void* target = reinterpret_cast<void*>(_addr(hook.address));
        status = MH_CreateHook(target, hook.detour, hook.original);
        if (status != MH_OK)
            break;
        ++created;
        status = MH_QueueEnableHook(target);
        if (status != MH_OK)
            break;
    }
    if (status == MH_OK)
        status = MH_ApplyQueued();
    if (status != MH_OK) {
        for (size_t i = 0; i < created; ++i) {
            void* target = reinterpret_cast<void*>(_addr(hooks[i].address));
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
        HookLog("D3D9: controller hook installation failed status=%d; rolled back %zu hooks\n",
            (int)status, created);
        return false;
    } else {
        HookLog("D3D9: installed presentation controller (%zu native hooks)\n", created);
    }
    return true;
}

int D3D9Reset_GetRenderingThreadOverride()
{
    if (!D3D9Reset_RenderingThreadOverrideAvailable) return -1;
    static const bool loaded = [] {
        char path[MAX_PATH] = {};
        LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
        const int mode = static_cast<int>(GetPrivateProfileIntA(
            "Advanced", "RenderingThreadOverride", 0, path));
        g_renderingThreadOverride.store(mode >= -1 && mode <= 1 ? mode : 0);
        return true;
    }();
    (void)loaded;
    return g_renderingThreadOverride.load();
}

bool D3D9Reset_IsRenderingThreadEnabled()
{
    return g_renderingThreadEnabled.load(std::memory_order_relaxed);
}

bool D3D9Reset_SetRenderingThreadOverride(int mode)
{
    if (!D3D9Reset_RenderingThreadOverrideAvailable) return false;
    if (mode < -1 || mode > 1) return false;
    D3D9Reset_GetRenderingThreadOverride();
    char path[MAX_PATH] = {};
    LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
    if (!IniSafe_WriteString("Advanced", "RenderingThreadOverride",
                             mode < 0 ? "-1" : mode ? "1" : "0", path))
        return false;
    g_renderingThreadOverride.store(mode);
    return true;
}

[[noreturn]] void D3D9Reset_AbortStartup(const char* reason)
{
    DisableReturnToLaunchMenuOnExit();
    FailTransition(E_FAIL, reason);
}

namespace {

bool InstallDeviceHook(IDirect3DDevice9* device)
{
    if (!device)
        return false;
    AcquireSRWLockExclusive(&g_deviceLock);
    if (g_resetTarget) {
        ReleaseSRWLockExclusive(&g_deviceLock);
        return true;
    }
    void* target = (*reinterpret_cast<void***>(device))[16];
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&HookResetDevice),
        reinterpret_cast<void**>(&g_originalReset));
    if (status == MH_OK) {
        status = MH_EnableHook(target);
        if (status == MH_OK)
            g_resetTarget = target;
        else
            MH_RemoveHook(target);
    }
    ReleaseSRWLockExclusive(&g_deviceLock);
    HookLog("D3D9: install device Reset hook %p status=%d\n", target, (int)status);
    return status == MH_OK;
}

void RecordNativeDevice(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& requested)
{
    AcquireSRWLockExclusive(&g_deviceLock);
    const bool replacement = g_device && g_device != device;
    ReleaseSRWLockExclusive(&g_deviceLock);
    // Rebinding ImGui and every native owner to a replacement device requires
    // a separate teardown. Do not silently attach them to a different device.
    if (replacement)
        D3D9Reset_AbortStartup("Native renderer device replacement requires restarting the game");
    AcquireSRWLockExclusive(&g_deviceLock);
    g_createThread = GetCurrentThreadId();
    D3DDEVICE_CREATION_PARAMETERS creation = {};
    g_creationFocusWindow = SUCCEEDED(device->GetCreationParameters(&creation))
        ? creation.hFocusWindow : nullptr;
    g_caps = {};
    if (FAILED(device->GetDeviceCaps(&g_caps)))
        g_caps = {};
    g_nativeReady.store(false);
    RememberActive(device, requested);
    ReleaseSRWLockExclusive(&g_deviceLock);
    // Install before native startup Reset, not after the first EndScene.
    if (!InstallDeviceHook(device))
        D3D9Reset_AbortStartup("Could not install the renderer Reset hook; see Timestone.log");
}

} // namespace

bool D3D9Reset_BeginOverlay(IDirect3DDevice9* device)
{
    if (g_transitions.load() || !g_nativeReady.load() ||
        !TryAcquireSRWLockExclusive(&g_deviceLock))
        return false;
    if (g_transitions.load() || !g_nativeReady.load() ||
        device != g_device || !g_deviceOperational || !g_resetTarget) {
        ReleaseSRWLockExclusive(&g_deviceLock);
        return false;
    }
    if (g_pluginInvalidated) {
        if (ImGui::GetCurrentContext()) {
            if (GetTickCount64() < g_nextPluginRetry) {
                ReleaseSRWLockExclusive(&g_deviceLock);
                return false;
            }
            if (!ImGui_ImplDX9_CreateDeviceObjects()) {
                g_nextPluginRetry = GetTickCount64() + 1000;
                HookLog("D3D9: overlay resource rebuild failed; native renderer remains ready\n");
                ReleaseSRWLockExclusive(&g_deviceLock);
                return false;
            }
        }
        FrameStepper_SetDevice(device);
        g_pluginInvalidated = false;
    }
    return true;
}

void D3D9Reset_EndOverlay()
{
    ReleaseSRWLockExclusive(&g_deviceLock);
}

void D3D9ResetDiagnostics_RecordGraphicsSync()
{
    InterlockedIncrement(&g_graphicsSyncCalls);
}

void D3D9ResetDiagnostics_BeginAppReset(bool forwarded)
{
    InterlockedIncrement(&g_appResetCalls);
    if (forwarded)
        InterlockedIncrement(&g_forwardedAppResetCalls);
}

void D3D9ResetDiagnostics_EndAppReset(uint64_t result, uint64_t elapsedMs)
{
    HookLog("D3D9: app Reset submission returned result=%llu elapsed=%llums\n",
        (unsigned long long)result, (unsigned long long)elapsedMs);
}

void D3D9ResetDiagnostics_RecordDeviceReset()
{
    InterlockedIncrement(&g_deviceResetCalls);
}

void D3D9ResetDiagnostics_StartStartupWatchdog()
{
    if (InterlockedCompareExchange(&g_watchdogStarted, 1, 0) != 0)
        return;

    g_watchdogStartTick = GetTickCount64();
    HANDLE thread = CreateThread(nullptr, 0, StartupWatchdogThread,
        nullptr, 0, nullptr);
    if (!thread) {
        HookLog("D3D9: failed to start startup watchdog error=%lu\n",
            GetLastError());
        return;
    }
    CloseHandle(thread);
}

void D3D9ResetDiagnostics_CompleteStartup()
{
    InterlockedExchange(&g_startupComplete, 1);
    HookLog("D3D9: startup watchdog complete elapsed=%llums "
        "syncCalls=%ld appResetCalls=%ld forwardedAppResets=%ld "
        "deviceResetCalls=%ld\n",
        (unsigned long long)(GetTickCount64() - g_watchdogStartTick),
        ReadCounter(&g_graphicsSyncCalls), ReadCounter(&g_appResetCalls),
        ReadCounter(&g_forwardedAppResetCalls),
        ReadCounter(&g_deviceResetCalls));
}
