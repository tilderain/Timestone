#include "../pch.h"

#include "rollback_restore.h"
#include "rollback_cleanup.h"
#include "rollback_effect.h"
#include "rollback_memory.h"
#include "rollback_profile.h"
#include "rollback_shot.h"
#include "rollback_snapshot.h"
#include "concrete_snapshot.h"
#include "mk10utils.h"

#include <cmath>
#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SSHOT = 0x140d47f40;
static constexpr uintptr_t PTR_SHITSOLVER = 0x140d44060;
static constexpr uintptr_t PTR_SMAIN = 0x140e177e8;
static constexpr uintptr_t PTR_SMVC3MGR = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT = 0x140e17698;
static constexpr uintptr_t PTR_SCOLLISION = 0x140e18250;
static constexpr uintptr_t FN_SSHOT_ENQUEUE_UNIT = 0x1401512A0;
static constexpr uintptr_t FN_SSHOT_FLUSH_PENDING_UNITS = 0x1401517A0;
static constexpr uintptr_t FN_SUNIT_ADD_UNIT = 0x14051A6B0;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr uintptr_t FN_RESET_HITNODES = 0x140259E90;

static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uint32_t SACTION_MODEL_JOINT_WRAPPER_OFFSET = 0x120;
static constexpr uint32_t SACTION_MODEL_JOINT_ALLOCATION_OFFSET = 0x70;
static constexpr uint32_t SACTION_MODEL_JOINT_DATA_OFFSET = 0x10;
static constexpr size_t SACTION_MODEL_JOINT_DATA_SIZE = 0x70;
static constexpr size_t SACTION_MODEL_JOINT_ALLOCATION_SIZE =
    SACTION_MODEL_JOINT_DATA_OFFSET + SACTION_MODEL_JOINT_DATA_SIZE;
static constexpr uintptr_t TEAM0_OFFSET = 0x40;
static constexpr uintptr_t TEAM1_OFFSET = 0x300;
static constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t UCHRCPU_VTABLE = 0x140A87B08;
static constexpr uint32_t UCHRCPU_VM_STATE_OFFSET = 0xD0;
static constexpr uint32_t UCHRCPU_VM_STATE_SIZE = 0x1580;
static constexpr uint32_t CCHR_MOTION_BASE = 0x560;
static constexpr uint32_t CCHR_MOTION_STRIDE = 0x180;
static constexpr int CCHR_MOTION_COUNT = 8;
static constexpr uint32_t CCHR_DRAW_STATE_START = 0x80;
static constexpr uint32_t CCHR_DRAW_STATE_END = CCHR_MOTION_BASE;
static constexpr uint32_t CCHR_MODEL_TAIL_START = 0x1160;
static constexpr uint32_t CCHR_MODEL_TAIL_END = 0x1300;
static constexpr uint32_t CCHR_INPUT_OFFSET = 0x21C8;
static constexpr size_t CCHR_INPUT_SIZE = 0x5A0;
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SUNIT_CHARACTER_CATEGORY = 8;
static constexpr uint32_t CUNIT_LIVE_STATE_OFFSETS[] = {0x10, 0x18, 0x20, 0x38};
static constexpr size_t SMAIN_SIZE = 0x40080;
static constexpr int SSHOT_CATEGORY = 10;
static constexpr uint32_t SAUX_FIRST_CATEGORY = 9;
static constexpr uint32_t SSHOT_FIRST_CATEGORY = 10;
static constexpr uint32_t SEFFECT_LAST_CATEGORY = 16;
static constexpr int MAX_REGIONS = ROLLBACK_MAX_REGIONS;
static constexpr size_t SHITSOLVER_SIZE = 0x3808C;
static constexpr size_t SHITSOLVER_DATA_OFF = 0x58;

using SnapshotRange = RollbackSnapshotRange;
using ShotRecipe = RollbackShotRecipe;
using SnapshotSlot = RollbackSnapshotSlot;
using ShotEnqueueFn = char(__fastcall*)(void*, void*, int);
using UnitAddFn = char(__fastcall*)(void*, int, void*, void*);
using UnitRemoveFn = char(__fastcall*)(void*, void*);
#define g_slots g_rollbackSnapshotSlots

static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    {0x28, 0x08}, {0x48, 0x38}, {0x80, 0x100}, {0xA8, 0x04},
    {0x12C, 0x40}, {0x550, 0x10}, {0x29E, 0x12}, {0x1300, 0x08},
    {0x1308, 0x40}, {0x1350, 0x98}, {0x14F0, 0x28}, {0x154C, 0x14},
    {0x1578, 0x70}, {0x15F8, 0x70}, {0x1678, 0x70}, {0x16F8, 0x70},
    {0x1778, 0x70}, {0x17F8, 0x70}, {0x1870, 0x08}, {0x1644, 0x04},
    {0x1898, 0x30}, {0x2D9C, 0x24}, {0x2D80, 0x10}, {0x28B8, 0x20},
    {0x2AD0, 0x20}, {0x3F48, 0x18C}, {0x40D4, 0x4C}, {0x4120, 0xD8},
    {0x4168, 0x90}, {0x4D10, 0x100}, {0x6340, 0x10}, {0x6360, 0xC8},
    {0x68D0, 0x20},
};

static constexpr SnapshotRange SMAIN_RESTORE_RANGES[] = {
    {0x30, 0x64}, {0xA4, 0x1C}, {0x40010, 0x10},
    {0x40028, 0x18}, {0x40048, 0x14},
};

static constexpr SnapshotRange SACTION_RESTORE_RANGES[] = {
    {0x28, 0xE8}, {0x140, 0x08}, {0x150, 0x08}, {0x168, 0x09},
    {0x218, 0x04}, {0x250, 0x10}, {0x268, 0x34},
};

static constexpr SnapshotRange CCHR_TEAM_RESTORE_RANGES[] = {
    {0x50, 0xC0}, {0x110, 0x1B0},
};

static bool IsActiveUnit(void* unit);

static void* ValidateShotObjectReference(void* reference, uint64_t expectedDti,
                                         bool expectedActiveUnit)
{
    if (!reference) return nullptr;
    if (!expectedDti || !HasCallableDtiVirtual(reference) ||
        GetObjectDti((uint64_t)reference) != expectedDti) {
        return nullptr;
    }
    if (expectedActiveUnit && !IsActiveUnit(reference)) return nullptr;
    return reference;
}

namespace {

constexpr size_t SCOLLISION_SIZE = 0x239E0;
constexpr uint32_t SCOLLISION_BODY_START = 0x38;
constexpr uint32_t SCOLLISION_ALLOC_START = 0x23970;
constexpr size_t CCHR_EXTERNAL_ANIM_SIZE = 0xE0;
constexpr uintptr_t FN_COLLISION_NODE_OBJECT_CREATE = 0x1405AD2A0;
constexpr uintptr_t FN_COLLISION_NODE_COPY = 0x1405A4DA0;

} // namespace

void Rollback_RestoreCollisionBody(void* destination, const uint8_t* source, size_t size)
{
    if (size < SCOLLISION_SIZE) return;

    struct LivePointerField {
        uint32_t offset;
        uint64_t value;
    };
    std::vector<LivePointerField> livePointers;
    for (uint32_t offset = SCOLLISION_BODY_START;
         offset + sizeof(uint64_t) <= SCOLLISION_ALLOC_START;
         offset += sizeof(uint64_t)) {
        uint64_t liveValue = *(uint64_t*)((uint8_t*)destination + offset);
        uint64_t savedValue = *(const uint64_t*)(source + offset);
        if (IsAccessiblePointer(liveValue) || IsAccessiblePointer(savedValue))
            livePointers.push_back({offset, liveValue});
    }

    // Rewind inline collision state while retaining live allocator ownership.
    std::memcpy((uint8_t*)destination + SCOLLISION_BODY_START,
                source + SCOLLISION_BODY_START,
                SCOLLISION_ALLOC_START - SCOLLISION_BODY_START);
    for (const LivePointerField& field : livePointers)
        *(uint64_t*)((uint8_t*)destination + field.offset) = field.value;
}

void Rollback_RestoreExternalAnimBlock(void* destination, const uint8_t* source, size_t size)
{
    if (size != CCHR_EXTERNAL_ANIM_SIZE) return;

    // Keep the live collision-node topology but rewind its gameplay binding.
    std::memcpy((uint8_t*)destination + 0x08, source + 0x08, 0x01);
    *((uint8_t*)destination + 0x74) = 0;
    std::memcpy((uint8_t*)destination + 0x80, source + 0x80, 0x10);
    std::memcpy((uint8_t*)destination + 0x90, source + 0x90, 0x28);
    std::memcpy((uint8_t*)destination + 0xC8, source + 0xC8, 0x18);
}

void Rollback_DestroyCollisionNodeClones(RollbackSnapshotSlot& slot)
{
    for (const RollbackCollisionNodeClone& entry : slot.collisionNodeClones) {
        if (!entry.clone || !HasCallableDtiVirtual(entry.clone)) continue;
        uint64_t vtable = *(uint64_t*)entry.clone;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        if (destroy && IsAccessibleAddress((uint64_t)destroy))
            destroy(entry.clone, 1);
    }
    InvalidateMemoryQueryCache();
    slot.collisionNodeClones.clear();
}

void Rollback_CaptureCollisionNodeClones(RollbackSnapshotSlot& slot)
{
    auto createNode =
        (void*(__fastcall*)(void*))_addr(FN_COLLISION_NODE_OBJECT_CREATE);
    auto copyNode =
        (void(__fastcall*)(void*, void*, bool, bool))_addr(FN_COLLISION_NODE_COPY);

    for (int i = 0; i < slot.regionCount; ++i) {
        const RollbackRegion& region = slot.regions[i];
        if (!region.ownerSlot || region.size != CCHR_EXTERNAL_ANIM_SIZE) continue;
        if (!HasCallableDtiVirtual(region.ptr)) continue;

        void* clone = createNode(nullptr);
        InvalidateMemoryQueryCache();
        if (!clone) continue;
        // Inherit the source child factory before deep-copying its children.
        copyNode(clone, region.ptr, false, true);
        slot.collisionNodeClones.push_back({region.ownerSlot, region.ptr, clone});
    }
}

void Rollback_RestoreCollisionNodeClones(RollbackSnapshotSlot& slot)
{
    auto copyNode =
        (void(__fastcall*)(void*, void*, bool, bool))_addr(FN_COLLISION_NODE_COPY);
    for (const RollbackCollisionNodeClone& entry : slot.collisionNodeClones) {
        if (*(uint64_t*)entry.ownerSlot == (uint64_t)entry.savedPtr &&
            HasCallableDtiVirtual(entry.savedPtr) &&
            HasCallableDtiVirtual(entry.clone)) {
            copyNode(entry.savedPtr, entry.clone, false, true);
        }
    }
}


// Full snapshot restoration and dynamic projectile reconstruction.
static void RestorePreSpawnRng(const ShotRecipe& recipe)
{
    if (!recipe.hasPreSpawnRng) return;
    std::memcpy((void*)_addr(0x140D765D8), recipe.preSpawnGlobalRng, 16);
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    if (sEffect)
        std::memcpy((uint8_t*)sEffect + 0x3C, recipe.preSpawnEffectRng, 16);
    if (recipe.ownerChr)
        std::memcpy((uint8_t*)recipe.ownerChr + 0x2D80,
                    recipe.preSpawnOwnerRng, 16);
}


static bool IsActiveUnit(void* unit)
{
    if (!unit) return false;
    uint32_t state = *(uint32_t*)((uint8_t*)unit + 0x10);
    return (state & 7u) < 3u && (state & 0x3F8u) != 0x3F8u;
}

static void RestoreKingKobunHelper(void* shot, const ShotRecipe& saved)
{
    constexpr uintptr_t KING_KOBUN_DTI = 0x140D4D2B0;
    constexpr uintptr_t KOBUN_DTI = 0x140D455B0;
    constexpr size_t HELPER_OFFSET = 0x2158;
    if (!shot || saved.concrete.dti != (uint64_t)_addr(KING_KOBUN_DTI) ||
        saved.concrete.body.size() < HELPER_OFFSET + sizeof(uint64_t)) {
        return;
    }

    void* savedHelper = *(void**)(saved.concrete.body.data() + HELPER_OFFSET);
    void* helper = ValidateShotObjectReference(
        savedHelper, (uint64_t)_addr(KOBUN_DTI), true);

    // King Servbot setup normally obtains this relationship from Tron's
    // transient action-side-camera state. That lookup is no longer populated
    // when a historical shot is reconstructed, so fall back to the unique live
    // uKobun helper in the character category.
    if (!helper) {
        uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
        if (sUnit && *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) >
                         SUNIT_CHARACTER_CATEGORY) {
            uint64_t node = *(uint64_t*)(sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
            for (int safety = 64; node && safety-- > 0; node = *(uint64_t*)(node + 0x18)) {
                if (GetObjectDti(node) == (uint64_t)_addr(KOBUN_DTI) &&
                    IsActiveUnit((void*)node)) {
                    if (helper) return; // Ambiguous: do not bind another player's helper.
                    helper = (void*)node;
                }
            }
        }
    }

    *(void**)((uint8_t*)shot + HELPER_OFFSET) = helper;
}

static void RestoreShotAndEffectRecipes(SnapshotSlot& slot)
{
    if (!Shot_HooksReady()) {
        HookLog("Rollback: shot restore skipped because lifecycle hooks are unavailable\n");
        return;
    }

    void* sUnit = *(void**)_addr(PTR_SUNIT);
    auto removeUnit = (UnitRemoveFn)_addr(FN_SUNIT_REMOVE_UNIT);
    const uint64_t profileStart = Profile_Now();
    if (g_activeRollbackProfile)
        g_activeRollbackProfile->restoreSavedShots += (uint32_t)slot.shotRecipes.size();

    // Each live uShot owns the rShot resource stored in its recipe. Pin every
    // resource before destroying current shots so native reconstruction cannot
    // dereference attack/collision data released with the old object.
    auto addRef = (void(__fastcall*)(void*))_addr(0x14050D100);
    auto releaseRef = (void(__fastcall*)(void*))_addr(0x14050D5A0);
    std::vector<void*> pinnedShotResources;
    pinnedShotResources.reserve(slot.shotRecipes.size());
    for (const ShotRecipe& saved : slot.shotRecipes) {
        if (!saved.shotElement || !HasCallableDtiVirtual(saved.shotElement))
            continue;
        addRef(saved.shotElement);
        pinnedShotResources.push_back(saved.shotElement);
    }
    const uint64_t pinResourcesEnd = Profile_Now();

    int removedInvalidUnits = 0;
    int retiredEffects = 0;
    uint64_t preSanitizeEnd = 0;
    {
        ScopedMemoryQueryCache memoryCache;
        removedInvalidUnits = Shot_SanitizeUnitCategoryLists(
            sUnit, SAUX_FIRST_CATEGORY, SEFFECT_LAST_CATEGORY);
        if (removedInvalidUnits != 0) {
            HookLog("Rollback: removed %d invalid sUnit category entries before restore\n",
                     removedInvalidUnits);
        }
        preSanitizeEnd = Profile_Now();

        // Effect bodies and generator graphs are not rewound. Retaining even a
        // snapshot-era effect lets its independently advanced graph execute after
        // restore; observed failures cover move, finish, draw, and destruction.
        // Retire all effects through native two-phase cleanup. hkUnitRemove
        // sanitizes owned resource/generator fields at the destruction boundary.
        retiredEffects = Effect_RetireCurrent(sUnit);
    }
    const uint64_t retireEffectsEnd = Profile_Now();

    // A projectile that has already advanced cannot be made equivalent to its
    // saved state by patching trajectory fields: hit-consumption, collision,
    // callback, and subclass-tail state remain advanced. Remove every current
    // shot synchronously before recreating the saved set. Merely setting state
    // 3 leaves the object in both sUnit and sShot for two more moveCategory
    // passes, so rollback would briefly run the retired shot beside its clone.
    int retiredShots = 0;
    std::vector<void*> currentShots;
    Shot_CollectCurrent(currentShots);
    if (g_activeRollbackProfile)
        g_activeRollbackProfile->restoreCurrentShots += (uint32_t)currentShots.size();
    const uint64_t collectShotsEnd = Profile_Now();
    for (void* shot : currentShots) {
        // Some concrete shots embed an sEffect at +0x2140. Its destructor
        // unlinks through the owner and then walks owner+0xA8's child chain.
        // That graph is not reconstructed by the shot recipe path, so even a
        // readable owner can contain a stale child and crash in
        // sEffect__markChildrenDirtyForUnlink. Detach the embedded particle
        // links before destruction; the native factory creates fresh effects
        // for the restored shot when needed.
        Shot_DetachEmbeddedEffect(shot);
        int malformedCollisionSlots =
            Shot_SanitizeCollisionSlotsForDestruction(shot);
        if (malformedCollisionSlots != 0) {
            HookLog("Rollback: detached %d malformed collision arrays before destroying shot 0x%llX\n",
                    malformedCollisionSlots, (uint64_t)shot);
        }
        uint32_t* state = (uint32_t*)((uint8_t*)shot + 0x10);
        Shot_RemoveFromAllMultiMaskArrays(sUnit, shot);
        removeUnit(sUnit, shot);
        *state &= ~7u;
        uint64_t vtable = *(uint64_t*)shot;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        destroy(shot, 1);
        ++retiredShots;
    }
    const uint64_t destroyShotsEnd = Profile_Now();

    // Concrete shot destructors can release attached uGameEffect objects
    // directly. Those effects may still be present in sUnit's primary and
    // multi-mask lists with a zeroed vtable, causing the next parallel move
    // job to dispatch through null at vtable+0x40. Remove such dead entries
    // after all shot-owned teardown has completed and before reconstruction.
    // Only shot destructors can introduce the dead attached-effect entries this
    // second pass repairs. With no current shots, topology is unchanged since
    // the pre-restore sanitation pass, so rescanning every sUnit list is pure
    // overhead.
    int removedShotOwnedEffects = 0;
    uint64_t postSanitizeEnd = 0;
    {
        ScopedMemoryQueryCache memoryCache;
        removedShotOwnedEffects = currentShots.empty()
            ? 0
            : Shot_SanitizeUnitCategoryLists(
                sUnit, SSHOT_FIRST_CATEGORY, SEFFECT_LAST_CATEGORY);
        if (removedShotOwnedEffects != 0) {
            HookLog("Rollback: removed %d dead sUnit entries after shot destruction\n",
                     removedShotOwnedEffects);
        }
        postSanitizeEnd = Profile_Now();
        Effect_SanitizeRetired(sUnit);
    }
    const uint64_t sanitizeEffectsEnd = Profile_Now();

    void* sShot = *(void**)_addr(PTR_SSHOT);
    auto enqueueUnit = (ShotEnqueueFn)_addr(FN_SSHOT_ENQUEUE_UNIT);
    auto addUnit = (UnitAddFn)_addr(FN_SUNIT_ADD_UNIT);
    auto flushPendingUnits =
        (void(__fastcall*)(void*))_addr(FN_SSHOT_FLUSH_PENDING_UNITS);
    uint32_t* pendingCountPtr = (uint32_t*)((uint8_t*)sShot + 0x2448);
    if (*pendingCountPtr != 0) {
        HookLog("Rollback: discarding %u pre-restore pending-shot entries\n",
                *pendingCountPtr);
        *pendingCountPtr = 0;
    }
    int respawnedShots = 0;
    int failedShots = 0;

    // Native shot construction consumes action/global/effect RNG while
    // rebuilding resources and owned effects. Reconstruction is rollback
    // plumbing, not a simulated gameplay event, so it must not advance any
    // deterministic stream before the first re-simulated frame.
    uint8_t restoredGlobalRng[16] = {};
    uint8_t restoredEffectRng[16] = {};
    struct SavedCharacterRng {
        void* character;
        uint8_t state[16];
    };
    std::vector<SavedCharacterRng> restoredCharacterRngs;
    uint64_t sEffect = *(uint64_t*)_addr(0x140E17A38);
    std::memcpy(restoredGlobalRng, (void*)_addr(0x140d765d8), sizeof(restoredGlobalRng));
    if (sEffect) std::memcpy(restoredEffectRng, (void*)(sEffect + 0x3C), sizeof(restoredEffectRng));

    // Some shot setup methods dispatch RNG through their owning uCharacter.
    // Preserve each owner's stream so native reconstruction remains plumbing.
    for (const ShotRecipe& saved : slot.shotRecipes) {
        if (!saved.ownerChr) continue;
        bool alreadySaved = false;
        for (const SavedCharacterRng& characterRng : restoredCharacterRngs) {
            if (characterRng.character == saved.ownerChr) {
                alreadySaved = true;
                break;
            }
        }
        if (alreadySaved) continue;

        SavedCharacterRng characterRng{};
        characterRng.character = saved.ownerChr;
        std::memcpy(characterRng.state, (uint8_t*)saved.ownerChr + 0x2D80,
                    sizeof(characterRng.state));
        restoredCharacterRngs.push_back(characterRng);
    }
    const uint64_t respawnSetupEnd = Profile_Now();

    uint64_t respawnPrepareTicks = 0;
    uint64_t respawnFactoryTicks = 0;
    uint64_t respawnRegisterTicks = 0;
    uint64_t respawnReleaseEffectsTicks = 0;
    uint64_t respawnConcreteTicks = 0;
    uint64_t respawnOverlayTicks = 0;
    for (ShotRecipe& saved : slot.shotRecipes) {
        const uint64_t prepareStart = Profile_Now();
        RestorePreSpawnRng(saved);
        void* objectRef1 = ValidateShotObjectReference(
            saved.objectRef1, saved.objectRef1Dti,
            saved.objectRef1WasActiveUnit);
        void* objectRef2 = ValidateShotObjectReference(
            saved.objectRef2, saved.objectRef2Dti,
            saved.objectRef2WasActiveUnit);
        const uint64_t factoryStart = Profile_Now();
        respawnPrepareTicks += factoryStart - prepareStart;
        void* fresh = Shot_SpawnOriginal(sShot, saved, objectRef1, objectRef2);
        const uint64_t factoryEnd = Profile_Now();
        respawnFactoryTicks += factoryEnd - factoryStart;
        if (!fresh) {
            failedShots++;
            continue;
        }

        // The factory may create an inline effect with a valid vtable, but its
        // owner/list links still point at the old prediction graph. Detach it
        // before the broad concrete overlay can copy any stale relationship.
        Shot_TrackReconstructed(fresh, saved);
        saved.restoredPtr = fresh;
        // Rebuild registration through the same pending-unit path used by the
        // native shot callers. Drain it before resimulation so all restored
        // shots enter the category list in one native registration pass.
        // The native pending queue holds 0x200 entries. Its enqueue function
        // increments the counter even on overflow, which would make the later
        // flush walk beyond the queue buffer. Match the native caller fallback
        // without touching the counter once the queue is full.
        uint32_t pendingCount = *pendingCountPtr;
        if (pendingCount >= 0x200 || !enqueueUnit(sShot, fresh, SSHOT_CATEGORY)) {
            addUnit(sUnit, SSHOT_CATEGORY, fresh, nullptr);
        }
        const uint64_t registerEnd = Profile_Now();
        respawnRegisterTicks += registerEnd - factoryEnd;
        Shot_ReleaseFreshEmbeddedEffectGraph(fresh);
        const uint64_t releaseEffectsEnd = Profile_Now();
        respawnReleaseEffectsTicks += releaseEffectsEnd - registerEnd;
        const void* validTargets[6] = {};
        size_t validTargetCount = 0;
        uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
        for (int teamIndex = 0; sCharacter && teamIndex < 2; ++teamIndex) {
            uint64_t node = sCharacter +
                (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
                TEAM_FIRST_NODE_OFFSET;
            for (int member = 0; node && member < 3; ++member) {
                void* character = *(void**)(node + NODE_CHR_OFFSET);
                if (character) validTargets[validTargetCount++] = character;
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
        if (!ConcreteShotSnapshot_Restore(fresh, saved.concrete, validTargets,
                                          validTargetCount)) {
            HookLog("Rollback: restored shot 0x%llX has incomplete collision topology\n",
                     (uint64_t)fresh);
        }
        const uint64_t concreteEnd = Profile_Now();
        respawnConcreteTicks += concreteEnd - releaseEffectsEnd;
        RestoreKingKobunHelper(fresh, saved);
        if (saved.concrete.dti == (uint64_t)_addr(0x140D4D2B0) &&
            saved.concrete.body.size() >= 0x2164) {
            // These are the controller's scalar mode/phase fields. The broad
            // shot overlay deliberately preserves pointer-looking words, and
            // +0x2158 is fixed separately above; explicitly restore the
            // remaining King Servbot fields so reconstruction cannot leave the
            // fresh factory mode latched at its pre-move state.
            static constexpr size_t kingRanges[][2] = {
                {0x2140, 0x08},
                {0x214C, 0x04},
                {0x2150, 0x08},
                {0x2160, 0x04},
            };
            for (const auto& range : kingRanges) {
                std::memcpy((uint8_t*)fresh + range[0],
                            saved.concrete.body.data() + range[0], range[1]);
            }
        }
        // Concrete snapshots do not reconstruct the global particle-owner
        // graph referenced by inline sEffect objects. Leave that graph
        // detached so later native shot destruction cannot walk stale nodes.
        Shot_DetachEmbeddedEffect(fresh);

        // Reapply confirmed pointer-free common fields after the broad object
        // overlay so packed floats/flags that resemble addresses still rewind.
        Shot_RestoreArmState(fresh, saved);
        Shot_ApplyFlight(fresh, saved.flight);

        // ApplyShotFlight preserves the category assigned by addUnit while
        // restoring the saved gameplay lifecycle bits.
        uint32_t* unitState = (uint32_t*)((uint8_t*)fresh + 0x10);
        *unitState = (*unitState & ~0x3F8u) | (SSHOT_CATEGORY << 3);
        const uint64_t overlayEnd = Profile_Now();
        respawnOverlayTicks += overlayEnd - concreteEnd;
        respawnedShots++;
    }
    const uint64_t respawnEnd = Profile_Now();

    // Native enqueue increments the counter before rejecting an overflow. A
    // concurrent/native spawn during reconstruction can therefore poison it
    // again even though our own calls stop at capacity. Never let flush walk
    // beyond the 512-entry buffer into the activity array at +0x2450.
    if (*pendingCountPtr > 0x200) {
        HookLog("Rollback: clamping pending-shot count %u to 512 before flush\n",
                *pendingCountPtr);
        *pendingCountPtr = 0x200;
    }
    flushPendingUnits(sShot);

    std::memcpy((void*)_addr(0x140d765d8), restoredGlobalRng, sizeof(restoredGlobalRng));
    if (sEffect) std::memcpy((void*)(sEffect + 0x3C), restoredEffectRng, sizeof(restoredEffectRng));
    for (const SavedCharacterRng& characterRng : restoredCharacterRngs) {
        std::memcpy((uint8_t*)characterRng.character + 0x2D80, characterRng.state,
                    sizeof(characterRng.state));
    }
    for (void* resource : pinnedShotResources)
        releaseRef(resource);
    const uint64_t finalizeEnd = Profile_Now();

    if (g_activeRollbackProfile) {
        g_activeRollbackProfile->restoreShotPinResources += pinResourcesEnd - profileStart;
        g_activeRollbackProfile->restoreShotPreSanitize += preSanitizeEnd - pinResourcesEnd;
        g_activeRollbackProfile->restoreShotRetireEffects += retireEffectsEnd - preSanitizeEnd;
        g_activeRollbackProfile->restoreShotCollect += collectShotsEnd - retireEffectsEnd;
        g_activeRollbackProfile->restoreShotDestroy += destroyShotsEnd - collectShotsEnd;
        g_activeRollbackProfile->restoreShotPostSanitize += postSanitizeEnd - destroyShotsEnd;
        g_activeRollbackProfile->restoreShotSanitizeEffects += sanitizeEffectsEnd - postSanitizeEnd;
        g_activeRollbackProfile->restoreShotRespawnSetup += respawnSetupEnd - sanitizeEffectsEnd;
        g_activeRollbackProfile->restoreShotRespawn += respawnEnd - respawnSetupEnd;
        g_activeRollbackProfile->restoreShotRespawnPrepare += respawnPrepareTicks;
        g_activeRollbackProfile->restoreShotRespawnFactory += respawnFactoryTicks;
        g_activeRollbackProfile->restoreShotRespawnRegister += respawnRegisterTicks;
        g_activeRollbackProfile->restoreShotRespawnReleaseEffects += respawnReleaseEffectsTicks;
        g_activeRollbackProfile->restoreShotRespawnConcrete += respawnConcreteTicks;
        g_activeRollbackProfile->restoreShotRespawnOverlay += respawnOverlayTicks;
        g_activeRollbackProfile->restoreShotFinalize += finalizeEnd - respawnEnd;
    }

    HookLog("Rollback: retired shots=%d effects=%d; shot recipes respawned=%d failed=%d\n",
            retiredShots, retiredEffects, respawnedShots, failedShots);
}

void Rollback_RestoreSnapshot(int32_t frame)
{
    const uint64_t profileStart = Profile_Now();
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    // Verify the slot contains data for exactly this frame (ring-buffer safety)
    if (!slot.data || slot.size == 0 || slot.regionCount == 0 || slot.frame != frame) {
        HookLog("Rollback: no snapshot available for frame %d (slot has frame %d)\n",
                frame, slot.frame);
        return;
    }

    // Restore using SAVED region metadata (addresses + sizes), not re-resolved.
    // This ensures dynamic objects (projectiles, effects) map to the exact
    // same memory addresses that were captured, even if the game rebuilt its
    // internal lists between save and restore.
    uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
    uint64_t sCollision = *(uint64_t*)_addr(PTR_SCOLLISION);
    uint64_t sMvc3Mgr = *(uint64_t*)_addr(PTR_SMVC3MGR);
    uint64_t sAction = sMvc3Mgr ? *(uint64_t*)(sMvc3Mgr + SMVC3MGR_ACTION_OFFSET) : 0;
    uint64_t modelJointAllocationSlot = 0;
    uint64_t liveModelJointAllocation = 0;
    if (sAction && IsAccessibleAddress(
            sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET)) {
        uint64_t modelJointWrapper = *(uint64_t*)(
            sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET);
        if (modelJointWrapper && IsAccessibleAddress(
                modelJointWrapper + SACTION_MODEL_JOINT_ALLOCATION_OFFSET)) {
            modelJointAllocationSlot = modelJointWrapper +
                SACTION_MODEL_JOINT_ALLOCATION_OFFSET;
            liveModelJointAllocation = *(uint64_t*)modelJointAllocationSlot;
        }
    }
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    uint64_t team0 = sCharacter ? sCharacter + TEAM0_OFFSET : 0;
    uint64_t team1 = sCharacter ? sCharacter + TEAM1_OFFSET : 0;
    uint64_t teamCharacters[16] = {};
    size_t teamCharacterCount = 0;
    if (sCharacter) {
        for (int teamIndex = 0; teamIndex < 2; ++teamIndex) {
            uint64_t node = sCharacter + (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
                TEAM_FIRST_NODE_OFFSET;
            for (int n = 0; node && n < 8; ++n) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                if (character && teamCharacterCount < std::size(teamCharacters))
                    teamCharacters[teamCharacterCount++] = character;
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
    }
    auto isTeamCharacter = [&teamCharacters, teamCharacterCount](uint64_t character) {
        for (size_t i = 0; i < teamCharacterCount; ++i) {
            if (teamCharacters[i] == character) return true;
        }
        return false;
    };
    bool ownerStillMatches[MAX_REGIONS] = {};
    uint64_t liveOwnerPointers[MAX_REGIONS] = {};
    for (int i = 0; i < slot.regionCount; ++i) {
        uint64_t ownerSlot = slot.regions[i].ownerSlot;
        if (ownerSlot == 0) {
            ownerStillMatches[i] = true;
            continue;
        }
        liveOwnerPointers[i] = *(uint64_t*)ownerSlot;
        ownerStillMatches[i] = liveOwnerPointers[i] == (uint64_t)slot.regions[i].ptr;
    }
    const uint64_t setupEnd = Profile_Now();

    size_t offset = 0;
    int collisionRestoreCount = 0;
    size_t collisionRestoreBytes = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        size_t copySize = slot.regions[i].size;
        if (offset + copySize > slot.size) {
            copySize = slot.size - offset;
        }
        bool objectStillMatches = slot.regions[i].expectedVtable == 0 ||
            *(uint64_t*)slot.regions[i].ptr == slot.regions[i].expectedVtable;
        if (slot.regions[i].ownerSlot == modelJointAllocationSlot &&
            slot.regions[i].size == SACTION_MODEL_JOINT_ALLOCATION_SIZE &&
            liveModelJointAllocation &&
            IsAccessibleAddress(liveModelJointAllocation +
                SACTION_MODEL_JOINT_ALLOCATION_SIZE - 1) &&
            copySize > SACTION_MODEL_JOINT_DATA_OFFSET) {
            // The payload is the evaluated action-model joint state. Throw
            // capture consumes its root-motion result as well as the limits at
            // +0x54/+0x58. Keeping the future payload makes the first restored
            // throw tick see already-consumed motion and produce no diagonal
            // displacement. Rewind the payload in the current allocation while
            // preserving the live allocator header and pointer identity.
            size_t payloadSize = copySize - SACTION_MODEL_JOINT_DATA_OFFSET;
            std::memcpy((uint8_t*)liveModelJointAllocation +
                            SACTION_MODEL_JOINT_DATA_OFFSET,
                        slot.data + offset + SACTION_MODEL_JOINT_DATA_OFFSET,
                        payloadSize);
        } else if (!ownerStillMatches[i] || !objectStillMatches) {
            // The external blend/animation allocation was replaced after the
            // snapshot, or a helper cChr was destroyed/reused. Never write old
            // bytes into a freed or repurposed allocation.
        } else if (slot.regions[i].ownerSlot != 0 &&
                   slot.regions[i].size == CCHR_EXTERNAL_ANIM_SIZE) {
            Rollback_RestoreExternalAnimBlock(slot.regions[i].ptr,
                                     slot.data + offset,
                                     copySize);
        } else if ((uint64_t)slot.regions[i].ptr == sCollision &&
                   slot.regions[i].size == SCOLLISION_SIZE) {
            ++collisionRestoreCount;
            collisionRestoreBytes += copySize;
            Rollback_RestoreCollisionBody(slot.regions[i].ptr, slot.data + offset, copySize);
        } else if ((uint64_t)slot.regions[i].ptr == *(uint64_t*)_addr(PTR_SMAIN) &&
                   slot.regions[i].size == SMAIN_SIZE) {
            for (const SnapshotRange& range : SMAIN_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }
        } else if ((uint64_t)slot.regions[i].ptr == sAction &&
                   slot.regions[i].size == 0x300) {
            for (const SnapshotRange& range : SACTION_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }
        } else if ((uint64_t)slot.regions[i].ptr == sHitSolver &&
            slot.regions[i].size == SHITSOLVER_SIZE &&
            copySize > SHITSOLVER_DATA_OFF) {
            // Preserve the live vtable, CRITICAL_SECTION, flags and the
            // heap-backed MtArray<cHitNode*> header.
            std::memcpy((void*)(sHitSolver + SHITSOLVER_DATA_OFF),
                        slot.data + offset + SHITSOLVER_DATA_OFF,
                        copySize - SHITSOLVER_DATA_OFF);
        } else if ((uint64_t)slot.regions[i].ptr == team0 ||
                   (uint64_t)slot.regions[i].ptr == team1) {
            // Keep the live cChrNode list head and vtable, but restore the
            // active-character reference and all team gameplay aggregates.
            uint64_t liveActive = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + 0x48);
            for (const SnapshotRange& range : CCHR_TEAM_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }
            std::memcpy((uint8_t*)slot.regions[i].ptr + 0x48,
                        slot.data + offset + 0x48, sizeof(uint64_t));
            // Do not leave a saved pointer to a character that no longer
            // belongs to the current live team list.
            uint64_t restoredActive = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + 0x48);
            if (restoredActive == 0 || restoredActive != liveActive) {
                // The roster itself is stable for a snapshot restore; the
                // saved pointer is valid when it is one of the current nodes.
                // If it was replaced, retain the live selection instead.
                bool found = false;
                uint64_t node = (uint64_t)slot.regions[i].ptr + TEAM_FIRST_NODE_OFFSET;
                for (int n = 0; node && n < 8; ++n) {
                    if (*(uint64_t*)(node + NODE_CHR_OFFSET) == restoredActive) {
                        found = true;
                        break;
                    }
                    node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
                }
                if (!found)
                    *(uint64_t*)((uint8_t*)slot.regions[i].ptr + 0x48) = liveActive;
            }
        } else if (slot.regions[i].skipVtable && copySize > sizeof(uint64_t)) {
            uint64_t liveUnitState[sizeof(CUNIT_LIVE_STATE_OFFSETS) / sizeof(CUNIT_LIVE_STATE_OFFSETS[0])];
            for (size_t field = 0;
                 field < sizeof(CUNIT_LIVE_STATE_OFFSETS) / sizeof(CUNIT_LIVE_STATE_OFFSETS[0]);
                 ++field) {
                liveUnitState[field] =
                    *(uint64_t*)((uint8_t*)slot.regions[i].ptr + CUNIT_LIVE_STATE_OFFSETS[field]);
            }
            uint8_t liveDrawState[CCHR_DRAW_STATE_END - CCHR_DRAW_STATE_START];
            std::memcpy(liveDrawState,
                        (uint8_t*)slot.regions[i].ptr + CCHR_DRAW_STATE_START,
                        sizeof(liveDrawState));
            uint8_t liveModelTail[CCHR_MODEL_TAIL_END - CCHR_MODEL_TAIL_START];
            std::memcpy(liveModelTail,
                        (uint8_t*)slot.regions[i].ptr + CCHR_MODEL_TAIL_START,
                        sizeof(liveModelTail));

            struct LivePointerField { uint32_t offset; uint64_t value; };
            LivePointerField livePointers[512];
            size_t livePointerCount = 0;
            for (size_t fieldOffset = sizeof(uint64_t);
                 fieldOffset + sizeof(uint64_t) <= copySize && livePointerCount < 512;
                 fieldOffset += sizeof(uint64_t)) {
                uint64_t liveValue = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + fieldOffset);
                uint64_t savedValue = *(uint64_t*)(slot.data + offset + fieldOffset);
                if (IsLikelyPointer(liveValue) || IsLikelyPointer(savedValue)) {
                    livePointers[livePointerCount++] = {
                        (uint32_t)fieldOffset, liveValue
                    };
                }
            }
            std::memcpy((uint8_t*)slot.regions[i].ptr + sizeof(uint64_t),
                        slot.data + offset + sizeof(uint64_t),
                        copySize - sizeof(uint64_t));
            // cChr derives from cUnit. Preserve its live lifecycle state and
            // intrusive links: rewinding +0x10 can re-run setup/createProperty
            // for an already-live helper, while rewinding the links corrupts
            // sUnit traversal shared with objects outside this snapshot.
            for (size_t field = 0;
                 field < sizeof(CUNIT_LIVE_STATE_OFFSETS) / sizeof(CUNIT_LIVE_STATE_OFFSETS[0]);
                 ++field) {
                uint32_t fieldOffset = CUNIT_LIVE_STATE_OFFSETS[field];
                if (fieldOffset == 0x10 && isTeamCharacter((uint64_t)slot.regions[i].ptr))
                    continue; // restore the saved MtObject alive/lifecycle flags
                *(uint64_t*)((uint8_t*)slot.regions[i].ptr + fieldOffset) = liveUnitState[field];
            }
            // Keep live draw-list/resource ownership and uModel lifecycle state,
            // but rewind Motion[8] at +0x560..+0x115f. Those records must agree
            // with the separately restored blend buffers and animation blocks.
            std::memcpy((uint8_t*)slot.regions[i].ptr + CCHR_DRAW_STATE_START,
                        liveDrawState,
                        sizeof(liveDrawState));
            std::memcpy((uint8_t*)slot.regions[i].ptr + CCHR_MODEL_TAIL_START,
                        liveModelTail,
                        sizeof(liveModelTail));
            // Preserve every live pointer-like field in the remainder of cChr.
            // The legacy body contains weak shot/effect refs, resource refs and
            // subclass-tail pointers whose targets are not recreated by restore.
            for (size_t pointer = 0; pointer < livePointerCount; ++pointer) {
                *(uint64_t*)((uint8_t*)slot.regions[i].ptr + livePointers[pointer].offset) =
                    livePointers[pointer].value;
            }
            // The broad pointer guard can mistake packed flags and pairs of
            // floats for aligned addresses (for example speed + zero accel).
            // Reapply confirmed pointer-free gameplay ranges so grounded state,
            // gravity and velocity come from the restored frame.
            for (const SnapshotRange& range : CCHR_SCALAR_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }

            uint64_t concreteDti = GetObjectDti((uint64_t)slot.regions[i].ptr);
            ConcreteCharacterSnapshot_RestoreFields(
                slot.regions[i].ptr,
                slot.data + offset,
                copySize,
                concreteDti);

            // uChrCpu's interpreter stack contains 32-bit script values, so
            // pairs of words can look like pointers to the generic guard. A
            // split restore of that stack and its cursor lets the VM combine
            // stale words into a bogus manager pointer during re-simulation.
            // Locate the embedded uChrCpu by its vtable and restore the VM
            // stack plus scalar execution state as one pointer-free block.
            const uint64_t cpuVtable = _addr(UCHRCPU_VTABLE);
            if (cpuVtable != 0) {
                for (size_t cpuOffset = 0;
                     cpuOffset + sizeof(uint64_t) <= copySize;
                     cpuOffset += sizeof(uint64_t)) {
                    uint64_t liveVtable = *(uint64_t*)((uint8_t*)slot.regions[i].ptr +
                                                       cpuOffset);
                    if (liveVtable != cpuVtable) continue;
                    if (cpuOffset + UCHRCPU_VM_STATE_OFFSET +
                            UCHRCPU_VM_STATE_SIZE > copySize)
                        break;

                    std::memcpy((uint8_t*)slot.regions[i].ptr + cpuOffset +
                                    UCHRCPU_VM_STATE_OFFSET,
                                slot.data + offset + cpuOffset +
                                    UCHRCPU_VM_STATE_OFFSET,
                                UCHRCPU_VM_STATE_SIZE);
                    break;
                }
            }

            // Motion slots contain many pairs of floats whose packed 64-bit
            // value passes IsLikelyPointer (for example {1.0f, 0.0f}). The
            // generic pointer guard above would therefore retain prediction-
            // timeline frame, blend and root-motion values. Rewind the known
            // scalar/vector portions while keeping the slot vtable and its
            // resource/allocation pointers at +0x120..+0x138, +0x160,
            // +0x170 and +0x178 live.
            for (int motion = 0; motion < CCHR_MOTION_COUNT; ++motion) {
                size_t motionBase = CCHR_MOTION_BASE + motion * CCHR_MOTION_STRIDE;
                static constexpr SnapshotRange motionRestoreRanges[] = {
                    {0x08, 0x118},
                    {0x140, 0x20},
                    {0x168, 0x08},
                };
                for (const SnapshotRange& range : motionRestoreRanges) {
                    size_t fieldOffset = motionBase + range.offset;
                    if (fieldOffset + range.size <= copySize) {
                        std::memcpy((uint8_t*)slot.regions[i].ptr + fieldOffset,
                                    slot.data + offset + fieldOffset,
                                    range.size);
                    }
                }
            }

            // Rewind all five cChrTargetInfo records at +0x6490..+0x670f.
            // They cache target, attacker, damage, throw and parent character
            // state. Keep each vtable live and validate its saved character
            // pointer against the current roster.
            static constexpr uint32_t targetInfoBase = 0x6490;
            static constexpr uint32_t targetInfoStride = 0x80;
            for (uint32_t targetInfo = 0; targetInfo < 5; ++targetInfo) {
                uint32_t record = targetInfoBase + targetInfo * targetInfoStride;
                uint32_t pointerField = record + 0x08;
                uint32_t dataField = record + 0x10;
                if ((size_t)dataField + 0x70 > copySize) continue;

                uint64_t savedTarget = *(const uint64_t*)(slot.data + offset + pointerField);
                uint64_t liveTarget = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + pointerField);
                if (savedTarget == 0 || savedTarget == liveTarget || isTeamCharacter(savedTarget)) {
                    *(uint64_t*)((uint8_t*)slot.regions[i].ptr + pointerField) = savedTarget;
                    std::memcpy((uint8_t*)slot.regions[i].ptr + dataField,
                                slot.data + offset + dataField,
                                0x70);
                }
            }

            // The generic pointer guard above can mistake aligned primitive
            // flags, coordinates and IDs for addresses, retaining post-hit
            // disabled geometry. Rewind confirmed pointer-free fields while
            // preserving each primitive's vtable and live parent references.
            static constexpr uint32_t capsuleBase = 0x4210;
            static constexpr uint32_t capsuleStride = 0xB0;
            for (uint32_t primitive = 0; primitive < 16; ++primitive) {
                uint32_t entry = capsuleBase + primitive * capsuleStride;
                static constexpr SnapshotRange fields[] = {
                    {0x08, 0x01}, // inherited active flag
                    {0x10, 0x04}, // primitive kind
                    {0x20, 0x24}, // capsule endpoints and radius
                    {0x50, 0x04}, // active/attribute flags
                    {0x68, 0x0C}, // parent indices and size rate
                    {0x80, 0x20}, // local endpoint offsets
                };
                for (const SnapshotRange& field : fields) {
                    if ((size_t)entry + field.offset + field.size <= copySize) {
                        std::memcpy((uint8_t*)slot.regions[i].ptr + entry + field.offset,
                                    slot.data + offset + entry + field.offset,
                                    field.size);
                    }
                }
            }

            static constexpr uint32_t sphereBases[] = {0x4E20, 0x5530, 0x5C40};
            static constexpr uint32_t sphereStride = 0x70;
            for (uint32_t base : sphereBases) {
                for (uint32_t primitive = 0; primitive < 16; ++primitive) {
                    uint32_t entry = base + primitive * sphereStride;
                    static constexpr SnapshotRange fields[] = {
                        {0x08, 0x01}, // inherited active flag
                        {0x10, 0x04}, // primitive kind
                        {0x20, 0x10}, // sphere center and radius
                        {0x30, 0x04}, // active/attribute flags
                        {0x40, 0x08}, // parent index and size rate
                        {0x50, 0x10}, // local center offset
                    };
                    for (const SnapshotRange& field : fields) {
                        if ((size_t)entry + field.offset + field.size <= copySize) {
                            std::memcpy((uint8_t*)slot.regions[i].ptr + entry + field.offset,
                                        slot.data + offset + entry + field.offset,
                                        field.size);
                        }
                    }
                }
            }

            // cAtkCtrl::TargetInfo entries are 0x18 bytes: vtable at +0x00,
            // target reference at +0x08, then mTotalHit and mHitTime at
            // +0x10/+0x14. There are 32 inline entries in each table. The
            // target reference is part of the attack's
            // per-target consumed state. Preserving the post-hit live pointer
            // can make a restored pre-hit attack remain associated with the
            // target it already hit; starting another attack clears this,
            // which explains why the next attack works.
            static constexpr uint32_t targetTables[] = {0x18C8, 0x1BC8, 0x1EC8};
            for (uint32_t table : targetTables) {
                for (uint32_t target = 0; target < 32; ++target) {
                    uint32_t targetOffset = table + target * 0x18 + 0x08;
                    uint32_t field = table + target * 0x18 + 0x10;
                    if ((size_t)targetOffset + sizeof(uint64_t) <= copySize) {
                        uint64_t savedTarget = *(const uint64_t*)(slot.data + offset + targetOffset);
                        uint64_t liveTarget = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + targetOffset);
                        if (savedTarget == 0 || savedTarget == liveTarget ||
                            isTeamCharacter(savedTarget)) {
                            *(uint64_t*)((uint8_t*)slot.regions[i].ptr + targetOffset) = savedTarget;
                        }
                    }
                    if ((size_t)field + 0x08 <= copySize) {
                        std::memcpy((uint8_t*)slot.regions[i].ptr + field,
                                    slot.data + offset + field,
                                    0x08);
                    }
                }
            }

            // cChrInput is embedded in the captured character body. Restore it
            // exactly after all pointer-preserving character fixups; packed
            // input history words can otherwise be mistaken for pointers.
            if (CCHR_INPUT_OFFSET + CCHR_INPUT_SIZE <= copySize) {
                std::memcpy((uint8_t*)slot.regions[i].ptr + CCHR_INPUT_OFFSET,
                            slot.data + offset + CCHR_INPUT_OFFSET,
                            CCHR_INPUT_SIZE);
            }
        } else if (copySize > 0) {
            std::memcpy(slot.regions[i].ptr, slot.data + offset, copySize);
        }
        offset += slot.regions[i].size;
    }
    const uint64_t regionsEnd = Profile_Now();

    Rollback_RestoreCollisionNodeClones(slot);
    const uint64_t collisionClonesEnd = Profile_Now();

    // Full cChr body restore also rewinds its external pointer fields. If an
    // allocation changed, put the current pointer back after skipping its old
    // buffer so the live character does not retain a stale address.
    for (int i = 0; i < slot.regionCount; ++i) {
        if (slot.regions[i].ownerSlot != 0 && !ownerStillMatches[i]) {
            *(uint64_t*)slot.regions[i].ownerSlot = liveOwnerPointers[i];
        }
    }
    const uint64_t ownerPointersEnd = Profile_Now();

    // Active hit nodes are one-frame work items. Restoring them after a hit has
    // already been applied replays the resolver callback on every subsequent
    // restore, compounding launch/pushback (the effect is largest immediately
    // after a hit and decays once the native solver consumes the node).
    // Keep the live MtArray allocation and solver infrastructure, but discard
    // the transient pending-hit list.
    if (sHitSolver) {
        ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(sHitSolver);
        *(uint32_t*)(sHitSolver + 0x30064) = 0; // mHitSolverCnt
        *(uint32_t*)(sHitSolver + 0x30070) = 0; // sort bucket A count
        *(uint32_t*)(sHitSolver + 0x34078) = 0; // sort bucket B count
    }
    const uint64_t hitSolverEnd = Profile_Now();

    // The character body restore includes slot-table pointers to heap objects
    // that are not part of the snapshot. Clear them before the next game tick
    // can release one through MtObject__replaceSlot_6F30.
    Rollback_ClearStalePointers();
    const uint64_t stalePointersEnd = Profile_Now();

    // Shot construction registers owned effects through sGameEffect and scans
    // its tracked-object slots with MtObject::isKindOf. Clear references to
    // effects retired by this restore before the first factory call.
    Rollback_ClearGameEffectSlots();
    const uint64_t effectSlotsEnd = Profile_Now();
    RestoreShotAndEffectRecipes(slot);
    const uint64_t shotsEnd = Profile_Now();

    // Character slot tables contain live model/resource references. Clearing
    // all 64 entries makes restored dead characters invisible.
    Rollback_SyncActiveCharacterUi();
    const uint64_t uiEnd = Profile_Now();

    if (g_activeRollbackProfile) {
        g_activeRollbackProfile->restoreTotal += uiEnd - profileStart;
        g_activeRollbackProfile->restoreSetup += setupEnd - profileStart;
        g_activeRollbackProfile->restoreRegions += regionsEnd - setupEnd;
        g_activeRollbackProfile->restoreCollisionClones += collisionClonesEnd - regionsEnd;
        g_activeRollbackProfile->restoreOwnerPointers += ownerPointersEnd - collisionClonesEnd;
        g_activeRollbackProfile->restoreHitSolver += hitSolverEnd - ownerPointersEnd;
        g_activeRollbackProfile->restoreStalePointers += stalePointersEnd - hitSolverEnd;
        g_activeRollbackProfile->restoreEffectSlots += effectSlotsEnd - stalePointersEnd;
        g_activeRollbackProfile->restoreShots += shotsEnd - effectSlotsEnd;
        g_activeRollbackProfile->restoreUi += uiEnd - shotsEnd;
    }

    HookLog("Rollback: restored frame %d with %d regions, total size %zu bytes, "
            "sCollision inline regions=%d bytes=%zu\n",
            frame, slot.regionCount, offset, collisionRestoreCount, collisionRestoreBytes);
}
