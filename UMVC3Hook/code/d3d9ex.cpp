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
#include <unordered_map>

extern void HookLog(const char*, ...);
extern "C" bool FrameStepper_IsBattleSynchronized();

namespace {
bool enabled = false;
// VSync off on the flip swapchain. An ordinary Present there still queues
// each frame behind the last and blocks when the queue is full, which shows
// as uneven frame times; FORCEIMMEDIATE replaces the queued frame instead.
bool immediatePresent = false;
constexpr DWORD kForceImmediate = 0x00000100; // D3DPRESENT_FORCEIMMEDIATE; not in these headers
bool forceImmediateRefused = false;
// VSync on. With a one-frame queue an ordinary Present waits inside Direct3D
// for the queue to empty, about a whole frame, and holds the device's lock
// while it does (the device is created MULTITHREADED). The game's loader
// thread creates and fills textures through the same device, so it got the
// lock for about a millisecond a frame: on one Windows machine a character's
// textures took seconds to appear (2026-11-19, a 0.6.3 report: 363 textures,
// 845 ms inside the native create against 56 ms once Present stopped holding
// the lock, while every Present took 16 ms).
//
// The frame is still presented by the driver's own VSync, which is what keeps
// it evenly paced; only where the waiting happens changes. A first try with
// DONOTWAIT presents the frame if the queue has room. If it is full, the wait
// for the next refresh is spent in DwmFlush, outside Direct3D, and then an
// ordinary waiting Present is made, which now has little or nothing left to
// wait for. Polling the queue with a timer instead was tried and measured:
// it started frames up to 1.9 ms late.
constexpr DWORD kDoNotWait = 0x00000001; // D3DPRESENT_DONOTWAIT
constexpr HRESULT kStillDrawing = static_cast<HRESULT>(0x8876021CL); // D3DERR_WASSTILLDRAWING
bool doNotWaitRefused = false;
bool fellBack = false; // wanted, but the Ex device could not be created
// [Graphics] ExperimentalD3D9Ex=2: use the Ex device even where it is known
// to be a bad idea (see OnTranslationLayer).
bool forced = false;
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

// Which GPU textures have a staging copy, by texture. Every native texture
// map and unmap asks, adapted texture or not, and asking the texture itself
// (GetPrivateData) is a COM call with a GUID search and a reference count
// each time. A staging object lists itself when it is attached and takes
// itself out as it dies, which is while its GPU texture is being released.
struct Staging;
SRWLOCK stagingLock = SRWLOCK_INIT;
std::unordered_map<IDirect3DBaseTexture9*, Staging*> stagingByTexture;

// The real GPU texture owns this via private data. Shared native texture views
// automatically share its staging storage, and final COM release frees it.
struct Staging final : IUnknown {
    std::atomic<ULONG> refs {1};
    IDirect3DBaseTexture9* texture;
    IDirect3DBaseTexture9* gpu = nullptr; // set once attached; its key in stagingByTexture
    SRWLOCK mutex = SRWLOCK_INIT;
    unsigned locks = 0;
    bool pending = false; // nothing has gone up to the GPU texture yet
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
        if (!left) {
            if (gpu) {
                AcquireSRWLockExclusive(&stagingLock);
                const auto found = stagingByTexture.find(gpu);
                if (found != stagingByTexture.end() && found->second == this)
                    stagingByTexture.erase(found);
                ReleaseSRWLockExclusive(&stagingLock);
            }
            texture->Release();
            delete this;
        }
        return left;
    }
};

// Textures created but not yet sent to the GPU, by GPU texture. The managed
// pool the game was written for uploads a texture when it is first drawn, not
// when it is created; uploading at creation instead put the whole cost of a
// character or stage into its load (2026-11-19: loads noticeably longer than
// on the ordinary backend). So a new texture waits here until it is bound
// (SetTexture) or the per-frame warm-up reaches it. The set holds a reference
// on both objects, so neither can go away while it is listed.
SRWLOCK pendingLock = SRWLOCK_INIT;
std::unordered_map<IDirect3DBaseTexture9*, Staging*> pendingTextures;
std::atomic<unsigned> pendingCount {0};

// Since the last log line; see LogTextureStats. Counters only, added to
// without a lock from whichever thread creates, binds or presents; times are
// whole microseconds.
struct TextureStats {
    std::atomic<unsigned> created {0}, boundUploads {0}, warmUploads {0}, dropped {0};
    std::atomic<int64_t> fillUs {0}, gpuCreateUs {0}, boundUs {0}, boundWorstUs {0}, warmUs {0};
    std::atomic<bool> changed {false}; // anything to report
};
TextureStats textureStats;

void AddStat(std::atomic<unsigned>& count, std::atomic<int64_t>& total, double ms)
{
    count.fetch_add(1, std::memory_order_relaxed);
    total.fetch_add(static_cast<int64_t>(ms * 1000.0), std::memory_order_relaxed);
    textureStats.changed.store(true, std::memory_order_relaxed);
}

// The staging copy of a GPU texture, with a reference the caller releases,
// or null for a texture that has none.
Staging* GetStaging(IDirect3DBaseTexture9* texture)
{
    if (!texture) return nullptr;
    Staging* staging = nullptr;
    AcquireSRWLockShared(&stagingLock);
    const auto found = stagingByTexture.find(texture);
    if (found != stagingByTexture.end()) {
        // One on its way out (count already zero) is as good as gone.
        ULONG count = found->second->refs.load();
        while (count && !found->second->refs.compare_exchange_weak(count, count + 1)) {}
        if (count) staging = found->second;
    }
    ReleaseSRWLockShared(&stagingLock);
    return staging;
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

double NowMs()
{
    static double toMs = 0.0;
    LARGE_INTEGER value;
    if (toMs == 0.0) {
        QueryPerformanceFrequency(&value);
        toMs = 1000.0 / static_cast<double>(value.QuadPart);
    }
    QueryPerformanceCounter(&value);
    return static_cast<double>(value.QuadPart) * toMs;
}

// A new texture: its first upload is owed.
void Defer(Staging* staging, IDirect3DBaseTexture9* gpu)
{
    staging->pending = true;
    staging->AddRef();
    gpu->AddRef();
    AcquireSRWLockExclusive(&pendingLock);
    pendingTextures.emplace(gpu, staging);
    pendingCount.store(static_cast<unsigned>(pendingTextures.size()), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&pendingLock);
}

// Takes a texture out of the waiting set: `gpu` itself, or any one when it is
// null. The caller then owns the set's two references.
bool TakePending(IDirect3DBaseTexture9*& gpu, Staging*& staging)
{
    AcquireSRWLockExclusive(&pendingLock);
    const auto found = gpu ? pendingTextures.find(gpu) : pendingTextures.begin();
    const bool taken = found != pendingTextures.end();
    if (taken) {
        gpu = found->first;
        staging = found->second;
        pendingTextures.erase(found);
        pendingCount.store(static_cast<unsigned>(pendingTextures.size()), std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&pendingLock);
    return taken;
}

enum class Flush { Uploaded, Dropped, Mapped };

// Sends a taken texture up and lets go of the set's references. One the game
// still has mapped cannot be read yet and goes back in the set; one nobody
// else holds any more (`binding` rules that out) is simply released.
Flush FlushPending(IDirect3DBaseTexture9* gpu, Staging* staging, bool binding)
{
    Flush outcome = Flush::Uploaded;
    AcquireSRWLockExclusive(&staging->mutex);
    bool unused = false;
    if (!binding) {
        gpu->AddRef();
        unused = gpu->Release() == 1; // only the set's own reference is left
    }
    if (unused) {
        outcome = Flush::Dropped;
        staging->pending = false;
    } else if (staging->locks) {
        outcome = Flush::Mapped;
    } else {
        Upload(staging, gpu);
        staging->pending = false;
    }
    ReleaseSRWLockExclusive(&staging->mutex);
    if (outcome == Flush::Mapped) {
        AcquireSRWLockExclusive(&pendingLock);
        pendingTextures.emplace(gpu, staging);
        pendingCount.store(static_cast<unsigned>(pendingTextures.size()), std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&pendingLock);
        return outcome;
    }
    staging->Release();
    gpu->Release();
    return outcome;
}

// Most time a frame gives to uploading textures ahead of their first use. In
// a running battle a frame has little to spare; anywhere else (menus, loading
// screens, waiting for the opponent) nothing is lost by a longer frame.
constexpr double kWarmBattleMs = 1.0;
constexpr double kWarmIdleMs = 6.0;

// Once a frame, before it is presented: upload waiting textures until the
// frame's budget is spent, so few are left to be uploaded at their first draw.
void WarmTextures()
{
    unsigned left = pendingCount.load(std::memory_order_relaxed);
    if (!left) return;
    const double budget = FrameStepper_IsBattleSynchronized() ? kWarmBattleMs : kWarmIdleMs;
    const double start = NowMs();
    double now = start;
    for (; left && now - start < budget; --left) {
        IDirect3DBaseTexture9* gpu = nullptr;
        Staging* staging = nullptr;
        if (!TakePending(gpu, staging)) break;
        const Flush outcome = FlushPending(gpu, staging, false);
        const double after = NowMs();
        if (outcome == Flush::Uploaded) {
            AddStat(textureStats.warmUploads, textureStats.warmUs, after - now);
        } else if (outcome == Flush::Dropped) {
            textureStats.dropped.fetch_add(1, std::memory_order_relaxed);
            textureStats.changed.store(true, std::memory_order_relaxed);
        }
        now = after;
    }
}

// What the texture path cost since the last line, at most every two seconds.
// A frame with nothing to report pays one load. Temporary: it is how a load
// on a real Windows driver gets compared with the ordinary backend.
void LogTextureStats()
{
    if (!textureStats.changed.load(std::memory_order_relaxed)) return;
    static ULONGLONG lastLog = 0;
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastLog < 2000) return;
    lastLog = tick;
    textureStats.changed.store(false, std::memory_order_relaxed);
    const auto count = [](std::atomic<unsigned>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const auto ms = [](std::atomic<int64_t>& value) {
        return static_cast<double>(value.exchange(0, std::memory_order_relaxed)) / 1000.0;
    };
    const unsigned created = count(textureStats.created);
    const unsigned bound = count(textureStats.boundUploads);
    const unsigned warm = count(textureStats.warmUploads);
    const unsigned dropped = count(textureStats.dropped);
    const double fill = ms(textureStats.fillUs), gpuCreate = ms(textureStats.gpuCreateUs);
    const double boundMs = ms(textureStats.boundUs), worst = ms(textureStats.boundWorstUs);
    const double warmMs = ms(textureStats.warmUs);
    HookLog("D3D9Ex: textures created=%u (fill %.1f ms, GPU create %.1f ms) "
            "uploaded at first draw=%u (%.1f ms, longest %.1f) ahead of use=%u (%.1f ms) "
            "released unused=%u waiting=%u\n",
            created, fill, gpuCreate, bound, boundMs, worst, warm, warmMs, dropped,
            pendingCount.load(std::memory_order_relaxed));
}

// Waits for the desktop compositor's next pass, which follows the display's
// refresh. Not a Direct3D call, so the device's lock is free meanwhile. Where
// there is no compositor to wait for it returns at once and the Present that
// follows does the waiting, as it always did.
void WaitForRefresh()
{
    using Flush = HRESULT(WINAPI*)();
    static const Flush flush = [] {
        const HMODULE module = LoadLibraryW(L"dwmapi.dll");
        return module ? reinterpret_cast<Flush>(GetProcAddress(module, "DwmFlush")) : nullptr;
    }();
    if (flush) flush();
}

// Direct3D 9 running through Microsoft's D3D9On12 layer: GPUs with no
// Direct3D 9 driver of their own (Intel Arc, Iris Xe, UHD). The one machine
// where 0.6.3 loaded slowly was an Arc A750 (2026-11-19: a character on the
// character select took about five seconds to appear, still after texture
// creation and Present had been shown not to be the wait), and the layer is
// known for trouble with this game on other programs. What the Ex device and
// its default-pool resources cost there is not understood, so on that layer
// the game stays on the ordinary backend, which 0.6.2 ran on without the
// problem.
bool OnTranslationLayer()
{
    return GetModuleHandleA("d3d9on12.dll") != nullptr;
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
    const double createStart = NowMs();
    Field<int>(object, 0x18) = 3; // Native USAGE_STAGING -> D3DPOOL_SYSTEMMEM.
    originalTextureCreate(object, nullptr, flags, data);
    Field<int>(object, 0x18) = usage;
    const double filled = NowMs();
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
    Check(gpu->SetPrivateData(stagingKey, staging, sizeof(IUnknown*), D3DSPD_IUNKNOWN),
        "attach managed texture storage");
    staging->gpu = gpu;
    AcquireSRWLockExclusive(&stagingLock);
    stagingByTexture[gpu] = staging;
    ReleaseSRWLockExclusive(&stagingLock);
    slot = gpu;
    // Nothing is uploaded here: see pendingTextures.
    Defer(staging, gpu);
    staging->Release();
    AddStat(textureStats.created, textureStats.fillUs, filled - createStart);
    textureStats.gpuCreateUs.fetch_add(static_cast<int64_t>((NowMs() - filled) * 1000.0),
                                       std::memory_order_relaxed);
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
        // A texture still waiting for its first upload goes up whole then.
        if (!staging->pending) UploadSubresource(staging, gpu, subresource);
        ReleaseSRWLockExclusive(&staging->mutex);
    }
    staging->Release();
}

using VertexCreate = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
using IndexCreate = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
using Present = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using TextureBind = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
VertexCreate originalVertexCreate = nullptr;
IndexCreate originalIndexCreate = nullptr;
Present originalPresent = nullptr;
TextureBind originalSetTexture = nullptr;

// A texture about to be drawn with: if its first upload is still owed, it
// happens now. With nothing waiting this is one load and a compare.
HRESULT WINAPI SetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture)
{
    if (texture && device == exDevice && pendingCount.load(std::memory_order_relaxed)) {
        IDirect3DBaseTexture9* gpu = texture;
        Staging* staging = nullptr;
        if (TakePending(gpu, staging)) {
            const double start = NowMs();
            const Flush outcome = FlushPending(gpu, staging, true);
            const double took = NowMs() - start;
            if (outcome == Flush::Uploaded) {
                AddStat(textureStats.boundUploads, textureStats.boundUs, took);
                // Binds come from one thread; a lost update here would only
                // understate the longest one.
                const int64_t us = static_cast<int64_t>(took * 1000.0);
                if (us > textureStats.boundWorstUs.load(std::memory_order_relaxed))
                    textureStats.boundWorstUs.store(us, std::memory_order_relaxed);
            }
        }
    }
    return originalSetTexture(device, stage, texture);
}

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
    if (device == exDevice && D3D9Reset_TransitionInProgress()) {
        // The renderer is being rebuilt and the window restyled under it.
        // DXVK presents on its own thread, so a present issued now is still
        // in flight while the window changes; at start-up that left
        // vkQueuePresentKHR blocked for good and the game frozen on its next
        // buffer lock (Proton, 2026-11-20). Nothing worth showing has been
        // drawn in a transition, so the frame is simply not presented.
        static bool reported = false;
        if (!reported) {
            reported = true;
            HookLog("D3D9Ex: not presenting during a graphics transition\n");
        }
        return D3D_OK;
    }
    if (device == exDevice) {
        // Before the pacing hold, so the hold absorbs the time this takes.
        WarmTextures();
        LogTextureStats();
    }
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
        } else if (device == exDevice && !doNotWaitRefused) {
            result = exDevice->PresentEx(src, dst, window, dirty, kDoNotWait);
            if (result == kStillDrawing) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    HookLog("D3D9Ex: waiting for the refresh outside Present\n");
                }
                WaitForRefresh();
                result = originalPresent(device, src, dst, window, dirty);
            } else if (result == D3DERR_INVALIDCALL) {
                doNotWaitRefused = true;
                HookLog("D3D9Ex: DONOTWAIT refused; using ordinary Present\n");
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
    const int setting = path[0] ? GetPrivateProfileIntA("Graphics", "ExperimentalD3D9Ex", 1, path) : 1;
    enabled = setting != 0;
    forced = setting == 2;
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
    doNotWaitRefused = false;
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
    if (!forced && OnTranslationLayer()) {
        HookLog("D3D9Ex: Direct3D 9 runs through D3D9On12 here; using the ordinary backend "
                "(set [Graphics] ExperimentalD3D9Ex=2 to use 9Ex anyway)\n");
        core->Release();
        return E_FAIL;
    }
    IDirect3DDevice9Ex* device = nullptr;
    result = core->CreateDeviceEx(adapter, type, window, behavior, parameters, nullptr, &device);
    // Which driver this really is. Some GPUs have no Direct3D 9 driver of
    // their own and run it through Microsoft's D3D9On12 layer (Intel Arc; seen
    // 2026-11-19 on the one machine where loads were slow), which behaves
    // differently enough to need telling apart in a log.
    D3DADAPTER_IDENTIFIER9 identifier = {};
    if (SUCCEEDED(core->GetAdapterIdentifier(adapter, 0, &identifier)))
        HookLog("D3D9Ex: adapter \"%s\" driver %s vendor=0x%04lX device=0x%04lX "
                "d3d9on12=%d d3d12=%d dxvk=%d\n",
                identifier.Description, identifier.Driver, identifier.VendorId, identifier.DeviceId,
                GetModuleHandleA("d3d9on12.dll") ? 1 : 0, GetModuleHandleA("d3d12.dll") ? 1 : 0,
                GetModuleHandleA("dxvk_d3d9.dll") || GetModuleHandleA("winevulkan.dll") ? 1 : 0);
    core->Release();
    if (FAILED(result)) return result;
    // The layer may only be loaded once a device exists.
    if (!forced && OnTranslationLayer()) {
        HookLog("D3D9Ex: Direct3D 9 runs through D3D9On12 here; using the ordinary backend "
                "(set [Graphics] ExperimentalD3D9Ex=2 to use 9Ex anyway)\n");
        device->Release();
        return E_FAIL;
    }
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
        {vtable[65], (void*)SetTexture, (void**)&originalSetTexture},
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
