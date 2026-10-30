// Top-level rollback hook initialization.
#include "../pch.h"

#include "rollback.h"
#include "rollback_effect.h"
#include "rollback_resim.h"
#include "rollback_rng.h"
#include "rollback_shot.h"
#include "gameplay_hooks.h"

#include <atomic>
#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

namespace {
struct GameplayPatch {
    void* target;
    std::vector<uint8_t> original;
};

// Registration finishes before native startup. Disabling happens once, on the
// game thread at the launcher hold, and leaves trampolines alive for stack unwind.
std::vector<LPVOID> g_gameplayTargets;
std::vector<GameplayPatch> g_gameplayPatches;
std::atomic<bool> g_gameplayActive{false};
bool g_gameplayDisabled = false;
bool g_gameplayEnableFailed = false;
}

MH_STATUS GameplayHooks_Create(LPVOID target, LPVOID detour, LPVOID* original)
{
    if (g_gameplayDisabled)
        return MH_ERROR_DISABLED;
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK)
        g_gameplayTargets.push_back(target);
    return status;
}

void GameplayHooks_RecordPatch(void* target, size_t size)
{
    for (const GameplayPatch& patch : g_gameplayPatches)
        if (patch.target == target)
            return;
    const auto* bytes = static_cast<const uint8_t*>(target);
    g_gameplayPatches.push_back({target, {bytes, bytes + size}});
}

MH_STATUS GameplayHooks_QueueEnable(LPVOID target)
{
    if (g_gameplayDisabled)
        return MH_ERROR_DISABLED;
    const MH_STATUS status = MH_QueueEnableHook(target);
    if (status != MH_OK) {
        g_gameplayEnableFailed = true;
        HookLog("GameplayHooks: could not queue enable target=%p status=%d\n", target, status);
    }
    return status;
}

bool GameplayHooks_Apply()
{
    if (g_gameplayDisabled || g_gameplayEnableFailed)
        return false;
    const MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) {
        HookLog("GameplayHooks: could not apply startup detours status=%d\n", status);
        return false;
    }
    HookLog("GameplayHooks: applied queued startup detours\n");
    return true;
}

bool GameplayHooks_Disable()
{
    g_gameplayActive.store(false, std::memory_order_release);
    if (g_gameplayDisabled)
        return true;
    for (LPVOID target : g_gameplayTargets) {
        const MH_STATUS status = MH_QueueDisableHook(target);
        if (status != MH_OK && status != MH_ERROR_DISABLED) {
            HookLog("GameplayHooks: could not queue disable target=%p status=%d\n", target, status);
            return false;
        }
    }
    const MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) {
        HookLog("GameplayHooks: could not disable detours status=%d\n", status);
        return false;
    }
    for (auto it = g_gameplayPatches.rbegin(); it != g_gameplayPatches.rend(); ++it) {
        DWORD protection = 0;
        if (!VirtualProtect(it->target, it->original.size(), PAGE_EXECUTE_READWRITE, &protection)) {
            HookLog("GameplayHooks: could not restore patch target=%p\n", it->target);
            return false;
        }
        std::memcpy(it->target, it->original.data(), it->original.size());
        const bool flushed = FlushInstructionCache(GetCurrentProcess(), it->target, it->original.size());
        DWORD ignored = 0;
        const bool restored = VirtualProtect(it->target, it->original.size(), protection, &ignored);
        if (!flushed || !restored)
            return false;
    }
    g_gameplayDisabled = true;
    HookLog("GameplayHooks: disabled %zu gameplay detours and restored %zu patches for Normal Launch\n",
            g_gameplayTargets.size(), g_gameplayPatches.size());
    return true;
}

bool GameplayHooks_IsActive()
{
    return g_gameplayActive.load(std::memory_order_acquire);
}

void GameplayHooks_Activate()
{
    if (!g_gameplayDisabled)
        g_gameplayActive.store(true, std::memory_order_release);
}

void Rollback_InitHooks()
{
    Rollback_PatchHitSolverAlwaysInline();
    Rollback_PatchBattleUiAllocationFailure();
    Rollback_InitNativeNullGuards();
    Rollback_InitOnlineBattleHook();
    Rollback_InitReSimulationHooks();
    Rollback_InitRngHooks();
    Effect_InitHooks();
    Shot_InitHooks();
}
