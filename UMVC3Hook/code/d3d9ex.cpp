#include "d3d9ex.h"
#include "d3d9reset.h"
#include "mk10utils.h"
#include "launchmenu_utils.h"
#include "lag_diagnostics.h"
#include "frame_pacing.h"
#include "../kiero/minhook/include/MinHook.h"
#include <atomic>
#include <cstdint>
#include <cstring>

extern void HookLog(const char*, ...);

namespace {
bool enabled = false;
// VSync off on the flip swapchain. An ordinary Present there still queues
// each frame behind the last and blocks when the queue is full, which shows
// as uneven frame times; FORCEIMMEDIATE replaces the queued frame instead.
bool immediatePresent = false;
constexpr DWORD kForceImmediate = 0x00000100; // D3DPRESENT_FORCEIMMEDIATE; not in these headers
bool forceImmediateRefused = false;
bool fellBack = false; // wanted, but the Ex device could not be created
IDirect3DDevice9Ex* exDevice = nullptr; // Borrowed from the native renderer.
HWND deviceWindow = nullptr;
const GUID stagingKey = {0x8cad6d71, 0xb8b5, 0x482c, {0xb2,0x14,0x71,0x73,0x65,0x78,0x30,0x31}};

template<typename T>
T& Field(void* object, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        HookLog("D3D9Ex: %s failed HRESULT=0x%08X\n", operation, (unsigned)result);
        D3D9Reset_AbortStartup(operation);
    }
}

// The real GPU texture owns this via private data. Shared native texture views
// automatically share its staging storage, and final COM release frees it.
struct Staging final : IUnknown {
    std::atomic<ULONG> refs {1};
    IDirect3DBaseTexture9* texture;
    SRWLOCK mutex = SRWLOCK_INIT;
    unsigned locks = 0;
    explicit Staging(IDirect3DBaseTexture9* value) : texture(value) {}
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualGUID(iid, IID_IUnknown)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return ++refs; }
    ULONG WINAPI Release() override {
        const ULONG left = --refs;
        if (!left) { texture->Release(); delete this; }
        return left;
    }
};

Staging* GetStaging(IDirect3DBaseTexture9* texture)
{
    Staging* staging = nullptr;
    DWORD size = sizeof(staging);
    if (texture && SUCCEEDED(texture->GetPrivateData(stagingKey, &staging, &size)))
        return staging; // D3DSPD_IUNKNOWN returns an owned reference.
    return nullptr;
}

void Upload(Staging* staging, IDirect3DBaseTexture9* gpu)
{
    // Explicitly dirty level zero: native code can edit any mip independently.
    switch (staging->texture->GetType()) {
    case D3DRTYPE_TEXTURE:
        Check(static_cast<IDirect3DTexture9*>(staging->texture)->AddDirtyRect(nullptr), "dirty texture");
        break;
    case D3DRTYPE_CUBETEXTURE:
        for (unsigned face = 0; face < 6; ++face)
            Check(static_cast<IDirect3DCubeTexture9*>(staging->texture)->AddDirtyRect(
                static_cast<D3DCUBEMAP_FACES>(face), nullptr), "dirty cube texture");
        break;
    case D3DRTYPE_VOLUMETEXTURE:
        Check(static_cast<IDirect3DVolumeTexture9*>(staging->texture)->AddDirtyBox(nullptr), "dirty volume texture");
        break;
    default: D3D9Reset_AbortStartup("Unsupported staged texture type");
    }
    Check(exDevice->UpdateTexture(staging->texture, gpu), "upload managed texture");
}

// A 2D or cube texture sends up only the mip (and face) that was written.
// Volumes have no per-level surface copy: they go up whole, once nothing
// is mapped any more.
void UploadSubresource(Staging* staging, IDirect3DBaseTexture9* gpu, unsigned subresource)
{
    IDirect3DSurface9* from = nullptr;
    IDirect3DSurface9* to = nullptr;
    switch (staging->texture->GetType()) {
    case D3DRTYPE_TEXTURE:
        Check(static_cast<IDirect3DTexture9*>(staging->texture)->GetSurfaceLevel(subresource, &from),
            "staging texture level");
        Check(static_cast<IDirect3DTexture9*>(gpu)->GetSurfaceLevel(subresource, &to),
            "GPU texture level");
        break;
    case D3DRTYPE_CUBETEXTURE: {
        const UINT levels = staging->texture->GetLevelCount();
        const auto face = static_cast<D3DCUBEMAP_FACES>(subresource / levels);
        Check(static_cast<IDirect3DCubeTexture9*>(staging->texture)->GetCubeMapSurface(
            face, subresource % levels, &from), "staging cube level");
        Check(static_cast<IDirect3DCubeTexture9*>(gpu)->GetCubeMapSurface(
            face, subresource % levels, &to), "GPU cube level");
        break;
    }
    default:
        if (!staging->locks) Upload(staging, gpu);
        return;
    }
    const HRESULT result = exDevice->UpdateSurface(from, nullptr, to, nullptr);
    from->Release();
    to->Release();
    Check(result, "upload managed texture level");
}

using TextureCreate = void(__fastcall*)(void*, void*, uint64_t, void*);
using TextureMap = void*(__fastcall*)(void*, void*, unsigned, int, void*);
using TextureUnmap = void(__fastcall*)(void*, unsigned);
TextureCreate originalTextureCreate = nullptr;
TextureMap originalTextureMap = nullptr;
TextureUnmap originalTextureUnmap = nullptr;

void __fastcall CreateTexture(void* object, void* shared, uint64_t flags, void* data)
{
    const int usage = Field<int>(object, 0x18);
    if (!exDevice || usage == 1 || usage == 2 || usage == 3 || usage == 5 ||
        (shared && reinterpret_cast<bool(__fastcall*)(void*, void*)>(_addr(0x140781FD0))(shared, object))) {
        originalTextureCreate(object, shared, flags, data);
        return;
    }
    // Keep the engine's format, compressed-block, mip, cube-face and volume
    // upload logic intact, but have it populate a CPU texture instead of MANAGED.
    auto*& slot = Field<IDirect3DBaseTexture9*>(object, 0x48);
    IDirect3DBaseTexture9* const before = slot;
    Field<int>(object, 0x18) = 3; // Native USAGE_STAGING -> D3DPOOL_SYSTEMMEM.
    originalTextureCreate(object, nullptr, flags, data);
    Field<int>(object, 0x18) = usage;
    // The native function creates nothing for texture kinds 0 and 8 (it
    // leaves or clears the slot): there is then nothing to adapt.
    if (!slot || slot == before) return;
    IDirect3DBaseTexture9* gpu = nullptr;
    const UINT levels = slot->GetLevelCount();
    switch (slot->GetType()) {
    case D3DRTYPE_TEXTURE: {
        D3DSURFACE_DESC desc = {};
        Check(static_cast<IDirect3DTexture9*>(slot)->GetLevelDesc(0, &desc), "texture description");
        IDirect3DTexture9* texture = nullptr;
        Check(exDevice->CreateTexture(desc.Width, desc.Height, levels, 0, desc.Format,
            D3DPOOL_DEFAULT, &texture, nullptr), "create GPU texture");
        gpu = texture;
        break;
    }
    case D3DRTYPE_CUBETEXTURE: {
        D3DSURFACE_DESC desc = {};
        Check(static_cast<IDirect3DCubeTexture9*>(slot)->GetLevelDesc(0, &desc), "cube description");
        IDirect3DCubeTexture9* texture = nullptr;
        Check(exDevice->CreateCubeTexture(desc.Width, levels, 0, desc.Format,
            D3DPOOL_DEFAULT, &texture, nullptr), "create GPU cube texture");
        gpu = texture;
        break;
    }
    case D3DRTYPE_VOLUMETEXTURE: {
        D3DVOLUME_DESC desc = {};
        Check(static_cast<IDirect3DVolumeTexture9*>(slot)->GetLevelDesc(0, &desc), "volume description");
        IDirect3DVolumeTexture9* texture = nullptr;
        Check(exDevice->CreateVolumeTexture(desc.Width, desc.Height, desc.Depth, levels, 0,
            desc.Format, D3DPOOL_DEFAULT, &texture, nullptr), "create GPU volume texture");
        gpu = texture;
        break;
    }
    default: D3D9Reset_AbortStartup("Unsupported native managed texture type");
    }
    auto* staging = new Staging(slot); // Takes the native CPU texture reference.
    Upload(staging, gpu);
    Check(gpu->SetPrivateData(stagingKey, staging, sizeof(IUnknown*), D3DSPD_IUNKNOWN),
        "attach managed texture storage");
    slot = gpu;
    staging->Release();
}

void* __fastcall MapTexture(void* object, void* result, unsigned subresource, int type, void* rect)
{
    auto* gpu = Field<IDirect3DBaseTexture9*>(object, 0x48);
    Staging* staging = GetStaging(gpu);
    if (!staging) return originalTextureMap(object, result, subresource, type, rect);
    {
        AcquireSRWLockExclusive(&staging->mutex);
        D3DLOCKED_RECT mapped = {};
        D3DLOCKED_BOX box = {};
        HRESULT hr = D3DERR_INVALIDCALL;
        switch (staging->texture->GetType()) {
        case D3DRTYPE_TEXTURE:
            hr = static_cast<IDirect3DTexture9*>(staging->texture)->LockRect(
                subresource, &mapped, static_cast<RECT*>(rect), 0);
            break;
        case D3DRTYPE_CUBETEXTURE: {
            const UINT levels = staging->texture->GetLevelCount();
            hr = static_cast<IDirect3DCubeTexture9*>(staging->texture)->LockRect(
                static_cast<D3DCUBEMAP_FACES>(subresource / levels), subresource % levels,
                &mapped, static_cast<RECT*>(rect), 0);
            break;
        }
        case D3DRTYPE_VOLUMETEXTURE:
            // Native volume map ignores prect as well.
            hr = static_cast<IDirect3DVolumeTexture9*>(staging->texture)->LockBox(
                subresource, &box, nullptr, 0);
            mapped.pBits = box.pBits; mapped.Pitch = box.RowPitch;
            break;
        default: break;
        }
        Check(hr, "map managed texture");
        Field<void*>(result, 0) = mapped.pBits;
        Field<int>(result, 8) = mapped.Pitch;
        Field<int>(result, 12) = box.pBits ? box.SlicePitch : mapped.Pitch;
        ++staging->locks;
        ReleaseSRWLockExclusive(&staging->mutex);
    }
    staging->Release();
    return result;
}

void __fastcall UnmapTexture(void* object, unsigned subresource)
{
    auto* gpu = Field<IDirect3DBaseTexture9*>(object, 0x48);
    Staging* staging = GetStaging(gpu);
    if (!staging) { originalTextureUnmap(object, subresource); return; }
    {
        AcquireSRWLockExclusive(&staging->mutex);
        if (!staging->locks) D3D9Reset_AbortStartup("Unbalanced managed texture unmap");
        HRESULT hr = D3DERR_INVALIDCALL;
        switch (staging->texture->GetType()) {
        case D3DRTYPE_TEXTURE:
            hr = static_cast<IDirect3DTexture9*>(staging->texture)->UnlockRect(subresource); break;
        case D3DRTYPE_CUBETEXTURE: {
            const UINT levels = staging->texture->GetLevelCount();
            hr = static_cast<IDirect3DCubeTexture9*>(staging->texture)->UnlockRect(
                static_cast<D3DCUBEMAP_FACES>(subresource / levels), subresource % levels); break;
        }
        case D3DRTYPE_VOLUMETEXTURE:
            hr = static_cast<IDirect3DVolumeTexture9*>(staging->texture)->UnlockBox(subresource); break;
        default: break;
        }
        Check(hr, "unmap managed texture");
        --staging->locks;
        UploadSubresource(staging, gpu, subresource);
        ReleaseSRWLockExclusive(&staging->mutex);
    }
    staging->Release();
}

using VertexCreate = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using IndexCreate = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
using Present = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
VertexCreate originalVertexCreate = nullptr;
IndexCreate originalIndexCreate = nullptr;
Present originalPresent = nullptr;

HRESULT WINAPI CreateVertex(IDirect3DDevice9* device, UINT size, DWORD usage, DWORD fvf,
    D3DPOOL pool, IDirect3DVertexBuffer9** output, HANDLE* shared)
{
    if (device == exDevice && pool == D3DPOOL_MANAGED) pool = D3DPOOL_DEFAULT;
    return originalVertexCreate(device, size, usage, fvf, pool, output, shared);
}
HRESULT WINAPI CreateIndex(IDirect3DDevice9* device, UINT size, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, IDirect3DIndexBuffer9** output, HANDLE* shared)
{
    if (device == exDevice && pool == D3DPOOL_MANAGED) pool = D3DPOOL_DEFAULT;
    return originalIndexCreate(device, size, usage, format, pool, output, shared);
}
HRESULT WINAPI PresentFrame(IDirect3DDevice9* device, const RECT* src, const RECT* dst,
    HWND window, const RGNDATA* dirty)
{
    HRESULT result;
    const int64_t pacing = FramePacing_BeforePresent(device);
    {
        // This hook owns Present on an Ex device, so the backend timing that
        // dllmain would otherwise bind to the same function is taken here.
        LagDiagnosticScope diagnostic(LagMetric::BackendPresent);
        if (device == exDevice && immediatePresent && !forceImmediateRefused) {
            result = exDevice->PresentEx(src, dst, window, dirty, kForceImmediate);
            if (result == D3DERR_INVALIDCALL) {
                // Not accepted here: present the ordinary way from now on.
                forceImmediateRefused = true;
                HookLog("D3D9Ex: FORCEIMMEDIATE refused; using ordinary Present\n");
                result = originalPresent(device, src, dst, window, dirty);
            }
        } else {
            result = originalPresent(device, src, dst, window, dirty);
        }
    }
    FramePacing_OnPresent(device, pacing);
    if (device != exDevice) return result;
    if (result == S_PRESENT_OCCLUDED) return D3D_OK;
    if (result == S_PRESENT_MODE_CHANGED) return D3DERR_DEVICELOST;
    Check(result, "D3D9Ex Present (restart required)");
    return result;
}

struct Hook { void* target; void* detour; void** original; };
bool Install(const Hook* hooks, size_t count)
{
    size_t created = 0;
    MH_STATUS status = MH_OK;
    for (size_t i = 0; i < count; ++i) {
        status = MH_CreateHook(hooks[i].target, hooks[i].detour, hooks[i].original);
        if (status != MH_OK) break;
        ++created;
        status = MH_QueueEnableHook(hooks[i].target);
        if (status != MH_OK) break;
    }
    if (status == MH_OK) status = MH_ApplyQueued();
    if (status == MH_OK) return true;
    for (size_t i = 0; i < created; ++i) {
        MH_DisableHook(hooks[i].target);
        MH_RemoveHook(hooks[i].target);
    }
    HookLog("D3D9Ex: hook installation failed status=%d\n", (int)status);
    return false;
}
} // namespace

bool D3D9Ex_Initialize()
{
    char path[MAX_PATH] = {};
    LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
    // On unless the ini turns it off.
    enabled = !path[0] || GetPrivateProfileIntA("Graphics", "ExperimentalD3D9Ex", 1, path) != 0;
    if (!enabled) return true;
    const Hook hooks[] = {
        {(void*)_addr(0x140782060), (void*)CreateTexture, (void**)&originalTextureCreate},
        {(void*)_addr(0x1407832C0), (void*)MapTexture, (void**)&originalTextureMap},
        {(void*)_addr(0x140783A40), (void*)UnmapTexture, (void**)&originalTextureUnmap},
    };
    HookLog("D3D9Ex: backend enabled; set [Graphics] ExperimentalD3D9Ex=0 to revert\n");
    return Install(hooks, sizeof(hooks) / sizeof(hooks[0]));
}

bool D3D9Ex_Enabled() { return enabled; }

void D3D9Ex_FallBack()
{
    // The texture hooks stay installed: without an Ex device they pass
    // straight through to the game's own code.
    enabled = false;
    fellBack = true;
}

const char* D3D9Ex_BackendName()
{
    if (exDevice) return "Direct3D 9Ex (flip)";
    return fellBack ? "Direct3D 9 (9Ex unavailable)" : "Direct3D 9";
}
bool D3D9Ex_IsDevice(IDirect3DDevice9* device)
{
    return device && device == exDevice;
}

void D3D9Ex_Prepare(D3DPRESENT_PARAMETERS& p)
{
    if (!enabled) return;
    p.Windowed = TRUE;
    p.FullScreen_RefreshRateInHz = 0;
    p.SwapEffect = D3DSWAPEFFECT_FLIPEX;
    p.BackBufferCount = 2;
    p.MultiSampleType = D3DMULTISAMPLE_NONE;
    p.MultiSampleQuality = 0;
    p.Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    immediatePresent = p.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE;
    forceImmediateRefused = false;
    HookLog("D3D9Ex: presentation interval=0x%08X force-immediate presents=%d\n",
            (unsigned)p.PresentationInterval, immediatePresent ? 1 : 0);
}

HRESULT D3D9Ex_Create(UINT adapter, D3DDEVTYPE type, HWND window, DWORD behavior,
    D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** output)
{
    if (!parameters || !output || exDevice) return D3DERR_INVALIDCALL;
    *output = nullptr;
    // Do not silently stack the native-resource adapter on a translation layer.
    HMODULE module = GetModuleHandleA("d3d9.dll");
    char actual[MAX_PATH] = {}, system[MAX_PATH] = {};
    const DWORD actualLength = module ? GetModuleFileNameA(module, actual, MAX_PATH) : 0;
    const UINT systemLength = GetSystemDirectoryA(system, MAX_PATH);
    if (!actualLength || actualLength >= MAX_PATH || !systemLength || systemLength >= MAX_PATH)
        return E_FAIL;
    if (strlen(system) + sizeof("\\d3d9.dll") > sizeof(system)) return E_FAIL;
    strcat(system, "\\d3d9.dll");
    if (_stricmp(actual, system) != 0) {
        HookLog("D3D9Ex: d3d9.dll is not the system one (%s)\n", actual);
        return E_FAIL;
    }
    using CreateEx = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    auto create = reinterpret_cast<CreateEx>(GetProcAddress(module, "Direct3DCreate9Ex"));
    if (!create) return E_FAIL; // not NOTAVAILABLE: the create hook retries that
    IDirect3D9Ex* core = nullptr;
    HRESULT result = create(D3D_SDK_VERSION, &core);
    if (FAILED(result)) return result;
    IDirect3DDevice9Ex* device = nullptr;
    result = core->CreateDeviceEx(adapter, type, window, behavior, parameters, nullptr, &device);
    core->Release();
    if (FAILED(result)) return result;
    // Nothing below may leave a half-adopted device behind: the caller falls
    // back to an ordinary one.
    result = device->SetMaximumFrameLatency(1);
    if (FAILED(result)) {
        HookLog("D3D9Ex: set maximum frame latency failed HRESULT=0x%08X\n", (unsigned)result);
        device->Release();
        return result;
    }
    exDevice = device;
    deviceWindow = parameters->hDeviceWindow ? parameters->hDeviceWindow : window;
    auto** vtable = *reinterpret_cast<void***>(device);
    const Hook hooks[] = {
        {vtable[26], (void*)CreateVertex, (void**)&originalVertexCreate},
        {vtable[27], (void*)CreateIndex, (void**)&originalIndexCreate},
        {vtable[17], (void*)PresentFrame, (void**)&originalPresent},
    };
    if (!Install(hooks, sizeof(hooks) / sizeof(hooks[0]))) {
        exDevice = nullptr;
        deviceWindow = nullptr;
        device->Release();
        return E_FAIL;
    }
    *output = device;
    HookLog("D3D9Ex: active FLIPEX device=%p maximumFrameLatency=1\n", device);
    return D3D_OK;
}

HRESULT D3D9Ex_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
{
    if (!D3D9Ex_IsDevice(device)) return D3DERR_INVALIDCALL;
    const HRESULT result = exDevice->ResetEx(parameters, nullptr);
    if (result == D3D_OK) Check(exDevice->SetMaximumFrameLatency(1), "restore maximum frame latency");
    return result;
}

HRESULT D3D9Ex_Check(IDirect3DDevice9* device)
{
    if (!D3D9Ex_IsDevice(device)) return device->TestCooperativeLevel();
    const HRESULT result = exDevice->CheckDeviceState(deviceWindow);
    if (result == S_PRESENT_OCCLUDED) return D3D_OK;
    if (result == S_PRESENT_MODE_CHANGED) return D3DERR_DEVICENOTRESET;
    return result;
}
