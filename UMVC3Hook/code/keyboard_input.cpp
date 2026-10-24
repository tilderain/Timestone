#include "keyboard_input.h"
#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"
#include <dinput.h>

#include <atomic>
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

namespace {
constexpr uintptr_t KeyboardMoveAddress = 0x1406a9d40;
constexpr size_t HeldOffset = 0x138;
static_assert(std::atomic<uint32_t>::is_always_lock_free);
void (__fastcall* g_originalMove)(void*) = nullptr;
std::atomic<uint32_t> g_revision{0}, g_held[8] = {}, g_toggles{0};
uint32_t g_writeRevision = 0, g_previousHeld[8] = {}, g_toggleState = 0;
bool g_firstUpdate = true;
std::atomic<IDirectInputDevice8A*> g_device{nullptr};
BYTE g_vkTable[256] = {};

void PublishKeyboard(const uint32_t (&held)[8])
{
    const int lockKeys[] = {VK_CAPITAL, VK_NUMLOCK, VK_SCROLL};
    for (unsigned i = 0; i < 3; ++i) {
        const int key = lockKeys[i];
        const uint32_t bit = 1u << (key & 31);
        if (!g_firstUpdate && (held[key >> 5] & ~g_previousHeld[key >> 5] & bit))
            g_toggleState ^= 1u << i;
    }
    std::memcpy(g_previousHeld, held, sizeof(held));
    g_firstUpdate = false;

    g_revision.store(++g_writeRevision, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (unsigned i = 0; i < 8; ++i)
        g_held[i].store(held[i], std::memory_order_relaxed);
    g_toggles.store(g_toggleState, std::memory_order_relaxed);
    g_revision.store(++g_writeRevision, std::memory_order_release);
}

void __fastcall CaptureKeyboardMove(void* keyboard)
{
    g_originalMove(keyboard);
    uint32_t held[8];
    // Native move owns these fields. Capture here, not from the render thread.
    std::memcpy(held, static_cast<BYTE*>(keyboard) + HeldOffset, sizeof(held));
    if (!g_device.load(std::memory_order_relaxed)) {
        auto* device = *reinterpret_cast<IDirectInputDevice8A**>(
            static_cast<BYTE*>(keyboard) + 0xa40);
        if (device) {
            // The capture hook and device reference live until process exit.
            device->AddRef();
            std::memcpy(g_vkTable, static_cast<BYTE*>(keyboard) + 0x38, sizeof(g_vkTable));
            g_device.store(device, std::memory_order_release);
        }
    }
    PublishKeyboard(held);
}
} // namespace

bool KeyboardInput_Initialize()
{
    // Seed lock toggles once at startup; normal frames reuse the native capture.
    BYTE state[256] = {};
    GetKeyboardState(state);
    g_toggleState = (state[VK_CAPITAL] & 1) | ((state[VK_NUMLOCK] & 1) << 1) |
                    ((state[VK_SCROLL] & 1) << 2);
    void* target = reinterpret_cast<void*>(_addr(KeyboardMoveAddress));
    MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(&CaptureKeyboardMove),
                                    reinterpret_cast<void**>(&g_originalMove));
    if (status == MH_OK) status = MH_EnableHook(target);
    HookLog("KeyboardInput: native sKeyboard::move capture status=%d\n", (int)status);
    return status == MH_OK;
}

bool KeyboardInput_Read(BYTE (&state)[256], bool (&pressed)[256], bool frozen)
{
    // Owned by the serialized render callback, shared by hotkeys and ImGui.
    static bool initialized = false;
    static bool armed[256] = {};
    static DWORD releaseTick[256] = {};
    std::memset(state, 0, sizeof(state));
    std::memset(pressed, 0, sizeof(pressed));

    if (frozen) {
        // Queue state alone goes stale while paused. Use it only for lock toggles.
        BYTE queueState[256] = {};
        GetKeyboardState(queueState);
        auto* device = g_device.load(std::memory_order_acquire);
        if (!device) return false;
        BYTE scans[256] = {};
        HRESULT status = device->GetDeviceState(sizeof(scans), scans);
        if (FAILED(status)) {
            // At most one reacquisition and retry. Never run the native busy loop.
            status = device->Acquire();
            if (SUCCEEDED(status)) status = device->GetDeviceState(sizeof(scans), scans);
        }
        if (FAILED(status)) return false;
        for (unsigned scan = 0; scan < 256; ++scan)
            if ((scans[scan] & 0x80) && g_vkTable[scan]) state[g_vkTable[scan]] = 0x80;
        state[VK_CAPITAL] |= queueState[VK_CAPITAL] & 1;
        state[VK_NUMLOCK] |= queueState[VK_NUMLOCK] & 1;
        state[VK_SCROLL] |= queueState[VK_SCROLL] & 1;
    } else {
        const uint32_t revision = g_revision.load(std::memory_order_acquire);
        if (revision == 0 || (revision & 1)) return false;
        uint32_t held[8];
        for (unsigned i = 0; i < 8; ++i)
            held[i] = g_held[i].load(std::memory_order_relaxed);
        const uint32_t toggles = g_toggles.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_revision.load(std::memory_order_relaxed) != revision) return false;

        for (int key = 0; key < 256; ++key)
            state[key] = (held[key >> 5] & (1u << (key & 31))) ? 0x80 : 0;
        state[VK_CAPITAL] |= toggles & 1;
        state[VK_NUMLOCK] |= (toggles >> 1) & 1;
        state[VK_SCROLL] |= (toggles >> 2) & 1;
    }
    state[VK_SHIFT] = (state[VK_LSHIFT] | state[VK_RSHIFT]) & 0x80;
    state[VK_CONTROL] = (state[VK_LCONTROL] | state[VK_RCONTROL]) & 0x80;
    state[VK_MENU] = (state[VK_LMENU] | state[VK_RMENU]) & 0x80;

    const DWORD now = GetTickCount();
    for (int key = 0; key < 256; ++key) {
        const bool down = (state[key] & 0x80) != 0;
        if (!initialized) {
            armed[key] = !down;
        } else if (down) {
            pressed[key] = armed[key];
            armed[key] = false;
            releaseTick[key] = 0;
        } else if (!armed[key]) {
            if (releaseTick[key] == 0)
                releaseTick[key] = now;
            else if (now - releaseTick[key] >= 30)
                armed[key] = true;
        }
    }
    initialized = true;
    return true;
}
