#include "../pch.h"

#include "rollback_model_policy.h"
#include "rollback_effect.h"
#include "rollback_memory.h"
#include "mk10utils.h"

#include <algorithm>
#include <cstring>

extern void HookLog(const char* fmt, ...);

namespace {

struct ShotModelPolicy {
    uintptr_t dtiAddress;
    const char* name;
    uint32_t resourceOffsets[2];
    uint32_t resourceCount;
    bool restorePlayback;
    bool retrySetupAfterSpawn;
};

constexpr uintptr_t DTI_USHOT_VULCAN_WEAPON_CART = 0x140D4A1A8;
constexpr uintptr_t DTI_USHOT_VULCAN_LEGION_TIGER = 0x140D4ABB8;
constexpr uintptr_t DTI_USHOT_SHIELD_REPULSOR_BLAST_SP = 0x140D4ADA0;
constexpr uintptr_t FN_CCHRNODE_SET_NEXT = 0x140003D50;
constexpr uintptr_t FN_MTOBJECT_ADD_REF = 0x14050D100;
constexpr uintptr_t FN_UMODEL_PROCESS_ACTION_ANMCHR = 0x140529420;
constexpr uintptr_t FN_UMODEL_MOVE = 0x140566580;

constexpr ShotModelPolicy SHOT_MODEL_POLICIES[] = {
    // King Kobun and Hand Grenade consume model playback in gameplay.
    // Their non-model helper/contact repairs deliberately remain class-specific.
    {0x140D4D2B0, "King Kobun", {0x100, 0x11B0}, 2, true, false},
    {0x140D48BB0, "Fire Grenade", {0x100, 0x11B0}, 2, true, true},
    {0x140D48CD0, "Hand Grenade", {0x100, 0x11B0}, 2, true, true},
    {0x140D49520, "Somersault Grenade", {0x100, 0x11B0}, 2, true, true},

    // Existing model-driven shots using the shared playback overlay or setup
    // readiness check. Pin their primary model and motion resources; Cart's
    // separately-owned model effects still use their dedicated pinning path.
    {0x140D49A60, "Missile Emplacement", {0x100, 0x11B0}, 2, true, false},
    {0x140D4A1A8, "Weapon Cart", {0x100, 0x11B0}, 2, true, false},
    {0x140D4C4C8, "Pendulum Log", {0x100, 0x11B8}, 2, true, false},
    {0x140D4ABB8, "Legion Tiger", {0x100, 0x11B0}, 2, false, true},
};

const ShotModelPolicy* FindShotModelPolicy(uint64_t runtimeDti)
{
    for (const ShotModelPolicy& policy : SHOT_MODEL_POLICIES) {
        if (runtimeDti == (uint64_t)_addr(policy.dtiAddress))
            return &policy;
    }
    return nullptr;
}

bool IsShotModelReady(void* shot)
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

bool RetryShotModelSetup(void* shot, const ShotModelPolicy& policy,
                         uint32_t attempts)
{
    if (IsShotModelReady(shot))
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
    while (!IsShotModelReady(shot) && attempts-- > 0) {
        setup(shot);
        if (!IsShotModelReady(shot))
            Sleep(1);
    }
    InvalidateMemoryQueryCache();
    if (!IsShotModelReady(shot)) {
        HookLog("Rollback: %s model unavailable after setup retry\n",
                policy.name);
        return false;
    }
    return true;
}

void PinUnique(void* resource, std::vector<void*>& pinnedResources)
{
    if (!resource || !HasCallableDtiVirtual(resource) ||
        std::find(pinnedResources.begin(), pinnedResources.end(), resource) !=
            pinnedResources.end()) {
        return;
    }
    ((void(__fastcall*)(void*))_addr(FN_MTOBJECT_ADD_REF))(resource);
    pinnedResources.push_back(resource);
}

}

bool Rollback_ShouldRestoreShotModelPlayback(uint64_t runtimeDti)
{
    const ShotModelPolicy* policy = FindShotModelPolicy(runtimeDti);
    return policy && policy->restorePlayback;
}

void Rollback_PinShotModelResources(
    void* shot, std::vector<void*>& pinnedShotResources,
    std::vector<void*>& pinnedEffectResources)
{
    if (!shot || !HasCallableDtiVirtual(shot))
        return;

    uint64_t runtimeDti = GetObjectDti((uint64_t)shot);
    const ShotModelPolicy* policy = FindShotModelPolicy(runtimeDti);
    for (uint32_t index = 0;
         policy && index < policy->resourceCount; ++index) {
        PinUnique(*(void**)((uint8_t*)shot + policy->resourceOffsets[index]),
                  pinnedShotResources);
    }

    if (runtimeDti != (uint64_t)_addr(DTI_USHOT_VULCAN_WEAPON_CART))
        return;
    for (uint32_t index = 0; index < 13; ++index) {
        void* effect = *(void**)((uint8_t*)shot + 0x2288 + index * 8);
        if (!effect || !IsAccessibleRange((uint64_t)effect + 0x108,
                                           sizeof(void*))) {
            continue;
        }
        PinUnique(*(void**)((uint8_t*)effect + 0x108), pinnedEffectResources);
    }
}

int Rollback_RetireShotModelEffects(
    void* sUnit, const std::vector<void*>& currentShots)
{
    std::vector<void*> retiredModels;
    int retiredCount = 0;
    for (void* shot : currentShots) {
        if (!shot || !HasCallableDtiVirtual(shot) ||
            GetObjectDti((uint64_t)shot) !=
                (uint64_t)_addr(DTI_USHOT_VULCAN_WEAPON_CART)) {
            continue;
        }
        for (uint32_t index = 0; index < 13; ++index) {
            void* effect = *(void**)((uint8_t*)shot + 0x2288 + index * 8);
            if (!effect ||
                std::find(retiredModels.begin(), retiredModels.end(), effect) !=
                    retiredModels.end()) {
                continue;
            }
            if (Effect_RetireGameEffectModel(sUnit, effect)) {
                retiredModels.push_back(effect);
                ++retiredCount;
            }
        }
    }

    if (retiredModels.empty())
        return retiredCount;
    for (void* shot : currentShots) {
        if (!shot || !HasCallableDtiVirtual(shot) ||
            GetObjectDti((uint64_t)shot) !=
                (uint64_t)_addr(DTI_USHOT_VULCAN_WEAPON_CART)) {
            continue;
        }
        for (uint32_t index = 0; index < 13; ++index) {
            void** modelSlot = (void**)((uint8_t*)shot + 0x2288 + index * 8);
            if (std::find(retiredModels.begin(), retiredModels.end(),
                          *modelSlot) != retiredModels.end()) {
                *modelSlot = nullptr;
            }
            void** callbackSlot =
                (void**)((uint8_t*)shot + 0x1F78 + index * 8);
            if (std::find(retiredModels.begin(), retiredModels.end(),
                          *callbackSlot) != retiredModels.end()) {
                *callbackSlot = nullptr;
            }
        }
    }
    return retiredCount;
}

void Rollback_PrepareShotModelAfterSpawn(
    void* shot, uint64_t runtimeDti,
    std::vector<uintptr_t>& waitedForModelDtis)
{
    const ShotModelPolicy* policy = FindShotModelPolicy(runtimeDti);
    if (!policy || !policy->retrySetupAfterSpawn)
        return;

    bool alreadyWaited = std::find(waitedForModelDtis.begin(),
                                   waitedForModelDtis.end(),
                                   policy->dtiAddress) !=
                         waitedForModelDtis.end();
    if (!alreadyWaited)
        waitedForModelDtis.push_back(policy->dtiAddress);
    // Wait only for the first shared model of each DTI. Later factories should
    // resolve the resource from the cache populated by that setup.
    RetryShotModelSetup(shot, *policy, alreadyWaited ? 1 : 500);
}

void Rollback_RestoreShotModelLinks(void* shot, void* ownerChr,
                                    uint64_t runtimeDti)
{
    if (!shot || !ownerChr ||
        runtimeDti != (uint64_t)_addr(DTI_USHOT_VULCAN_WEAPON_CART) ||
        !IsAccessibleRange((uint64_t)shot + 0x2288, sizeof(void*)) ||
        !IsAccessibleRange((uint64_t)ownerChr + 0x6498, sizeof(void*))) {
        return;
    }
    void* modelEffect = *(void**)((uint8_t*)shot + 0x2288);
    if (!modelEffect || !HasCallableDtiVirtual(modelEffect))
        return;

    // Debris attachment resolves joints through owner+0x6498; the saved node
    // still names the retired primary model.
    ((void(__fastcall*)(void*, void*))_addr(FN_CCHRNODE_SET_NEXT))(
        (uint8_t*)ownerChr + 0x6490, modelEffect);
}

void Rollback_EvaluateRestoredShotModel(void* shot, uint64_t runtimeDti,
                                        const uint8_t* savedBody,
                                        size_t savedBodySize)
{
    const bool legionTiger = runtimeDti ==
        (uint64_t)_addr(DTI_USHOT_VULCAN_LEGION_TIGER);
    const bool repulsorBlast = runtimeDti ==
        (uint64_t)_addr(DTI_USHOT_SHIELD_REPULSOR_BLAST_SP);
    if (!shot || (!legionTiger && !repulsorBlast))
        return;

    uint32_t animationNo = *(uint32_t*)((uint8_t*)shot + 0x1310);
    if (repulsorBlast && savedBody && savedBodySize >= 0x56A) {
        uint16_t savedMotionNo = 0;
        std::memcpy(&savedMotionNo, savedBody + 0x568,
                    sizeof(savedMotionNo));
        animationNo = savedMotionNo;
    }
    float animationTime = *(float*)((uint8_t*)shot + 0x1328);
    if (repulsorBlast || animationTime > 0.0f) {
        ((uint64_t(__fastcall*)(void*, uint32_t, uint32_t, float, float,
                               float, uint32_t))
            _addr(FN_UMODEL_PROCESS_ACTION_ANMCHR))(
                shot, 0, animationNo, 0.0f,
                std::max(0.0f, animationTime - 1.0f), 1.0f,
                legionTiger ? 0x40 : 0);
    }
    // Reconstructed shots missed the model-category pass for this tick.
    ((void(__fastcall*)(void*))_addr(FN_UMODEL_MOVE))(shot);
}
