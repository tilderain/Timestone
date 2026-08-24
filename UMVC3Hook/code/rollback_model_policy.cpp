#include "../pch.h"

#include "rollback_model_policy.h"
#include "rollback_effect.h"
#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_profile.h"
#include "rollback_shot.h"
#include "rollback_snapshot.h"
#include "concrete_snapshot.h"
#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

extern void HookLog(const char* fmt, ...);
extern bool g_multiResimEnabled;

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
constexpr uintptr_t DTI_USHOT_VULCAN_DOG_MISSILE = 0x140D4B888;
constexpr uintptr_t DTI_USHOT_VULCAN_ITEM_THROW = 0x140D4B980;
constexpr uint16_t DOG_MISSILE_MOTION_NO = 0x00CB;
constexpr uintptr_t DTI_USHOT_LASER = 0x140D482E8;
constexpr uintptr_t DTI_USHOT_OPTION_SHIELD_SLASH = 0x140D48A68;
constexpr uintptr_t DTI_USHOT_OPTION_SPADA_BOOMERANG = 0x140D4D118;
constexpr uintptr_t DTI_USHOT_VULCAN_CROSS_SWORD = 0x140D488B8;
constexpr uintptr_t DTI_USHOT_VULCAN_IRON_PIPE_ROLLING = 0x140D4A4E8;
constexpr uintptr_t DTI_USHOT_VULCAN_LAND_MINE = 0x140D48D98;
constexpr uintptr_t DTI_USHOT_SHIELD_KIKOUSYOU = 0x140D48F70;
constexpr uintptr_t DTI_USHOT_VULCAN_MISSILE_EMPLACEMENT = 0x140D49A60;
constexpr uintptr_t DTI_USHOT_VULCAN_PRICKLE_BOARD = 0x140D4C528;
constexpr uintptr_t DTI_USHOT_SHIELD_WEAPON_CART_SPREAD = 0x140D4A098;
constexpr uintptr_t DTI_USHOT_VULCAN_EXATK_ROCKET_LAUNCHER = 0x140D4BA48;
constexpr uintptr_t DTI_USHOT_VULCAN_ROCKET_LAUNCHER_EXPLODE = 0x140D4BAC8;
constexpr uintptr_t DTI_USHOT_SHIELD_RAID_SWORD = 0x140D4D470;
constexpr uintptr_t DTI_USHOT_SHIELD_SPIRAL_SWORD = 0x140D4D4C8;
constexpr uintptr_t DTI_USHOT_SHIELD_STORM_SWORD = 0x140D4D508;
constexpr uintptr_t DTI_USHOT_VULCAN_STORM_SWORD_SPREAD = 0x140D4D5E8;
constexpr uintptr_t DTI_USHOT_VULCAN_KING_KOBUN = 0x140D4D2B0;
constexpr uintptr_t DTI_UKOBUN = 0x140D455B0;
constexpr uintptr_t VTABLE_UARTHUR = 0x140A70CD0;
constexpr uintptr_t VTABLE_UCAP_AMERICA = 0x140A71430;
constexpr uintptr_t VTABLE_UDOCTOR_DOOM = 0x140A73A40;
constexpr uintptr_t VTABLE_URROCKET_RACCOON = 0x140A7DF30;
constexpr uintptr_t VTABLE_UTRISH = 0x140A81A20;
constexpr uintptr_t PTR_SUNIT = 0x140E17698;
constexpr uintptr_t PTR_SCHARACTER = 0x140D44A70;
constexpr uintptr_t PTR_SREPLAY = 0x140D4D9D8;
constexpr uintptr_t PTR_SSHOT = 0x140D47F40;
constexpr uintptr_t PTR_SGAMECONFIG = 0x140D50F88;
constexpr uintptr_t FN_MTDTI_IS_KIND_OF = 0x1400039D0;
constexpr uintptr_t FN_KING_KOBUN_MOVE = 0x1401BA2D0;
constexpr uintptr_t FN_MISSILE_EMPLACEMENT_MOVE = 0x14016F220;
constexpr uintptr_t FN_WEAPON_CART_MOVE = 0x14017A9D0;
constexpr uintptr_t FN_WEAPON_CART_SPAWN_DEBRIS = 0x14017A200;
constexpr uintptr_t FN_SSHOT_FLUSH_PENDING_UNITS = 0x1401517A0;
constexpr uintptr_t FN_MTOBJECT_ADD_REF = 0x14050D100;
constexpr uintptr_t FN_UMODEL_PROCESS_ACTION_ANMCHR = 0x140529420;
constexpr uintptr_t FN_UMODEL_MOVE = 0x140566580;
constexpr uintptr_t FN_UMODEL_UPDATE_MOTION_EVALUATION = 0x140531A00;
constexpr uintptr_t FN_UGAMEEFFECT_RESET_EFFECT_STATE = 0x14017BD90;
constexpr uintptr_t FN_CRESOURCE_ASSIGN_SLOT = 0x140529730;
constexpr uintptr_t FN_CRESOURCE_RELEASE_REF = 0x14050D5A0;
constexpr uintptr_t FN_SRESOURCE_GET_INSTANCE = 0x140001B10;
constexpr uintptr_t FN_SRESOURCE_LOAD_SYNC = 0x140518BF0;
constexpr uintptr_t FN_CACTOR_GAME_MODEL_ANM_BIND_COMMAND = 0x140013680;
constexpr uintptr_t FN_CACTOR_GAME_MODEL_ANM_ADVANCE_COMMAND = 0x140013040;
constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
constexpr uint32_t SUNIT_CHARACTER_CATEGORY = 8;
constexpr uintptr_t TEAM0_OFFSET = 0x40;
constexpr uintptr_t TEAM1_OFFSET = 0x300;
constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;
constexpr uint32_t UCAP_AMERICA_SHIELD_SLASH_OFFSET = 0x68F0;
constexpr uint32_t UARTHUR_CROSS_SWORD_FLAG_OFFSET = 0x68F9;
constexpr uint32_t UDOCTOR_DOOM_MISSILE_OFFSET = 0x68F0;
constexpr uint32_t UTRISH_SPADA_BOOMERANG_OFFSET = 0x68F0;
constexpr size_t SSHOT_ID_POOL_OFFSET = 0x38;
constexpr size_t SSHOT_ID_POOL_SIZE = 0x404;
constexpr size_t SSHOT_PENDING_COUNT_OFFSET = 0x2448;
constexpr size_t SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET = 0x4A89C;
constexpr size_t SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE = 10;

using ShotMoveFn = void(__fastcall*)(void*);
ShotMoveFn oKingKobunMove = nullptr;
ShotMoveFn oMissileEmplacementMove = nullptr;
ShotMoveFn oWeaponCartMove = nullptr;
std::unordered_set<void*> g_resimSpawnedPendulums;
std::unordered_map<void*, bool> g_pendulumNativeMotionReady;
int g_kingKobunTraceCount = 0;

struct DeferredWeaponCartDebris {
    bool pending = false;
    uint8_t preGlobalRng[16] = {};
    uint8_t preEffectRng[16] = {};
    uint8_t preOwnerRng[16] = {};
    uint8_t postGlobalRng[16] = {};
    uint8_t postEffectRng[16] = {};
    uint8_t postOwnerRng[16] = {};
    uint8_t preShotIdPool[SSHOT_ID_POOL_SIZE] = {};
    uint8_t postShotIdPool[SSHOT_ID_POOL_SIZE] = {};
    uint8_t preShotSlotFlags[SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE] = {};
    uint8_t postShotSlotFlags[SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE] = {};
};

std::unordered_map<void*, DeferredWeaponCartDebris> g_deferredWeaponCartDebris;

constexpr ShotModelPolicy SHOT_MODEL_POLICIES[] = {
    // King Kobun and Hand Grenade consume model playback in gameplay.
    // Their non-model helper/contact repairs deliberately remain class-specific.
    {0x140D4D2B0, "King Kobun", {0x100, 0x11B0}, 2, true, true},
    {0x140D48BB0, "Fire Grenade", {0x100, 0x11B0}, 2, true, true},
    {0x140D48CD0, "Hand Grenade", {0x100, 0x11B0}, 2, true, true},
    {0x140D4A4E8, "Iron Pipe", {0x100, 0x11B0}, 2, true, true},
    {0x140D49520, "Somersault Grenade", {0x100, 0x11B0}, 2, true, true},
    {0x140D4B888, "Dog Missile", {0x100, 0x11B0}, 2, true, true},
    {DTI_USHOT_VULCAN_PRICKLE_BOARD, "Bear Trap",
     {0x100, 0x11B0}, 2, true, true},
    {DTI_USHOT_VULCAN_ITEM_THROW, "Evidence Item Throw",
     {0x100, 0x11B0}, 2, true, true},

    // Existing model-driven shots using the shared playback overlay or setup
    // readiness check. Pin their primary model and motion resources; Cart's
    // separately-owned model effects still use their dedicated pinning path.
    {0x140D49A60, "Missile Emplacement", {0x100, 0x11B0}, 2, true, false},
    {0x140D4A1A8, "Weapon Cart", {0x100, 0x11B0}, 2, true, false},
    {0x140D4C4C8, "Pendulum Log", {0x100, 0x11B8}, 2, true, false},
    {0x140D4D2F0, "Servbot Launcher", {0x100, 0x11B0}, 2, true, false},
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
    if (!shot) return false;
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
    if (!shot) return false;

    uint64_t vtable = *(uint64_t*)shot;
    uint64_t setupAddress = *(uint64_t*)(vtable + 0x28);
    if (!setupAddress) return false;

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
    // sResource::addRef deliberately DebugBreaks when the current count is not
    // positive. Shot fields can retain a retired resource after native teardown;
    // RTTI callability is incidental, while this is addRef's actual contract.
    if (!resource || *(int32_t*)((uint8_t*)resource + 0x4C) <= 0 ||
        std::find(pinnedResources.begin(), pinnedResources.end(), resource) !=
            pinnedResources.end()) {
        return;
    }
    ((void(__fastcall*)(void*))_addr(FN_MTOBJECT_ADD_REF))(resource);
    pinnedResources.push_back(resource);
}

bool IsActiveUnit(void* unit)
{
    if (!unit) return false;
    uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
    return (state & 7u) < 3u && (state & 0x3F8u) != 0x3F8u;
}

void RestoreKingKobunHelper(void* shot, const RollbackShotRecipe& saved)
{
    constexpr size_t HELPER_OFFSET = 0x2158;
    if (!shot || saved.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_KING_KOBUN) ||
        saved.concrete.body.size() < HELPER_OFFSET + sizeof(uint64_t)) {
        return;
    }

    void* helper = nullptr;
    void* savedHelper = *(void**)(saved.concrete.body.data() + HELPER_OFFSET);
    if (savedHelper && HasCallableDtiVirtual(savedHelper) &&
        GetObjectDti((uint64_t)savedHelper) == (uint64_t)_addr(DTI_UKOBUN) &&
        IsActiveUnit(savedHelper)) {
        helper = savedHelper;
    }
    if (!helper) {
        uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
        if (sUnit && *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) >
                         SUNIT_CHARACTER_CATEGORY) {
            uint64_t node = *(uint64_t*)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
            for (int safety = 64; node && safety-- > 0;
                 node = *(uint64_t*)(node + 0x18)) {
                if (GetObjectDti(node) == (uint64_t)_addr(DTI_UKOBUN) &&
                    IsActiveUnit((void*)node)) {
                    if (helper) return;
                    helper = (void*)node;
                }
            }
        }
    }
    *(void**)((uint8_t*)shot + HELPER_OFFSET) = helper;
}

void RepairResimRepulsorBlastMotion(void* shot)
{
    if (!g_isResimulating || !shot ||
        GetObjectDti((uint64_t)shot) !=
            (uint64_t)_addr(DTI_USHOT_SHIELD_REPULSOR_BLAST_SP)) {
        return;
    }
    uint32_t jointCount = *(uint32_t*)((uint8_t*)shot + 0x530);
    uint64_t joints = *(uint64_t*)((uint8_t*)shot + 0x538);
    uint32_t motionStatus = *(uint32_t*)((uint8_t*)shot + 0x550);
    if (motionStatus || jointCount == 0 || jointCount > 256 || !joints ||
        !IsAccessibleRange(joints, (size_t)jointCount * 0xC0)) {
        return;
    }
    ((uint64_t(__fastcall*)(void*, uint32_t, uint32_t, float, float, float,
                           uint32_t))
        _addr(FN_UMODEL_PROCESS_ACTION_ANMCHR))(
            shot, 0, 0, 0.0f, 0.0f, 1.0f, 0);
}

void RepairResimDogMissileMotion(void* shot)
{
    if (!g_isResimulating || !shot ||
        GetObjectDti((uint64_t)shot) !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_DOG_MISSILE)) {
        return;
    }

    // Dog setup only writes actor request 0/1. The normal model-category pass
    // maps it to NHweapon_l0 entry 0xCB, but a resim-spawned shot misses that
    // pass and otherwise remains at MotionNo 0xFFFF with no resource bindings.
    ((uint64_t(__fastcall*)(void*, uint32_t, uint32_t, float, float, float,
                           uint32_t))
        _addr(FN_UMODEL_PROCESS_ACTION_ANMCHR))(
            shot, 0, DOG_MISSILE_MOTION_NO, 0.0f, 0.0f, 1.0f, 0x40);
    if (*(uint16_t*)((uint8_t*)shot + 0x568) == DOG_MISSILE_MOTION_NO)
        ((void(__fastcall*)(void*))_addr(FN_UMODEL_MOVE))(shot);
}

bool PrimaryMotionBindingsReady(void* shot);

void LoadPrickleBoardMotionSynchronously(void* shot)
{
    if (!shot) return;
    void* pending = *(void**)((uint8_t*)shot + 0x11B0);
    if (!pending || !HasCallableDtiVirtual(pending) ||
        !IsAccessibleRange((uint64_t)pending, 0x78)) {
        return;
    }
    uint64_t dti = GetObjectDti((uint64_t)pending);
    uint64_t key = *(uint64_t*)((uint8_t*)pending + 0x60);
    char* path = (char*)pending + 0x0C;
    void* sResource = ((void*(__fastcall*)())_addr(
        FN_SRESOURCE_GET_INSTANCE))();
    if (!dti || !sResource) return;
    void* loaded = ((void*(__fastcall*)(void*, void*, char*, uint64_t,
                                       uint32_t))
        _addr(FN_SRESOURCE_LOAD_SYNC))(
            sResource, (void*)dti, path, key, 1);
    if (!loaded) return;
    ((void(__fastcall*)(void*, void*, uint32_t))_addr(
        FN_CRESOURCE_ASSIGN_SLOT))(shot, loaded, 0);
    ((void(__fastcall*)(void*))_addr(FN_CRESOURCE_RELEASE_REF))(loaded);
    InvalidateMemoryQueryCache();
}

void RepairPrickleBoardMotionBindings(
    void* shot, const RollbackShotRecipe* recipe)
{
    if (!shot ||
        GetObjectDti((uint64_t)shot) !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_PRICKLE_BOARD)) {
        return;
    }
    if ((!g_isResimulating && !(recipe && recipe->reconstructed)) ||
        !IsShotModelReady(shot)) return;

    if (PrimaryMotionBindingsReady(shot)) return;

    uint16_t motionNo = *(uint16_t*)((uint8_t*)shot + 0x568);
    if (motionNo == UINT16_MAX) motionNo = 0;
    float motionFrame = *(float*)((uint8_t*)shot + 0x5AC);
    uint32_t motionFlags = *(uint32_t*)((uint8_t*)shot + 0x570);
    uint8_t savedMotion[0x180];
    std::memcpy(savedMotion, (uint8_t*)shot + 0x560,
                sizeof(savedMotion));
    ((uint64_t(__fastcall*)(void*, uint32_t, uint32_t, float, float, float,
                           uint32_t))
        _addr(FN_UMODEL_PROCESS_ACTION_ANMCHR))(
            shot, 0, motionNo, 0.0f, motionFrame, 1.0f, motionFlags);

    // processActionAnmchr rebuilds the resource-owned bindings but also resets
    // playback. Keep the exact saved cursor and completion edge while retaining
    // the newly bound pointer slots. The native shot tick remains responsible
    // for advancing and evaluating the model pose.
    static constexpr size_t scalarRanges[][2] = {
        {0x08, 0xD8}, {0xE8, 0x18}, {0x108, 0x18},
        {0x140, 0x20}, {0x168, 0x08},
    };
    for (const auto& range : scalarRanges) {
        std::memcpy((uint8_t*)shot + 0x560 + range[0],
                    savedMotion + range[0], range[1]);
    }
}

void RestoreReconstructedPrickleBoardCommandState(
    void* shot, const RollbackShotRecipe& saved)
{
    if (!shot || saved.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_PRICKLE_BOARD) ||
        GetObjectDti((uint64_t)shot) != saved.concrete.dti) {
        return;
    }

    uint16_t savedMotionNo = *(uint16_t*)((uint8_t*)shot + 0x568);
    if (savedMotionNo != 0) return;

    LoadPrickleBoardMotionSynchronously(shot);
    uint8_t savedMotion[0x180];
    uint8_t savedRootPose[0x70];
    std::memcpy(savedMotion, (uint8_t*)shot + 0x560,
                sizeof(savedMotion));
    std::memcpy(savedRootPose, (uint8_t*)shot + 0x1B0,
                sizeof(savedRootPose));

    int32_t animationNo = *(int32_t*)((uint8_t*)shot + 0x1310);
    if (animationNo < 0)
        animationNo = *(int32_t*)((uint8_t*)shot + 0x1318);
    if (animationNo < 0) return;

    uint8_t bound = ((uint8_t(__fastcall*)(void*, int32_t, uint32_t, uint8_t))
        _addr(FN_CACTOR_GAME_MODEL_ANM_BIND_COMMAND))(
            (uint8_t*)shot + 0x1348, animationNo, 0, 1);
    if (!bound) return;
    ((void(__fastcall*)(void*))_addr(
        FN_CACTOR_GAME_MODEL_ANM_ADVANCE_COMMAND))(
            (uint8_t*)shot + 0x1348);

    // Binding rebuilds resource-owned pointers but can also restart playback.
    // If command catch-up selected the same motion as the snapshot, retain its
    // exact cursor and pose while keeping the newly derived command/bindings.
    if (*(uint16_t*)((uint8_t*)shot + 0x568) == savedMotionNo) {
        static constexpr size_t scalarRanges[][2] = {
            {0x08, 0xD8}, {0xE8, 0x18}, {0x108, 0x18},
            {0x140, 0x20}, {0x168, 0x08},
        };
        for (const auto& range : scalarRanges) {
            std::memcpy((uint8_t*)shot + 0x560 + range[0],
                        savedMotion + range[0], range[1]);
        }
        std::memcpy((uint8_t*)shot + 0x1B0, savedRootPose,
                    sizeof(savedRootPose));
    }
}

bool PrimaryMotionBindingsReady(void* shot)
{
    if (!shot) return false;
    return *(uint32_t*)((uint8_t*)shot + 0x530) > 0 &&
        *(uint64_t*)((uint8_t*)shot + 0x538) != 0 &&
        *(uint32_t*)((uint8_t*)shot + 0x550) != 0 &&
        *(uint64_t*)((uint8_t*)shot + 0x640) != 0 &&
        *(uint64_t*)((uint8_t*)shot + 0x660) != 0 &&
        *(uint64_t*)((uint8_t*)shot + 0x680) != 0 &&
        *(uint64_t*)((uint8_t*)shot + 0x688) != 0 &&
        *(uint64_t*)((uint8_t*)shot + 0x690) != 0 &&
        *(uint64_t*)((uint8_t*)shot + 0x698) != 0;
}

bool PendulumNativeMotionReady(void* shot)
{
    if (!shot) return false;
    void* pending = *(void**)((uint8_t*)shot + 0x11B8);
    if (!pending || !HasCallableDtiVirtual(pending) ||
        !IsAccessibleRange((uint64_t)pending, 0x78)) {
        return false;
    }
    uint64_t table = *(uint64_t*)((uint8_t*)pending + 0x70);
    return table && IsAccessibleRange(table, 0x10) &&
        *(uint16_t*)(table + 0x06) != 0 && *(uint64_t*)(table + 0x08) != 0;
}

void ProcessPendulumMotionIfReady(void* shot)
{
    float currentFrame = *(float*)((uint8_t*)shot + 0x1F58);
    float previousFrame = std::max(currentFrame - 1.0f, 0.0f);
    float sampleFrame = std::max(currentFrame - 4.0f, 0.0f);
    ((uint64_t(__fastcall*)(void*, uint32_t, uint32_t, float, float, float,
                           uint32_t))
        _addr(FN_UMODEL_PROCESS_ACTION_ANMCHR))(
            shot, 0, 0, 0.0f, sampleFrame, 1.0f, 0);
    float poseFrame = currentFrame <= 3.0f ? currentFrame : previousFrame;
    *(float*)((uint8_t*)shot + 0x5AC) = poseFrame;
    *(float*)((uint8_t*)shot + 0x5B0) = std::max(poseFrame - 1.0f, 0.0f);
}

void EvaluatePendulumPose(void* shot)
{
    if (*(float*)((uint8_t*)shot + 0x1F58) <= 3.0f) return;
    ((void(__fastcall*)(void*))_addr(
        FN_UMODEL_UPDATE_MOTION_EVALUATION))(shot);
    uint64_t vtable = *(uint64_t*)shot;
    uint64_t updateWorld = *(uint64_t*)(vtable + 0x90);
    uint64_t updateChildren = *(uint64_t*)(vtable + 0x98);
    if (updateWorld)
        ((void(__fastcall*)(void*))updateWorld)(shot);
    if (updateChildren)
        ((void(__fastcall*)(void*))updateChildren)(shot);
    float currentFrame = *(float*)((uint8_t*)shot + 0x1F58);
    *(float*)((uint8_t*)shot + 0x5AC) = currentFrame;
    *(float*)((uint8_t*)shot + 0x5B0) = std::max(currentFrame - 1.0f, 0.0f);
}

void LoadPendulumMotionSynchronously(void* shot)
{
    if (!shot) return;
    void* pending = *(void**)((uint8_t*)shot + 0x11B8);
    if (!pending || !HasCallableDtiVirtual(pending) ||
        !IsAccessibleRange((uint64_t)pending, 0x78)) {
        return;
    }
    if (PendulumNativeMotionReady(shot)) return;
    uint64_t dti = GetObjectDti((uint64_t)pending);
    uint64_t key = *(uint64_t*)((uint8_t*)pending + 0x60);
    char* path = (char*)pending + 0x0C;
    void* sResource = ((void*(__fastcall*)())_addr(
        FN_SRESOURCE_GET_INSTANCE))();
    if (!dti || !sResource) return;
    void* loaded = ((void*(__fastcall*)(void*, void*, char*, uint64_t,
                                       uint32_t))
        _addr(FN_SRESOURCE_LOAD_SYNC))(
            sResource, (void*)dti, path, key, 1);
    if (!loaded) return;
    ((void(__fastcall*)(void*, void*, uint32_t))_addr(
        FN_CRESOURCE_ASSIGN_SLOT))(shot, loaded, 0);
    ((void(__fastcall*)(void*))_addr(FN_CRESOURCE_RELEASE_REF))(loaded);
    InvalidateMemoryQueryCache();
}

void RepairResimPendulumModel(
    void* shot, const RollbackShotRecipe* recipe)
{
    bool reconstructed = recipe && recipe->reconstructed;
    bool resimSpawned = g_resimSpawnedPendulums.find(shot) !=
        g_resimSpawnedPendulums.end();
    uint64_t observedDti = shot ? GetObjectDti((uint64_t)shot) : 0;
    if ((!g_isResimulating && !reconstructed && !resimSpawned) || !shot ||
        observedDti != (uint64_t)_addr(0x140D4C4C8)) {
        return;
    }
    ProcessPendulumMotionIfReady(shot);
    if (PrimaryMotionBindingsReady(shot)) {
        EvaluatePendulumPose(shot);
        return;
    }
    LoadPendulumMotionSynchronously(shot);
    ProcessPendulumMotionIfReady(shot);
    if (PrimaryMotionBindingsReady(shot)) {
        EvaluatePendulumPose(shot);
        return;
    }
    uint64_t vtable = *(uint64_t*)shot;
    uint64_t setupAddress = *(uint64_t*)(vtable + 0x28);
    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
    if (!setupAddress || !sUnit) return;
    void* oldCallbacks[2] = {
        *(void**)((uint8_t*)shot + 0x1F78),
        *(void**)((uint8_t*)shot + 0x1F80),
    };
    ((void(__fastcall*)(void*))setupAddress)(shot);
    InvalidateMemoryQueryCache();
    ProcessPendulumMotionIfReady(shot);
    if (PrimaryMotionBindingsReady(shot)) EvaluatePendulumPose(shot);
    for (uint32_t index = 0; index < 2; ++index) {
        void* replacement = *(void**)((uint8_t*)shot + 0x1F78 + index * 8);
        if (oldCallbacks[index] && replacement != oldCallbacks[index])
            Effect_RetireOne((void*)sUnit, oldCallbacks[index]);
    }
}

void __fastcall hkKingKobunMove(void* shot)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::KingKobunMove);
    bool trace = shot && GetObjectDti((uint64_t)shot) ==
        (uint64_t)_addr(DTI_USHOT_VULCAN_KING_KOBUN) &&
        g_kingKobunTraceCount < 128;

    uint32_t beforeMode = 0;
    uint32_t beforePhase = 0;
    float beforePos = 0.0f;
    if (trace) {
        beforeMode = *(uint32_t*)((uint8_t*)shot + 0x214C);
        beforePhase = *(uint32_t*)((uint8_t*)shot + 0x2140);
        beforePos = *(float*)((uint8_t*)shot + 0x50);
    }
    oKingKobunMove(shot);
    if (!trace) return;
    uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
    int frame = replay ? *(int32_t*)(replay + 0x54) : -1;
    HookLog("KINGMOVE shot=0x%llX frame=%d resim=%d mode=%u->%u phase=%u->%u "
            "x=%.6g->%.6g speed=%.6g dt=%.6g facing=%u helper=0x%llX state=0x%08X\n",
            (uint64_t)shot, frame, g_multiResimEnabled ? 1 : 0,
            beforeMode, *(uint32_t*)((uint8_t*)shot + 0x214C),
            beforePhase, *(uint32_t*)((uint8_t*)shot + 0x2140),
            beforePos, *(float*)((uint8_t*)shot + 0x50),
            *(float*)((uint8_t*)shot + 0x2150),
            *(float*)((uint8_t*)shot + 0x28),
            *(uint8_t*)((uint8_t*)shot + 0x2154),
            *(uint64_t*)((uint8_t*)shot + 0x2158),
            *(uint32_t*)((uint8_t*)shot + 0x10));
    ++g_kingKobunTraceCount;
}

bool IsReconstructedShot(void* shot, uintptr_t dtiAddress,
                         RollbackShotRecipe* recipe = nullptr)
{
    RollbackShotRecipe found;
    if (!Shot_GetRecipe(shot, found) || !found.reconstructed ||
        GetObjectDti((uint64_t)shot) != (uint64_t)_addr(dtiAddress)) {
        return false;
    }
    if (recipe) *recipe = found;
    return true;
}

void RetryMissileEmplacementSetup(void* shot)
{
    constexpr size_t TRANSFORM_STATE_OFFSET = 0x50;
    constexpr size_t CONCRETE_STATE_OFFSET = 0x2120;
    constexpr size_t MOTION_HEADER_OFFSET = 0x550;
    constexpr size_t MOTION_BASE = 0x560;
    constexpr size_t MOTION_STRIDE = 0x180;
    constexpr size_t MOTION_COUNT = 8;
    struct MotionPlaybackState {
        uint8_t state08[0x118];
        uint8_t state140[0x20];
        uint8_t state168[0x08];
    };
    uint8_t transformState[0xB0];
    uint8_t concreteState[0x50];
    uint8_t motionHeader[0x10];
    MotionPlaybackState motionState[MOTION_COUNT];
    std::memcpy(transformState, (uint8_t*)shot + TRANSFORM_STATE_OFFSET,
                sizeof(transformState));
    std::memcpy(concreteState, (uint8_t*)shot + CONCRETE_STATE_OFFSET,
                sizeof(concreteState));
    std::memcpy(motionHeader, (uint8_t*)shot + MOTION_HEADER_OFFSET,
                sizeof(motionHeader));
    for (size_t motion = 0; motion < MOTION_COUNT; ++motion) {
        uint8_t* source = (uint8_t*)shot + MOTION_BASE + motion * MOTION_STRIDE;
        std::memcpy(motionState[motion].state08, source + 0x08,
                    sizeof(motionState[motion].state08));
        std::memcpy(motionState[motion].state140, source + 0x140,
                    sizeof(motionState[motion].state140));
        std::memcpy(motionState[motion].state168, source + 0x168,
                    sizeof(motionState[motion].state168));
    }
    uint64_t vtable = *(uint64_t*)shot;
    uint64_t setup = *(uint64_t*)(vtable + 0x28);
    if (!setup) return;
    ((void(__fastcall*)(void*))setup)(shot);
    InvalidateMemoryQueryCache();
    // Setup mirrors Z and replaces orientation from Doom's side flag. The
    // snapshot already contains that transform, so a readiness retry must not
    // apply the side conversion a second time.
    std::memcpy((uint8_t*)shot + TRANSFORM_STATE_OFFSET, transformState,
                sizeof(transformState));
    std::memcpy((uint8_t*)shot + CONCRETE_STATE_OFFSET, concreteState,
                sizeof(concreteState));
    std::memcpy((uint8_t*)shot + MOTION_HEADER_OFFSET, motionHeader,
                sizeof(motionHeader));
    for (size_t motion = 0; motion < MOTION_COUNT; ++motion) {
        uint8_t* destination =
            (uint8_t*)shot + MOTION_BASE + motion * MOTION_STRIDE;
        std::memcpy(destination + 0x08, motionState[motion].state08,
                    sizeof(motionState[motion].state08));
        std::memcpy(destination + 0x140, motionState[motion].state140,
                    sizeof(motionState[motion].state140));
        std::memcpy(destination + 0x168, motionState[motion].state168,
                    sizeof(motionState[motion].state168));
    }
}

bool MissileEmplacementJointsReady(void* shot)
{
    uint32_t jointCount = *(uint32_t*)((uint8_t*)shot + 0x530);
    uint64_t joints = *(uint64_t*)((uint8_t*)shot + 0x538);
    bool ready = jointCount > 0 && jointCount <= 256 && joints &&
        IsAccessibleRange(joints, 0xC0 * jointCount);
    for (uint32_t index = 0; ready && index < jointCount; ++index)
        ready = *(uint64_t*)(joints + index * 0xC0) != 0;
    return ready;
}

void __fastcall hkMissileEmplacementMove(void* shot)
{
    if (IsReconstructedShot(shot, DTI_USHOT_VULCAN_MISSILE_EMPLACEMENT) &&
        !MissileEmplacementJointsReady(shot)) {
        RetryMissileEmplacementSetup(shot);
        if (!MissileEmplacementJointsReady(shot)) return;
    }
    oMissileEmplacementMove(shot);
}

void CaptureWeaponCartTransitionState(
    DeferredWeaponCartDebris& state, void* owner, bool post)
{
    uint8_t* globalRng = (uint8_t*)_addr(0x140D765D8);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    uint8_t* effectRng = sEffect ? (uint8_t*)sEffect + 0x3C : nullptr;
    void* sShot = *(void**)_addr(PTR_SSHOT);
    void* sGameConfig = *(void**)_addr(PTR_SGAMECONFIG);
    std::memcpy(post ? state.postGlobalRng : state.preGlobalRng,
                globalRng, 16);
    if (effectRng)
        std::memcpy(post ? state.postEffectRng : state.preEffectRng,
                    effectRng, 16);
    if (owner)
        std::memcpy(post ? state.postOwnerRng : state.preOwnerRng,
                    (uint8_t*)owner + 0x2D80, 16);
    if (sShot)
        std::memcpy(post ? state.postShotIdPool : state.preShotIdPool,
                    (uint8_t*)sShot + SSHOT_ID_POOL_OFFSET,
                    SSHOT_ID_POOL_SIZE);
    if (sGameConfig)
        std::memcpy(post ? state.postShotSlotFlags : state.preShotSlotFlags,
                    (uint8_t*)sGameConfig + SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET,
                    SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE);
}

void RestoreWeaponCartTransitionState(
    const DeferredWeaponCartDebris& state, void* owner, bool post)
{
    uint8_t* globalRng = (uint8_t*)_addr(0x140D765D8);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    uint8_t* effectRng = sEffect ? (uint8_t*)sEffect + 0x3C : nullptr;
    void* sShot = *(void**)_addr(PTR_SSHOT);
    void* sGameConfig = *(void**)_addr(PTR_SGAMECONFIG);
    std::memcpy(globalRng,
                post ? state.postGlobalRng : state.preGlobalRng, 16);
    if (effectRng)
        std::memcpy(effectRng,
                    post ? state.postEffectRng : state.preEffectRng, 16);
    if (owner)
        std::memcpy((uint8_t*)owner + 0x2D80,
                    post ? state.postOwnerRng : state.preOwnerRng, 16);
    if (sShot)
        std::memcpy((uint8_t*)sShot + SSHOT_ID_POOL_OFFSET,
                    post ? state.postShotIdPool : state.preShotIdPool,
                    SSHOT_ID_POOL_SIZE);
    if (sGameConfig)
        std::memcpy((uint8_t*)sGameConfig +
                        SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET,
                    post ? state.postShotSlotFlags : state.preShotSlotFlags,
                    SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE);
}

void RecoverWeaponCartEndTransition(void* shot, void* owner,
                                    DeferredWeaponCartDebris& state)
{
    uint64_t vtable = *(uint64_t*)shot;
    uint64_t selectMotion = *(uint64_t*)(vtable + 0x190);
    if (selectMotion) {
        uint32_t animation = *(uint8_t*)((uint8_t*)shot + 0x22F4) ? 7 : 3;
        ((void(__fastcall*)(void*, uint32_t))selectMotion)(shot, animation);
    }
    void* sShot = *(void**)_addr(PTR_SSHOT);
    if (!sShot) return;
    uint32_t* pendingCount = (uint32_t*)((uint8_t*)sShot +
                                         SSHOT_PENDING_COUNT_OFFSET);
    if (*pendingCount == 0) {
        RestoreWeaponCartTransitionState(state, owner, false);
        ((void(__fastcall*)(void*))_addr(FN_WEAPON_CART_SPAWN_DEBRIS))(shot);
        RestoreWeaponCartTransitionState(state, owner, true);
    }
    if (*pendingCount != 0) {
        ((void(__fastcall*)(void*))_addr(FN_SSHOT_FLUSH_PENDING_UNITS))(sShot);
        state.pending = false;
    }
}

void __fastcall hkWeaponCartMove(void* shot)
{
    RollbackShotRecipe recipe;
    bool reconstructed = IsReconstructedShot(
        shot, DTI_USHOT_VULCAN_WEAPON_CART, &recipe);
    void* owner = reconstructed ? recipe.ownerChr : nullptr;
    uint32_t beforeGate = *(uint32_t*)((uint8_t*)shot + 0x221C);
    if (reconstructed && owner && beforeGate == 3) {
        auto deferred = g_deferredWeaponCartDebris.find(owner);
        if (deferred != g_deferredWeaponCartDebris.end() &&
            deferred->second.pending) {
            RecoverWeaponCartEndTransition(shot, owner, deferred->second);
        }
    }
    DeferredWeaponCartDebris transitionState{};
    bool captureTransition = reconstructed && owner && beforeGate == 2;
    size_t recipeCountBefore = Shot_GetRecipeCount();
    if (captureTransition)
        CaptureWeaponCartTransitionState(transitionState, owner, false);
    if (reconstructed) {
        if (*(uint32_t*)((uint8_t*)shot + 0x550) == 0) return;
        float timer = *(float*)((uint8_t*)shot + 0x1F58);
        if (timer >= 67.0f && timer < 122.0f) {
            *(uint32_t*)((uint8_t*)shot + 0x221C) = 1;
            *(float*)((uint8_t*)shot + 0x2080) = 0.0f;
        }
    }
    oWeaponCartMove(shot);
    uint32_t afterGate = *(uint32_t*)((uint8_t*)shot + 0x221C);
    if (captureTransition && afterGate == 3) {
        CaptureWeaponCartTransitionState(transitionState, owner, true);
        if (Shot_GetRecipeCount() == recipeCountBefore) {
            transitionState.pending = true;
            g_deferredWeaponCartDebris[owner] = std::move(transitionState);
        } else {
            g_deferredWeaponCartDebris.erase(owner);
        }
    }
    if (reconstructed && *(float*)((uint8_t*)shot + 0x1F58) >= 122.0f &&
        *(uint32_t*)((uint8_t*)shot + 0x221C) == 1) {
        *(uint32_t*)((uint8_t*)shot + 0x221C) = 2;
        *(float*)((uint8_t*)shot + 0x2080) = 20.0f;
    }
    if (reconstructed && *(float*)((uint8_t*)shot + 0x1F58) >= 141.0f &&
        *(uint32_t*)((uint8_t*)shot + 0x221C) == 3 &&
        *(uint64_t*)((uint8_t*)shot + 0x568) != 0x68) {
        *(uint64_t*)((uint8_t*)shot + 0x558) = 0x20000044;
        *(uint64_t*)((uint8_t*)shot + 0x568) = 0x68;
        *(uint64_t*)((uint8_t*)shot + 0x570) = 0x44;
    }
}

void InstallPolicyHook(uintptr_t address, void* replacement, void** original,
                       const char* success, const char* failure)
{
    MH_STATUS status = MH_CreateHook((LPVOID)_addr(address), replacement, original);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(address));
        HookLog("%s\n", success);
    } else {
        HookLog(failure, status);
    }
}

}

void Rollback_ResetShotModelPolicy()
{
    g_resimSpawnedPendulums.clear();
    g_pendulumNativeMotionReady.clear();
    g_deferredWeaponCartDebris.clear();
}

void Rollback_ForgetShotModelPolicy(void* shot)
{
    g_resimSpawnedPendulums.erase(shot);
    g_pendulumNativeMotionReady.erase(shot);
}

uint32_t Rollback_AfterShotSpawn(void* shot, void* parent)
{
    RepairResimRepulsorBlastMotion(shot);
    RepairResimDogMissileMotion(shot);
    RepairResimPendulumModel(shot, nullptr);
    if (g_isResimulating && shot) {
        uint64_t dti = GetObjectDti((uint64_t)shot);
        if (dti == (uint64_t)_addr(0x140D4C4C8))
            g_resimSpawnedPendulums.insert(shot);
    }
    return Rollback_RepairNemesisExplosionAttack(shot, parent);
}

void Rollback_BeforeShotHitboxUpdate(
    void* shot, const RollbackShotRecipe* recipe)
{
    RepairPrickleBoardMotionBindings(shot, recipe);
    RepairResimPendulumModel(shot, recipe);
    if (recipe) {
        Rollback_RepairNemesisExplosionAttack(
            shot, (void*)recipe->initParam, recipe->spawnAttackInfoId);
    }
}

bool Rollback_AfterShotHitboxUpdate(
    void* shot, const RollbackShotRecipe* recipe)
{
    bool pendulum = shot &&
        GetObjectDti((uint64_t)shot) == (uint64_t)_addr(0x140D4C4C8) &&
        (g_isResimulating || (recipe && recipe->reconstructed));
    if (pendulum) RepairResimPendulumModel(shot, recipe);
    return pendulum;
}

bool Rollback_ConsumePendulumCollisionReset(void* shot, uint64_t runtimeDti)
{
    if (!shot || runtimeDti != (uint64_t)_addr(0x140D4C4C8))
        return false;

    bool ready = PendulumNativeMotionReady(shot);
    auto [entry, inserted] = g_pendulumNativeMotionReady.emplace(shot, ready);
    if (inserted) return false;
    bool transitioned = !entry->second && ready;
    entry->second = ready;
    return transitioned;
}

void Rollback_BeforeShotHitCollision(
    void* shot, const RollbackShotRecipe* recipe)
{
    if (recipe) {
        Rollback_RepairNemesisExplosionAttack(
            shot, (void*)recipe->initParam, recipe->spawnAttackInfoId);
    }
}

void Rollback_TraceShotPolicySpawn(
    void* shot, uint64_t spawnSequence, int32_t frame)
{
    if (!shot ||
        GetObjectDti((uint64_t)shot) !=
            (uint64_t)_addr(DTI_USHOT_SHIELD_KIKOUSYOU)) {
        return;
    }
    void* callback1 = *(void**)((uint8_t*)shot + 0x1F78);
    void* callback2 = *(void**)((uint8_t*)shot + 0x1F80);
    HookLog("KIKOSHOSPAWNRESULT frame=%d sequence=%llu shot=0x%llX "
            "slot1=0x%llX slot2=0x%llX\n",
            frame, spawnSequence, (uint64_t)shot,
            (uint64_t)callback1, (uint64_t)callback2);
}

bool Rollback_ShouldReapplyShotCapsules(uint64_t runtimeDti)
{
    return runtimeDti != (uint64_t)_addr(DTI_USHOT_VULCAN_LEGION_TIGER) &&
        runtimeDti != (uint64_t)_addr(DTI_USHOT_SHIELD_REPULSOR_BLAST_SP) &&
        runtimeDti != (uint64_t)_addr(0x140D4C4C8);
}

void Rollback_InitShotModelPolicyHooks()
{
    /*InstallPolicyHook(FN_KING_KOBUN_MOVE, (LPVOID)&hkKingKobunMove,
                      (void**)&oKingKobunMove,
                      "Rollback: hooked uShotVulcanKingKobun::move trace",
                      "Rollback: failed to hook uShotVulcanKingKobun::move (%d)\n");*/
    InstallPolicyHook(FN_MISSILE_EMPLACEMENT_MOVE,
                      (LPVOID)&hkMissileEmplacementMove,
                      (void**)&oMissileEmplacementMove,
                      "Rollback: hooked reconstructed missile emplacement model retry",
                      "Rollback: failed to hook missile emplacement model retry (%d)\n");
    InstallPolicyHook(FN_WEAPON_CART_MOVE, (LPVOID)&hkWeaponCartMove,
                      (void**)&oWeaponCartMove,
                      "Rollback: hooked reconstructed Weapon Cart model retry",
                      "Rollback: failed to hook Weapon Cart model retry (%d)\n");
}

bool Rollback_ShouldRestoreShotModelPlayback(uint64_t runtimeDti)
{
    const ShotModelPolicy* policy = FindShotModelPolicy(runtimeDti);
    return policy && policy->restorePlayback;
}

bool Rollback_ShouldFinalizeShotBeforeRetirement(uint64_t runtimeDti)
{
    return runtimeDti && ((uint8_t(__fastcall*)(void*, void*))_addr(
        FN_MTDTI_IS_KIND_OF))((void*)runtimeDti,
                              (void*)_addr(DTI_USHOT_LASER));
}

void Rollback_PinShotModelResources(
    void* shot, std::vector<void*>& pinnedShotResources,
    std::vector<void*>& pinnedEffectResources)
{
    if (!shot) return;

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
        if (!effect) continue;
        PinUnique(*(void**)((uint8_t*)effect + 0x108), pinnedEffectResources);
    }
}

int Rollback_RetireShotModelEffects(
    void* sUnit, const std::vector<void*>& currentShots)
{
    std::vector<void*> retiredModels;
    int retiredCount = 0;
    for (void* shot : currentShots) {
        if (!shot ||
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
        if (!shot ||
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

void Rollback_EvaluateRestoredDogMissileModel(void* shot,
                                               uint64_t runtimeDti)
{
    if (!shot || runtimeDti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_DOG_MISSILE)) {
        return;
    }

    // Match Legion Tiger's reconstruction path, but pass the saved uModel
    // motion-list entry rather than mAnmNo. Dog setup requests actor animation
    // 0/1, which native actor processing maps to motion 0xCB. Passing 0 directly
    // makes processActionAnmchr invalidate Motion[0] as an unresolved entry.
    uint16_t motionNo = *(uint16_t*)((uint8_t*)shot + 0x568);
    if (motionNo == UINT16_MAX)
        motionNo = DOG_MISSILE_MOTION_NO;
    float animationTime = *(float*)((uint8_t*)shot + 0x1328);
    if (animationTime > 0.0f) {
        ((uint64_t(__fastcall*)(void*, uint32_t, uint32_t, float, float,
                               float, uint32_t))
            _addr(FN_UMODEL_PROCESS_ACTION_ANMCHR))(
                shot, 0, motionNo, 0.0f,
                std::max(0.0f, animationTime - 1.0f), 1.0f, 0x40);
    }
    ((void(__fastcall*)(void*))_addr(FN_UMODEL_MOVE))(shot);
}

bool Rollback_RemapShotSpawnParent(
    const RollbackShotRecipe& saved,
    const std::vector<RollbackShotRecipe>& recipes,
    RollbackShotRecipe& remapped)
{
    if (saved.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_SHIELD_WEAPON_CART_SPREAD) &&
        saved.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_ROCKET_LAUNCHER_EXPLODE) &&
        saved.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_STORM_SWORD_SPREAD)) {
        return false;
    }
    remapped = saved;
    for (const RollbackShotRecipe& candidate : recipes) {
        if (candidate.savedPtr == (void*)saved.initParam &&
            candidate.restoredPtr) {
            remapped.initParam = (uint64_t)candidate.restoredPtr;
            return true;
        }
    }
    // A child can survive in the snapshot after its terminal parent recipe was
    // intentionally omitted. Never pass that retired allocation to cArm. The
    // factory supports a null attachment, and restore reapplies the child's
    // captured position, quaternion, flight state, and attack matrices.
    remapped.initParam = 0;
    return true;
}

void Rollback_PrepareShotFactory(
    const RollbackShotRecipe& saved, RollbackShotFactoryPolicyState& state)
{
    if (!saved.ownerChr) {
        return;
    }

    uint32_t groupOffset = 0;
    uint32_t swordIdOffset = 0;
    if (saved.concrete.dti ==
        (uint64_t)_addr(DTI_USHOT_SHIELD_SPIRAL_SWORD)) {
        swordIdOffset = 0x2210;
    } else if (saved.concrete.dti ==
               (uint64_t)_addr(DTI_USHOT_SHIELD_RAID_SWORD)) {
        groupOffset = 6;
        swordIdOffset = 0x2140;
        state.ownerUsrFField =
            (uint32_t*)((uint8_t*)saved.ownerChr + 0x1510);
        state.ownerUsrF = *state.ownerUsrFField;
    } else if (saved.concrete.dti ==
               (uint64_t)_addr(DTI_USHOT_SHIELD_STORM_SWORD)) {
        groupOffset = 12;
        swordIdOffset = 0x2210;
    } else {
        return;
    }
    if (saved.concrete.body.size() < swordIdOffset + sizeof(uint32_t)) {
        return;
    }

    uint32_t swordId = 0;
    std::memcpy(&swordId, saved.concrete.body.data() + swordIdOffset,
                 sizeof(swordId));
    if (swordId < 1 || swordId > sizeof(state.ownerSlots)) return;

    state.ownerSlotTable =
        (uint8_t*)saved.ownerChr + 0x68F0 + groupOffset;
    std::memcpy(state.ownerSlots, state.ownerSlotTable,
                sizeof(state.ownerSlots));
    std::memset(state.ownerSlotTable, 1, sizeof(state.ownerSlots));
    state.ownerSlotTable[swordId - 1] = 0;
}

void Rollback_FinishShotFactory(RollbackShotFactoryPolicyState& state)
{
    if (state.ownerSlotTable) {
        std::memcpy(state.ownerSlotTable, state.ownerSlots,
                    sizeof(state.ownerSlots));
    }
    if (state.ownerUsrFField) {
        *state.ownerUsrFField = state.ownerUsrF;
    }
}

void Rollback_RebuildShotStateBeforeRegistration(
    void* shot, const RollbackShotRecipe& saved)
{
    if (saved.concrete.dti != (uint64_t)_addr(DTI_USHOT_VULCAN_LAND_MINE) ||
        saved.concrete.body.size() <= 0x2214 ||
        saved.concrete.body[0x2214] == 0 ||
        !saved.concrete.callbackEffects[0].occupied ||
        *(void**)((uint8_t*)shot + 0x1F78) != nullptr ||
        *(uint8_t*)((uint8_t*)shot + 0x2214) != 0) {
        return;
    }
    uint64_t vtable = *(uint64_t*)shot;
    uint64_t contactAddress = *(uint64_t*)(vtable + 0x318);
    if (contactAddress)
        ((void(__fastcall*)(void*, void*))contactAddress)(shot, nullptr);
}

uint32_t Rollback_RepairNemesisExplosionAttack(
    void* shot, void* parent, uint32_t restoredId)
{
    uint64_t shotDti = shot ? GetObjectDti((uint64_t)shot) : 0;
    uint64_t parentDti = parent ? GetObjectDti((uint64_t)parent) : 0;
    if (!shot || !parent ||
        shotDti != (uint64_t)_addr(DTI_USHOT_VULCAN_ROCKET_LAUNCHER_EXPLODE) ||
        parentDti != (uint64_t)_addr(DTI_USHOT_VULCAN_EXATK_ROCKET_LAUNCHER)) {
        return UINT32_MAX;
    }
    uint32_t& attackInfoId = *(uint32_t*)((uint8_t*)shot + 0x1498);
    attackInfoId = restoredId != UINT32_MAX
        ? restoredId : *(uint32_t*)((uint8_t*)parent + 0x1498);
    return attackInfoId;
}

void Rollback_RestoreShotClassState(
    void* shot, const RollbackShotRecipe& saved, void* spawnParent)
{
    if (saved.flight.step == 3 &&
        (saved.concrete.dti ==
             (uint64_t)_addr(DTI_USHOT_SHIELD_SPIRAL_SWORD) ||
         saved.concrete.dti ==
             (uint64_t)_addr(DTI_USHOT_SHIELD_RAID_SWORD) ||
         saved.concrete.dti ==
             (uint64_t)_addr(DTI_USHOT_SHIELD_STORM_SWORD))) {
        // The factory creates an active callback effect. Re-enter native
        // cleanup before callback timing is restored, otherwise a step-3 sword
        // can remain attached to Vergil and be recreated on every rollback.
        ((void(__fastcall*)(void*))_addr(FN_UGAMEEFFECT_RESET_EFFECT_STATE))(
            shot);
    }

    if (saved.concrete.dti ==
            (uint64_t)_addr(DTI_USHOT_VULCAN_MISSILE_EMPLACEMENT) &&
        saved.concrete.body.size() >= 0x2170) {
        std::memcpy((uint8_t*)shot + 0x2120,
                    saved.concrete.body.data() + 0x2120, 0x50);
    }

    RestoreKingKobunHelper(shot, saved);
    if (saved.concrete.dti ==
            (uint64_t)_addr(DTI_USHOT_VULCAN_KING_KOBUN) &&
        saved.concrete.body.size() >= 0x2164) {
        static constexpr size_t ranges[][2] = {
            {0x2140, 0x08}, {0x214C, 0x04},
            {0x2150, 0x08}, {0x2160, 0x04},
        };
        for (const auto& range : ranges) {
            std::memcpy((uint8_t*)shot + range[0],
                        saved.concrete.body.data() + range[0], range[1]);
        }
    }
    Rollback_RepairNemesisExplosionAttack(
        shot, spawnParent, saved.spawnAttackInfoId);
    RestoreReconstructedPrickleBoardCommandState(shot, saved);
    Rollback_EvaluateRestoredShotModel(
        shot, saved.concrete.dti, saved.concrete.body.data(),
        saved.concrete.body.size());
}

void Rollback_RemapCharacterShotLinks(
    void* character, uint64_t expectedVtable, const uint8_t* savedCharacter,
    size_t savedCharacterSize, const std::vector<RollbackShotRecipe>& recipes)
{
    if (expectedVtable == (uint64_t)_addr(VTABLE_UARTHUR)) {
        if (savedCharacterSize <= UARTHUR_CROSS_SWORD_FLAG_OFFSET) return;

        // Native clears t (savedCharacterSize <= UARTHUR_CROSS_SWORD_FLAG_OFFSET) return;

        // Native clears this detector from Cross Sword's kill callbacks. A shot
        // already in cleanup step 3 is intentionally omitted from the snapshot,
        // so restoring the saved byte can otherwise leave Arthur locked out with
        // no surviving shot to clear it. Derive it from the reconstructed set.
        bool hasCrossSword = false;
        for (const RollbackShotRecipe& saved : recipes) {
            if (saved.ownerChr == character && saved.restoredPtr &&
                saved.concrete.dti ==
                    (uint64_t)_addr(DTI_USHOT_VULCAN_CROSS_SWORD)) {
                hasCrossSword = true;
                break;
            }
        }
        *((uint8_t*)character + UARTHUR_CROSS_SWORD_FLAG_OFFSET) =
            hasCrossSword ? 1 : 0;
        return;
    }

}

void Rollback_CaptureCharacterShotReferences(RollbackSnapshotSlot& slot)
{
    slot.characterShotReferences.clear();

    std::unordered_set<void*> characters;
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    for (int team = 0; sCharacter && team < 2; ++team) {
        uint64_t node = sCharacter + (team == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
            TEAM_FIRST_NODE_OFFSET;
        for (int safety = 16; node && safety-- > 0;) {
            void* character = *(void**)(node + NODE_CHR_OFFSET);
            if (character) characters.insert(character);
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
    if (sUnit && *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) >
                     SUNIT_CHARACTER_CATEGORY) {
        void* character = *(void**)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
            SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
        for (int safety = 64; character && safety-- > 0;) {
            characters.insert(character);
            character = *(void**)((uint8_t*)character + 0x18);
        }
    }

    auto findUniqueOwnedShot = [&](void* character, uintptr_t dti,
                                   bool enabled) {
        uint64_t sequence = 0;
        if (!enabled) return sequence;
        for (const RollbackShotRecipe& shot : slot.shotRecipes) {
            if (shot.ownerChr != character ||
                shot.concrete.dti != (uint64_t)_addr(dti)) continue;
            if (sequence) return (uint64_t)0;
            sequence = shot.spawnSequence;
        }
        return sequence;
    };

    size_t dataOffset = 0;
    for (int regionIndex = 0; regionIndex < slot.regionCount; ++regionIndex) {
        const RollbackRegion& region = slot.regions[regionIndex];
        if (dataOffset + region.size > slot.size) break;
        if (characters.find(region.ptr) == characters.end()) {
            dataOffset += region.size;
            continue;
        }

        const uint8_t* savedCharacter = slot.data + dataOffset;
        auto capture = [&](uint32_t offset,
                           RollbackCharacterShotReferenceRole role,
                           uintptr_t fallbackDti = 0,
                           bool captureEmpty = false) {
            if ((size_t)offset + sizeof(void*) > region.size) return;
            void* savedPointer = *(void* const*)(savedCharacter + offset);
            uint64_t sequence = 0;
            bool tracked = savedPointer &&
                Shot_GetSpawnSequence(savedPointer, sequence);
            if (!tracked && fallbackDti) {
                sequence = findUniqueOwnedShot(
                    region.ptr, fallbackDti, captureEmpty || savedPointer);
            }
            if (!tracked && !sequence && !captureEmpty) return;
            slot.characterShotReferences.push_back({
                region.ptr, region.expectedVtable, sequence, offset, role,
            });
        };

        capture(0x90, RollbackCharacterShotReferenceRole::AnimationReturn);
        capture(0x6428, RollbackCharacterShotReferenceRole::ObjectReturn);

        if (region.expectedVtable == (uint64_t)_addr(VTABLE_URROCKET_RACCOON)) {
            for (uint32_t offset : {
                    0x6908u, 0x6910u, 0x6918u, 0x6920u,
                    0x6938u, 0x6940u, 0x6948u}) {
                capture(offset, RollbackCharacterShotReferenceRole::RocketShot,
                        0, true);
            }
        } else if (region.expectedVtable ==
                   (uint64_t)_addr(VTABLE_UCAP_AMERICA)) {
            capture(UCAP_AMERICA_SHIELD_SLASH_OFFSET,
                    RollbackCharacterShotReferenceRole::ShieldSlash,
                    DTI_USHOT_OPTION_SHIELD_SLASH);
        } else if (region.expectedVtable ==
                   (uint64_t)_addr(VTABLE_UDOCTOR_DOOM)) {
            capture(UDOCTOR_DOOM_MISSILE_OFFSET,
                    RollbackCharacterShotReferenceRole::MissileEmplacement,
                    DTI_USHOT_VULCAN_MISSILE_EMPLACEMENT, true);
        } else if (region.expectedVtable == (uint64_t)_addr(VTABLE_UTRISH)) {
            capture(UTRISH_SPADA_BOOMERANG_OFFSET,
                    RollbackCharacterShotReferenceRole::SpadaBoomerang,
                    DTI_USHOT_OPTION_SPADA_BOOMERANG);
        }
        dataOffset += region.size;
    }
}

void Rollback_RestoreCharacterShotReferences(RollbackSnapshotSlot& slot)
{
    for (const RollbackCharacterShotReference& saved :
         slot.characterShotReferences) {
        if (!saved.character || *(uint64_t*)saved.character != saved.expectedVtable)
            continue;
        RollbackShotReference reference{};
        reference.kind = saved.shotSpawnSequence
            ? RollbackShotReferenceKind::Shot
            : RollbackShotReferenceKind::None;
        reference.shotSpawnSequence = saved.shotSpawnSequence;
        *(void**)((uint8_t*)saved.character + saved.offset) =
            Shot_ResolveReference(reference);
    }
}
