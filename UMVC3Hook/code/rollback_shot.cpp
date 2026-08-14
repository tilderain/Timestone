#include "../pch.h"

#include "rollback_shot.h"

#include "rollback_memory.h"
#include "rollback_effect.h"
#include "rollback.h"
#include "rollback_profile.h"
#include "rollback_snapshot.h"
#include "mk10utils.h"
#include "../kiero/minhook/include/MinHook.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern void HookLog(const char* fmt, ...);
extern bool g_multiResimEnabled;

static_assert(sizeof(RollbackRegion) <= 64);

static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SUNIT_MAX_CATEGORIES = 64;
static constexpr uint32_t SUNIT_MULTI_MASK_COUNT_OFFSET = 0x1840;
static constexpr uint32_t SUNIT_MULTI_MASK_ARRAYS_OFFSET = 0xC50;
static constexpr uintptr_t VTABLE_USHOT_VULCAN_ZIREITOU = 0x140AC0E20;
static constexpr uintptr_t PTR_SREPLAY = 0x140d4d9d8;
static constexpr uintptr_t PTR_SUNIT = 0x140E17698;
static constexpr uintptr_t PTR_SSHOT = 0x140D47F40;
static constexpr uintptr_t PTR_SGAMECONFIG = 0x140D50F88;
static constexpr uintptr_t FN_SSHOT_SPAWN = 0x140150E00;
static constexpr uintptr_t FN_SSHOT_SPAWN_EX = 0x140150F40;
static constexpr uintptr_t FN_USHOT_PROCESS_HIT_COLLISION = 0x1401C6950;
static constexpr uintptr_t FN_USHOT_UPDATE_HITBOXES = 0x1401C9000;
static constexpr uintptr_t FN_USHOT_KILL_CALLBACKS = 0x1401C9A20;
static constexpr uintptr_t FN_USHOT_CLEAR_DEAD_CALLBACKS = 0x1401C9AB0;
static constexpr uintptr_t FN_BUILD_ATTACH_MATRIX = 0x14014F960;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr uintptr_t FN_KING_KOBUN_MOVE = 0x1401BA2D0;
static constexpr uintptr_t FN_MISSILE_EMPLACEMENT_MOVE = 0x14016F220;
static constexpr uintptr_t FN_WEAPON_CART_MOVE = 0x14017A9D0;
static constexpr uintptr_t FN_WEAPON_CART_SPAWN_DEBRIS = 0x14017A200;
static constexpr uintptr_t FN_SSHOT_FLUSH_PENDING_UNITS = 0x1401517A0;
static constexpr uintptr_t FN_CCHRNODE_SET_NEXT = 0x140003D50;
static constexpr uintptr_t FN_UMODEL_PROCESS_ACTION_ANMCHR = 0x140529420;
static constexpr uintptr_t FN_UMODEL_UPDATE_MOTION_EVALUATION = 0x140531A00;
static constexpr uintptr_t FN_CRESOURCE_ASSIGN_SLOT = 0x140529730;
static constexpr uintptr_t FN_CRESOURCE_RELEASE_REF = 0x14050D5A0;
static constexpr uintptr_t FN_SRESOURCE_GET_INSTANCE = 0x140001B10;
static constexpr uintptr_t FN_SRESOURCE_LOAD_SYNC = 0x140518BF0;
static constexpr uintptr_t DTI_USHOT_VULCAN_MISSILE_EMPLACEMENT = 0x140D49A60;
static constexpr uintptr_t DTI_USHOT_VULCAN_LEGION_TIGER = 0x140D4ABB8;
static constexpr uintptr_t DTI_USHOT_SHIELD_REPULSOR_BLAST_SP = 0x140D4ADA0;
static constexpr uintptr_t DTI_USHOT_VULCAN_WEAPON_CART = 0x140D4A1A8;
static constexpr uintptr_t DTI_USHOT_VULCAN_EXATK_ROCKET_LAUNCHER = 0x140D4BA48;
static constexpr uintptr_t DTI_USHOT_VULCAN_ROCKET_LAUNCHER_EXPLODE = 0x140D4BAC8;
static constexpr uintptr_t DTI_USHOT_VULCAN_PENDULUM_LOG = 0x140D4C4C8;
static constexpr int DYNAMIC_SAFETY = 128;
static constexpr int SSHOT_SENTINEL_COUNT = 10;
static constexpr size_t SSHOT_NODE_OFFSET = 0x1450;
static constexpr size_t SSHOT_NODE_NEXT = 0x08;
static constexpr uint32_t SUNIT_CHARACTER_CATEGORY = 8;
static constexpr size_t SSHOT_ID_POOL_OFFSET = 0x38;
static constexpr size_t SSHOT_ID_POOL_SIZE = 0x404;
static constexpr size_t SSHOT_PENDING_COUNT_OFFSET = 0x2448;
static constexpr size_t SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET = 0x4A89C;
static constexpr size_t SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE = 10;

using ShotSpawnFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*,
    void*, uint32_t, void*);
using ShotSpawnExFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*,
    void*, uint32_t, void*, int, void*, int, void*);
using UnitRemoveFn = char(__fastcall*)(void*, void*);
using BuildAttachMatrixFn = void(__fastcall*)(void*, void*, char);
using ShotProcessHitCollisionFn = void(__fastcall*)(void*, void*, void*);
using ShotUpdateHitboxesFn = void(__fastcall*)(void*);
using KingKobunMoveFn = void(__fastcall*)(void*);
using MissileEmplacementMoveFn = void(__fastcall*)(void*);
using WeaponCartMoveFn = void(__fastcall*)(void*);

static ShotSpawnFn oShotSpawn = nullptr;
static ShotSpawnExFn oShotSpawnEx = nullptr;
static UnitRemoveFn oUnitRemove = nullptr;
static BuildAttachMatrixFn oBuildAttachMatrix = nullptr;
static ShotProcessHitCollisionFn oShotProcessHitCollision = nullptr;
static ShotUpdateHitboxesFn oShotUpdateHitboxes = nullptr;
static void(__fastcall* oShotKillCallbacks)(void*, uint32_t) = nullptr;
static void(__fastcall* oShotClearDeadCallbacks)(void*) = nullptr;
static KingKobunMoveFn oKingKobunMove = nullptr;
static MissileEmplacementMoveFn oMissileEmplacementMove = nullptr;
static WeaponCartMoveFn oWeaponCartMove = nullptr;
static std::unordered_map<void*, RollbackShotRecipe> g_liveShotRecipes;
static std::unordered_set<void*> g_resimSpawnedPendulums;
static uint64_t g_shotSpawnSequence = 0;
static int g_kingKobunTraceCount = 0;
static uint32_t g_unitLinkRepairLogCount = 0;
static uint32_t g_staleHitTargetLogCount = 0;

// A reconstructed gate-2 transition may queue no child recipes. Replaying the
// native debris spawn later must leave every deterministic allocator unchanged.
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

static std::unordered_map<void*, DeferredWeaponCartDebris>
    g_deferredWeaponCartDebris;

static bool HasValidUnitLink(void* unit)
{
    // Link validation must not dispatch a DTI virtual. Some scheduler-owned
    // units use a base layout where vtable+0x20 is a deleting destructor, not
    // cUnit::getDTI. Reciprocal list checks below establish link identity.
    return !unit || IsAccessibleRange((uint64_t)unit, 0x28);
}

static void RepairUnitLinksBeforeRemoval(void* manager, void* unit)
{
    if (!manager || !unit ||
        !IsAccessibleRange((uint64_t)manager + SUNIT_CATEGORY_COUNT_OFFSET,
                           sizeof(uint32_t)) ||
        !IsAccessibleRange((uint64_t)unit, 0x28)) {
        return;
    }

    uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
    if ((state & 0x3F8u) == 0x3F8u) return;

    uint32_t category = (state >> 3) & 0x7Fu;
    uint32_t categoryCount = *(uint32_t*)((uint8_t*)manager +
                                         SUNIT_CATEGORY_COUNT_OFFSET);
    if (categoryCount > SUNIT_MAX_CATEGORIES)
        categoryCount = SUNIT_MAX_CATEGORIES;

    void* next = *(void**)((uint8_t*)unit + 0x18);
    void* previous = *(void**)((uint8_t*)unit + 0x20);
    bool linksValid = category < categoryCount && HasValidUnitLink(next) &&
        HasValidUnitLink(previous);
    if (linksValid) {
        auto* head = (void**)((uint8_t*)manager + SUNIT_CATEGORY_LISTS_OFFSET +
                              SUNIT_CATEGORY_LIST_STRIDE * category);
        auto* tail = head + 1;
        linksValid = previous
            ? *(void**)((uint8_t*)previous + 0x18) == unit
            : *head == unit;
        linksValid = linksValid && (next
            ? *(void**)((uint8_t*)next + 0x20) == unit
            : *tail == unit);
    }
    if (linksValid) return;

    void* foundPrevious = nullptr;
    void* foundNext = nullptr;
    uint32_t foundCategory = UINT32_MAX;
    for (uint32_t scanCategory = 0;
         scanCategory < categoryCount && foundCategory == UINT32_MAX;
         ++scanCategory) {
        void* scanPrevious = nullptr;
        void* node = *(void**)((uint8_t*)manager + SUNIT_CATEGORY_LISTS_OFFSET +
                               SUNIT_CATEGORY_LIST_STRIDE * scanCategory);
        for (int safety = 4096; node && safety-- > 0;) {
            if (!IsAccessibleRange((uint64_t)node, 0x20)) break;
            void* scanNext = *(void**)((uint8_t*)node + 0x18);
            if (node == unit) {
                foundPrevious = scanPrevious;
                foundNext = scanNext;
                foundCategory = scanCategory;
                break;
            }
            scanPrevious = node;
            node = scanNext;
        }
    }

    if (foundCategory != UINT32_MAX) {
        auto* foundHead = (void**)((uint8_t*)manager +
            SUNIT_CATEGORY_LISTS_OFFSET +
            SUNIT_CATEGORY_LIST_STRIDE * foundCategory);
        auto* foundTail = foundHead + 1;
        if (!HasValidUnitLink(foundNext)) {
            // The forward link is already allocator debris. Truncate the
            // damaged suffix at this node so native unlink can safely repair
            // the reachable prefix and category tail.
            foundNext = nullptr;
            *foundTail = unit;
        }
        *(void**)((uint8_t*)unit + 0x18) = foundNext;
        *(void**)((uint8_t*)unit + 0x20) = foundPrevious;
        *(uint32_t*)((uint8_t*)unit + 0x10) =
            (state & ~0x3F8u) | (foundCategory << 3);
    } else {
        // The caller may immediately re-add a scheduler-created unit in a new
        // category. Mark an orphan as detached so native removeUnit is a no-op.
        *(void**)((uint8_t*)unit + 0x18) = nullptr;
        *(void**)((uint8_t*)unit + 0x20) = nullptr;
        *(uint32_t*)((uint8_t*)unit + 0x10) = state | 0x3F8u;
    }

    if (g_unitLinkRepairLogCount++ < 64) {
        HookLog("Rollback: repaired corrupt sUnit links unit=0x%llX "
                "state=0x%08X category=%u next=0x%llX previous=0x%llX "
                "foundCategory=%u foundNext=0x%llX foundPrevious=0x%llX "
                "caller=0x%llX\n",
                (uint64_t)unit, state, category, (uint64_t)next,
                (uint64_t)previous, foundCategory, (uint64_t)foundNext,
                (uint64_t)foundPrevious,
                (uint64_t)__builtin_return_address(0));
    }
}

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

static bool IsCurrentCharacterCollisionTarget(void* target)
{
    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
    if (!target || !sUnit ||
        !IsAccessibleRange(sUnit + SUNIT_CATEGORY_COUNT_OFFSET,
                           sizeof(uint32_t)) ||
        *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) <=
            SUNIT_CHARACTER_CATEGORY) {
        return false;
    }

    void* character = *(void**)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
        SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
    for (int safety = 128; character && safety-- > 0;) {
        if (character == target) return true;
        if (!IsAccessibleRange((uint64_t)character, 0x20)) return false;
        character = *(void**)((uint8_t*)character + 0x18);
    }
    return false;
}

static bool IsTerminalShot(void* shot)
{
    if (!shot || !IsActiveUnit(shot)) return false;
    uint32_t step = *(uint32_t*)((uint8_t*)shot + 0x212C);
    uint8_t flags = *(uint8_t*)((uint8_t*)shot + 0x1F64);
    // Step 3 is uShot's cleanup branch. Once native hit judgment is disabled,
    // reconstructing the still-linked object can resurrect stale collisions.
    return step == 3 && (flags & 0x02) == 0;
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

static void TraceResimShotSpawn(const char* kind,
                                const RollbackShotRecipe& recipe,
                                void* shot,
                                void* caller)
{
    if (!g_activeRollbackProfile) return;

    uint32_t ownerAfter[4] = {};
    if (recipe.ownerChr) {
        std::memcpy(ownerAfter, (uint8_t*)recipe.ownerChr + 0x2D80,
                    sizeof(ownerAfter));
    }
    uint64_t elementDti = recipe.shotElement &&
        HasCallableDtiVirtual(recipe.shotElement)
        ? GetObjectDti((uint64_t)recipe.shotElement) : 0;
    uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
    int32_t frame = replay ? *(int32_t*)(replay + 0x54) : -1;
    const uint32_t* ownerBefore =
        reinterpret_cast<const uint32_t*>(recipe.preSpawnOwnerRng);
    HookLog("SHOTSPAWNTRACE frame=%d kind=%s sequence=%llu caller=0x%llX "
            "element=0x%llX elementDti=0x%llX owner=0x%llX slot=%u result=0x%llX "
            "ownerRng=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X\n",
            frame, kind, recipe.spawnSequence, (uint64_t)caller,
            (uint64_t)recipe.shotElement, elementDti, (uint64_t)recipe.ownerChr,
            recipe.slotId, (uint64_t)shot,
            ownerBefore[0], ownerBefore[1], ownerBefore[2], ownerBefore[3],
            ownerAfter[0], ownerAfter[1], ownerAfter[2], ownerAfter[3]);

    constexpr uintptr_t DTI_USHOT_SHIELD_KIKOUSYOU = 0x140D48F70;
    if (shot && HasCallableDtiVirtual(shot) &&
        GetObjectDti((uint64_t)shot) ==
            (uint64_t)_addr(DTI_USHOT_SHIELD_KIKOUSYOU)) {
        void* callback1 = *(void**)((uint8_t*)shot + 0x1F78);
        void* callback2 = *(void**)((uint8_t*)shot + 0x1F80);
        HookLog("KIKOSHOSPAWNRESULT frame=%d sequence=%llu shot=0x%llX "
                "slot1=0x%llX slot2=0x%llX\n",
                frame, recipe.spawnSequence, (uint64_t)shot,
                (uint64_t)callback1, (uint64_t)callback2);
    }
}

static void RepairResimRepulsorBlastMotion(void* shot)
{
    if (!g_isResimulating || !shot || !HasCallableDtiVirtual(shot) ||
        GetObjectDti((uint64_t)shot) !=
            (uint64_t)_addr(DTI_USHOT_SHIELD_REPULSOR_BLAST_SP) ||
        !IsAccessibleRange((uint64_t)shot + 0x558, sizeof(uint32_t))) {
        return;
    }

    uint32_t jointCount = *(uint32_t*)((uint8_t*)shot + 0x530);
    uint64_t joints = *(uint64_t*)((uint8_t*)shot + 0x538);
    uint32_t motionStatus = *(uint32_t*)((uint8_t*)shot + 0x550);
    if (motionStatus || jointCount == 0 || jointCount > 256 || !joints ||
        !IsAccessibleRange(joints, (size_t)jointCount * 0xC0)) {
        return;
    }

    // Forced re-simulation can create the ball after its asynchronous model is
    // ready but before setup successfully binds motion 0. Retry only that native
    // bind; the normal model pass advances and evaluates it later this tick.
    ((uint64_t(__fastcall*)(void*, uint32_t, uint32_t, float, float, float,
                           uint32_t))
        _addr(FN_UMODEL_PROCESS_ACTION_ANMCHR))(
            shot, 0, 0, 0.0f, 0.0f, 1.0f, 0);
}

void Shot_RepairNemesisExplosionAttack(void* shot, void* parent)
{
    uint64_t shotDti = shot && HasCallableDtiVirtual(shot)
        ? GetObjectDti((uint64_t)shot) : 0;
    uint64_t parentDti = parent && HasCallableDtiVirtual(parent)
        ? GetObjectDti((uint64_t)parent) : 0;
    if (!shot || !parent || !HasCallableDtiVirtual(shot) ||
        !HasCallableDtiVirtual(parent) ||
        shotDti != (uint64_t)_addr(DTI_USHOT_VULCAN_ROCKET_LAUNCHER_EXPLODE) ||
        parentDti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_EXATK_ROCKET_LAUNCHER) ||
        !IsAccessibleRange((uint64_t)shot + 0x1498, sizeof(uint32_t)) ||
        !IsAccessibleRange((uint64_t)parent + 0x1498, sizeof(uint32_t))) {
        return;
    }

    uint32_t& attackInfoId = *(uint32_t*)((uint8_t*)shot + 0x1498);
    uint32_t parentAttackInfoId =
        *(uint32_t*)((uint8_t*)parent + 0x1498);
    if (attackInfoId == 0 && parentAttackInfoId != 0) {
        // Native BIOWEAPON ASSAULT creation inherits attack 1 from the rocket.
        // A rollback-created explosion can retain the factory default and apply
        // the attack-0 air launch, moving its target 13.333 units farther.
        attackInfoId = parentAttackInfoId;
    }
}

static bool PendulumMotionReady(void* shot)
{
    if (!shot || !IsAccessibleRange((uint64_t)shot + 0x6A0, 1)) return false;
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

static void ProcessPendulumMotionIfReady(void* shot)
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

static void EvaluatePendulumPose(void* shot)
{
    if (*(float*)((uint8_t*)shot + 0x1F58) <= 3.0f) return;
    ((void(__fastcall*)(void*))_addr(
        FN_UMODEL_UPDATE_MOTION_EVALUATION))(shot);
    uint64_t vtable = *(uint64_t*)shot;
    if (!IsAccessibleRange(vtable + 0x90, 0x10)) return;
    uint64_t updateWorld = *(uint64_t*)(vtable + 0x90);
    uint64_t updateChildren = *(uint64_t*)(vtable + 0x98);
    if (IsExecutableAddress(updateWorld)) {
        ((void(__fastcall*)(void*))updateWorld)(shot);
    }
    if (IsExecutableAddress(updateChildren)) {
        ((void(__fastcall*)(void*))updateChildren)(shot);
    }
    float currentFrame = *(float*)((uint8_t*)shot + 0x1F58);
    *(float*)((uint8_t*)shot + 0x5AC) = currentFrame;
    *(float*)((uint8_t*)shot + 0x5B0) = std::max(currentFrame - 1.0f, 0.0f);
}

static void LoadPendulumMotionSynchronously(void* shot)
{
    if (!shot || !IsAccessibleRange((uint64_t)shot + 0x11C0, 1)) return;
    void* pending = *(void**)((uint8_t*)shot + 0x11B8);
    if (!pending || !HasCallableDtiVirtual(pending) ||
        !IsAccessibleRange((uint64_t)pending, 0x78)) {
        return;
    }
    uint64_t table = *(uint64_t*)((uint8_t*)pending + 0x70);
    if (table && IsAccessibleRange(table, 0x10) &&
        *(uint16_t*)(table + 0x06) != 0 && *(uint64_t*)(table + 0x08) != 0) {
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

static void RepairResimPendulumModel(void* shot)
{
    auto liveRecipe = g_liveShotRecipes.find(shot);
    bool reconstructed = liveRecipe != g_liveShotRecipes.end() &&
        liveRecipe->second.reconstructed;
    bool resimSpawned = g_resimSpawnedPendulums.find(shot) !=
        g_resimSpawnedPendulums.end();
    uint64_t observedDti = shot && HasCallableDtiVirtual(shot)
        ? GetObjectDti((uint64_t)shot) : 0;
    if ((!g_isResimulating && !reconstructed && !resimSpawned) || !shot ||
        !HasCallableDtiVirtual(shot) ||
        observedDti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_PENDULUM_LOG)) {
        return;
    }

    // Resolve motion-list resource slot 0 through the same native action path
    // used by rendered model updates. Headless re-simulation does not dispatch
    // the pending mAnmNoReq that normally performs this bind.
    ProcessPendulumMotionIfReady(shot);
    if (PendulumMotionReady(shot)) {
        EvaluatePendulumPose(shot);
        return;
    }
    LoadPendulumMotionSynchronously(shot);
    ProcessPendulumMotionIfReady(shot);
    if (PendulumMotionReady(shot)) {
        EvaluatePendulumPose(shot);
        return;
    }

    uint64_t vtable = *(uint64_t*)shot;
    uint64_t setupAddress = IsAccessibleRange(vtable + 0x28, sizeof(uint64_t))
        ? *(uint64_t*)(vtable + 0x28) : 0;
    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
    if (!setupAddress || !IsExecutableAddress(setupAddress) || !sUnit) return;

    // Resource completion is dispatched between simulation ticks. Retry once
    // per tick rather than blocking here; a sleep loop prevents the motion
    // request from reaching its completion callback.
    void* oldCallbacks[2] = {
        *(void**)((uint8_t*)shot + 0x1F78),
        *(void**)((uint8_t*)shot + 0x1F80),
    };
    ((void(__fastcall*)(void*))setupAddress)(shot);
    InvalidateMemoryQueryCache();
    ProcessPendulumMotionIfReady(shot);
    if (PendulumMotionReady(shot)) EvaluatePendulumPose(shot);
    for (uint32_t index = 0; index < 2; ++index) {
        void* replacement = *(void**)((uint8_t*)shot + 0x1F78 + index * 8);
        if (oldCallbacks[index] && replacement != oldCallbacks[index]) {
            Effect_RetireOne((void*)sUnit, oldCallbacks[index]);
        }
    }
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
    RepairResimRepulsorBlastMotion(shot);
    RepairResimPendulumModel(shot);
    TraceResimShotSpawn("standard", recipe, shot,
                        __builtin_return_address(0));
    InvalidateMemoryQueryCache();
    if (shot) {
        if (g_isResimulating && HasCallableDtiVirtual(shot) &&
            GetObjectDti((uint64_t)shot) ==
                (uint64_t)_addr(DTI_USHOT_VULCAN_PENDULUM_LOG)) {
            g_resimSpawnedPendulums.insert(shot);
        }
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
    RepairResimRepulsorBlastMotion(shot);
    RepairResimPendulumModel(shot);
    Shot_RepairNemesisExplosionAttack(shot, (void*)initParam);
    TraceResimShotSpawn("extended", recipe, shot,
                        __builtin_return_address(0));
    InvalidateMemoryQueryCache();
    if (shot) {
        if (g_isResimulating && HasCallableDtiVirtual(shot) &&
            GetObjectDti((uint64_t)shot) ==
                (uint64_t)_addr(DTI_USHOT_VULCAN_PENDULUM_LOG)) {
            g_resimSpawnedPendulums.insert(shot);
        }
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
    RepairResimPendulumModel(shot);
    oShotUpdateHitboxes(shot);
    auto recipe = g_liveShotRecipes.find(shot);
    bool pendulum = shot && HasCallableDtiVirtual(shot) &&
        GetObjectDti((uint64_t)shot) ==
            (uint64_t)_addr(DTI_USHOT_VULCAN_PENDULUM_LOG) &&
        (g_isResimulating ||
         (recipe != g_liveShotRecipes.end() &&
          recipe->second.reconstructed));
    if (pendulum) {
        RepairResimPendulumModel(shot);
        oShotUpdateHitboxes(shot);
        ConcreteShotSnapshot_RebuildLiveCapsuleWorld(
            shot, recipe->second.concrete);
    }
    if (recipe != g_liveShotRecipes.end() && recipe->second.reconstructed &&
        recipe->second.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_LEGION_TIGER) &&
        recipe->second.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_SHIELD_REPULSOR_BLAST_SP) &&
        recipe->second.concrete.dti !=
            (uint64_t)_addr(DTI_USHOT_VULCAN_PENDULUM_LOG)) {
        ConcreteShotSnapshot_ReapplyCapsules(shot, recipe->second.concrete);
    }
}

static void __fastcall hkShotProcessHitCollision(void* shot, void* target,
                                                  void* contact)
{
    auto recipe = g_liveShotRecipes.find(shot);
    if (recipe != g_liveShotRecipes.end() &&
        (g_isResimulating || recipe->second.reconstructed)) {
        // The explosion factory can finalize cAtkCtrl after reconstruction's
        // initial repair. Reapply the parent's attack variant at the last
        // gameplay-safe point before the hit response consumes mAtkInfoId.
        Shot_RepairNemesisExplosionAttack(
            shot, (void*)recipe->second.initParam);
    }
    if (recipe != g_liveShotRecipes.end() && recipe->second.reconstructed &&
        !IsCurrentCharacterCollisionTarget(target)) {
        uint64_t dti = target && HasCallableDtiVirtual(target)
            ? GetObjectDti((uint64_t)target) : 0;
        uint32_t state = target &&
            IsAccessibleRange((uint64_t)target + 0x10, sizeof(uint32_t))
            ? *(uint32_t*)((uint8_t*)target + 0x10) : 0;
        uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
        int32_t frame = replay ? *(int32_t*)(replay + 0x54) : -1;
        if (g_staleHitTargetLogCount++ < 64) {
            HookLog("Rollback: rejected stale shot hit target frame=%d "
                    "shot=0x%llX target=0x%llX dti=0x%llX state=0x%08X\n",
                    frame, (uint64_t)shot, (uint64_t)target, dti, state);
        }
        return;
    }
    oShotProcessHitCollision(shot, target, contact);
}

static void __fastcall hkShotKillCallbacks(void* shot, uint32_t slot)
{
    constexpr uintptr_t DTI_USHOT_SHIELD_KIKOUSYOU = 0x140D48F70;
    void* before[2] = {};
    uint32_t states[2] = {};
    const bool trace = shot && HasCallableDtiVirtual(shot) &&
        GetObjectDti((uint64_t)shot) ==
            (uint64_t)_addr(DTI_USHOT_SHIELD_KIKOUSYOU);
    if (trace) {
        for (uint32_t index = 0; index < 2; ++index) {
            before[index] = *(void**)((uint8_t*)shot + 0x1F78 + index * 8);
            if (before[index] &&
                IsAccessibleRange((uint64_t)before[index] + 0x10, 4)) {
                states[index] =
                    *(uint32_t*)((uint8_t*)before[index] + 0x10);
            }
        }
    }

    void* caller = __builtin_return_address(0);
    oShotKillCallbacks(shot, slot);

    if (trace) {
        void* after0 = *(void**)((uint8_t*)shot + 0x1F78);
        void* after1 = *(void**)((uint8_t*)shot + 0x1F80);
        if (before[0] != after0 || before[1] != after1) {
            uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
            int32_t frame = replay ? *(int32_t*)(replay + 0x54) : -1;
            HookLog("KIKOSHOEFFECTKILL frame=%d resim=%d caller=0x%llX "
                    "shot=0x%llX requested=%u "
                    "slot1=0x%llX->0x%llX state1=0x%08X "
                    "slot2=0x%llX->0x%llX state2=0x%08X\n",
                    frame, g_isResimulating ? 1 : 0, (uint64_t)caller,
                    (uint64_t)shot, slot,
                    (uint64_t)before[0], (uint64_t)after0, states[0],
                    (uint64_t)before[1], (uint64_t)after1, states[1]);
        }
    }
}

static void __fastcall hkShotClearDeadCallbacks(void* shot)
{
    constexpr uintptr_t DTI_USHOT_SHIELD_KIKOUSYOU = 0x140D48F70;
    void* before[2] = {};
    uint32_t states[2] = {};
    const bool trace = shot && HasCallableDtiVirtual(shot) &&
        GetObjectDti((uint64_t)shot) ==
            (uint64_t)_addr(DTI_USHOT_SHIELD_KIKOUSYOU);
    if (trace) {
        for (uint32_t index = 0; index < 2; ++index) {
            before[index] = *(void**)((uint8_t*)shot + 0x1F78 + index * 8);
            if (before[index] && IsAccessibleRange((uint64_t)before[index] + 0x10, 4))
                states[index] = *(uint32_t*)((uint8_t*)before[index] + 0x10);
        }
    }

    oShotClearDeadCallbacks(shot);

    if (trace) {
        void* after0 = *(void**)((uint8_t*)shot + 0x1F78);
        void* after1 = *(void**)((uint8_t*)shot + 0x1F80);
        if (before[0] != after0 || before[1] != after1) {
            uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
            int32_t frame = replay ? *(int32_t*)(replay + 0x54) : -1;
            HookLog("KIKOSHOEFFECTCLEAR frame=%d resim=%d shot=0x%llX "
                    "slot1=0x%llX->0x%llX state1=0x%08X "
                    "slot2=0x%llX->0x%llX state2=0x%08X\n",
                    frame, g_isResimulating ? 1 : 0, (uint64_t)shot,
                    (uint64_t)before[0], (uint64_t)after0, states[0],
                    (uint64_t)before[1], (uint64_t)after1, states[1]);
        }
    }
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

static bool IsReconstructedMissileEmplacement(void* shot)
{
    auto recipe = g_liveShotRecipes.find(shot);
    return recipe != g_liveShotRecipes.end() && recipe->second.reconstructed &&
        HasCallableDtiVirtual(shot) &&
        GetObjectDti((uint64_t)shot) ==
             (uint64_t)_addr(DTI_USHOT_VULCAN_MISSILE_EMPLACEMENT);
}

static void RetryMissileEmplacementSetup(void* shot)
{
    constexpr size_t CONCRETE_STATE_OFFSET = 0x2120;
    constexpr size_t CONCRETE_STATE_SIZE = 0x50;
    constexpr size_t MOTION_HEADER_OFFSET = 0x550;
    constexpr size_t MOTION_BASE = 0x560;
    constexpr size_t MOTION_STRIDE = 0x180;
    constexpr size_t MOTION_COUNT = 8;
    struct MotionPlaybackState {
        uint8_t state08[0x118];
        uint8_t state140[0x20];
        uint8_t state168[0x08];
    };
    uint8_t concreteState[CONCRETE_STATE_SIZE];
    uint8_t motionHeader[0x10];
    MotionPlaybackState motionState[MOTION_COUNT];
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
    uint64_t setup = IsAccessibleRange(vtable + 0x28, sizeof(uint64_t))
        ? *(uint64_t*)(vtable + 0x28) : 0;
    if (setup && IsExecutableAddress(setup)) {
        ((void(__fastcall*)(void*))setup)(shot);
        InvalidateMemoryQueryCache();
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
}

static bool MissileEmplacementJointsReady(void* shot)
{
    constexpr size_t JOINT_TABLE_OFFSET = 0x538;
    constexpr size_t JOINT_STRIDE = 0xC0;
    constexpr size_t JOINT_COUNT_OFFSET = 0x530;
    uint32_t jointCount = *(uint32_t*)((uint8_t*)shot + JOINT_COUNT_OFFSET);
    uint64_t joints = *(uint64_t*)((uint8_t*)shot + JOINT_TABLE_OFFSET);
    bool ready = jointCount > 0 && jointCount <= 256 && joints &&
        IsAccessibleRange(joints, JOINT_STRIDE * jointCount);
    for (uint32_t index = 0; ready && index < jointCount; ++index) {
        ready = HasCallableDtiVirtual(
            (void*)(joints + index * JOINT_STRIDE));
    }
    return ready;
}

static void __fastcall hkMissileEmplacementMove(void* shot)
{
    if (IsReconstructedMissileEmplacement(shot) &&
        !MissileEmplacementJointsReady(shot)) {
        RetryMissileEmplacementSetup(shot);
        if (!MissileEmplacementJointsReady(shot))
            return;
    }
    oMissileEmplacementMove(shot);
}

static bool IsReconstructedWeaponCart(void* shot)
{
    auto recipe = g_liveShotRecipes.find(shot);
    return recipe != g_liveShotRecipes.end() && recipe->second.reconstructed &&
        HasCallableDtiVirtual(shot) &&
        GetObjectDti((uint64_t)shot) ==
            (uint64_t)_addr(DTI_USHOT_VULCAN_WEAPON_CART);
}

static bool WeaponCartMotionReady(void* shot)
{
    return *(uint32_t*)((uint8_t*)shot + 0x550) != 0;
}

static void CaptureWeaponCartTransitionState(
    DeferredWeaponCartDebris& state, void* owner, bool post)
{
    // Capture both sides of the native transition so a deferred spawn can run
    // with its original inputs and then restore the already-observed outputs.
    uint8_t* globalRng = (uint8_t*)_addr(0x140D765D8);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    uint8_t* effectRng = sEffect ? (uint8_t*)sEffect + 0x3C : nullptr;
    void* sShot = *(void**)_addr(PTR_SSHOT);
    void* sGameConfig = *(void**)_addr(PTR_SGAMECONFIG);

    std::memcpy(post ? state.postGlobalRng : state.preGlobalRng,
                globalRng, 16);
    if (effectRng) {
        std::memcpy(post ? state.postEffectRng : state.preEffectRng,
                    effectRng, 16);
    }
    if (owner) {
        std::memcpy(post ? state.postOwnerRng : state.preOwnerRng,
                    (uint8_t*)owner + 0x2D80, 16);
    }
    if (sShot) {
        std::memcpy(post ? state.postShotIdPool : state.preShotIdPool,
                    (uint8_t*)sShot + SSHOT_ID_POOL_OFFSET,
                    SSHOT_ID_POOL_SIZE);
    }
    if (sGameConfig) {
        std::memcpy(post ? state.postShotSlotFlags : state.preShotSlotFlags,
                    (uint8_t*)sGameConfig +
                        SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET,
                    SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE);
    }
}

static void RestoreWeaponCartTransitionState(
    const DeferredWeaponCartDebris& state, void* owner, bool post)
{
    uint8_t* globalRng = (uint8_t*)_addr(0x140D765D8);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    uint8_t* effectRng = sEffect ? (uint8_t*)sEffect + 0x3C : nullptr;
    void* sShot = *(void**)_addr(PTR_SSHOT);
    void* sGameConfig = *(void**)_addr(PTR_SGAMECONFIG);

    std::memcpy(globalRng,
                post ? state.postGlobalRng : state.preGlobalRng, 16);
    if (effectRng) {
        std::memcpy(effectRng,
                    post ? state.postEffectRng : state.preEffectRng, 16);
    }
    if (owner) {
        std::memcpy((uint8_t*)owner + 0x2D80,
                    post ? state.postOwnerRng : state.preOwnerRng, 16);
    }
    if (sShot) {
        std::memcpy((uint8_t*)sShot + SSHOT_ID_POOL_OFFSET,
                    post ? state.postShotIdPool : state.preShotIdPool,
                    SSHOT_ID_POOL_SIZE);
    }
    if (sGameConfig) {
        std::memcpy((uint8_t*)sGameConfig +
                        SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET,
                    post ? state.postShotSlotFlags : state.preShotSlotFlags,
                    SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE);
    }
}

static void RecoverWeaponCartEndTransition(void* shot, void* owner,
                                           DeferredWeaponCartDebris& state)
{
    // Native gate-2 handling selects the explosion/return animation before it
    // creates debris; reconstruction must reproduce that omitted side effect.
    uint64_t vtable = *(uint64_t*)shot;
    uint64_t selectMotion = IsAccessibleRange(vtable + 0x190, sizeof(uint64_t))
        ? *(uint64_t*)(vtable + 0x190) : 0;
    if (selectMotion && IsExecutableAddress(selectMotion)) {
        uint32_t animation = *(uint8_t*)((uint8_t*)shot + 0x22F4) ? 7 : 3;
        ((void(__fastcall*)(void*, uint32_t))selectMotion)(shot, animation);
    }

    void* sShot = *(void**)_addr(PTR_SSHOT);
    if (!sShot) return;
    uint32_t* pendingCount = (uint32_t*)((uint8_t*)sShot +
                                         SSHOT_PENDING_COUNT_OFFSET);
    if (*pendingCount == 0) {
        // Run debris creation against pre-transition state, then put RNG and
        // allocator bookkeeping back at the post-transition values.
        RestoreWeaponCartTransitionState(state, owner, false);
        ((void(__fastcall*)(void*))_addr(FN_WEAPON_CART_SPAWN_DEBRIS))(shot);
        RestoreWeaponCartTransitionState(state, owner, true);
    }

    if (*pendingCount != 0) {
        ((void(__fastcall*)(void*))_addr(FN_SSHOT_FLUSH_PENDING_UNITS))(sShot);
        state.pending = false;
    }
}

static void __fastcall hkWeaponCartMove(void* shot)
{
    auto liveRecipe = g_liveShotRecipes.find(shot);
    bool reconstructed = liveRecipe != g_liveShotRecipes.end() &&
        liveRecipe->second.reconstructed && IsReconstructedWeaponCart(shot);
    void* owner = liveRecipe != g_liveShotRecipes.end()
        ? liveRecipe->second.ownerChr : nullptr;
    uint32_t beforeGate = *(uint32_t*)((uint8_t*)shot + 0x221C);
    // The owner caches the primary model independently of the shot. Refresh it
    // every move because forced reconstruction replaces that model each frame.
    if (reconstructed && owner &&
        IsAccessibleRange((uint64_t)shot + 0x2288, sizeof(void*)) &&
        IsAccessibleRange((uint64_t)owner + 0x6498, sizeof(void*))) {
        void* modelEffect = *(void**)((uint8_t*)shot + 0x2288);
        if (modelEffect && HasCallableDtiVirtual(modelEffect)) {
            ((void(__fastcall*)(void*, void*))_addr(FN_CCHRNODE_SET_NEXT))(
                (uint8_t*)owner + 0x6490, modelEffect);
        }
    }
    if (reconstructed && owner && beforeGate == 3) {
        auto deferred = g_deferredWeaponCartDebris.find(owner);
        if (deferred != g_deferredWeaponCartDebris.end() &&
            deferred->second.pending) {
            RecoverWeaponCartEndTransition(shot, owner, deferred->second);
        }
    }

    DeferredWeaponCartDebris transitionState{};
    bool captureTransition = reconstructed && owner && beforeGate == 2;
    size_t liveRecipeCountBefore = g_liveShotRecipes.size();
    if (captureTransition)
        CaptureWeaponCartTransitionState(transitionState, owner, false);
    if (reconstructed) {
        if (!WeaponCartMotionReady(shot)) return;

        // The primary cart effect normally raises this transition gate when
        // mTimer reaches 67, then advances it after the timer-122 model tick.
        // Forced reconstruction replaces that effect every frame, but the
        // authoritative shot timer remains available.
        constexpr size_t TIMER_OFFSET = 0x1F58;
        constexpr size_t TRANSITION_GATE_OFFSET = 0x221C;
        constexpr size_t LINEAR_SPEED_OFFSET = 0x2080;
        float timer = *(float*)((uint8_t*)shot + TIMER_OFFSET);
        if (timer >= 67.0f && timer < 122.0f) {
            *(uint32_t*)((uint8_t*)shot + TRANSITION_GATE_OFFSET) = 1;
            *(float*)((uint8_t*)shot + LINEAR_SPEED_OFFSET) = 0.0f;
        }
    }
    oWeaponCartMove(shot);
    uint32_t afterGate = *(uint32_t*)((uint8_t*)shot + 0x221C);
    if (captureTransition && afterGate == 3) {
        CaptureWeaponCartTransitionState(transitionState, owner, true);
        // New live recipes prove native debris creation succeeded. Otherwise
        // retain the transition snapshot for recovery on the next move.
        if (g_liveShotRecipes.size() == liveRecipeCountBefore) {
            transitionState.pending = true;
            g_deferredWeaponCartDebris[owner] = std::move(transitionState);
        } else {
            g_deferredWeaponCartDebris.erase(owner);
        }
    }
    if (reconstructed && *(float*)((uint8_t*)shot + 0x1F58) >= 122.0f &&
        *(uint32_t*)((uint8_t*)shot + 0x221C) == 1) {
        // The model effect normally performs this end-of-tick gate change.
        *(uint32_t*)((uint8_t*)shot + 0x221C) = 2;
        *(float*)((uint8_t*)shot + 0x2080) = 20.0f;
    }
    if (reconstructed && *(float*)((uint8_t*)shot + 0x1F58) >= 141.0f &&
        *(uint32_t*)((uint8_t*)shot + 0x221C) == 3 &&
        *(uint64_t*)((uint8_t*)shot + 0x568) != 0x68) {
        // Gate 3 expects the first motion record at its return-slide cursor.
        *(uint64_t*)((uint8_t*)shot + 0x558) = 0x20000044;
        *(uint64_t*)((uint8_t*)shot + 0x568) = 0x68;
        *(uint64_t*)((uint8_t*)shot + 0x570) = 0x44;
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
    RepairUnitLinksBeforeRemoval(manager, unit);
    char result;
    {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveNative);
        result = oUnitRemove(manager, unit);
    }
    InvalidateMemoryQueryCache();
    if (result && unit) {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveRecipeCleanup);
        if (!IsLinkedShot(unit)) g_liveShotRecipes.erase(unit);
        g_resimSpawnedPendulums.erase(unit);
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
    g_resimSpawnedPendulums.clear();
    g_deferredWeaponCartDebris.clear();
    g_shotSpawnSequence = 0;
    g_staleHitTargetLogCount = 0;
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
    InstallHook(FN_USHOT_PROCESS_HIT_COLLISION,
                (LPVOID)&hkShotProcessHitCollision,
                (void**)&oShotProcessHitCollision,
                "Rollback: hooked uShot hit collision validation",
                "Rollback: failed to hook uShot hit collision validation (%d)\n");
    /*InstallHook(FN_USHOT_KILL_CALLBACKS,
                (LPVOID)&hkShotKillCallbacks,
                (void**)&oShotKillCallbacks,
                "Rollback: tracing Kikosho callback effect kills",
                "Rollback: failed to trace Kikosho callback effect kills (%d)\n");
    InstallHook(FN_USHOT_CLEAR_DEAD_CALLBACKS,
                (LPVOID)&hkShotClearDeadCallbacks,
                (void**)&oShotClearDeadCallbacks,
                "Rollback: tracing Kikosho callback effect clears",
                "Rollback: failed to trace Kikosho callback effect clears (%d)\n");*/
    InstallHook(FN_KING_KOBUN_MOVE, (LPVOID)&hkKingKobunMove,
                (void**)&oKingKobunMove,
                "Rollback: hooked uShotVulcanKingKobun::move trace",
                "Rollback: failed to hook uShotVulcanKingKobun::move (%d)\n");
    InstallHook(FN_MISSILE_EMPLACEMENT_MOVE,
                (LPVOID)&hkMissileEmplacementMove,
                (void**)&oMissileEmplacementMove,
                "Rollback: hooked reconstructed missile emplacement model retry",
                "Rollback: failed to hook missile emplacement model retry (%d)\n");
    InstallHook(FN_WEAPON_CART_MOVE, (LPVOID)&hkWeaponCartMove,
                (void**)&oWeaponCartMove,
                "Rollback: hooked reconstructed Weapon Cart model retry",
                "Rollback: failed to hook Weapon Cart model retry (%d)\n");
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
    // +0x1F78..+0x1FDF holds 13 uEffect pointers owned by the fresh shot.
    // Rewind only the scalar fields around that array; restoring the retired
    // shot's entries makes native cleanup dispatch through stale sentinels.
    std::memcpy(bytes + 0x1F70, in.resourceScalars, 0x08);
    std::memcpy(bytes + 0x1FE0, in.resourceScalars + 0x70, 0x20);
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
    // The generator chain at +0x218 and particle-manager array at +0x220 are
    // owned allocations. Release them before detaching the stale embedded graph;
    // clearing these pointers first leaks every allocation on each reconstruction.
    Effect_ReleaseOwnedAllocations(effect);
    *(uint64_t*)(effect + 0x50) = 0;
    *(uint64_t*)(effect + 0x58) = 0;
    *(uint64_t*)(effect + 0x60) = 0;
    *(uint32_t*)(effect + 0x70) = 0xFFFFFFFFu;
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
