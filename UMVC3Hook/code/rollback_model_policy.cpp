#include "../pch.h"

#include "rollback_model_policy.h"
#include "rollback_memory.h"
#include "mk10utils.h"

extern void HookLog(const char* fmt, ...);

namespace {

constexpr ShotModelPolicy SHOT_MODEL_POLICIES[] = {
    // King Kobun and Hand Grenade both consume model playback in gameplay.
    // Their non-model helper/contact repairs deliberately remain class-specific.
    {0x140D4D2B0, "King Kobun", {0x100, 0x11B0}, 2, true, false},
    {0x140D48CD0, "Hand Grenade", {0x100, 0x11B0}, 2, true, true},

    // Existing model-driven shots using the shared playback overlay or setup
    // readiness check. Pin their primary model and motion resources; Cart's
    // separately-owned model effects still use their dedicated pinning path.
    {0x140D49A60, "Missile Emplacement", {0x100, 0x11B0}, 2, true, false},
    {0x140D4A1A8, "Weapon Cart", {0x100, 0x11B0}, 2, true, false},
    {0x140D4C4C8, "Pendulum Log", {0x100, 0x11B8}, 2, true, false},
    {0x140D4ABB8, "Legion Tiger", {0x100, 0x11B0}, 2, false, true},
};

}

const ShotModelPolicy* Rollback_FindShotModelPolicy(uint64_t runtimeDti)
{
    for (const ShotModelPolicy& policy : SHOT_MODEL_POLICIES) {
        if (runtimeDti == (uint64_t)_addr(policy.dtiAddress))
            return &policy;
    }
    return nullptr;
}

bool Rollback_IsShotModelReady(void* shot)
{
    if (!shot || !IsAccessibleRange((uint64_t)shot + 0x538,
                                     sizeof(uint64_t))) {
        return false;
    }
    uint32_t count = *(uint32_t*)((uint8_t*)shot + 0x530);
    uint64_t joints = *(uint64_t*)((uint8_t*)shot + 0x538);
    return count > 0 && count <= 256 && joints &&
           IsAccessibleRange(joints, (size_t)count * 0xC0);
}

bool Rollback_RetryShotModelSetup(void* shot,
                                  const ShotModelPolicy& policy,
                                  uint32_t attempts)
{
    if (Rollback_IsShotModelReady(shot))
        return true;
    if (!shot || !IsAccessibleRange((uint64_t)shot, sizeof(uint64_t)))
        return false;

    uint64_t vtable = *(uint64_t*)shot;
    uint64_t setupAddress = vtable &&
        IsAccessibleRange(vtable + 0x28, sizeof(uint64_t))
        ? *(uint64_t*)(vtable + 0x28) : 0;
    if (!setupAddress || !IsExecutableAddress(setupAddress))
        return false;

    auto setup = (void(__fastcall*)(void*))setupAddress;
    while (!Rollback_IsShotModelReady(shot) && attempts-- > 0) {
        setup(shot);
        if (!Rollback_IsShotModelReady(shot))
            Sleep(1);
    }
    InvalidateMemoryQueryCache();
    if (!Rollback_IsShotModelReady(shot)) {
        HookLog("Rollback: %s model unavailable after setup retry\n",
                policy.name);
        return false;
    }
    return true;
}
