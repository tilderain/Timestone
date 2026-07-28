#include "../pch.h"

#include "rollback_shot.h"

#include "rollback_memory.h"
#include "rollback_effect.h"
#include "rollback_profile.h"
#include "rollback_snapshot.h"
#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

extern void HookLog(const char* fmt, ...);
extern bool g_multiResimEnabled;

static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SUNIT_MAX_CATEGORIES = 64;
static constexpr uint32_t SUNIT_MULTI_MASK_COUNT_OFFSET = 0x1840;
static constexpr uint32_t SUNIT_MULTI_MASK_ARRAYS_OFFSET = 0xC50;
static constexpr uintptr_t VTABLE_USHOT_VULCAN_ZIREITOU = 0x140AC0E20;
static constexpr uintptr_t PTR_SREPLAY = 0x140d4d9d8;
static constexpr uintptr_t FN_SSHOT_SPAWN = 0x140150E00;
static constexpr uintptr_t FN_SSHOT_SPAWN_EX = 0x140150F40;
static constexpr uintptr_t FN_USHOT_UPDATE_HITBOXES = 0x1401C9000;
static constexpr uintptr_t FN_BUILD_ATTACH_MATRIX = 0x14014F960;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr uintptr_t FN_KING_KOBUN_MOVE = 0x1401BA2D0;
static constexpr int DYNAMIC_SAFETY = 128;
static constexpr int SSHOT_SENTINEL_COUNT = 10;
static constexpr size_t SSHOT_NODE_OFFSET = 0x1450;
static constexpr size_t SSHOT_NODE_NEXT = 0x08;

using ShotSpawnFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*,
    void*, uint32_t, void*);
using ShotSpawnExFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*,
    void*, uint32_t, void*, int, void*, int, void*);
using UnitRemoveFn = char(__fastcall*)(void*, void*);
using BuildAttachMatrixFn = void(__fastcall*)(void*, void*, char);
using ShotUpdateHitboxesFn = void(__fastcall*)(void*);
using KingKobunMoveFn = void(__fastcall*)(void*);

static ShotSpawnFn oShotSpawn = nullptr;
static ShotSpawnExFn oShotSpawnEx = nullptr;
static UnitRemoveFn oUnitRemove = nullptr;
static BuildAttachMatrixFn oBuildAttachMatrix = nullptr;
static ShotUpdateHitboxesFn oShotUpdateHitboxes = nullptr;
static KingKobunMoveFn oKingKobunMove = nullptr;
static std::unordered_map<void*, RollbackShotRecipe> g_liveShotRecipes;
static uint64_t g_shotSpawnSequence = 0;
static int g_kingKobunTraceCount = 0;

static bool IsActiveUnit(void* unit)
{
    if (!unit) return false;
    uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
    return (state & 7u) < 3u && (state & 0x3F8u) != 0x3F8u;
}

static bool IsLinkedShot(void* shot)
{
    for (int list = 0; list < SSHOT_SENTINEL_COUNT; ++list) {
        uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + list * 0x28;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t nodeRef = *(uint64_t*)(sentinel + 0x10);
        int safety = DYNAMIC_SAFETY;
        while (nodeRef && nodeRef != sentinelRef && safety-- > 0) {
            if ((void*)(nodeRef - SSHOT_NODE_OFFSET) == shot) return true;
            nodeRef = *(uint64_t*)(nodeRef + SSHOT_NODE_NEXT);
        }
    }
    return false;
}

static bool IsTerminalShot(void* shot)
{
    if (!shot || !IsActiveUnit(shot)) return false;
    float lifeFrame = *(float*)((uint8_t*)shot + 0x2078);
    float timer = *(float*)((uint8_t*)shot + 0x1F60);
    uint32_t step = *(uint32_t*)((uint8_t*)shot + 0x212C);
    return std::isfinite(lifeFrame) && lifeFrame <= 0.0f &&
           std::isfinite(timer) && timer > 0.0f && step == 3;
}

static void CapturePreSpawnRng(RollbackShotRecipe& recipe, void* ownerChr)
{
    std::memcpy(recipe.preSpawnGlobalRng, (void*)_addr(0x140D765D8), 16);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    if (sEffect)
        std::memcpy(recipe.preSpawnEffectRng, (uint8_t*)sEffect + 0x3C, 16);
    if (ownerChr)
        std::memcpy(recipe.preSpawnOwnerRng, (uint8_t*)ownerChr + 0x2D80, 16);
    recipe.hasPreSpawnRng = true;
}

static void* __fastcall hkShotSpawn(void* manager, void* element, uint64_t initParam,
                                     void* posData, void* ownerChr, void* objectRef1,
                                     uint32_t slotId, void* objectRef2)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ShotSpawn);
    RollbackShotRecipe recipe{};
    recipe.shotElement = element;
    recipe.initParam = initParam;
    recipe.posData = posData;
    recipe.ownerChr = ownerChr;
    recipe.objectRef1 = objectRef1;
    recipe.objectRef1Dti = objectRef1 && HasCallableDtiVirtual(objectRef1)
        ? GetObjectDti((uint64_t)objectRef1) : 0;
    recipe.objectRef1WasActiveUnit = objectRef1 && IsUnitObject(objectRef1) &&
        IsActiveUnit(objectRef1);
    recipe.slotId = slotId;
    recipe.objectRef2 = objectRef2;
    recipe.objectRef2Dti = objectRef2 && HasCallableDtiVirtual(objectRef2)
        ? GetObjectDti((uint64_t)objectRef2) : 0;
    recipe.objectRef2WasActiveUnit = objectRef2 && IsUnitObject(objectRef2) &&
        IsActiveUnit(objectRef2);
    recipe.kind = RollbackShotSpawnKind::Standard;
    recipe.spawnSequence = ++g_shotSpawnSequence;
    CapturePreSpawnRng(recipe, ownerChr);
    void* shot = oShotSpawn(manager, element, initParam, posData, ownerChr,
                            objectRef1, slotId, objectRef2);
    InvalidateMemoryQueryCache();
    if (shot) {
        recipe.savedPtr = shot;
        recipe.restoredPtr = shot;
        g_liveShotRecipes[shot] = recipe;
    }
    return shot;
}

static void* __fastcall hkShotSpawnEx(void* manager, void* element, uint64_t initParam,
                                       void* posData, void* ownerChr, void* objectRef1,
                                       uint32_t slotId, void* objectRef2, int a9,
                                       void* a10, int a11, void* a12)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ShotSpawnEx);
    RollbackShotRecipe recipe{};
    recipe.shotElement = element;
    recipe.initParam = initParam;
    recipe.posData = posData;
    recipe.ownerChr = ownerChr;
    recipe.objectRef1 = objectRef1;
    recipe.objectRef1Dti = objectRef1 && HasCallableDtiVirtual(objectRef1)
        ? GetObjectDti((uint64_t)objectRef1) : 0;
    recipe.objectRef1WasActiveUnit = objectRef1 && IsUnitObject(objectRef1) &&
        IsActiveUnit(objectRef1);
    recipe.slotId = slotId;
    recipe.objectRef2 = objectRef2;
    recipe.objectRef2Dti = objectRef2 && HasCallableDtiVirtual(objectRef2)
        ? GetObjectDti((uint64_t)objectRef2) : 0;
    recipe.objectRef2WasActiveUnit = objectRef2 && IsUnitObject(objectRef2) &&
        IsActiveUnit(objectRef2);
    recipe.kind = RollbackShotSpawnKind::Extended;
    recipe.exA9 = a9;
    recipe.exA10 = a10;
    recipe.exA11 = a11;
    recipe.exA12 = a12;
    recipe.spawnSequence = ++g_shotSpawnSequence;
    CapturePreSpawnRng(recipe, ownerChr);
    void* shot = oShotSpawnEx(manager, element, initParam, posData, ownerChr,
                              objectRef1, slotId, objectRef2, a9, a10, a11, a12);
    InvalidateMemoryQueryCache();
    if (shot) {
        recipe.savedPtr = shot;
        recipe.restoredPtr = shot;
        g_liveShotRecipes[shot] = recipe;
    }
    return shot;
}

static bool ValidAttachDispatchObject(uint64_t object)
{
    if (!object || !IsAccessiblePointer(object) ||
        !HasCallableDtiVirtual((void*)object)) return false;
    uint64_t vtable = *(uint64_t*)object;
    return IsAccessibleAddress(vtable + 0x2C8) &&
           IsAccessibleAddress(vtable + 0x2D0) &&
           IsExecutableAddress(*(uint64_t*)(vtable + 0x2C8)) &&
           IsExecutableAddress(*(uint64_t*)(vtable + 0x2D0));
}

static void __fastcall hkBuildAttachMatrix(void* arm, void* output, char reset)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::BuildAttachMatrix);
    if (!arm || !output || !IsAccessiblePointer((uint64_t)arm + 0xD0)) return;
    void* shot = (uint8_t*)arm - 0x1DC0;
    auto recipe = g_liveShotRecipes.find(shot);
    if (recipe == g_liveShotRecipes.end() || !recipe->second.reconstructed) {
        oBuildAttachMatrix(arm, output, reset);
        return;
    }
    auto* bytes = (uint8_t*)arm;
    uint64_t primary = *(uint64_t*)(bytes + 0x70);
    uint64_t secondary = *(uint64_t*)(bytes + 0x78);
    uint64_t tertiary = *(uint64_t*)(bytes + 0x80);
    if (ValidAttachDispatchObject(primary) &&
        (secondary == 0 || HasCallableDtiVirtual((void*)secondary)) &&
        (tertiary == 0 || HasCallableDtiVirtual((void*)tertiary))) {
        oBuildAttachMatrix(arm, output, reset);
        return;
    }
    std::memcpy(output, bytes + 0x90, 0x40);
}

static void __fastcall hkShotUpdateHitboxes(void* shot)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ShotUpdateHitboxes);
    oShotUpdateHitboxes(shot);
    auto recipe = g_liveShotRecipes.find(shot);
    if (recipe != g_liveShotRecipes.end() && recipe->second.reconstructed)
        ConcreteShotSnapshot_ReapplyCapsules(shot, recipe->second.concrete);
}

static void __fastcall hkKingKobunMove(void* shot)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::KingKobunMove);
    bool trace = shot && GetObjectDti((uint64_t)shot) ==
        (uint64_t)_addr(0x140D4D2B0) && g_kingKobunTraceCount < 128;
    uint32_t beforeMode = 0;
    uint32_t beforePhase = 0;
    float beforePos = 0.0f;
    if (trace) {
        beforeMode = *(uint32_t*)((uint8_t*)shot + 0x214C);
        beforePhase = *(uint32_t*)((uint8_t*)shot + 0x2140);
        beforePos = *(float*)((uint8_t*)shot + 0x50);
    }
    oKingKobunMove(shot);
    if (trace) {
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
}

static char __fastcall hkUnitRemove(void* manager, void* unit)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemove);
    decltype(g_liveShotRecipes)::iterator recipe;
    {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveRecipeLookup);
        recipe = g_liveShotRecipes.find(unit);
    }
    if (recipe != g_liveShotRecipes.end() && recipe->second.reconstructed) {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveShotDetach);
        Shot_DetachEmbeddedEffect(unit);
    }
    Effect_PrepareUnitRemoval(unit);
    char result;
    {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveNative);
        result = oUnitRemove(manager, unit);
    }
    InvalidateMemoryQueryCache();
    if (result && unit) {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveRecipeCleanup);
        if (!IsLinkedShot(unit)) g_liveShotRecipes.erase(unit);
        Effect_FinishUnitRemoval(unit);
    }
    return result;
}

void Shot_CaptureRecipes(RollbackSnapshotSlot& slot)
{
    slot.shotRecipes.clear();
    for (int list = 0; list < SSHOT_SENTINEL_COUNT; ++list) {
        uint64_t sentinel = (uint64_t)_addr(0x140d47f90) + list * 0x28;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t nodeRef = *(uint64_t*)(sentinel + 0x10);
        int safety = DYNAMIC_SAFETY;
        while (nodeRef && nodeRef != sentinelRef && safety-- > 0) {
            void* shot = (void*)(nodeRef - SSHOT_NODE_OFFSET);
            auto recipe = g_liveShotRecipes.find(shot);
            if (recipe != g_liveShotRecipes.end() && !IsTerminalShot(shot)) {
                RollbackShotRecipe saved = recipe->second;
                saved.savedPtr = shot;
                Shot_CaptureFlight(shot, saved.flight);
                ConcreteShotSnapshot_Capture(shot, saved.concrete);
                slot.shotRecipes.push_back(saved);
            }
            nodeRef = *(uint64_t*)(nodeRef + SSHOT_NODE_NEXT);
        }
    }
    std::sort(slot.shotRecipes.begin(), slot.shotRecipes.end(),
              [](const RollbackShotRecipe& a, const RollbackShotRecipe& b) {
                  return a.spawnSequence < b.spawnSequence;
              });
}

bool Shot_HooksReady()
{
    return oShotSpawn && oShotSpawnEx;
}

bool Shot_GetRecipe(void* shot, RollbackShotRecipe& recipe)
{
    auto found = g_liveShotRecipes.find(shot);
    if (found == g_liveShotRecipes.end()) return false;
    recipe = found->second;
    return true;
}

void Shot_CollectCurrent(std::vector<void*>& shots)
{
    shots.clear();
    shots.reserve(g_liveShotRecipes.size());
    for (const auto& entry : g_liveShotRecipes) {
        if (IsLinkedShot(entry.first)) shots.push_back(entry.first);
    }
}

void* Shot_SpawnOriginal(void* manager, const RollbackShotRecipe& recipe,
                         void* objectRef1, void* objectRef2)
{
    return recipe.kind == RollbackShotSpawnKind::Extended
        ? oShotSpawnEx(manager, recipe.shotElement, recipe.initParam, recipe.posData,
                       recipe.ownerChr, objectRef1, recipe.slotId, objectRef2,
                       recipe.exA9, recipe.exA10, recipe.exA11, recipe.exA12)
        : oShotSpawn(manager, recipe.shotElement, recipe.initParam, recipe.posData,
                     recipe.ownerChr, objectRef1, recipe.slotId, objectRef2);
}

void Shot_TrackReconstructed(void* shot, const RollbackShotRecipe& recipe)
{
    RollbackShotRecipe live = recipe;
    live.savedPtr = shot;
    live.restoredPtr = shot;
    live.reconstructed = true;
    g_liveShotRecipes[shot] = live;
}

void Shot_Reset()
{
    g_liveShotRecipes.clear();
    g_shotSpawnSequence = 0;
}

static void InstallHook(uintptr_t address, void* replacement, void** original,
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

void Shot_InitHooks()
{
    InstallHook(FN_SSHOT_SPAWN, (LPVOID)&hkShotSpawn, (void**)&oShotSpawn,
                "Rollback: hooked sShot__spawnShot",
                "Rollback: failed to hook sShot__spawnShot (%d)\n");
    InstallHook(FN_SSHOT_SPAWN_EX, (LPVOID)&hkShotSpawnEx, (void**)&oShotSpawnEx,
                "Rollback: hooked sShot__spawnShotEx",
                "Rollback: failed to hook sShot__spawnShotEx (%d)\n");
    InstallHook(FN_BUILD_ATTACH_MATRIX, (LPVOID)&hkBuildAttachMatrix,
                (void**)&oBuildAttachMatrix,
                "Rollback: hooked uShot attachment transform guard",
                "Rollback: failed to hook uShot attachment transform guard (%d)\n");
    InstallHook(FN_USHOT_UPDATE_HITBOXES, (LPVOID)&hkShotUpdateHitboxes,
                (void**)&oShotUpdateHitboxes,
                "Rollback: hooked uShot hitbox update tail repair",
                "Rollback: failed to hook uShot hitbox update tail repair (%d)\n");
    InstallHook(FN_KING_KOBUN_MOVE, (LPVOID)&hkKingKobunMove,
                (void**)&oKingKobunMove,
                "Rollback: hooked uShotVulcanKingKobun::move trace",
                "Rollback: failed to hook uShotVulcanKingKobun::move (%d)\n");
    InstallHook(FN_SUNIT_REMOVE_UNIT, (LPVOID)&hkUnitRemove, (void**)&oUnitRemove,
                "Rollback: hooked sUnit__removeUnit for recipe lifetime",
                "Rollback: failed to hook sUnit__removeUnit (%d)\n");
}

static bool IsZireitouShot(void* shot)
{
    return shot && *(uintptr_t*)shot ==
        (uintptr_t)_addr(VTABLE_USHOT_VULCAN_ZIREITOU);
}

void Shot_CaptureFlight(void* shot, RollbackShotFlightState& out)
{
    auto* bytes = (uint8_t*)shot;
    std::memcpy(&out.unitState, bytes + 0x10, sizeof(out.unitState));
    std::memcpy(out.timeScaleState, bytes + 0x28, sizeof(out.timeScaleState));
    std::memcpy(out.transformState, bytes + 0x50, sizeof(out.transformState));
    std::memcpy(out.runtimeState, bytes + 0x3E8, sizeof(out.runtimeState));
    std::memcpy(out.shotMatrices, bytes + 0x1E50, sizeof(out.shotMatrices));
    std::memcpy(&out.timerThresh, bytes + 0x1F58, sizeof(out.timerThresh));
    std::memcpy(&out.timer, bytes + 0x1F60, sizeof(out.timer));
    std::memcpy(&out.delayTime, bytes + 0x1F5C, sizeof(out.delayTime));
    std::memcpy(&out.autoAddDivStartTime, bytes + 0x1F64,
                sizeof(out.autoAddDivStartTime));
    std::memcpy(out.resourceScalars, bytes + 0x1F70, sizeof(out.resourceScalars));
    std::memcpy(out.attackScalars, bytes + 0x2038, sizeof(out.attackScalars));
    std::memcpy(out.movementScalars, bytes + 0x2078, sizeof(out.movementScalars));
    std::memcpy(out.animationScalars, bytes + 0x2118, sizeof(out.animationScalars));
    std::memcpy(&out.step, bytes + 0x212C, sizeof(out.step));
    std::memcpy(&out.randomProgress, bytes + 0xA8, sizeof(out.randomProgress));
    std::memcpy(&out.randomSelection, bytes + 0xC4, sizeof(out.randomSelection));
    std::memcpy(&out.randomFlags, bytes + 0xC8, sizeof(out.randomFlags));
    std::memcpy(&out.randomState, bytes + 0x1B58, sizeof(out.randomState));
    out.hasZireitouState = IsZireitouShot(shot);
    if (out.hasZireitouState)
        std::memcpy(out.zireitouState, bytes + 0x2210, sizeof(out.zireitouState));
}

void Shot_ApplyFlight(void* shot, const RollbackShotFlightState& in)
{
    auto* bytes = (uint8_t*)shot;
    uint32_t unitState = *(uint32_t*)(bytes + 0x10);
    unitState = (in.unitState & ~0x3F8u) | (unitState & 0x3F8u);
    std::memcpy(bytes + 0x10, &unitState, sizeof(unitState));
    std::memcpy(bytes + 0x28, in.timeScaleState, sizeof(in.timeScaleState));
    std::memcpy(bytes + 0x50, in.transformState, sizeof(in.transformState));
    std::memcpy(bytes + 0x3E8, in.runtimeState, sizeof(in.runtimeState));
    std::memcpy(bytes + 0x1E50, in.shotMatrices, sizeof(in.shotMatrices));
    std::memcpy(bytes + 0x1F58, &in.timerThresh, sizeof(in.timerThresh));
    std::memcpy(bytes + 0x1F60, &in.timer, sizeof(in.timer));
    std::memcpy(bytes + 0x1F5C, &in.delayTime, sizeof(in.delayTime));
    std::memcpy(bytes + 0x1F64, &in.autoAddDivStartTime,
                sizeof(in.autoAddDivStartTime));
    std::memcpy(bytes + 0x1F70, in.resourceScalars, sizeof(in.resourceScalars));
    std::memcpy(bytes + 0x2038, in.attackScalars, sizeof(in.attackScalars));
    std::memcpy(bytes + 0x2078, in.movementScalars, sizeof(in.movementScalars));
    std::memcpy(bytes + 0x2118, in.animationScalars, sizeof(in.animationScalars));
    std::memcpy(bytes + 0x212C, &in.step, sizeof(in.step));
    std::memcpy(bytes + 0xA8, &in.randomProgress, sizeof(in.randomProgress));
    std::memcpy(bytes + 0xC4, &in.randomSelection, sizeof(in.randomSelection));
    std::memcpy(bytes + 0xC8, &in.randomFlags, sizeof(in.randomFlags));
    std::memcpy(bytes + 0x1B58, &in.randomState, sizeof(in.randomState));
    if (in.hasZireitouState && IsZireitouShot(shot))
        std::memcpy(bytes + 0x2210, in.zireitouState, sizeof(in.zireitouState));
}

void Shot_RestoreArmState(void* shot, const RollbackShotRecipe& saved)
{
    if (!shot || saved.concrete.body.size() < 0x1E50) return;
    uint8_t* arm = (uint8_t*)shot + 0x1DC0;
    const uint8_t* savedArm = saved.concrete.body.data() + 0x1DC0;
    void* shootParent = *(void**)(arm + 0x78);
    void* shootWayParent = *(void**)(arm + 0x80);
    std::memcpy(arm + 0x10, savedArm + 0x10, 0x60);
    std::memcpy(arm + 0x88, savedArm + 0x88, 0x08);
    *(void**)(arm + 0x70) = shot;
    *(void**)(arm + 0x78) = shootParent;
    *(void**)(arm + 0x80) = shootWayParent;
}

void Shot_DetachEmbeddedEffect(void* shot)
{
    constexpr uint32_t EFFECT_OFFSET = 0x2160;
    constexpr uint32_t EFFECT_SIZE = 0x230;
    if (!shot || !HasCallableDtiVirtual(shot)) return;
    if (DecodeDtiSize(GetObjectDti((uint64_t)shot)) < EFFECT_OFFSET + EFFECT_SIZE)
        return;
    uint8_t* effect = (uint8_t*)shot + EFFECT_OFFSET;
    if (!IsEffectObject(effect)) return;
    *(uint64_t*)(effect + 0x50) = 0;
    *(uint64_t*)(effect + 0x58) = 0;
    *(uint64_t*)(effect + 0x60) = 0;
    *(uint32_t*)(effect + 0x70) = 0xFFFFFFFFu;
    *(uint64_t*)(effect + 0x218) = 0;
    *(uint64_t*)(effect + 0x220) = 0;
    *(uint64_t*)(effect + 0x228) = 0;
    *(uint32_t*)(effect + 0x208) = 0;
    *(uint32_t*)(effect + 0x210) = 0;
}

void Shot_ReleaseFreshEmbeddedEffectGraph(void* shot)
{
    constexpr uint32_t EFFECT_OFFSET = 0x2160;
    constexpr uint32_t EFFECT_SIZE = 0x230;
    if (!shot || !HasCallableDtiVirtual(shot)) return;
    if (DecodeDtiSize(GetObjectDti((uint64_t)shot)) < EFFECT_OFFSET + EFFECT_SIZE)
        return;
    void* effect = (uint8_t*)shot + EFFECT_OFFSET;
    if (IsEffectObject(effect))
        ((void(__fastcall*)(void*))_addr(0x1405E6890))(effect);
}

int Shot_SanitizeCollisionSlotsForDestruction(void* shot)
{
    if (!shot || !IsAccessiblePointer((uint64_t)shot + 0x2078)) return 0;
    int sanitized = 0;
    auto* bytes = (uint8_t*)shot;
    for (uint32_t index = 0; index < 4; ++index) {
        uint64_t slot = *(uint64_t*)(bytes + 0x2058 + index * sizeof(uint64_t));
        if (!slot || !IsAccessiblePointer(slot + 0x38)) continue;
        uint32_t count = *(uint32_t*)(slot + 0x20);
        uint32_t capacity = *(uint32_t*)(slot + 0x24);
        uint64_t entries = *(uint64_t*)(slot + 0x30);
        bool malformed = count > capacity || count > 128 ||
            (count != 0 && !IsAccessiblePointer(entries)) ||
            (entries != 0 && !IsAccessiblePointer(entries));
        if (!malformed) continue;
        std::memset((void*)(slot + 0x20), 0, 0x18);
        ++sanitized;
    }
    return sanitized;
}

int Shot_SanitizeUnitCategoryLists(void* sUnit, uint32_t firstCategory,
                                   uint32_t lastCategory)
{
    if (!sUnit || !IsAccessiblePointer((uint64_t)sUnit)) return 0;

    struct ValidatedUnit {
        void* unit;
        bool valid;
    };
    std::vector<ValidatedUnit> validatedUnits;
    validatedUnits.reserve(128);

    auto isValidUnit = [&](void* unit) {
        for (const ValidatedUnit& cached : validatedUnits) {
            if (cached.unit == unit) return cached.valid;
        }

        bool valid = IsUnitObject(unit);
        if (valid) {
            uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
            if ((state & 7u) == 2u && (state & 0x400u) != 0) {
                uint64_t vtable = *(uint64_t*)unit;
                valid = IsAccessibleAddress(vtable + 0x40) &&
                        IsExecutableAddress(*(uint64_t*)(vtable + 0x40));
            }
        }
        validatedUnits.push_back({unit, valid});
        return valid;
    };

    auto logInvalidUnit = [&](uint32_t category, void* previous, void* unit,
                              void* next) {
        uint64_t address = (uint64_t)unit;
        uint64_t vtable = IsAccessibleRange(address, sizeof(uint64_t))
            ? *(uint64_t*)unit : 0;
        uint64_t dti = vtable && HasCallableDtiVirtual(unit)
            ? GetObjectDti(address) : 0;
        uint32_t state = IsAccessibleRange(address + 0x10, sizeof(uint32_t))
            ? *(uint32_t*)(address + 0x10) : 0;
        uint64_t owner = 0;
        uint64_t ownerDti = 0;
        size_t ownerOffset = 0;

        uint32_t totalCategories = *(uint32_t*)((uint8_t*)sUnit +
                                                SUNIT_CATEGORY_COUNT_OFFSET);
        if (totalCategories > 8) {
            uint64_t character = *(uint64_t*)((uint8_t*)sUnit +
                SUNIT_CATEGORY_LISTS_OFFSET + SUNIT_CATEGORY_LIST_STRIDE * 8);
            for (int safety = 64; character && safety-- > 0;) {
                if (!IsAccessibleRange(character, 0x20)) break;
                uint64_t characterDti = HasCallableDtiVirtual((void*)character)
                    ? GetObjectDti(character) : 0;
                uint32_t characterSize = DecodeDtiSize(characterDti);
                if (characterSize >= 8 && characterSize <= 0x40000 &&
                    address >= character && address < character + characterSize) {
                    owner = character;
                    ownerDti = characterDti;
                    ownerOffset = (size_t)(address - character);
                    break;
                }
                character = *(uint64_t*)(character + 0x18);
            }
        }

        HookLog("Rollback: invalid sUnit category=%u previous=0x%llX node=0x%llX "
                "next=0x%llX vtable=0x%llX dti=0x%llX state=0x%08X "
                "containingCharacter=0x%llX ownerDti=0x%llX ownerOffset=0x%zX\n",
                category, (uint64_t)previous, address, (uint64_t)next,
                vtable, dti, state, owner, ownerDti, ownerOffset);
    };

    uint32_t categoryCount =
        *(uint32_t*)((uint8_t*)sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    if (categoryCount > SUNIT_MAX_CATEGORIES)
        categoryCount = SUNIT_MAX_CATEGORIES;
    if (firstCategory >= categoryCount || firstCategory > lastCategory)
        return 0;
    if (lastCategory >= categoryCount)
        lastCategory = categoryCount - 1;

    uint32_t maskCount =
        *(uint32_t*)((uint8_t*)sUnit + SUNIT_MULTI_MASK_COUNT_OFFSET);
    if (maskCount > SUNIT_MAX_CATEGORIES)
        maskCount = SUNIT_MAX_CATEGORIES;

    int removed = 0;
    for (uint32_t category = firstCategory; category <= lastCategory; ++category) {
        auto* head = (void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                              SUNIT_CATEGORY_LIST_STRIDE * category);
        auto* tail = head + 1;
        void* previous = nullptr;
        void* node = *head;
        int safety = 4096;

        while (node && safety-- > 0) {
            if (!IsAccessibleAddress((uint64_t)node + 0x20)) {
                logInvalidUnit(category, previous, node, nullptr);
                if (previous)
                    *(void**)((uint8_t*)previous + 0x18) = nullptr;
                else
                    *head = nullptr;
                *tail = previous;
                ++removed;
                break;
            }

            void* next = *(void**)((uint8_t*)node + 0x18);
            if (isValidUnit(node)) {
                previous = node;
                node = next;
                continue;
            }

            logInvalidUnit(category, previous, node, next);

            for (uint32_t mask = 0; mask < maskCount; ++mask) {
                uint8_t* array = (uint8_t*)sUnit + SUNIT_MULTI_MASK_ARRAYS_OFFSET +
                                 SUNIT_CATEGORY_LIST_STRIDE * mask;
                uint32_t* count = (uint32_t*)(array + 0x08);
                uint32_t capacity = *(uint32_t*)(array + 0x0C);
                void** entries = *(void***)(array + 0x18);
                if (*count > capacity || !entries ||
                    !IsAccessibleAddress((uint64_t)entries +
                                         (size_t)capacity * sizeof(void*))) {
                    continue;
                }

                for (uint32_t i = 0; i < *count;) {
                    if (entries[i] != node) {
                        ++i;
                        continue;
                    }
                    for (uint32_t j = i + 1; j < *count; ++j)
                        entries[j - 1] = entries[j];
                    entries[--*count] = nullptr;
                }
            }

            if (previous)
                *(void**)((uint8_t*)previous + 0x18) = next;
            else
                *head = next;

            if (next && IsAccessibleAddress((uint64_t)next + 0x20))
                *(void**)((uint8_t*)next + 0x20) = previous;
            else if (next) {
                next = nullptr;
                if (previous)
                    *(void**)((uint8_t*)previous + 0x18) = nullptr;
                else
                    *head = nullptr;
            }

            if (*tail == node || !next)
                *tail = previous;

            ++removed;
            node = next;
        }

        if (node && safety <= 0) {
            if (previous)
                *(void**)((uint8_t*)previous + 0x18) = nullptr;
            else
                *head = nullptr;
            *tail = previous;
        }
    }
    return removed;
}

void Shot_RemoveFromAllMultiMaskArrays(void* sUnit, void* unit)
{
    if (!sUnit || !unit) return;

    uint32_t maskCount =
        *(uint32_t*)((uint8_t*)sUnit + SUNIT_MULTI_MASK_COUNT_OFFSET);
    if (maskCount > SUNIT_MAX_CATEGORIES)
        maskCount = SUNIT_MAX_CATEGORIES;

    for (uint32_t mask = 0; mask < maskCount; ++mask) {
        uint8_t* array = (uint8_t*)sUnit + SUNIT_MULTI_MASK_ARRAYS_OFFSET +
                         SUNIT_CATEGORY_LIST_STRIDE * mask;
        uint32_t* count = (uint32_t*)(array + 0x08);
        uint32_t capacity = *(uint32_t*)(array + 0x0C);
        void** entries = *(void***)(array + 0x18);
        if (*count > capacity || !entries ||
            !IsAccessibleAddress((uint64_t)entries +
                                 (size_t)capacity * sizeof(void*))) {
            continue;
        }

        for (uint32_t i = 0; i < *count;) {
            if (entries[i] != unit) {
                ++i;
                continue;
            }
            for (uint32_t j = i + 1; j < *count; ++j)
                entries[j - 1] = entries[j];
            entries[--*count] = nullptr;
        }
    }

    *(uint64_t*)((uint8_t*)unit + 0x30) = 0;
}
