#include "shader_constants.h"
#include "../kiero/minhook/include/MinHook.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr UINT REGISTERS = 256; // float registers the game's shader model can address

using SetConstants = HRESULT (STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

struct RegisterFile {
    SetConstants original = nullptr;
    float value[REGISTERS][4] = {};
    uint8_t current[REGISTERS] = {}; // value[i] is what the device holds
};

RegisterFile g_vertex, g_pixel;
std::atomic<bool> g_forget{false};
std::atomic<DWORD> g_owner{0}; // the thread whose calls are filtered

HRESULT Set(RegisterFile& file, IDirect3DDevice9* device, UINT first, const float* data, UINT count)
{
    const DWORD thread = GetCurrentThreadId();
    DWORD owner = g_owner.load(std::memory_order_relaxed);
    if (!owner && g_owner.compare_exchange_strong(owner, thread, std::memory_order_relaxed))
        owner = thread;
    if (thread != owner) {
        // The copy belongs to one thread. Anything another thread sets goes
        // straight through and invalidates the copy on both sides of the call.
        g_forget.store(true, std::memory_order_relaxed);
        const HRESULT result = file.original(device, first, data, count);
        g_forget.store(true, std::memory_order_relaxed);
        return result;
    }
    if (g_forget.exchange(false, std::memory_order_relaxed)) {
        std::memset(g_vertex.current, 0, sizeof(g_vertex.current));
        std::memset(g_pixel.current, 0, sizeof(g_pixel.current));
    }
    if (!data || !count || first >= REGISTERS || count > REGISTERS - first)
        return file.original(device, first, data, count);

    const size_t bytes = count * sizeof(file.value[0]);
    if (!std::memchr(file.current + first, 0, count) && !std::memcmp(file.value[first], data, bytes))
        return D3D_OK;

    const HRESULT result = file.original(device, first, data, count);
    if (SUCCEEDED(result)) {
        std::memcpy(file.value[first], data, bytes);
        std::memset(file.current + first, 1, count);
    } else {
        std::memset(file.current + first, 0, count);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE SetVertexConstants(IDirect3DDevice9* device, UINT first, const float* data, UINT count)
{
    return Set(g_vertex, device, first, data, count);
}

HRESULT STDMETHODCALLTYPE SetPixelConstants(IDirect3DDevice9* device, UINT first, const float* data, UINT count)
{
    return Set(g_pixel, device, first, data, count);
}

bool Hook(IDirect3DDevice9* device, int slot, void* detour, SetConstants* original, const char* name)
{
    void* target = (*reinterpret_cast<void***>(device))[slot];
    MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    if (status == MH_OK)
        status = MH_EnableHook(target);
    HookLog("ShaderConstants: hook %s %p status=%d\n", name, target, (int)status);
    return status == MH_OK;
}

} // namespace

void ShaderConstants_Install(IDirect3DDevice9* device)
{
    // IDirect3DDevice9 vtable: 94 SetVertexShaderConstantF, 109 SetPixelShaderConstantF.
    Hook(device, 94, reinterpret_cast<void*>(&SetVertexConstants), &g_vertex.original, "SetVertexShaderConstantF");
    Hook(device, 109, reinterpret_cast<void*>(&SetPixelConstants), &g_pixel.original, "SetPixelShaderConstantF");
}

void ShaderConstants_Forget()
{
    g_forget.store(true, std::memory_order_relaxed);
}
