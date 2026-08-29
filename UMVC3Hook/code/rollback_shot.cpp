#include "../pch.h"

#include "rollback_shot.h"

#include "rollback_memory.h"
#include "rollback_effect.h"
#include "rollback.h"
#include "rollback_model_policy.h"
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

static_assert(sizeof(RollbackRegion) <= 64);

static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SUNIT_MAX_CATEGORIES = 64;
static constexpr uint32_t SUNIT_MULTI_MASK_COUNT_OFFSET = 0x1840;
static constexpr uint32_t SUNIT_MULTI_MASK_ARRAYS_OFFSET = 0xC50;
static constexpr uintptr_t VTABLE_USHOT_VULCAN_ZIREITOU = 0x140AC0E20;
static constexpr uintptr_t DTI_USHOT_SHIELD_RAID_SWORD = 0x140D4D470;
static constexpr uintptr_t DTI_USHOT_SHIELD_SPIRAL_SWORD = 0x140D4D4C8;
static constexpr uintptr_t DTI_USHOT_SHIELD_STORM_SWORD = 0x140D4D508;
static constexpr uintptr_t PTR_SREPLAY = 0x140d4d9d8;
static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SUNIT = 0x140E17698;
static constexpr uintptr_t FN_SSHOT_SPAWN = 0x140150E00;
static constexpr uintptr_t FN_SSHOT_SPAWN_EX = 0x140150F40;
static constexpr uintptr_t FN_USHOT_PROCESS_HIT_COLLISION = 0x1401C6950;
static constexpr uintptr_t FN_USHOT_UPDATE_HITBOXES = 0x1401C9000;
static constexpr uintptr_t FN_BUILD_ATTACH_MATRIX = 0x14014F960;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr int DYNAMIC_SAFETY = 128;
static constexpr int SSHOT_SENTINEL_COUNT = 10;
static constexpr size_t SSHOT_NODE_OFFSET = 0x1450;
static constexpr size_t SSHOT_NODE_NEXT = 0x08;
static constexpr uint32_t SUNIT_CHARACTER_CATEGORY = 8;
static constexpr uintptr_t TEAM0_OFFSET = 0x40;
static constexpr uintptr_t TEAM1_OFFSET = 0x300;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;

using ShotSpawnFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*,
    void*, uint32_t, void*);
using ShotSpawnExFn = void*(__fastcall*)(void*, void*, uint64_t, void*, void*,
    void*, uint32_t, void*, int, void*, int, void*);
using UnitRemoveFn = char(__fastcall*)(void*, void*);
using BuildAttachMatrixFn = void(__fastcall*)(void*, void*, char);
using ShotProcessHitCollisionFn = void(__fastcall*)(void*, void*, void*);
using ShotUpdateHitboxesFn = void(__fastcall*)(void*);

static ShotSpawnFn oShotSpawn = nullptr;
static ShotSpawnExFn oShotSpawnEx = nullptr;
static UnitRemoveFn oUnitRemove = nullptr;
static BuildAttachMatrixFn oBuildAttachMatrix = nullptr;
static ShotProcessHitCollisionFn oShotProcessHitCollision = nullptr;
static ShotUpdateHitboxesFn oShotUpdateHitboxes = nullptr;
static std::unordered_map<void*, RollbackShotRecipe> g_liveShotRecipes;
static std::unordered_map<uint64_t, void*> g_liveShotsBySequence;
static std::unordered_set<void*> g_stableReferenceUnits;
static bool g_stableReferenceUnitsReady = false;
static uint64_t g_shotSpawnSequence = 0;
static uint32_t g_unitLinkRepairLogCount = 0;
static uint32_t g_staleHitTargetLogCount = 0;

static bool HasValidUnitLink(void* unit)
{
    // Link validation must not dispatch a DTI virtual. Some scheduler-owned
    // units use a base layout where vtable+0x20 is a deleting destructor, not
    // cUnit::getDTI. Reciprocal list checks below establish link identity.
    return !unit || IsAccessibleRange((uint64_t)unit, 0x28);
}

static void RepairUnitLinksBeforeRemoval(void* manager, void* unit)
{
    if (!manager || !unit) return;

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

static RollbackShotReference CaptureShotReference(void* reference)
{
    RollbackShotReference captured{};
    if (!reference) return captured;

    captured.savedPtr = reference;
    captured.expectedDti = GetObjectDti((uint64_t)reference);
    auto shot = g_liveShotRecipes.find(reference);
    if (shot != g_liveShotRecipes.end()) {
        captured.kind = RollbackShotReferenceKind::Shot;
        captured.shotSpawnSequence = shot->second.spawnSequence;
    } else {
        // The native factory stores these arguments as request-unit and target
        // references. Non-shot identities must therefore resolve through a
        // live unit registry rather than through their historical address.
        captured.kind = RollbackShotReferenceKind::StableUnit;
    }
    return captured;
}

static void BuildStableReferenceUnits();

void Shot_PrepareReferenceResolution(
    const std::vector<RollbackShotRecipe>& recipes)
{
    g_stableReferenceUnits.clear();
    g_stableReferenceUnitsReady = true;
    bool needsStableUnits = false;
    for (const RollbackShotRecipe& recipe : recipes) {
        if (recipe.objectRef1.kind == RollbackShotReferenceKind::StableUnit ||
            recipe.objectRef2.kind == RollbackShotReferenceKind::StableUnit) {
            needsStableUnits = true;
            break;
        }
    }
    if (!needsStableUnits) return;

    g_stableReferenceUnitsReady = false;
    BuildStableReferenceUnits();
}

static void BuildStableReferenceUnits()
{
    if (g_stableReferenceUnitsReady) return;
    g_stableReferenceUnitsReady = true;
    g_stableReferenceUnits.reserve(256);

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    for (int team = 0; sCharacter && team < 2; ++team) {
        uint64_t node = sCharacter + (team == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
            TEAM_FIRST_NODE_OFFSET;
        for (int safety = 16; node && safety-- > 0;) {
            void* character = *(void**)(node + NODE_CHR_OFFSET);
            if (character) g_stableReferenceUnits.insert(character);
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }

    uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
    if (!sUnit) return;
    uint32_t categoryCount = *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET);
    if (categoryCount > SUNIT_MAX_CATEGORIES)
        categoryCount = SUNIT_MAX_CATEGORIES;
    for (uint32_t category = 0; category < categoryCount; ++category) {
        void* unit = *(void**)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                               SUNIT_CATEGORY_LIST_STRIDE * category);
        for (int safety = 4096; unit && safety-- > 0;) {
            g_stableReferenceUnits.insert(unit);
            unit = *(void**)((uint8_t*)unit + 0x18);
        }
    }
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
        *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) <=
            SUNIT_CHARACTER_CATEGORY) {
        return false;
    }

    void* character = *(void**)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
        SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
    for (int safety = 128; character && safety-- > 0;) {
        if (character == target) return true;
        character = *(void**)((uint8_t*)character + 0x18);
    }
    return false;
}

static bool IsTerminalShot(void* shot)
{
    if (!shot || !IsActiveUnit(shot)) return false;
    uint32_t step = *(uint32_t*)((uint8_t*)shot + 0x212C);
    uint8_t flags = *(uint8_t*)((uint8_t*)shot + 0x1F64);
    if (step == 3 && (flags & 0x02) == 0) {
        uint64_t dti = GetObjectDti((uint64_t)shot);
        // Vergil's sword remains authoritative during cleanup. Its native
        // callback advances the local step and clears exactly one availability
        // bit; filtering it would force restore to guess that timing.
        if (dti == (uint64_t)_addr(DTI_USHOT_SHIELD_RAID_SWORD) ||
            dti == (uint64_t)_addr(DTI_USHOT_SHIELD_SPIRAL_SWORD) ||
            dti == (uint64_t)_addr(DTI_USHOT_SHIELD_STORM_SWORD)) {
            return false;
        }
    }
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
    recipe.objectRef1 = CaptureShotReference(objectRef1);
    recipe.slotId = slotId;
    recipe.objectRef2 = CaptureShotReference(objectRef2);
    recipe.kind = RollbackShotSpawnKind::Standard;
    recipe.spawnSequence = ++g_shotSpawnSequence;
    CapturePreSpawnRng(recipe, ownerChr);
    void* shot = oShotSpawn(manager, element, initParam, posData, ownerChr,
                            objectRef1, slotId, objectRef2);
    Rollback_AfterShotSpawn(shot, nullptr);
    InvalidateMemoryQueryCache();
    if (shot) {
        recipe.savedPtr = shot;
        recipe.restoredPtr = shot;
        g_liveShotRecipes[shot] = recipe;
        g_liveShotsBySequence[recipe.spawnSequence] = shot;
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
    recipe.objectRef1 = CaptureShotReference(objectRef1);
    recipe.slotId = slotId;
    recipe.objectRef2 = CaptureShotReference(objectRef2);
    recipe.kind = RollbackShotSpawnKind::Extended;
    recipe.exA9 = a9;
    recipe.exA10 = a10;
    recipe.exA11 = a11;
    recipe.exA12 = a12;
    recipe.spawnSequence = ++g_shotSpawnSequence;
    CapturePreSpawnRng(recipe, ownerChr);
    void* shot = oShotSpawnEx(manager, element, initParam, posData, ownerChr,
                              objectRef1, slotId, objectRef2, a9, a10, a11, a12);
    Rollback_AfterShotSpawn(shot, (void*)initParam);
    InvalidateMemoryQueryCache();
    if (shot) {
        recipe.savedPtr = shot;
        recipe.restoredPtr = shot;
        g_liveShotRecipes[shot] = recipe;
        g_liveShotsBySequence[recipe.spawnSequence] = shot;
    }
    return shot;
}

static bool ValidAttachDispatchObject(uint64_t object)
{
    if (!object || !HasCallableDtiVirtual((void*)object)) return false;
    uint64_t vtable = *(uint64_t*)object;
    return IsAccessibleAddress(vtable + 0x2C8) &&
           IsAccessibleAddress(vtable + 0x2D0) &&
           IsExecutableAddress(*(uint64_t*)(vtable + 0x2C8)) &&
           IsExecutableAddress(*(uint64_t*)(vtable + 0x2D0));
}

static bool ValidAttachMatrixSource(uint64_t object, int32_t jointIndex)
{
    if (!object) return true;
    if (jointIndex < 0) return HasCallableDtiVirtual((void*)object);

    // Indexed attachment dispatch expects a model/unit matrix callback. A
    // recycled particle generator can have an executable +0xA0 slot, but that
    // slot consumes RDX as a model pointer rather than a joint index.
    return IsBaseModelObject((void*)object) || IsUnitObject((void*)object) ||
           IsEffectObject((void*)object);
}

static void __fastcall hkBuildAttachMatrix(void* arm, void* output, char reset)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::BuildAttachMatrix);
    if (!arm || !output) return;
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
    int32_t secondaryJoint = *(int32_t*)(bytes + 0x88);
    int32_t tertiaryJoint = *(int32_t*)(bytes + 0x8C);
    if (ValidAttachDispatchObject(primary) &&
        ValidAttachMatrixSource(secondary, secondaryJoint) &&
        ValidAttachMatrixSource(tertiary, tertiaryJoint)) {
        oBuildAttachMatrix(arm, output, reset);
        return;
    }
    std::memcpy(output, bytes + 0x90, 0x40);
}

static void __fastcall hkShotUpdateHitboxes(void* shot)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ShotUpdateHitboxes);
    auto recipe = g_liveShotRecipes.find(shot);
    const RollbackShotRecipe* policyRecipe = recipe != g_liveShotRecipes.end()
        ? &recipe->second : nullptr;
    Rollback_BeforeShotHitboxUpdate(shot, policyRecipe);
    oShotUpdateHitboxes(shot);
    if (Rollback_AfterShotHitboxUpdate(shot, policyRecipe)) {
        oShotUpdateHitboxes(shot);
        ConcreteShotSnapshot_RebuildLiveCapsuleWorld(
            shot, recipe->second.concrete);
    }
    if (recipe != g_liveShotRecipes.end() && recipe->second.reconstructed &&
        Rollback_ShouldReapplyShotCapsules(recipe->second.concrete.dti)) {
        ConcreteShotSnapshot_ReapplyCapsules(shot, recipe->second.concrete);
    }
}

static void __fastcall hkShotProcessHitCollision(void* shot, void* target,
                                                  void* contact)
{
    auto recipe = g_liveShotRecipes.find(shot);
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

static char __fastcall hkUnitRemove(void* manager, void* unit)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::UnitRemove);
    decltype(g_liveShotRecipes)::iterator recipe;
    {
        RollbackHookProfileScope child(RollbackHookProfileId::UnitRemoveRecipeLookup);
        recipe = g_liveShotRecipes.find(unit);
    }
    const uint64_t recipeSequence = recipe != g_liveShotRecipes.end()
        ? recipe->second.spawnSequence : 0;
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
        if (!IsLinkedShot(unit)) {
            if (recipeSequence) g_liveShotsBySequence.erase(recipeSequence);
            g_liveShotRecipes.erase(unit);
        }
        Rollback_ForgetShotModelPolicy(unit);
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

bool Shot_GetSpawnSequence(void* shot, uint64_t& spawnSequence)
{
    auto found = g_liveShotRecipes.find(shot);
    if (found == g_liveShotRecipes.end()) return false;
    spawnSequence = found->second.spawnSequence;
    return true;
}

size_t Shot_GetRecipeCount()
{
    return g_liveShotRecipes.size();
}

void Shot_CollectCurrent(std::vector<void*>& shots)
{
    shots.clear();
    shots.reserve(g_liveShotRecipes.size());
    for (const auto& entry : g_liveShotRecipes) {
        if (IsLinkedShot(entry.first)) shots.push_back(entry.first);
    }
}

void* Shot_ResolveReference(const RollbackShotReference& reference)
{
    if (reference.kind == RollbackShotReferenceKind::None) return nullptr;
    if (reference.kind == RollbackShotReferenceKind::Shot) {
        auto shot = g_liveShotsBySequence.find(reference.shotSpawnSequence);
        return shot != g_liveShotsBySequence.end() ? shot->second : nullptr;
    }

    BuildStableReferenceUnits();
    if (g_stableReferenceUnits.find(reference.savedPtr) ==
        g_stableReferenceUnits.end()) return nullptr;
    if (reference.expectedDti &&
        GetObjectDti((uint64_t)reference.savedPtr) != reference.expectedDti) {
        return nullptr;
    }
    return reference.savedPtr;
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
    g_liveShotsBySequence[live.spawnSequence] = shot;
}

void Shot_Reset()
{
    g_liveShotRecipes.clear();
    g_liveShotsBySequence.clear();
    g_stableReferenceUnits.clear();
    g_stableReferenceUnitsReady = false;
    Rollback_ResetShotModelPolicy();
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
    Rollback_InitShotModelPolicyHooks();
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
    if (!shot) return;
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
    if (!shot) return 0;
    int sanitized = 0;
    auto* bytes = (uint8_t*)shot;
    for (uint32_t index = 0; index < 4; ++index) {
        uint64_t slot = *(uint64_t*)(bytes + 0x2058 + index * sizeof(uint64_t));
        if (!slot || !IsAccessiblePointer(slot + 0x38)) continue;
        uint32_t count = *(uint32_t*)(slot + 0x20);
        uint32_t capacity = *(uint32_t*)(slot + 0x24);
        uint64_t entries = *(uint64_t*)(slot + 0x30);
        bool malformed = count > capacity || count > 128 ||
            (count != 0 && !entries) ||
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
    if (!sUnit) return 0;

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
                uint64_t characterDti = GetObjectDti(character);
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
        if (*count > capacity || !entries) {
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
