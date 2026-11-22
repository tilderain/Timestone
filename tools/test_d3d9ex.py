"""Execute the production native-resource adapter against COM ownership mocks.

These tests do not validate a Windows driver, native callsites, or display latency.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_d3d9reset import function

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
#define WINAPI
#define __fastcall
using HRESULT = int;
using ULONG = unsigned;
using UINT = unsigned;
using DWORD = unsigned;
using REFIID = int;
using SRWLOCK = bool;
#define SRWLOCK_INIT false
void AcquireSRWLockExclusive(bool* p) { assert(!*p); *p = true; }
void ReleaseSRWLockExclusive(bool* p) { assert(*p); *p = false; }
void AcquireSRWLockShared(bool* p) { assert(!*p); *p = true; }
void ReleaseSRWLockShared(bool* p) { assert(*p); *p = false; }
int privateDataReads = 0;
constexpr int S_OK = 0, E_POINTER = -1, E_NOINTERFACE = -2, D3DERR_INVALIDCALL = -3;
constexpr int D3D_OK = 0, D3DERR_DEVICELOST = -4, D3DERR_DEVICENOTRESET = -5;
constexpr int S_PRESENT_OCCLUDED = 1, S_PRESENT_MODE_CHANGED = 2;
using HWND = void*;
HWND deviceWindow = nullptr;
constexpr int IID_IUnknown = 1, stagingKey = 2, bufferCopyKey = 3;
bool stageBuffers = true;
constexpr int D3DRTYPE_TEXTURE = 3, D3DRTYPE_CUBETEXTURE = 4, D3DRTYPE_VOLUMETEXTURE = 5;
constexpr int D3DPOOL_DEFAULT = 0, D3DPOOL_SYSTEMMEM = 2, D3DSPD_IUNKNOWN = 1;
constexpr int TRUE = 1, D3DSWAPEFFECT_FLIPEX = 5, D3DMULTISAMPLE_NONE = 0;
constexpr unsigned D3DPRESENTFLAG_LOCKABLE_BACKBUFFER = 1;
using D3DCUBEMAP_FACES = unsigned;
bool IsEqualGUID(int a, int b) { return a == b; }
#define SUCCEEDED(x) ((x) >= 0)
#define FAILED(x) ((x) < 0)
struct IUnknown {
    virtual HRESULT QueryInterface(REFIID, void**) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
};
void HookLog(const char*, ...) {}
[[noreturn]] void D3D9Reset_AbortStartup(const char* message) { throw std::runtime_error(message); }
struct RECT {};
struct D3DLOCKED_RECT { int Pitch; void* pBits; };
struct D3DLOCKED_BOX { int RowPitch, SlicePitch; void* pBits; };
struct D3DSURFACE_DESC { unsigned Width = 8, Height = 8, Format = 21; };
struct D3DVOLUME_DESC { unsigned Width = 8, Height = 8, Depth = 4, Format = 21; };
struct D3DPRESENT_PARAMETERS {
    unsigned Windowed = 0, FullScreen_RefreshRateInHz = 60, SwapEffect = 1;
    unsigned BackBufferCount = 1, MultiSampleType = 4, MultiSampleQuality = 2;
    unsigned Flags = 3, PresentationInterval = 1;
};
bool enabled = false;
int surfaceUploads = 0;
struct FakeTexture;
struct FakeSurface {
    FakeTexture* texture; unsigned index;
    ULONG Release() { delete this; return 0; }
};
using IDirect3DSurface9 = FakeSurface;
int aliveTextures = 0, updates = 0, dirtyCalls = 0, nativeMaps = 0, nativeUnmaps = 0;
bool failUpload = false;
struct FakeTexture {
    unsigned refs = 1, kind, levels = 3;
    IUnknown* metadata = nullptr;
    std::array<int, 18> data = {};
    std::set<unsigned> locked;
    explicit FakeTexture(unsigned k) : kind(k) { ++aliveTextures; }
    ULONG AddRef() { return ++refs; }
    ULONG Release() {
        unsigned left = --refs;
        if (!left) { if (metadata) metadata->Release(); --aliveTextures; delete this; }
        return left;
    }
    unsigned GetType() { return kind; }
    unsigned GetLevelCount() { return levels; }
    HRESULT GetLevelDesc(unsigned level, D3DSURFACE_DESC*) { assert(!level); return S_OK; }
    HRESULT GetLevelDesc(unsigned level, D3DVOLUME_DESC*) { assert(!level); return S_OK; }
    HRESULT SetPrivateData(int key, void* pointer, unsigned size, unsigned flags) {
        assert(key == stagingKey && size == sizeof(IUnknown*) && flags == D3DSPD_IUNKNOWN);
        assert(!metadata); metadata = static_cast<IUnknown*>(pointer); metadata->AddRef(); return S_OK;
    }
    HRESULT GetPrivateData(int key, void* out, DWORD* size) {
        assert(key == stagingKey && *size == sizeof(void*));
        ++privateDataReads;
        if (!metadata) return E_NOINTERFACE;
        metadata->AddRef(); *static_cast<IUnknown**>(out) = metadata; return S_OK;
    }
    HRESULT AddDirtyRect(const RECT*) { ++dirtyCalls; return S_OK; }
    HRESULT AddDirtyRect(unsigned face, const RECT*) { assert(face < 6); ++dirtyCalls; return S_OK; }
    HRESULT AddDirtyBox(void*) { ++dirtyCalls; return S_OK; }
    HRESULT LockRect(unsigned level, D3DLOCKED_RECT* out, RECT*, unsigned flags) {
        if (level >= levels || !locked.insert(level).second) return D3DERR_INVALIDCALL;
        assert(!flags); *out = {32, &data[level]}; return S_OK;
    }
    HRESULT LockRect(unsigned face, unsigned level, D3DLOCKED_RECT* out, RECT*, unsigned flags) {
        const unsigned index = face * levels + level;
        if (face >= 6 || level >= levels || !locked.insert(index).second) return D3DERR_INVALIDCALL;
        assert(!flags); *out = {32, &data[index]}; return S_OK;
    }
    HRESULT LockBox(unsigned level, D3DLOCKED_BOX* out, void*, unsigned flags) {
        if (level >= levels || !locked.insert(level).second) return D3DERR_INVALIDCALL;
        assert(!flags); *out = {32, 256, &data[level]}; return S_OK;
    }
    HRESULT UnlockRect(unsigned level) { return locked.erase(level) ? S_OK : D3DERR_INVALIDCALL; }
    HRESULT UnlockRect(unsigned face, unsigned level) { return UnlockRect(face * levels + level); }
    HRESULT UnlockBox(unsigned level) { return UnlockRect(level); }
    HRESULT GetSurfaceLevel(unsigned level, FakeSurface** out) {
        if (level >= levels) return D3DERR_INVALIDCALL;
        *out = new FakeSurface{this, level}; return S_OK;
    }
    HRESULT GetCubeMapSurface(unsigned face, unsigned level, FakeSurface** out) {
        if (face >= 6 || level >= levels) return D3DERR_INVALIDCALL;
        *out = new FakeSurface{this, face * levels + level}; return S_OK;
    }
};
using IDirect3DBaseTexture9 = FakeTexture;
using IDirect3DTexture9 = FakeTexture;
using IDirect3DCubeTexture9 = FakeTexture;
using IDirect3DVolumeTexture9 = FakeTexture;
constexpr unsigned kForceImmediate = 0x100, kDoNotWait = 1;
constexpr int kStillDrawing = -6;
bool doNotWaitRefused = false;
unsigned refreshWaits = 0;
void WaitForRefresh() { ++refreshWaits; }
struct RGNDATA;
struct Device {
    HRESULT resetResult = S_OK, state = S_OK;
    unsigned resets = 0, latencySets = 0;
    HRESULT ResetEx(D3DPRESENT_PARAMETERS* params, void* mode) {
        assert(!mode); ++resets; params->BackBufferCount = 0; return resetResult;
    }
    HRESULT SetMaximumFrameLatency(unsigned value) { assert(value == 1); ++latencySets; return S_OK; }
    HRESULT CheckDeviceState(HWND) { return state; }
    HRESULT forcedResult = S_OK, polledResult = S_OK;
    unsigned forcedPresents = 0, polledPresents = 0, busyPolls = 0;
    HRESULT PresentEx(const RECT*, const RECT*, HWND, const RGNDATA*, unsigned flags) {
        if (flags == kDoNotWait) {
            ++polledPresents;
            if (busyPolls) { --busyPolls; return kStillDrawing; }
            return polledResult;
        }
        assert(flags == kForceImmediate); ++forcedPresents; return forcedResult;
    }
    HRESULT TestCooperativeLevel() { return state; }
    HRESULT UpdateTexture(FakeTexture* cpu, FakeTexture* gpu) {
        assert(cpu != gpu && cpu->kind == gpu->kind && cpu->locked.empty());
        if (failUpload) return D3DERR_INVALIDCALL;
        gpu->data = cpu->data; ++updates; return S_OK;
    }
    HRESULT UpdateSurface(FakeSurface* from, const RECT* rect, FakeSurface* to, void* point) {
        assert(!rect && !point && from->texture != to->texture && from->index == to->index);
        assert(!from->texture->locked.count(from->index));
        if (failUpload) return D3DERR_INVALIDCALL;
        to->texture->data[to->index] = from->texture->data[from->index];
        ++surfaceUploads; return S_OK;
    }
    HRESULT CreateTexture(unsigned w, unsigned h, unsigned levels, unsigned usage, unsigned fmt,
                          unsigned pool, FakeTexture** out, void*) {
        assert(w == 8 && h == 8 && levels == 3 && !usage && fmt == 21 && pool == D3DPOOL_DEFAULT);
        *out = new FakeTexture(D3DRTYPE_TEXTURE); return S_OK;
    }
    HRESULT CreateCubeTexture(unsigned w, unsigned levels, unsigned usage, unsigned fmt,
                             unsigned pool, FakeTexture** out, void*) {
        assert(w == 8 && levels == 3 && !usage && fmt == 21 && pool == D3DPOOL_DEFAULT);
        *out = new FakeTexture(D3DRTYPE_CUBETEXTURE); return S_OK;
    }
    HRESULT CreateVolumeTexture(unsigned w, unsigned h, unsigned d, unsigned levels, unsigned usage,
                               unsigned fmt, unsigned pool, FakeTexture** out, void*) {
        assert(w == 8 && h == 8 && d == 4 && levels == 3 && !usage && fmt == 21 && pool == D3DPOOL_DEFAULT);
        *out = new FakeTexture(D3DRTYPE_VOLUMETEXTURE); return S_OK;
    }
} device;
Device* exDevice = &device;
using IDirect3DDevice9 = Device;
using D3DFORMAT = unsigned;
using D3DPOOL = unsigned;
using HANDLE = void*;
constexpr unsigned D3DUSAGE_WRITEONLY = 8, D3DFMT_INDEX16 = 101, D3DLOCK_READONLY = 0x10;
int aliveBuffers = 0;
struct FakeBuffer : IUnknown {
    unsigned refs = 1, pool, usage, size, format, locks = 0;
    IUnknown* copy = nullptr;
    std::vector<char> data;
    FakeBuffer(unsigned p, unsigned u, unsigned s, unsigned f)
        : pool(p), usage(u), size(s), format(f), data(s) { ++aliveBuffers; }
    virtual ~FakeBuffer() = default;
    HRESULT QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG AddRef() override { return ++refs; }
    ULONG Release() override {
        const unsigned left = --refs;
        if (!left) { assert(!locks); if (copy) copy->Release(); --aliveBuffers; delete this; }
        return left;
    }
    HRESULT Lock(unsigned offset, unsigned length, void** out, unsigned flags) {
        assert(!offset && !length);
        // Only a copy in memory that is not write-only is ever read back.
        if (flags == D3DLOCK_READONLY) assert(pool == D3DPOOL_SYSTEMMEM && !(usage & D3DUSAGE_WRITEONLY));
        else assert(!flags);
        ++locks; *out = data.data(); return S_OK;
    }
    HRESULT Unlock() { assert(locks); --locks; return S_OK; }
    HRESULT SetPrivateData(int key, void* pointer, unsigned size_, unsigned flags) {
        assert(key == bufferCopyKey && size_ == sizeof(IUnknown*) && flags == D3DSPD_IUNKNOWN && !copy);
        copy = static_cast<IUnknown*>(pointer); copy->AddRef(); return S_OK;
    }
};
using IDirect3DVertexBuffer9 = FakeBuffer;
using IDirect3DIndexBuffer9 = FakeBuffer;
struct RGNDATA {};
constexpr unsigned D3DPOOL_MANAGED = 1;
unsigned seenPool = 99, seenUsage = 99;
HRESULT BufferCreate(Device*, UINT size, DWORD usage, unsigned format, D3DPOOL pool, FakeBuffer** out, HANDLE*) {
    seenPool = pool; seenUsage = usage;
    if (out) *out = new FakeBuffer(pool, usage, size, format);
    return S_OK;
}
auto originalVertexCreate = BufferCreate;
auto originalIndexCreate = BufferCreate;
void NativeBufferCreate(void*, unsigned, void*);
void* NativeBufferDestroy(void* object, uint64_t);
auto originalVertexBufferCreate = NativeBufferCreate;
auto originalIndexBufferCreate = NativeBufferCreate;
auto originalVertexBufferDestroy = NativeBufferDestroy;
auto originalIndexBufferDestroy = NativeBufferDestroy;
using BufferCreateFn = void(*)(void*, unsigned, void*);
#define BufferCreate BufferCreateFn
HRESULT presentResult = S_OK;
unsigned nativePresents = 0;
HRESULT NativePresent(Device*, const RECT*, const RECT*, HWND, const RGNDATA*) { ++nativePresents; return presentResult; }
auto originalPresent = NativePresent;
bool Matches(void*, void*) { return true; }
uintptr_t _addr(uintptr_t address) { assert(address == 0x140781FD0); return (uintptr_t)Matches; }
void NativeCreate(void*, void*, uint64_t, void*);
void* NativeMap(void*, void* result, unsigned, int, void*) { ++nativeMaps; return result; }
void NativeUnmap(void*, unsigned) { ++nativeUnmaps; }
auto originalTextureCreate = NativeCreate;
auto originalTextureMap = NativeMap;
auto originalTextureUnmap = NativeUnmap;
enum class LagMetric { BackendPresent };
struct LagDiagnosticScope { explicit LagDiagnosticScope(LagMetric) {} };
int64_t FramePacing_BeforePresent(Device*) { return 0; }
bool D3D9Reset_TransitionInProgress() { return false; }
void FramePacing_OnPresent(Device*, int64_t) {}
bool immediatePresent = false, forceImmediateRefused = false;
int binds = 0;
HRESULT NativeBind(Device*, DWORD, FakeTexture*) { ++binds; return S_OK; }
auto originalSetTexture = NativeBind;
bool battle = false;
bool FrameStepper_IsBattleSynchronized() { return battle; }
// A clock that moves on by `clockStep` every time it is read.
double clockNow = 0, clockStep = 0;
double NowMs() { clockNow += clockStep; return clockNow; }
void LogTextureStats() {}
constexpr unsigned D3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000u;
'''

CASES = r'''
void NativeCreate(void* object, void* shared, uint64_t flags, void* data) {
    assert(flags == 123);
    if (shared) {
        Field<FakeTexture*>(object, 0x48) = Field<FakeTexture*>(shared, 0x48);
        return; // Native alias owns the parent, not a separate COM reference.
    }
    assert(Field<int>(object, 0x18) == 3);
    auto* texture = new FakeTexture(Field<unsigned>(object, 0x38));
    if (data) texture->data.fill(*static_cast<int*>(data));
    Field<FakeTexture*>(object, 0x48) = texture;
}
// The game's own create: MANAGED and write-only unless dynamic (1) or staging
// (3), filled at once when given data.
bool nativeIndex = false;
void NativeBufferCreate(void* object, unsigned size, void* data) {
    const int usage = Field<int>(object, 0x18);
    FakeBuffer* buffer = nullptr;
    const unsigned pool = usage == 1 ? D3DPOOL_DEFAULT : usage == 3 ? D3DPOOL_SYSTEMMEM : D3DPOOL_MANAGED;
    const unsigned flags = usage == 1 ? 0x200 : usage == 3 ? 0 : D3DUSAGE_WRITEONLY;
    if (nativeIndex) CreateIndex(&device, size, flags, D3DFMT_INDEX16, pool, &buffer, nullptr);
    else CreateVertex(&device, size, flags, 0, pool, &buffer, nullptr);
    Field<FakeBuffer*>(object, 0x30) = buffer;
    if (data) {
        void* to = nullptr;
        buffer->Lock(0, 0, &to, 0);
        memcpy(to, data, size);
        buffer->Unlock();
    }
}
int destroyed = 0;
void* NativeBufferDestroy(void* object, uint64_t) {
    if (auto*& buffer = Field<FakeBuffer*>(object, 0x30)) { buffer->Release(); buffer = nullptr; }
    ++destroyed;
    return object;
}
void BufferCases() {
    const char bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (bool index : {false, true}) {
        nativeIndex = index;
        const auto create = index ? CreateIndexBuffer : CreateVertexBuffer;
        const auto destroy = index ? DestroyIndexBuffer : DestroyVertexBuffer;
        std::array<uint64_t, 8> object = {};
        // Filled at once: made in memory, readable, and listed.
        create(object.data(), 8, (void*)bytes);
        auto* cpu = Field<FakeBuffer*>(object.data(), 0x30);
        assert(seenPool == D3DPOOL_SYSTEMMEM && seenUsage == 0 && pendingBufferCount == 1);
        assert(cpu->refs == 2 && aliveBuffers == 1 && !stagingBufferCreate);
        // The next frame makes the GPU buffer and hands it to the object.
        WarmBuffers();
        auto* gpu = Field<FakeBuffer*>(object.data(), 0x30);
        assert(gpu != cpu && pendingBufferCount == 0 && aliveBuffers == 2);
        assert(gpu->pool == D3DPOOL_DEFAULT && gpu->usage == D3DUSAGE_WRITEONLY && gpu->size == 8);
        assert(gpu->format == (index ? D3DFMT_INDEX16 : 0u));
        assert(!memcmp(gpu->data.data(), bytes, 8) && gpu->refs == 1);
        assert(gpu->copy == cpu && cpu->refs == 1 && !cpu->locks && !gpu->locks);
        assert(destroy(object.data(), 1) == object.data() && aliveBuffers == 0);

        // Destroyed while still listed: taken off the list, nothing made.
        create(object.data(), 8, (void*)bytes);
        assert(pendingBufferCount == 1 && aliveBuffers == 1);
        destroy(object.data(), 1);
        assert(pendingBufferCount == 0 && aliveBuffers == 0);
        WarmBuffers();
        assert(aliveBuffers == 0);

        // The game let go of the copy without destroying the object.
        create(object.data(), 8, (void*)bytes);
        Field<FakeBuffer*>(object.data(), 0x30)->Release();
        Field<FakeBuffer*>(object.data(), 0x30) = nullptr;
        WarmBuffers();
        assert(pendingBufferCount == 0 && aliveBuffers == 0 && !Field<FakeBuffer*>(object.data(), 0x30));

        // Created empty, dynamic, staging, or with the setting off: as before.
        create(object.data(), 8, nullptr);
        assert(seenPool == D3DPOOL_DEFAULT && seenUsage == D3DUSAGE_WRITEONLY && pendingBufferCount == 0);
        destroy(object.data(), 1);
        Field<int>(object.data(), 0x18) = 1;
        create(object.data(), 8, (void*)bytes);
        assert(seenPool == D3DPOOL_DEFAULT && seenUsage == 0x200 && pendingBufferCount == 0);
        destroy(object.data(), 1);
        Field<int>(object.data(), 0x18) = 3;
        create(object.data(), 8, (void*)bytes);
        assert(seenPool == D3DPOOL_SYSTEMMEM && pendingBufferCount == 0);
        destroy(object.data(), 1);
        Field<int>(object.data(), 0x18) = 0;
        stageBuffers = false;
        create(object.data(), 8, (void*)bytes);
        assert(seenPool == D3DPOOL_DEFAULT && seenUsage == D3DUSAGE_WRITEONLY && pendingBufferCount == 0);
        destroy(object.data(), 1);
        stageBuffers = true;
        assert(aliveBuffers == 0);

        // The frame's budget: 1 ms in a battle, each send taking 2.
        std::array<std::array<uint64_t, 8>, 3> objects = {};
        for (auto& each : objects) create(each.data(), 8, (void*)bytes);
        battle = true;
        clockStep = 2.0;
        WarmBuffers();
        assert(pendingBufferCount == 2);
        battle = false;
        WarmBuffers();
        assert(pendingBufferCount == 0 && aliveBuffers == 6);
        clockStep = 0;
        for (auto& each : objects) destroy(each.data(), 1);
        assert(aliveBuffers == 0 && pendingBuffers.empty());
    }
    nativeIndex = false;
}
int main() {
    BufferCases();
    D3DPRESENT_PARAMETERS params;
    D3D9Ex_Prepare(params);
    assert(params.SwapEffect == 1 && params.MultiSampleType == 4);
    enabled = true;
    D3D9Ex_Prepare(params);
    assert(params.Windowed && !params.FullScreen_RefreshRateInHz);
    assert(params.SwapEffect == 5 && params.BackBufferCount == 2);
    assert(params.Flags == 2 && !params.MultiSampleType && !params.MultiSampleQuality);
    assert(params.PresentationInterval == 1);
    Device helper;
    assert(D3D9Ex_Reset(&helper, &params) == D3DERR_INVALIDCALL);
    assert(!helper.resets && !device.resets);
    assert(D3D9Ex_Reset(&device, &params) == S_OK);
    assert(device.resets == 1 && device.latencySets == 1 && params.BackBufferCount == 0);
    device.resetResult = D3DERR_DEVICELOST;
    assert(D3D9Ex_Reset(&device, &params) == D3DERR_DEVICELOST);
    assert(device.latencySets == 1); // No resource/device calls after failed ResetEx.
    device.state = S_PRESENT_OCCLUDED;
    assert(D3D9Ex_Check(&device) == D3D_OK);
    device.state = S_PRESENT_MODE_CHANGED;
    assert(D3D9Ex_Check(&device) == D3DERR_DEVICENOTRESET);
    device.state = D3DERR_INVALIDCALL;
    assert(D3D9Ex_Check(&device) == D3DERR_INVALIDCALL);
    helper.state = D3DERR_DEVICELOST;
    assert(D3D9Ex_Check(&helper) == D3DERR_DEVICELOST);
    CreateVertex(&device, 64, 8, 0, D3DPOOL_MANAGED, nullptr, nullptr);
    assert(seenPool == D3DPOOL_DEFAULT);
    CreateIndex(&device, 64, 8, 101, D3DPOOL_MANAGED, nullptr, nullptr);
    assert(seenPool == D3DPOOL_DEFAULT);
    CreateVertex(&helper, 64, 8, 0, D3DPOOL_MANAGED, nullptr, nullptr);
    assert(seenPool == D3DPOOL_MANAGED);
    CreateIndex(&device, 64, 0, 101, D3DPOOL_SYSTEMMEM, nullptr, nullptr);
    assert(seenPool == D3DPOOL_SYSTEMMEM);
    // The default: one ordinary Present a frame, whatever the queue holds.
    assert(plainPresent);
    presentResult = S_OK;
    device.busyPolls = 5;
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK);
    assert(device.polledPresents == 0 && device.forcedPresents == 0 && nativePresents == 1 &&
           refreshWaits == 0);
    device.busyPolls = 0;
    nativePresents = 0;
    // [Graphics] D3D9ExPlainPresent=0, VSync on: the Ex device is presented
    // without waiting inside Direct3D.
    plainPresent = false;
    presentResult = S_PRESENT_OCCLUDED;
    device.polledResult = S_PRESENT_OCCLUDED;
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK);
    assert(device.polledPresents == 1 && nativePresents == 0);
    assert(PresentFrame(&helper, nullptr, nullptr, nullptr, nullptr) == S_PRESENT_OCCLUDED);
    assert(helper.polledPresents == 0 && nativePresents == 1);
    device.polledResult = S_PRESENT_MODE_CHANGED;
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3DERR_DEVICELOST);
    // A full queue: the refresh is waited for outside Direct3D, once, and the
    // frame then goes out through the ordinary waiting Present.
    device.polledResult = S_OK;
    device.busyPolls = 5;
    presentResult = S_OK;
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK);
    assert(refreshWaits == 1 && device.polledPresents == 3 && nativePresents == 2);
    device.busyPolls = 0;
    // Room in the queue: one try, no wait.
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK);
    assert(refreshWaits == 1 && device.polledPresents == 4 && nativePresents == 2);
    // A failure stops the game, as before.
    device.polledResult = D3DERR_DEVICELOST;
    bool presentStopped = false;
    try { PresentFrame(&device, nullptr, nullptr, nullptr, nullptr); }
    catch (const std::runtime_error&) { presentStopped = true; }
    assert(presentStopped);
    // A runtime that refuses the flag is presented the ordinary way from then on.
    device.polledResult = D3DERR_INVALIDCALL;
    const unsigned polledBefore = device.polledPresents;
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK);
    assert(doNotWaitRefused && device.polledPresents == polledBefore + 1 && nativePresents == 3);
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK);
    assert(device.polledPresents == polledBefore + 1 && nativePresents == 4);
    presentResult = D3DERR_INVALIDCALL;
    presentStopped = false;
    try { PresentFrame(&device, nullptr, nullptr, nullptr, nullptr); }
    catch (const std::runtime_error&) { presentStopped = true; }
    assert(presentStopped);
    // VSync off: presented with FORCEIMMEDIATE, and never again once refused.
    presentResult = S_OK;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    D3D9Ex_Prepare(params);
    assert(immediatePresent);
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK && device.forcedPresents == 1);
    assert(PresentFrame(&helper, nullptr, nullptr, nullptr, nullptr) == D3D_OK && helper.forcedPresents == 0);
    device.forcedResult = D3DERR_INVALIDCALL;
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK && device.forcedPresents == 2);
    assert(forceImmediateRefused);
    assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK && device.forcedPresents == 2);
    // ...and with VSync off a busy queue is never polled or waited out.
    {
        doNotWaitRefused = false;
        device.polledResult = S_OK;
        const unsigned waits = refreshWaits, polled = device.polledPresents;
        device.busyPolls = 1;
        assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK);
        assert(refreshWaits == waits && device.polledPresents == polled);
        device.busyPolls = 0;
    }

    for (unsigned kind : {D3DRTYPE_TEXTURE, D3DRTYPE_CUBETEXTURE, D3DRTYPE_VOLUMETEXTURE}) {
        std::array<uint64_t, 16> object = {}, alias = {};
        Field<unsigned>(object.data(), 0x38) = kind;
        int initial = 7;
        int before = updates;
        CreateTexture(object.data(), nullptr, 123, &initial);
        auto* gpu = Field<FakeTexture*>(object.data(), 0x48);
        assert(Field<int>(object.data(), 0x18) == 0 && aliveTextures == 2);
        // Creation uploads nothing: the texture waits for its first use.
        assert(gpu->data[0] == 0 && updates == before && pendingCount == 1 && gpu->refs == 2);
        Staging* stage = GetStaging(gpu);
        assert(stage && stage->refs == 3 && stage->texture != gpu && stage->pending);
        stage->Release();
        CreateTexture(alias.data(), object.data(), 123, nullptr);
        assert(Field<FakeTexture*>(alias.data(), 0x48) == gpu && aliveTextures == 2);

        const unsigned subresource = kind == D3DRTYPE_CUBETEXTURE ? 7 : 1;
        std::array<uint64_t, 2> mapped = {}, another = {};
        assert(MapTexture(alias.data(), mapped.data(), subresource, 0, nullptr) == mapped.data());
        assert(Field<int>(mapped.data(), 8) == 32);
        assert(Field<int>(mapped.data(), 12) == (kind == D3DRTYPE_VOLUMETEXTURE ? 256 : 32));
        *Field<int*>(mapped.data(), 0) = 42;
        // Bound while the game still has it mapped: it cannot be read yet, and stays waiting.
        const int bindsBefore = binds;
        const int surfacesStart = surfaceUploads;
        assert(SetTexture(&device, 0, gpu) == S_OK && binds == bindsBefore + 1);
        assert(updates == before && pendingCount == 1 && gpu->refs == 2);
        // Unmapping a texture that has never gone up sends nothing by itself.
        UnmapTexture(alias.data(), subresource);
        assert(updates == before && surfaceUploads == surfacesStart && pendingCount == 1);
        // Another device binding it changes nothing.
        assert(SetTexture(&helper, 0, gpu) == S_OK && updates == before && pendingCount == 1);
        // The first bind uploads it whole, once.
        assert(SetTexture(&device, 0, gpu) == S_OK && binds == bindsBefore + 3);
        assert(updates == before + 1 && pendingCount == 0 && gpu->refs == 1);
        assert(gpu->data[0] == 7 && gpu->data[subresource] == 42);
        assert(SetTexture(&device, 0, gpu) == S_OK && updates == before + 1);
        stage = GetStaging(gpu);
        assert(stage->refs == 2 && !stage->pending);
        stage->Release();

        // From here on an edit goes up as it is unmapped.
        MapTexture(alias.data(), mapped.data(), subresource, 0, nullptr);
        *Field<int*>(mapped.data(), 0) = 43;
        MapTexture(object.data(), another.data(), 0, 0, nullptr);
        *Field<int*>(another.data(), 0) = 12;
        const int surfacesBefore = surfaceUploads;
        UnmapTexture(alias.data(), subresource);
        if (kind == D3DRTYPE_VOLUMETEXTURE) {
            assert(updates == before + 1); // A volume waits for every level to be unmapped.
        } else {
            // Only the written level goes up, at once, with another still mapped.
            assert(surfaceUploads == surfacesBefore + 1 && gpu->data[subresource] == 43);
            assert(gpu->data[0] == 7);
        }
        UnmapTexture(object.data(), 0);
        assert(gpu->data[subresource] == 43 && gpu->data[0] == 12);
        if (kind == D3DRTYPE_VOLUMETEXTURE) assert(updates == before + 2 && surfaceUploads == surfacesBefore);
        else assert(updates == before + 1 && surfaceUploads == surfacesBefore + 2);
        assert(gpu->metadata && static_cast<Staging*>(gpu->metadata)->refs == 1);
        gpu->Release();
        assert(aliveTextures == 0); // Private data releases staging on final GPU release.
    }

    // The per-frame warm-up: waiting textures go up ahead of their first use,
    // within the frame's budget, and one the game already released is dropped.
    {
        std::array<std::array<uint64_t, 16>, 4> objects = {};
        int initial = 9;
        for (auto& object : objects) {
            Field<unsigned>(object.data(), 0x38) = D3DRTYPE_TEXTURE;
            CreateTexture(object.data(), nullptr, 123, &initial);
        }
        assert(pendingCount == 4 && aliveTextures == 8);
        Field<FakeTexture*>(objects[3].data(), 0x48)->Release(); // released before any use
        assert(aliveTextures == 8);
        const int before = updates;
        battle = true;   // 1 ms a frame
        clockStep = 2.0; // every upload takes longer than that
        WarmTextures();
        assert(pendingCount == 3);
        WarmTextures();
        assert(pendingCount == 2);
        battle = false;  // 6 ms a frame: the rest fit
        WarmTextures();
        assert(pendingCount == 0);
        clockStep = 0;
        assert(updates == before + 3 && aliveTextures == 6);
        for (int i = 0; i < 3; ++i) {
            auto* gpu = Field<FakeTexture*>(objects[i].data(), 0x48);
            assert(gpu->data[0] == 9 && gpu->refs == 1);
            gpu->Release();
        }
        assert(aliveTextures == 0);
        // A frame is presented with nothing waiting and with something waiting.
        Field<unsigned>(objects[0].data(), 0x38) = D3DRTYPE_TEXTURE;
        CreateTexture(objects[0].data(), nullptr, 123, &initial);
        presentResult = S_OK;
        assert(PresentFrame(&helper, nullptr, nullptr, nullptr, nullptr) == D3D_OK && pendingCount == 1);
        assert(PresentFrame(&device, nullptr, nullptr, nullptr, nullptr) == D3D_OK && pendingCount == 0);
        assert(updates == before + 4);
        Field<FakeTexture*>(objects[0].data(), 0x48)->Release();
        assert(aliveTextures == 0);

        // One texture the game keeps mapped does not hold the others up, and
        // is still found by a bind afterwards.
        for (auto& object : objects) {
            Field<unsigned>(object.data(), 0x38) = D3DRTYPE_TEXTURE;
            CreateTexture(object.data(), nullptr, 123, &initial);
        }
        for (auto& object : objects) {
            std::array<uint64_t, 2> mapped = {};
            MapTexture(object.data(), mapped.data(), 0, 0, nullptr);
            const int uploaded = updates;
            WarmTextures();
            assert(updates == uploaded + 3 && pendingCount == 1);
            auto* held = Field<FakeTexture*>(object.data(), 0x48);
            assert(SetTexture(&device, 0, held) == S_OK && updates == uploaded + 3 && pendingCount == 1);
            UnmapTexture(object.data(), 0);
            WarmTextures();
            assert(updates == uploaded + 4 && pendingCount == 0);
            for (auto& other : objects) Field<FakeTexture*>(other.data(), 0x48)->Release();
            assert(aliveTextures == 0);
            if (&object == &objects.back()) break;
            for (auto& other : objects) CreateTexture(other.data(), nullptr, 123, &initial);
        }

        // Once no texture has been created for a while the game is not
        // loading, and a frame gives the leftovers 1 ms, not 6.
        for (auto& object : objects) CreateTexture(object.data(), nullptr, 123, &initial);
        clockNow += 1000.0;
        clockStep = 2.0;
        WarmTextures();
        assert(pendingCount == 3);
        clockStep = 0;
        for (auto& object : objects) Field<FakeTexture*>(object.data(), 0x48)->Release();
        WarmTextures(); // all four released unused
        assert(pendingCount == 0 && aliveTextures == 0);
    }

    // A single-level texture goes up in one call when it is unmapped.
    {
        auto* cpu = new FakeTexture(D3DRTYPE_TEXTURE);
        auto* gpu = new FakeTexture(D3DRTYPE_TEXTURE);
        cpu->levels = gpu->levels = 1;
        Staging* single = new Staging(cpu);
        cpu->data[0] = 5;
        const int uploaded = updates, surfaces = surfaceUploads;
        UploadSubresource(single, gpu, 0);
        assert(updates == uploaded + 1 && surfaceUploads == surfaces && gpu->data[0] == 5);
        single->Release(); gpu->Release();
        assert(aliveTextures == 0);
    }
    assert(nativeMaps == 0 && nativeUnmaps == 0);
    std::array<uint64_t, 16> ordinary = {};
    auto* texture = new FakeTexture(D3DRTYPE_TEXTURE);
    Field<FakeTexture*>(ordinary.data(), 0x48) = texture;
    MapTexture(ordinary.data(), nullptr, 0, 0, nullptr);
    UnmapTexture(ordinary.data(), 0);
    assert(nativeMaps == 1 && nativeUnmaps == 1);
    texture->Release();
    // Looking a staging copy up never asks the texture, and the table is
    // empty again once every adapted texture is gone.
    assert(privateDataReads == 0 && stagingByTexture.empty());

    // A failed upload must not silently publish an apparently usable resource.
    Staging* stage = new Staging(new FakeTexture(D3DRTYPE_TEXTURE));
    texture = new FakeTexture(D3DRTYPE_TEXTURE);
    failUpload = true;
    bool stopped = false;
    try { Upload(stage, texture); } catch (const std::runtime_error&) { stopped = true; }
    assert(stopped);
    stage->Release(); texture->Release();
    assert(aliveTextures == 0);
}
'''


class D3D9ExTests(unittest.TestCase):
    def test_native_resource_adapter(self):
        source = (ROOT / "UMVC3Hook/code/d3d9ex.cpp").read_text()
        staging = source[source.index("struct Staging;"):source.index("// The staging copy of a GPU texture")]
        unit = MOCKS + function(source, "Field") + function(source, "Check") + staging
        for name in ("GetStaging", "Upload", "UploadSubresource", "Requeue", "Defer", "TakePending"):
            unit += function(source, name)
        # FlushPending, the warm-up budgets and WarmTextures.
        unit += source[source.index("enum class Flush"):source.index("// What the texture path cost")]
        for name in ("CreateTexture", "MapTexture", "UnmapTexture", "D3D9Ex_Prepare",
                     "D3D9Ex_IsDevice", "D3D9Ex_Reset", "D3D9Ex_Check"):
            unit += function(source, name)
        for name in ("SetTexture", "BufferPool", "CreateVertex", "CreateIndex", "CreateBuffer",
                     "CreateVertexBuffer", "CreateIndexBuffer", "ForgetBuffer", "DestroyVertexBuffer",
                     "DestroyIndexBuffer", "FillBuffer", "SendBuffer", "WarmBuffers", "PresentFrame"):
            unit += function(source, name)
        unit += CASES
        with tempfile.TemporaryDirectory(prefix="d3d9ex-", dir="/tmp/opencode") as tmp:
            cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
            cpp.write_text(unit)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
