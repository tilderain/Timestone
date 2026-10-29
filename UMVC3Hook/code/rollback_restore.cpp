#include "../pch.h"

#include "rollback_restore.h"
#include "rollback_cleanup.h"
#include "rollback_effect.h"
#include "rollback_memory.h"
#include "rollback_model_policy.h"
#include "rollback_profile.h"
#include "live_benchmark.h"
#include "rollback_shot.h"
#include "rollback_snapshot.h"
#include "concrete_snapshot.h"
#include "mk10utils.h"

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

static_assert(sizeof(RollbackRegion) <= 64);

static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SSTAGE = 0x140d4f718;
static constexpr uintptr_t PTR_SSHOT = 0x140d47f40;
static constexpr uintptr_t PTR_SHITSOLVER = 0x140d44060;
static constexpr uintptr_t PTR_SMAIN = 0x140e177e8;
static constexpr uintptr_t PTR_SMVC3MGR = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT = 0x140e17698;
static constexpr uintptr_t DTI_UIRONMAN = 0x140D45518;
static constexpr uintptr_t PTR_SCOLLISION = 0x140e18250;
static constexpr uintptr_t PTR_SCAMERA = 0x140e17930;
static constexpr uintptr_t PTR_SGAMEEFFECT = 0x140d46470;
static constexpr uintptr_t FN_SSHOT_ENQUEUE_UNIT = 0x1401512A0;
static constexpr uintptr_t FN_SSHOT_FLUSH_PENDING_UNITS = 0x1401517A0;
static constexpr uintptr_t FN_SUNIT_ADD_UNIT = 0x14051A6B0;
static constexpr uintptr_t FN_SUNIT_REMOVE_UNIT = 0x14051B9F0;
static constexpr uintptr_t FN_RESET_HITNODES = 0x140259E90;
static constexpr uintptr_t FN_MODEL_SET_CHILD_MATERIAL_MODE = 0x140200DB0;
static constexpr uintptr_t FN_MTOBJECT_ADD_REF = 0x14050D100;
static constexpr uintptr_t FN_CRESOURCE_RELEASE_REF = 0x14050D5A0;

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
static constexpr size_t CCHR_INPUT_SIZE = 0x570;
static constexpr uint32_t CCHR_ATTACK_CTRL_OFFSET = 0x1878;
static constexpr uint32_t CCHR_COMMAND_OFFSETS[] = {0x2738, 0x2950};
static constexpr size_t CCHR_COMMAND_SIZE = 0x218;
static constexpr uint32_t CCHR_JOINT_HOLD_OFFSET = 0x2B70;
static constexpr size_t CCHR_JOINT_HOLD_SIZE = 0xE0;
static constexpr uint32_t CCHR_WEAPON_COUNT_OFFSET = 0x3F38;
static constexpr uint32_t CCHR_WEAPON_ARRAY_OFFSET = 0x3F40;
static constexpr uint32_t CCHR_MESSAGE_OFFSET = 0x2C50;
static constexpr size_t CCHR_MESSAGE_STRIDE = 0x30;
static constexpr uint32_t CCHR_PAUSE_OFFSET = 0x2D70;
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SUNIT_CHARACTER_CATEGORY = 8;
static constexpr uint32_t SUNIT_CHARACTER_POST_CATEGORY = 18;
static constexpr uintptr_t DTI_UCHARACTER_POST = 0x140D44D60;
static constexpr uintptr_t DTI_UCHRWEAPON = 0x140D44968;
static constexpr uint32_t UCHARACTER_POST_SCALAR_STATE_OFFSET = 0x40;
static constexpr size_t UCHARACTER_POST_SCALAR_STATE_SIZE = 0x10;
static constexpr uint32_t UCHARACTER_POST_CHARACTER_CACHE_OFFSET = 0x50;
static constexpr size_t UCHARACTER_POST_SIZE = 0x60;
static constexpr uint32_t CUNIT_RNO_OFFSET = 0x08;
static constexpr uint32_t CUNIT_UNIT_PARAM_OFFSET = 0x0C;
static constexpr uint32_t CUNIT_FLAGS_OFFSET = 0x10;
static constexpr uint32_t CUNIT_MOVE_ENABLED_FLAG = 0x400;
static constexpr uint32_t CUNIT_GROUP_OFFSET = 0x30;
static constexpr uint32_t CUNIT_LIVE_STATE_OFFSETS[] = {0x10, 0x18, 0x20, 0x38};
static constexpr uint32_t CUTIN_DRAW_LAYER_BITS = 0x00700000;
static constexpr uint32_t MTOBJECT_VISIBLE_FLAG = 0x00004000;
static constexpr size_t SMAIN_SIZE = 0x40080;
static constexpr int SSHOT_CATEGORY = 10;
static constexpr uint32_t SSHOT_ID_POOL_OFFSET = 0x38;
static constexpr size_t SSHOT_ID_POOL_SIZE = 0x404;
static constexpr uintptr_t PTR_SGAMECONFIG = 0x140D50F88;
static constexpr uint32_t SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET = 0x4A89C;
static constexpr size_t SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE = 10;
static constexpr uint32_t SAUX_FIRST_CATEGORY = 9;
static constexpr uint32_t SSHOT_FIRST_CATEGORY = 10;
static constexpr uint32_t SEFFECT_LAST_CATEGORY = 16;
static constexpr int MAX_REGIONS = ROLLBACK_MAX_REGIONS;
static constexpr size_t SHITSOLVER_SIZE = 0x3808C;
static constexpr uint64_t MT_DTI_VTABLE = 0x140A69ED8;
static constexpr uintptr_t DTI_UACTION_SIDE_CAMERA = 0x140D44268;
static constexpr size_t SHITSOLVER_DATA_OFF = 0x58;

static void RestoreCharacterCollisionParentReferences(RollbackSnapshotSlot& slot)
{
    for (const RollbackCharacterCollisionParentReference& reference :
         slot.characterCollisionParentReferences) {
        auto* character = (uint8_t*)reference.character;
        if (!character || *(uint64_t*)character != reference.expectedCharacterVtable)
            continue;

        void* parent = nullptr;
        switch (reference.kind) {
        case RollbackCharacterCollisionParentKind::None:
            break;
        case RollbackCharacterCollisionParentKind::Character:
            parent = character;
            break;
        case RollbackCharacterCollisionParentKind::Weapon: {
            uint32_t weaponCount =
                *(uint32_t*)(character + CCHR_WEAPON_COUNT_OFFSET);
            void** weapons =
                *(void***)(character + CCHR_WEAPON_ARRAY_OFFSET);
            if (!weapons || weaponCount > 256 ||
                reference.weaponIndex >= weaponCount) {
                continue;
            }
            parent = weapons[reference.weaponIndex];
            if (!parent) continue;
            break;
        }
        }
        *(void**)(character + reference.offset) = parent;
    }
}

using SnapshotRange = RollbackSnapshotRange;
using ShotRecipe = RollbackShotRecipe;
using SnapshotSlot = RollbackSnapshotSlot;
static constexpr SnapshotRange UACTION_CAMERA_SCALAR_RESTORE_RANGES[] = {
    // Camera transform/view state; retain the unit header and ownership links.
    {0x50, 0x50},
    {0x80, 0x30},
    // Aim mode and tracking state, excluding attachment pointers.
    {0xB0, 0x04},
    {0xB8, 0x30},
    // Position-attachment joint selector.
    {0xE8, 0x04},
    // Aim interpolation payload.
    {0xF0, 0x50},
    {0x140, 0x40},
    {0x180, 0x40},
    // Include the aim block's tail through +0x224.
    {0x1C0, 0x65},
    // Maya playback/tracking state; resource +0x290 and parent +0x298 are handled separately.
    {0x230, 0x60},
    {0x2A0, 0x120},
    // Pending Maya request parameters; resource +0x3C0 and parent +0x3D0 are handled separately.
    {0x3C8, 0x08},
    {0x3D8, 0x20},
    // Field offsets used by camera tracking.
    {0x400, 0x10},
    // Camera center, overlook, and runtime tracking state.
    {0x430, 0x20},
    {0x450, 0x40},
    // Runtime state and camera tuning; leave height-target pointer +0x4C8 live.
    {0x490, 0x38},
    {0x4D8, 0xA4},
    // Camera option flags and Maya active/request flags at +0x588/+0x589.
    {0x57C, 0x0E},
};
using ShotEnqueueFn = char(__fastcall*)(void*, void*, int);
using UnitAddFn = char(__fastcall*)(void*, int, void*, void*);
using UnitRemoveFn = char(__fastcall*)(void*, void*);
#define g_slots g_rollbackSnapshotSlots

static constexpr SnapshotRange CCHR_SCALAR_RESTORE_RANGES[] = {
    // Explicit restore overrides; not every legacy range below is pointer-free.
    // Unit timing state.
    {0x28, 0x08},
    // Selected model and character-state fields.
    {0x1190, 0x04}, {0x1198, 0x04},
    {0x1529, 0x02}, {0x2892, 0x02}, {0x2AAA, 0x02}, {0x3D38, 0x04},
    // Coordinate selectors, position, rotation, and scale.
    {0x48, 0x38},
    // Local/world matrices; leave the model resource/material block live.
    {0x80, 0x80},
    // Local-matrix component, already included in the preceding range.
    {0xA8, 0x04},
    // Motion calculation/interpolation flags; leave model status at +0x550 live.
    {0x558, 0x08},
    // Actor animation playback header.
    {0x1300, 0x08},
    // Actor animation timing and transitions.
    {0x1308, 0x40},
    // Legacy command-interpreter restore: includes resource, owner, and command pointers.
    {0x1350, 0x98},
    // Hip-placement scaling; packed floats can be mistaken for pointers.
    {0x1480, 0x08},
    // Character/program flags.
    {0x14F0, 0x28},
    // Adjacent packed action flags.
    {0x1518, 0x18},
    // Status mode and life-related scalars, excluding status ownership pointers.
    {0x1548, 0x18},
    // Input has a dedicated restore below, not through this disabled range.
    //{0x21D8, 0x560},
    // Attack-controller scalar header.
    {0x18A0, 0x28},
    // Player/team/roster identity and appearance/assist selection.
    {0x2D9C, 0x24},
    // Character RNG and move counter; exclude setup state at +0x2D90.
    {0x2D80, 0x10},
    {0x2D94, 0x04},
    // Character body/assist configuration state.
    {0x28B8, 0x20},
    // Character state byte; exact meaning is unresolved.
    {0x28DC, 0x01},
    // Special/combo matcher flag predicates, overriding false pointer matches.
    {0x27B0, 0x0C}, {0x29C8, 0x0C},
    // Combo-command matcher tail, excluding the reference at +0x2AD0.
    {0x2AD8, 0x18},
    // Weapon-slot count.
    {0x3F38, 0x04},
    // Animation requests, extra-part state, and related playback parameters.
    {0x3F48, 0x18C},
    // Movement vectors, speed, and acceleration, overriding false pointer matches.
    {0x40B0, 0x70},
    // Movement, collision, and guard/charge-related state.
    {0x4120, 0xD8},
    // Overlapping movement/collision state.
    {0x4168, 0x90},
    // Sixteen capsule-position cache entries, each a 16-byte vector slot.
    {0x4D10, 0x100},
    // Damage-state header and counters.
    {0x6340, 0x10},
    // Damage/gauge multipliers.
    {0x6350, 0x10},
    // Damage, air-state, combo, stun, and knockdown counters.
    {0x6360, 0xC8},
    // Object-return reference (remapped later), followed by hit/animation state.
    {0x6428, 0x08}, {0x6430, 0x2C},
    // Projectile repeat limits and defense/muteki selectors.
    {0x67F8, 0x10},
    // Throw-reaction reuse state and cached attack transforms.
    {0x680D, 0x01}, {0x6810, 0x24}, {0x6828, 0x04}, {0x6840, 0x80},
    // Combat/link finalization state, including the +0x68C4 latch.
    {0x68C0, 0x10},
    // Late character parameters and material-effect vector, not attack transforms.
    {0x68D0, 0x20},
    // Subclass extension: interpretation at +0x68F0 depends on the concrete character.
    {0x68F0, 0x04},
};

// Reapply selected character fields after recipe/model reconstruction.
static constexpr SnapshotRange CCHR_POST_RESTORE_TAIL_RANGES[] = {
    // Hip scaling and damage/gauge multipliers.
    {0x1480, 0x08},
    {0x6350, 0x10},
    // Active collision-bank selectors.
    {0x6360, 0x10},
    {0x6430, 0x2C},
    {0x67F8, 0x10}, {0x68C0, 0x10},
    // Throw-reaction state and cached attack transforms.
    {0x680D, 0x01}, {0x6810, 0x24}, {0x6828, 0x04}, {0x6840, 0x80},
    // Late character parameters and material-effect vector.
    {0x68D0, 0x20},
    // Subclass-specific first tail field.
    {0x68F0, 0x04},
    // Five target-info payloads; exclude each vtable and character reference.
    {0x64A0, 0x70}, {0x6520, 0x70}, {0x65A0, 0x70},
    {0x6620, 0x70}, {0x66A0, 0x70},
    // Animation extra-part counters.
    {0x3F84, 0x18},
    // Animation-related scalar at +0x3EA8.
    {0x3EA8, 0x04},
    // Capsule-position cache.
    {0x4D10, 0x100},
    // Packed action flags and command-matcher tail.
    {0x1518, 0x18},
    {0x2AD8, 0x18},
};

static constexpr SnapshotRange SMAIN_RESTORE_RANGES[] = {
    // Main frame/pause controls.
    {0x30, 0x64},
    // Frame timing state.
    {0xA4, 0x1C},
    // Native frame-pacing accumulators.
    {0x40010, 0x10},
    // Timing values and counter-conversion state.
    {0x40028, 0x18},
    // Current counter/timing values.
    {0x40048, 0x14},
};

static constexpr SnapshotRange SACTION_RESTORE_RANGES[] = {
    // Action timing, flags, and phase state.
    {0x28, 0xE8},
    // Action transition/frame-sync fields.
    {0x140, 0x08},
    // Additional action transition state.
    {0x150, 0x08},
    // Restore the flag at +0x170, leaving the live fade reference at +0x168 untouched.
    {0x170, 0x01},
    // Action phase counter.
    {0x218, 0x04},
};

static constexpr SnapshotRange CCHR_TEAM_RESTORE_RANGES[] = {
    // Team timing, flags, life, meter, assist, and X-Factor state.
    {0x50, 0xC0},
    // Overlapping team program/transition state, including +0xFC.
    {0xC0, 0x50},
    // Remaining team position, orientation, and replay state.
    {0x110, 0x1B0},
};

namespace {

constexpr size_t SCOLLISION_SIZE = 0x239E0;
constexpr uint32_t SCOLLISION_BODY_START = 0x38;
constexpr uint32_t SCOLLISION_ALLOC_START = 0x23970;
constexpr size_t CCHR_EXTERNAL_ANIM_SIZE = 0xE0;
constexpr uintptr_t FN_COLLISION_NODE_OBJECT_CREATE = 0x1405AD2A0;
constexpr uintptr_t FN_COLLISION_NODE_COPY = 0x1405A4DA0;
constexpr uintptr_t FN_COLLISION_NODE_PRUNE_CHILD = 0x14004C9D0;

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

    // Restore the inline body, preserving pointer-like words and the allocator tail.
    std::memcpy((uint8_t*)destination + SCOLLISION_BODY_START,
                source + SCOLLISION_BODY_START,
                SCOLLISION_ALLOC_START - SCOLLISION_BODY_START);
    for (const LivePointerField& field : livePointers)
        *(uint64_t*)((uint8_t*)destination + field.offset) = field.value;
}

void Rollback_RestoreExternalAnimBlock(void* destination, const uint8_t* source, size_t size)
{
    if (size != CCHR_EXTERNAL_ANIM_SIZE) return;

    // Restore selected wrapper fields and clear +0x74; keep live topology/user data.
    std::memcpy((uint8_t*)destination + 0x08, source + 0x08, 0x01);
    *((uint8_t*)destination + 0x74) = 0;
    std::memcpy((uint8_t*)destination + 0x80, source + 0x80, 0x10);
    std::memcpy((uint8_t*)destination + 0x90, source + 0x90, 0x28);
    std::memcpy((uint8_t*)destination + 0xC8, source + 0xC8, 0x18);
}

void Rollback_DestroyCollisionNodeClones(RollbackSnapshotSlot& slot)
{
    bool destroyedClone = false;
    for (const RollbackCollisionNodeClone& entry : slot.collisionNodeClones) {
        if (!entry.clone) continue;
        uint64_t vtable = *(uint64_t*)entry.clone;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        if (destroy) {
            destroy(entry.clone, 1);
            destroyedClone = true;
        }
    }
    if (destroyedClone) InvalidateMemoryQueryCache();
    slot.collisionNodeClones.clear();
}

void Rollback_CaptureCollisionNodeClones(RollbackSnapshotSlot& slot)
{
    auto createNode =
        (void*(__fastcall*)(void*))_addr(FN_COLLISION_NODE_OBJECT_CREATE);
    auto copyNode =
        (void(__fastcall*)(void*, void*, bool, bool))_addr(FN_COLLISION_NODE_COPY);

    size_t capturedCount = 0;
    bool mutatedClones = false;
    uint32_t capturedChildren = 0;
    uint32_t cloneCopies = 0;
    uint32_t emptyCloneCopies = 0;
    uint32_t cloneCreates = 0;
    uint32_t cloneDestroys = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        const RollbackRegion& region = slot.regions[i];
        if (!region.ownerSlot || region.size != CCHR_EXTERNAL_ANIM_SIZE) continue;

        uint32_t childCount = *(const uint32_t*)((const uint8_t*)region.ptr + 0x20);
        uint64_t childArray = *(const uint64_t*)((const uint8_t*)region.ptr + 0x30);
        if (childCount > 4096 || (childCount != 0 && !childArray)) {
            HookLog("Rollback: collision wrapper has invalid child array "
                    "wrapper=0x%llX count=%u data=0x%llX\n",
                    (uint64_t)region.ptr, childCount, childArray);
            continue;
        }

        const uint64_t wrapperDti = GetObjectDti((uint64_t)region.ptr);
        if (!wrapperDti) continue;

        size_t existingIndex = capturedCount;
        while (existingIndex < slot.collisionNodeClones.size() &&
               slot.collisionNodeClones[existingIndex].ownerSlot != region.ownerSlot) {
            ++existingIndex;
        }
        if (existingIndex == slot.collisionNodeClones.size()) {
            slot.collisionNodeClones.emplace_back();
        } else if (existingIndex != capturedCount) {
            std::swap(slot.collisionNodeClones[capturedCount],
                      slot.collisionNodeClones[existingIndex]);
        }

        RollbackCollisionNodeClone& entry =
            slot.collisionNodeClones[capturedCount];
        entry.ownerSlot = region.ownerSlot;
        entry.savedPtr = region.ptr;
        entry.wrapperDti = wrapperDti;
        entry.childCount = childCount;
        capturedChildren += childCount;
        std::memcpy(entry.scalarState, region.ptr, sizeof(entry.scalarState));
        entry.childDtis.resize(childCount);
        for (uint32_t child = 0; child < childCount; ++child) {
            uint64_t childPtr = *(const uint64_t*)(
                childArray + (size_t)child * sizeof(uint64_t));
            // Record child types for comparison after native copyFrom.
            entry.childDtis[child] = childPtr ? GetObjectDti(childPtr) : 0;
        }
        if (childCount == 0) {
            if (entry.clone) {
                copyNode(entry.clone, region.ptr, false, true);
                mutatedClones = true;
                ++cloneCopies;
                ++emptyCloneCopies;
            }
            ++capturedCount;
            continue;
        }

        if (!entry.clone) {
            entry.clone = createNode(nullptr);
            if (!entry.clone) continue;
            ++cloneCreates;
        }
        // false copies the source factory; true copies children through virtual +0x40.
        copyNode(entry.clone, region.ptr, false, true);
        mutatedClones = true;
        ++cloneCopies;
        ++capturedCount;
    }

    // Reuse clone roots across ring-slot captures; destroy roots no longer needed.
    for (size_t i = capturedCount; i < slot.collisionNodeClones.size(); ++i) {
        RollbackCollisionNodeClone& entry = slot.collisionNodeClones[i];
        if (!entry.clone) continue;
        uint64_t vtable = *(uint64_t*)entry.clone;
        auto destroy = (void(__fastcall*)(void*, uint32_t))*(uint64_t*)vtable;
        if (destroy) {
            destroy(entry.clone, 1);
            mutatedClones = true;
            ++cloneDestroys;
        }
    }
    slot.collisionNodeClones.resize(capturedCount);
    if (mutatedClones) InvalidateMemoryQueryCache();
    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        if (g_activeRollbackProfile) {
            g_activeRollbackProfile->snapshotCloneWrappers += capturedCount;
            g_activeRollbackProfile->snapshotCloneChildren += capturedChildren;
            g_activeRollbackProfile->snapshotCloneCopies += cloneCopies;
            g_activeRollbackProfile->snapshotCloneEmptyCopies += emptyCloneCopies;
            g_activeRollbackProfile->snapshotCloneCreates += cloneCreates;
            g_activeRollbackProfile->snapshotCloneDestroys += cloneDestroys;
        }
    }
}

void Rollback_RestoreCollisionNodeClones(RollbackSnapshotSlot& slot)
{
    auto copyNode =
        (void(__fastcall*)(void*, void*, bool, bool))_addr(FN_COLLISION_NODE_COPY);
    auto pruneChild =
        (void(__fastcall*)(void*, uint32_t))_addr(FN_COLLISION_NODE_PRUNE_CHILD);
    bool mutatedChildren = false;
    for (const RollbackCollisionNodeClone& entry : slot.collisionNodeClones) {
        void* destination = *(void**)entry.ownerSlot;
        if (!destination ||
            GetObjectDti((uint64_t)destination) != entry.wrapperDti) {
            HookLog("Rollback: collision wrapper incompatible owner=0x%llX "
                    "saved=0x%llX live=0x%llX dti=0x%llX\n",
                    entry.ownerSlot, (uint64_t)entry.savedPtr,
                    (uint64_t)destination, entry.wrapperDti);
            continue;
        }

        // Restore wrapper fields without replacing the current allocation/user data.
        Rollback_RestoreExternalAnimBlock(destination, entry.scalarState,
                                           sizeof(entry.scalarState));

        if (entry.childCount == 0) {
            auto* childArray = (uint8_t*)destination + 0x18;
            uint32_t liveCount = *(uint32_t*)((uint8_t*)destination + 0x20);
            mutatedChildren |= liveCount != 0;
            while (liveCount > 0)
                pruneChild(childArray, --liveCount);
        } else if (entry.clone) {
            copyNode(destination, entry.clone, false, true);
            mutatedChildren = true;
        }

        uint32_t restoredCount = *(uint32_t*)((uint8_t*)destination + 0x20);
        uint64_t restoredArray = *(uint64_t*)((uint8_t*)destination + 0x30);
        bool valid = restoredCount == entry.childCount &&
            entry.childDtis.size() == entry.childCount;
        for (uint32_t child = 0; valid && child < restoredCount; ++child) {
            uint64_t restoredChild = restoredArray ?
                *(uint64_t*)(restoredArray + (size_t)child * sizeof(uint64_t)) : 0;
            // Check the restored child DTI sequence, not historical addresses.
            uint64_t restoredDti = restoredChild ? GetObjectDti(restoredChild) : 0;
            valid = restoredDti == entry.childDtis[child];
        }
        if (!valid) {
            HookLog("Rollback: collision graph restore mismatch owner=0x%llX "
                    "wrapper=0x%llX expectedChildren=%u actualChildren=%u\n",
                    entry.ownerSlot, (uint64_t)destination,
                    entry.childCount, restoredCount);
        }
    }
    if (mutatedChildren) InvalidateMemoryQueryCache();
}


// Restore the RNG streams captured immediately before this shot's original spawn.
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

    // Hold recipe rShot resources across teardown; native shot destructors release them.
    auto addRef = (void(__fastcall*)(void*))_addr(0x14050D100);
    auto releaseRef = (void(__fastcall*)(void*))_addr(0x14050D5A0);
    std::vector<void*> pinnedShotResources;
    std::vector<void*> pinnedCallbackEffectResources;
    pinnedShotResources.reserve(slot.shotRecipes.size());
    for (const ShotRecipe& saved : slot.shotRecipes) {
        if (!saved.shotElement || !HasCallableDtiVirtual(saved.shotElement))
            continue;
        addRef(saved.shotElement);
        pinnedShotResources.push_back(saved.shotElement);
    }


    // Keep model and callback resources loaded while their owning shots are replaced.
    std::vector<void*> currentShots;
    Shot_CollectCurrent(currentShots);
    for (void* shot : currentShots) {
        Rollback_PinShotModelResources(
            shot, pinnedShotResources, pinnedCallbackEffectResources);
        for (uint32_t index = 0; index < 13; ++index) {
            void* effect = *(void**)((uint8_t*)shot + 0x1F78 + index * 8);
            if (!effect || !IsEffectObject(effect)) continue;
            void* resource = *(void**)((uint8_t*)effect + 0x108);
            if (!resource || !HasCallableDtiVirtual(resource) ||
                std::find(pinnedCallbackEffectResources.begin(),
                          pinnedCallbackEffectResources.end(), resource) !=
                    pinnedCallbackEffectResources.end()) {
                continue;
            }
            addRef(resource);
            pinnedCallbackEffectResources.push_back(resource);
        }
    }
    for (const RollbackEffectRecipe& saved : slot.effectRecipes) {
        void* resource = (void*)saved.state.resource;
        if (!saved.rocketShort || !resource || !HasCallableDtiVirtual(resource) ||
            std::find(pinnedCallbackEffectResources.begin(),
                      pinnedCallbackEffectResources.end(), resource) !=
                pinnedCallbackEffectResources.end())
            continue;
        addRef(resource);
        pinnedCallbackEffectResources.push_back(resource);
    }
    const uint64_t pinResourcesEnd = Profile_Now();

    int removedInvalidUnits = 0;
    int retiredEffects = 0;
    uint64_t preSanitizeEnd = 0;
    {
        // Retain transition tears before flushing the tracked kill queue.
        uint64_t sGameEffect = *(uint64_t*)_addr(PTR_SGAMEEFFECT);
        if (sGameEffect && *(uint32_t*)(sGameEffect + 0x128) != 0) {
            Effect_FlushTrackedForRestore((void*)sGameEffect);
            InvalidateMemoryQueryCache();
        }

        ScopedMemoryQueryCache memoryCache;
        removedInvalidUnits = Shot_SanitizeUnitCategoryLists(
            sUnit, SAUX_FIRST_CATEGORY, SEFFECT_LAST_CATEGORY);
        if (removedInvalidUnits != 0) {
            HookLog("Rollback: removed %d invalid sUnit category entries before restore\n",
                     removedInvalidUnits);
        }
        preSanitizeEnd = Profile_Now();

        // Invoke virtual kill only for laser shots; other classes may tear down needed parents.
        for (void* shot : currentShots) {
            uint64_t dti = GetObjectDti((uint64_t)shot);
            if (!Rollback_ShouldFinalizeShotBeforeRetirement(dti)) {
                continue;
            }
            uint64_t vtable = *(uint64_t*)shot;
            uint64_t kill = *(uint64_t*)(vtable + 0x78);
            if (kill)
                ((void(__fastcall*)(void*))kill)(shot);
        }

        // Retire replaceable effects, preserving the effect policy's retained owners.
        // Weapon Cart's model effects need a separate retirement path.
        retiredEffects += Rollback_RetireShotModelEffects(sUnit, currentShots);
        retiredEffects += Effect_RetireCurrent(sUnit);
    }
    const uint64_t retireEffectsEnd = Profile_Now();

    // Unlink and destroy current shots before reconstruction, rather than awaiting deferred cleanup.
    int retiredShots = 0;
    if (g_activeRollbackProfile)
        g_activeRollbackProfile->restoreCurrentShots += (uint32_t)currentShots.size();
    const uint64_t collectShotsEnd = Profile_Now();
    for (void* shot : currentShots) {
        // Release the embedded uEffect graph and detach its links before shot destruction.
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

    // Recheck scheduler entries after shot destructors may have released attached effects.
    // Skip this second list scan when there were no shots to destroy.
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
    void* sGameConfig = *(void**)_addr(PTR_SGAMECONFIG);
    const uint8_t* savedShotIdPool = nullptr;
    const uint8_t* savedShotSlotFlags = nullptr;
    size_t regionOffset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        if (slot.regions[i].ptr == (uint8_t*)sShot + SSHOT_ID_POOL_OFFSET &&
            slot.regions[i].size >= SSHOT_ID_POOL_SIZE &&
            regionOffset + SSHOT_ID_POOL_SIZE <= slot.size) {
            savedShotIdPool = slot.data + regionOffset;
        }
        if (sGameConfig &&
            slot.regions[i].ptr == (uint8_t*)sGameConfig +
                SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET &&
            slot.regions[i].size >= SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE &&
            regionOffset + SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE <= slot.size) {
            savedShotSlotFlags = slot.data + regionOffset;
        }
        regionOffset += slot.regions[i].size;
        if (savedShotIdPool && savedShotSlotFlags) break;
    }
    // Restore the ID pool/cursor after destruction and before factories allocate IDs.
    if (savedShotIdPool) {
        std::memcpy((uint8_t*)sShot + SSHOT_ID_POOL_OFFSET,
                    savedShotIdPool, SSHOT_ID_POOL_SIZE);
    }
    // Restore slot-active flags changed by shot teardown.
    if (savedShotSlotFlags) {
        std::memcpy((uint8_t*)sGameConfig + SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET,
                    savedShotSlotFlags, SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE);
    }
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

    // Save restored RNG state so reconstruction does not consume gameplay randomness.
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

    // Preserve each distinct shot owner's RNG as well as the global/effect streams.
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
    uint64_t respawnConcreteTicks = 0;
    uint64_t respawnOverlayTicks = 0;
    std::vector<uintptr_t> waitedForModelDtis;
    const uint64_t referenceCacheStart = Profile_Now();
    Shot_PrepareReferenceResolution(slot.shotRecipes);
    const uint64_t referenceCacheEnd = Profile_Now();
    for (ShotRecipe& saved : slot.shotRecipes) {
        const uint64_t prepareStart = Profile_Now();
        saved.restoredPtr = nullptr;
        RestorePreSpawnRng(saved);
        void* objectRef1 = Shot_ResolveReference(saved.objectRef1);
        void* objectRef2 = Shot_ResolveReference(saved.objectRef2);
        ShotRecipe remappedRecipe;
        const ShotRecipe* spawnRecipe = &saved;
        if (Rollback_RemapShotSpawnParent(
                saved, slot.shotRecipes, remappedRecipe))
            spawnRecipe = &remappedRecipe;
        const uint64_t factoryStart = Profile_Now();
        respawnPrepareTicks += factoryStart - prepareStart;

        RollbackShotFactoryPolicyState factoryPolicy;
        Rollback_PrepareShotFactory(saved, factoryPolicy);
        void* fresh = Shot_SpawnOriginal(
            sShot, *spawnRecipe, objectRef1, objectRef2);
        Rollback_FinishShotFactory(factoryPolicy);
        const uint64_t factoryEnd = Profile_Now();
        respawnFactoryTicks += factoryEnd - factoryStart;
        if (!fresh) {
            failedShots++;
            continue;
        }

        // Associate the fresh shot with the saved spawn sequence before class-specific repair.
        Shot_TrackReconstructed(fresh, *spawnRecipe);
        saved.restoredPtr = fresh;

        Rollback_PrepareShotModelAfterSpawn(
            fresh, saved.concrete.dti, waitedForModelDtis);

        // Retry missing callback setup only if all fresh slots are empty.
        // Repeating setup over existing callbacks can orphan registered effects.
        bool savedHasCallbacks = false;
        bool freshHasCallbacks = false;
        for (uint32_t index = 0; index < 13; ++index) {
            savedHasCallbacks |=
                saved.concrete.callbackEffects[index].occupied;
            freshHasCallbacks |=
                *(void**)((uint8_t*)fresh + 0x1F78 + index * sizeof(void*)) !=
                nullptr;
        }
        if (savedHasCallbacks && !freshHasCallbacks) {
            uint64_t vtable = *(uint64_t*)fresh;
            uint64_t setupAddress = *(uint64_t*)(vtable + 0x28);
            if (setupAddress) {
                auto setup = (void(__fastcall*)(void*))setupAddress;
                setup(fresh);
            }
        }
        Rollback_RebuildShotStateBeforeRegistration(fresh, saved);
        // Queue native registration, falling back to direct insertion at capacity.
        // Enqueue increments even on overflow; never call it on a full 512-entry queue.
        uint32_t pendingCount = *pendingCountPtr;
        if (pendingCount >= 0x200 || !enqueueUnit(sShot, fresh, SSHOT_CATEGORY)) {
            addUnit(sUnit, SSHOT_CATEGORY, fresh, nullptr);
        }
        const uint64_t registerEnd = Profile_Now();
        respawnRegisterTicks += registerEnd - factoryEnd;
        // Target-hit records use object identity; include assist/proxy nodes from both team lists.
        std::vector<const void*> validTargets;
        validTargets.reserve(16);
        uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
        for (int teamIndex = 0; sCharacter && teamIndex < 2; ++teamIndex) {
            uint64_t node = sCharacter +
                (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
                TEAM_FIRST_NODE_OFFSET;
            for (int safety = 16; node && safety-- > 0;) {
                void* character = *(void**)(node + NODE_CHR_OFFSET);
                if (character &&
                    std::find(validTargets.begin(), validTargets.end(), character) ==
                        validTargets.end()) {
                    validTargets.push_back(character);
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
        }
        // Also accept the stage manager's two body references as stable hit targets.
        uint64_t sStage = *(uint64_t*)_addr(PTR_SSTAGE);
        if (sStage) {
            for (uint32_t offset : {0x2050u, 0x2058u}) {
                void* stageBody = *(void**)(sStage + offset);
                if (stageBody &&
                    std::find(validTargets.begin(), validTargets.end(), stageBody) ==
                        validTargets.end()) {
                    validTargets.push_back(stageBody);
                }
            }
        }
        if (!ConcreteShotSnapshot_Restore(fresh, saved.concrete,
                                           validTargets.data(),
                                           validTargets.size())) {
            HookLog("Rollback: restored shot 0x%llX has incomplete collision topology\n",
                     (uint64_t)fresh);
        }
        if (saved.concrete.body.size() >= 0x1F54) {
            // Replace the factory-assigned ID with the snapshot ID.
            std::memcpy((uint8_t*)fresh + 0x1F50,
                        saved.concrete.body.data() + 0x1F50,
                        sizeof(uint32_t));
        }
        const uint64_t concreteEnd = Profile_Now();
        respawnConcreteTicks += concreteEnd - registerEnd;
        // Reapply explicit arm/flight/class state after the filtered body overlay.
        Shot_RestoreArmState(fresh, saved);
        Shot_ApplyFlight(fresh, saved.flight);
        Rollback_RestoreShotClassState(fresh, saved);

        // Remap +0x1F70 when the captured value was a shot self-reference.
        void* savedSlot0 = nullptr;
        std::memcpy(&savedSlot0, saved.flight.resourceScalars,
                    sizeof(savedSlot0));
        if (savedSlot0 == saved.savedPtr)
            *(void**)((uint8_t*)fresh + 0x1F70) = fresh;

        // Keep restored lifecycle flags, but explicitly assign the shot category.
        uint32_t* unitState = (uint32_t*)((uint8_t*)fresh + 0x10);
        *unitState = (*unitState & ~0x3F8u) | (SSHOT_CATEGORY << 3);

        // Reconcile callback occupancy and compatible state after restoring shot gameplay fields.
        // Callback allocations and generator topology remain factory-owned.
        ConcreteShotSnapshot_RestoreCallbackEffects(fresh, saved.concrete);
        const uint64_t overlayEnd = Profile_Now();
        respawnOverlayTicks += overlayEnd - concreteEnd;
        respawnedShots++;
    }
    const uint64_t respawnEnd = Profile_Now();

    Shot_RestoreBodyReferences(slot);

    // Restore typed shot links first, then clear remaining invalid object references.
    // +0x90 can also hold packed non-pointer values; +0x6428 is an object reference.
    std::vector<void*> characters;
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    for (int teamIndex = 0; sCharacter && teamIndex < 2; ++teamIndex) {
        uint64_t node = sCharacter +
            (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
            TEAM_FIRST_NODE_OFFSET;
        for (int member = 0; node && member < 8; ++member) {
            void* character = *(void**)(node + NODE_CHR_OFFSET);
            if (character) characters.push_back(character);
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    uint32_t categoryCount = sUnit
        ? *(uint32_t*)((uint8_t*)sUnit + SUNIT_CATEGORY_COUNT_OFFSET)
        : 0;
    if (categoryCount > SUNIT_CHARACTER_CATEGORY) {
        void* character = *(void**)((uint8_t*)sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
            SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
        for (int safety = 64; character && safety-- > 0;
             character = *(void**)((uint8_t*)character + 0x18)) {
            if (std::find(characters.begin(), characters.end(), character) == characters.end())
                characters.push_back(character);
        }
    }
    const uint64_t characterReferencesEarlyStart = Profile_Now();
    Rollback_RestoreCharacterShotReferences(slot);
    const uint64_t characterReferencesEarlyEnd = Profile_Now();
    for (void* character : characters) {
        for (uint32_t referenceOffset : {0x90u, 0x6428u}) {
            bool typedShotReference = std::any_of(
                slot.characterShotReferences.begin(),
                slot.characterShotReferences.end(),
                [=](const RollbackCharacterShotReference& saved) {
                    return saved.character == character &&
                           saved.offset == referenceOffset;
                });
            if (typedShotReference) continue;

            void** objectReturn = (void**)((uint8_t*)character + referenceOffset);
            void* savedReference = *objectReturn;
            const bool abandonedShot =
                std::find(currentShots.begin(), currentShots.end(), savedReference) !=
                currentShots.end();
            const bool invalidObject = savedReference &&
                (referenceOffset == 0x6428u ||
                 IsLikelyPointer((uint64_t)savedReference)) &&
                !HasCallableDtiVirtual(savedReference);
            if (abandonedShot || invalidObject) {
                *objectReturn = nullptr;
            }
        }
    }

    // Recompute character-specific shot latches from the reconstructed recipe set.
    size_t regionDataOffset = 0;
    for (int region = 0; region < slot.regionCount; ++region) {
        const RollbackRegion& savedRegion = slot.regions[region];
        if (regionDataOffset + savedRegion.size <= slot.size) {
            Rollback_RemapCharacterShotLinks(
                savedRegion.ptr, savedRegion.expectedVtable,
                slot.data + regionDataOffset, savedRegion.size,
                slot.shotRecipes);
        }
        regionDataOffset += savedRegion.size;
    }

    // Clamp before native flush, which trusts the count rather than the 512-entry capacity.
    if (*pendingCountPtr > 0x200) {
        HookLog("Rollback: clamping pending-shot count %u to 512 before flush\n",
                *pendingCountPtr);
        *pendingCountPtr = 0x200;
    }
    flushPendingUnits(sShot);

    // Evaluate saved poses after pending registration has completed.
    for (const RollbackShotRecipe& saved : slot.shotRecipes) {
        if (!saved.restoredPtr) continue;
        Rollback_EvaluateRestoredShotPose(
            saved.restoredPtr, saved.concrete.dti);
    }

    // Remap shot-valued factory arguments before reconstructing independent effects.
    for (RollbackEffectRecipe& effect : slot.effectRecipes) {
        for (const ShotRecipe& shot : slot.shotRecipes) {
            auto remap = [&](void*& reference) {
                if (reference == shot.savedPtr) reference = shot.restoredPtr;
            };
            remap(effect.attachTo);
            remap(effect.a7);
            remap(effect.a8);
            remap(effect.a10);
            remap(effect.a13);
        }
    }
    Effect_RestoreRecipes(slot);
    const uint64_t characterEffectReferencesEarlyStart = Profile_Now();
    Effect_RestoreCharacterReferences(slot);
    const uint64_t characterEffectReferencesEarlyEnd = Profile_Now();
    Effect_RestoreCharacterStates(slot);

    // Undo reconstruction's ID/slot allocations now that saved shot IDs are restored.
    if (savedShotIdPool) {
        std::memcpy((uint8_t*)sShot + SSHOT_ID_POOL_OFFSET,
                    savedShotIdPool, SSHOT_ID_POOL_SIZE);
    }
    if (savedShotSlotFlags) {
        std::memcpy((uint8_t*)sGameConfig + SGAMECONFIG_SHOT_SLOT_FLAGS_OFFSET,
                    savedShotSlotFlags, SGAMECONFIG_SHOT_SLOT_FLAGS_SIZE);
    }

    std::memcpy((void*)_addr(0x140d765d8), restoredGlobalRng, sizeof(restoredGlobalRng));
    if (sEffect) std::memcpy((void*)(sEffect + 0x3C), restoredEffectRng, sizeof(restoredEffectRng));
    for (const SavedCharacterRng& characterRng : restoredCharacterRngs) {
        std::memcpy((uint8_t*)characterRng.character + 0x2D80, characterRng.state,
                    sizeof(characterRng.state));
    }
    for (void* resource : pinnedShotResources)
        releaseRef(resource);
    for (void* resource : pinnedCallbackEffectResources)
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
        g_activeRollbackProfile->restoreShotReferenceCache +=
            referenceCacheEnd - referenceCacheStart;
        g_activeRollbackProfile->restoreShotRespawn += respawnEnd - respawnSetupEnd;
        g_activeRollbackProfile->restoreShotRespawnPrepare += respawnPrepareTicks;
        g_activeRollbackProfile->restoreShotRespawnFactory += respawnFactoryTicks;
        g_activeRollbackProfile->restoreShotRespawnRegister += respawnRegisterTicks;
        g_activeRollbackProfile->restoreShotRespawnConcrete += respawnConcreteTicks;
        g_activeRollbackProfile->restoreShotRespawnOverlay += respawnOverlayTicks;
        g_activeRollbackProfile->restoreShotFinalize += finalizeEnd - respawnEnd;
        g_activeRollbackProfile->restoreCharacterShotReferencesEarly +=
            characterReferencesEarlyEnd - characterReferencesEarlyStart;
        g_activeRollbackProfile->restoreCharacterShotReferenceCount +=
            (uint32_t)slot.characterShotReferences.size();
        g_activeRollbackProfile->restoreCharacterEffectReferencesEarly +=
            characterEffectReferencesEarlyEnd -
            characterEffectReferencesEarlyStart;
        g_activeRollbackProfile->restoreCharacterEffectReferenceCount +=
            (uint32_t)slot.characterEffectReferences.size();
    }

    if (failedShots != 0) {
        HookLog("Rollback: retired shots=%d effects=%d; shot recipes respawned=%d failed=%d\n",
                retiredShots, retiredEffects, respawnedShots, failedShots);
    }
}

void Rollback_RestoreSnapshot(int32_t frame)
{
    const uint64_t benchmarkStart = LiveBenchmark_Now();
    Rollback_RequireCurrentSnapshotLayout();

    const uint64_t profileStart = Profile_Now();
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    // Reject an empty or reused ring slot.
    if (!slot.data || slot.size == 0 || slot.regionCount == 0 || slot.frame != frame) {
        HookLog("Rollback: no snapshot available for frame %d (slot has frame %d)\n",
                frame, slot.frame);
        return;
    }

    uint32_t savedGlobalRng[4] = {};
    bool foundGlobalRng = false;
    size_t globalRngOffset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        if ((uint64_t)slot.regions[i].ptr == _addr(0x140D765D8) &&
            slot.regions[i].size >= sizeof(savedGlobalRng) &&
            globalRngOffset + sizeof(savedGlobalRng) <= slot.size) {
            std::memcpy(savedGlobalRng, slot.data + globalRngOffset,
                        sizeof(savedGlobalRng));
            foundGlobalRng = true;
            break;
        }
        globalRngOffset += slot.regions[i].size;
    }
    /*uint32_t liveGlobalRng[4] = {};
    std::memcpy(liveGlobalRng, (void*)_addr(0x140D765D8), sizeof(liveGlobalRng));
    HookLog("RNGRESTORE frame=%d stage=begin saved=%08X,%08X,%08X,%08X live=%08X,%08X,%08X,%08X found=%d\n",
            frame,
            savedGlobalRng[0], savedGlobalRng[1], savedGlobalRng[2], savedGlobalRng[3],
            liveGlobalRng[0], liveGlobalRng[1], liveGlobalRng[2], liveGlobalRng[3],
            foundGlobalRng ? 1 : 0);*/

    // Use saved region addresses with owner/vtable checks; dynamic shots/effects are rebuilt later.
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
    auto restoreCnsHelper = [&isTeamCharacter](
                                uint8_t* character, const uint8_t* savedCharacter,
                                size_t copySize, uint32_t helperOffset) {
        // Restore selected uCnsLockChr fields after pointer filtering.
        // Keep its owner, callback, and unrecognized references live.
        static constexpr SnapshotRange scalarRanges[] = {
            {0x90, 0x18}, // pause, joint, and axis selectors
            {0xB8, 0x20}, // first inline cLimitParam payload; skip its vtable
            {0xE0, 0x20}, // second inline cLimitParam payload; skip its vtable
            {0x108, 0x10}, // interpolation duration, length, debug flag, and angle
            {0x120, 0x08}, // mSelfState and mState
            {0x130, 0x48}, // angle/interpolation tail and adjacent bytes
        };
        for (const SnapshotRange& range : scalarRanges) {
            size_t fieldOffset = (size_t)helperOffset + range.offset;
            if (fieldOffset + range.size <= copySize) {
                std::memcpy(character + fieldOffset,
                            savedCharacter + fieldOffset, range.size);
            }
        }

        auto restorePointer = [&](uint32_t relativeOffset, uint64_t value) {
            size_t fieldOffset = (size_t)helperOffset + relativeOffset;
            if (fieldOffset + sizeof(uint64_t) <= copySize)
                *(uint64_t*)(character + fieldOffset) = value;
        };

        size_t targetOffset = (size_t)helperOffset + 0xA8;
        if (targetOffset + sizeof(uint64_t) <= copySize) {
            uint64_t savedTarget = *(const uint64_t*)(savedCharacter + targetOffset);
            if (savedTarget == 0 || isTeamCharacter(savedTarget))
                restorePointer(0xA8, savedTarget);
        }

        size_t limitOffset = (size_t)helperOffset + 0x100;
        if (limitOffset + sizeof(uint64_t) <= copySize) {
            uint64_t savedLimit = *(const uint64_t*)(savedCharacter + limitOffset);
            if (savedLimit == (uint64_t)character + helperOffset + 0xB0)
                restorePointer(0x100, (uint64_t)character + helperOffset + 0xB0);
            else if (savedLimit == (uint64_t)character + helperOffset + 0xD8)
                restorePointer(0x100, (uint64_t)character + helperOffset + 0xD8);
        }

        // Restore only null or either embedded constraint as mpRefCnsLock.
        size_t referenceOffset = (size_t)helperOffset + 0x118;
        if (referenceOffset + sizeof(uint64_t) <= copySize) {
            uint64_t savedReference = *(const uint64_t*)(
                savedCharacter + referenceOffset);
            if (savedReference == 0 ||
                savedReference == (uint64_t)character + 0x3BF0 ||
                savedReference == (uint64_t)character + 0x3D60) {
                restorePointer(0x118, savedReference);
            }
        }
    };
    auto restoreCharacterPostState = [&slot, sCharacter]() {
        if (!slot.characterPost.valid) return;

        uint64_t sUnit = *(uint64_t*)_addr(PTR_SUNIT);
        if (!sUnit || *(uint32_t*)(sUnit + SUNIT_CATEGORY_COUNT_OFFSET) <=
                SUNIT_CHARACTER_POST_CATEGORY) {
            return;
        }
        uint64_t characterPost = *(uint64_t*)(
            sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
            SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_POST_CATEGORY);
        if (!characterPost ||
            GetObjectDti(characterPost) != (uint64_t)_addr(DTI_UCHARACTER_POST)) {
            return;
        }

        *(int32_t*)(characterPost + CUNIT_RNO_OFFSET) = slot.characterPost.rno;
        *(int32_t*)(characterPost + CUNIT_UNIT_PARAM_OFFSET) =
            slot.characterPost.unitParam;
        uint32_t* unitFlags = (uint32_t*)(characterPost + CUNIT_FLAGS_OFFSET);
        *unitFlags = (*unitFlags & ~CUNIT_MOVE_ENABLED_FLAG) |
            (slot.characterPost.moveEnabled ? CUNIT_MOVE_ENABLED_FLAG : 0);
        *(uint64_t*)(characterPost + CUNIT_GROUP_OFFSET) =
            slot.characterPost.unitGroup;

        static_assert(sizeof(slot.characterPost.scalarState) ==
                      UCHARACTER_POST_SCALAR_STATE_SIZE);
        std::memcpy((void*)(characterPost + UCHARACTER_POST_SCALAR_STATE_OFFSET),
                    slot.characterPost.scalarState,
                    UCHARACTER_POST_SCALAR_STATE_SIZE);
        for (int team = 0; team < 2; ++team) {
            uint64_t character = 0;
            uint8_t savedMember = slot.characterPost.cachedCharacterMembers[team];
            uint64_t node = sCharacter
                ? sCharacter + (team == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
                    TEAM_FIRST_NODE_OFFSET
                : 0;
            for (uint8_t member = 0; node && member <= savedMember && member < 8;
                 ++member) {
                if (member == savedMember)
                    character = *(uint64_t*)(node + NODE_CHR_OFFSET);
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
            }
            *(uint64_t*)(characterPost + UCHARACTER_POST_CHARACTER_CACHE_OFFSET +
                         team * sizeof(uint64_t)) = character;
        }
    };
    auto restoreActionSideCamera = [&slot, &isTeamCharacter](
                                       uint64_t camera, const uint8_t* saved,
                                       size_t copySize) {
        for (const SnapshotRange& range : UACTION_CAMERA_SCALAR_RESTORE_RANGES) {
            if ((size_t)range.offset + range.size <= copySize) {
                std::memcpy((uint8_t*)camera + range.offset,
                            saved + range.offset, range.size);
            }
        }

        auto replaceResource = [](void** field, void* restored) {
            if (*field == restored) return;
            auto addRef = (void(__fastcall*)(void*))_addr(FN_MTOBJECT_ADD_REF);
            auto releaseRef =
                (void(__fastcall*)(void*))_addr(FN_CRESOURCE_RELEASE_REF);
            if (restored) addRef(restored);
            void* live = *field;
            *field = restored;
            if (live) {
                releaseRef(live);
            }
        };
        replaceResource((void**)(camera + 0x290),
                        slot.actionSideCamera.motionResource);
        replaceResource((void**)(camera + 0x3C0),
                        slot.actionSideCamera.requestResource);

        auto restoreCharacter = [&isTeamCharacter](void** field, void* restored) {
            if (!restored || restored == *field ||
                isTeamCharacter((uint64_t)restored)) {
                *field = restored;
            } else {
                *field = nullptr;
            }
        };
        restoreCharacter((void**)(camera + 0x298),
                         slot.actionSideCamera.motionParent);
        restoreCharacter((void**)(camera + 0x3D0),
                         slot.actionSideCamera.requestParent);
        // Restore +0x4D0 only as a roster reference; its precise role is unresolved.
        uint64_t target = 0;
        std::memcpy(&target, saved + 0x4D0, sizeof(target));
        *(uint64_t*)(camera + 0x4D0) = isTeamCharacter(target) ? target : 0;
    };

    auto traceRestoreState = [frame, team0, team1, &teamCharacters, teamCharacterCount](
                                 const char* phase) {
        return;
        uint64_t active0 = team0 ? *(uint64_t*)(team0 + 0x48) : 0;
        uint64_t active1 = team1 ? *(uint64_t*)(team1 + 0x48) : 0;
        HookLog("RESTORETRACE frame=%d phase=%s active0=0x%llX active1=0x%llX\n",
                frame, phase, active0, active1);
        for (size_t i = 0; i < teamCharacterCount; ++i) {
            uint64_t character = teamCharacters[i];
            HookLog("RESTORETRACE frame=%d phase=%s char[%u]=0x%llX "
                    "unit10=0x%llX unit18=0x%llX unit20=0x%llX unit38=0x%llX "
                    "14F8=0x%08X 151C=0x%08X 29D4=0x%08X 2AD0=0x%08X "
                    "2D94=0x%08X 64D4=0x%08X 6554=0x%08X charge=%g\n",
                    frame, phase, (unsigned)i, character,
                    *(uint64_t*)(character + 0x10),
                    *(uint64_t*)(character + 0x18),
                    *(uint64_t*)(character + 0x20),
                    *(uint64_t*)(character + 0x38),
                    *(uint32_t*)(character + 0x14F8),
                    *(uint32_t*)(character + 0x151C),
                    *(uint32_t*)(character + 0x29D4),
                    *(uint32_t*)(character + 0x2AD0),
                    *(uint32_t*)(character + 0x2D94),
                    *(uint32_t*)(character + 0x64D4),
                    *(uint32_t*)(character + 0x6554),
                    *(float*)(character + 0x68F0));
        }
    };
    traceRestoreState("before");
    bool ownerStillMatches[MAX_REGIONS];
    uint64_t liveOwnerPointers[MAX_REGIONS];
    uint32_t liveCommandLineCounts[MAX_REGIONS];
    for (int i = 0; i < slot.regionCount; ++i) {
        uint64_t ownerSlot = slot.regions[i].ownerSlot;
        if (ownerSlot == 0) {
            ownerStillMatches[i] = true;
            continue;
        }
        liveOwnerPointers[i] = *(uint64_t*)ownerSlot;
        ownerStillMatches[i] = liveOwnerPointers[i] == (uint64_t)slot.regions[i].ptr;
        if (slot.regions[i].commandLine)
            liveCommandLineCounts[i] = *(uint32_t*)(ownerSlot - 0x08);
    }
    const uint64_t setupEnd = Profile_Now();

    size_t offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        size_t copySize = slot.regions[i].size;
        if (offset + copySize > slot.size) {
            copySize = slot.size - offset;
        }
        bool objectStillMatches = slot.regions[i].expectedVtable == 0 ||
            *(uint64_t*)slot.regions[i].ptr == slot.regions[i].expectedVtable;
        if (*(uint64_t*)slot.regions[i].ptr == _addr(MT_DTI_VTABLE)) {
            HookLog("Rollback: skipping region with MtDTI vtable ptr=0x%llX "
                    "size=0x%zX owner=0x%llX expected=0x%llX\n",
                    (uint64_t)slot.regions[i].ptr, copySize,
                    slot.regions[i].ownerSlot,
                    slot.regions[i].expectedVtable);
            offset += slot.regions[i].size;
            continue;
        }
        if (slot.regions[i].ownerSlot == modelJointAllocationSlot &&
            slot.regions[i].size == SACTION_MODEL_JOINT_ALLOCATION_SIZE &&
            liveModelJointAllocation &&
            IsAccessibleAddress(liveModelJointAllocation +
                SACTION_MODEL_JOINT_ALLOCATION_SIZE - 1) &&
            copySize > SACTION_MODEL_JOINT_DATA_OFFSET) {
            // Restore the action-joint payload into the current allocation, excluding its header.
            size_t payloadSize = copySize - SACTION_MODEL_JOINT_DATA_OFFSET;
            std::memcpy((uint8_t*)liveModelJointAllocation +
                            SACTION_MODEL_JOINT_DATA_OFFSET,
                        slot.data + offset + SACTION_MODEL_JOINT_DATA_OFFSET,
                        payloadSize);
        } else if (!ownerStillMatches[i] || !objectStillMatches) {
            // Skip regions whose owner pointer or vtable no longer matches.
        } else if (slot.regions[i].ownerSlot != 0 &&
                   slot.regions[i].size == CCHR_EXTERNAL_ANIM_SIZE) {
            Rollback_RestoreExternalAnimBlock(slot.regions[i].ptr,
                                     slot.data + offset,
                                     copySize);
        } else if ((uint64_t)slot.regions[i].ptr == sCollision &&
                   slot.regions[i].size == SCOLLISION_SIZE) {
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
            // Keep the solver header, lock, and hit-node array header live.
            std::memcpy((void*)(sHitSolver + SHITSOLVER_DATA_OFF),
                        slot.data + offset + SHITSOLVER_DATA_OFF,
                        copySize - SHITSOLVER_DATA_OFF);
        } else if ((uint64_t)slot.regions[i].ptr == team0 ||
                   (uint64_t)slot.regions[i].ptr == team1) {
            // Restore team state and active selection, keeping the current node list.
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
            // Validate a changed active selection against the current team list.
            uint64_t restoredActive = *(uint64_t*)((uint8_t*)slot.regions[i].ptr + 0x48);
            if (restoredActive == 0 || restoredActive != liveActive) {
                // Fall back to the live selection if the saved character is absent.
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
        } else if ((uint64_t)slot.regions[i].ptr ==
                   (*(uint64_t*)_addr(PTR_SCAMERA) ?
                      *(uint64_t*)(*(uint64_t*)_addr(PTR_SCAMERA) + 0x58) : 0) &&
                   slot.regions[i].size >= 0x500 &&
                   GetObjectDti((uint64_t)slot.regions[i].ptr) ==
                       (uint64_t)_addr(DTI_UACTION_SIDE_CAMERA)) {
            // Restore camera state and handle its retained resources/references separately.
            restoreActionSideCamera((uint64_t)slot.regions[i].ptr,
                                    slot.data + offset, copySize);
        } else if (slot.regions[i].skipVtable &&
                   copySize >= CCHR_MODEL_TAIL_END &&
                   GetObjectDti((uint64_t)slot.regions[i].ptr) ==
                       (uint64_t)_addr(DTI_UCHRWEAPON)) {
            uint8_t* weapon = (uint8_t*)slot.regions[i].ptr;
            const uint8_t* savedWeapon = slot.data + offset;

            // Restore selected weapon model/attachment state, leaving lifecycle and owners live.
            static constexpr SnapshotRange weaponScalarRanges[] = {
                // Coordinate selectors, transform, and matrices.
                {0x48, 0xB8},
                // Model flags, joint count, and playback controls; exclude joint pointers.
                {0x510, 0x28}, {0x550, 0x10},
                // Selected model-tail state, excluding the owner at +0x1178.
                {0x1160, 0x18}, {0x1180, 0x30},
                // Aim translation/rotation/scale payloads, excluding their inline vtables.
                {0x1458, 0x48}, {0x14A8, 0x48}, {0x14F8, 0x48},
                // uChrWeapon flags, attachment selectors, and cached matrices.
                {0x1548, 0x98},
            };
            for (const SnapshotRange& range : weaponScalarRanges) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy(weapon + range.offset,
                                savedWeapon + range.offset,
                                range.size);
                }
            }
            for (int motion = 0; motion < CCHR_MOTION_COUNT; ++motion) {
                size_t motionBase = CCHR_MOTION_BASE +
                    motion * CCHR_MOTION_STRIDE;
                static constexpr SnapshotRange motionRestoreRanges[] = {
                    {0x08, 0x118}, {0x140, 0x20}, {0x168, 0x08},
                };
                for (const SnapshotRange& range : motionRestoreRanges) {
                    size_t fieldOffset = motionBase + range.offset;
                    if (fieldOffset + range.size <= copySize) {
                        std::memcpy(weapon + fieldOffset,
                                    savedWeapon + fieldOffset,
                                    range.size);
                    }
                }
            }
        } else if (slot.regions[i].skipVtable && copySize > sizeof(uint64_t)) {
            static constexpr uint32_t targetTableOffsets[] = {
                0x1408, 0x1410, 0x1418, 0x1420,
                0x1428, 0x1430, 0x1438,
            };
            uint64_t liveTargetTables[std::size(targetTableOffsets)] = {};
            uint32_t liveTargetCounts = 0;
            bool hasTargetTableState = copySize >= 0x1444;
            if (hasTargetTableState) {
                for (size_t target = 0; target < std::size(targetTableOffsets); ++target) {
                    liveTargetTables[target] = *(uint64_t*)(
                        (uint8_t*)slot.regions[i].ptr + targetTableOffsets[target]);
                }
                liveTargetCounts = *(uint32_t*)((uint8_t*)slot.regions[i].ptr + 0x1440);
            }

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
                if (fieldOffset == CUNIT_FLAGS_OFFSET &&
                    isTeamCharacter((uint64_t)slot.regions[i].ptr)) {
                    continue;
                }
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
            // Rewind roster lifecycle flags but keep helper flags live.
            // Pointer-like scheduler links are preserved by the pointer pass below.
            for (size_t field = 0;
                 field < sizeof(CUNIT_LIVE_STATE_OFFSETS) / sizeof(CUNIT_LIVE_STATE_OFFSETS[0]);
                 ++field) {
                uint32_t fieldOffset = CUNIT_LIVE_STATE_OFFSETS[field];
                if (fieldOffset == 0x18 || fieldOffset == 0x20 || fieldOffset == 0x38)
                    continue;
                if (fieldOffset == CUNIT_FLAGS_OFFSET &&
                    isTeamCharacter((uint64_t)slot.regions[i].ptr)) {
                    continue;
                }
                *(uint64_t*)((uint8_t*)slot.regions[i].ptr + fieldOffset) = liveUnitState[field];
            }
            // Keep draw/resource blocks live; restore motion slots separately below.
            std::memcpy((uint8_t*)slot.regions[i].ptr + CCHR_DRAW_STATE_START,
                        liveDrawState,
                        sizeof(liveDrawState));
            std::memcpy((uint8_t*)slot.regions[i].ptr + CCHR_MODEL_TAIL_START,
                        liveModelTail,
                        sizeof(liveModelTail));
            // Restore cached live words wherever either timeline looked pointer-like.
            for (size_t pointer = 0; pointer < livePointerCount; ++pointer) {
                *(uint64_t*)((uint8_t*)slot.regions[i].ptr + livePointers[pointer].offset) =
                    livePointers[pointer].value;
            }
            // Apply explicit restore ranges after the pointer heuristic.
            // Packed scalars can look like addresses; some legacy overrides also contain pointers.
            for (const SnapshotRange& range : CCHR_SCALAR_RESTORE_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }

            // Keep each packed helper-array count paired with its current allocation.
            if (hasTargetTableState) {
                uint32_t targetCounts = *(const uint32_t*)(slot.data + offset + 0x1440);
                for (size_t target = 0; target < std::size(targetTableOffsets); ++target) {
                    uint64_t savedTable = *(const uint64_t*)(
                        slot.data + offset + targetTableOffsets[target]);
                    if (liveTargetTables[target] == savedTable) continue;
                    uint32_t shift = (uint32_t)target * 4;
                    targetCounts = (targetCounts & ~(0xFu << shift)) |
                        (liveTargetCounts & (0xFu << shift));
                }
                *(uint32_t*)((uint8_t*)slot.regions[i].ptr + 0x1440) = targetCounts;
            }

            uint64_t concreteDti = GetObjectDti((uint64_t)slot.regions[i].ptr);
            ConcreteCharacterSnapshot_RestoreFields(
                slot.regions[i].ptr,
                slot.data + offset,
                copySize,
                concreteDti);

            uint8_t* character = (uint8_t*)slot.regions[i].ptr;
            const uint8_t* savedCharacter = slot.data + offset;

            if (concreteDti == (uint64_t)_addr(DTI_UIRONMAN) &&
                copySize >= 0x6928) {
                // Clear Iron Man's weak references when they no longer identify effects.
                for (uint32_t effectOffset : {0x68F0u, 0x6900u, 0x6920u}) {
                    void** effectSlot = (void**)(character + effectOffset);
                    if (*effectSlot && !IsEffectObject(*effectSlot)) {
                        HookLog("Rollback: cleared stale Iron Man effect slot "
                                "+0x%X object=0x%llX\n", effectOffset,
                                (uint64_t)*effectSlot);
                        *effectSlot = nullptr;
                    }
                }
            }

            restoreCnsHelper(character, savedCharacter, copySize, 0x3BF0);
            restoreCnsHelper(character, savedCharacter, copySize, 0x3D60);

            // Restore six inline ability payloads, keeping their vtables and +0x78 sound cookies live.
            constexpr uint32_t abilityPrimOffset = 0x1570;
            constexpr uint32_t abilityPrimStride = 0x80;
            if (abilityPrimOffset + 6 * abilityPrimStride <= copySize) {
                for (uint32_t prim = 0; prim < 6; ++prim) {
                    uint32_t primOffset = abilityPrimOffset + prim * abilityPrimStride;
                    std::memcpy(character + primOffset + sizeof(uint64_t),
                                savedCharacter + primOffset + sizeof(uint64_t),
                                0x70);
                }
            }

            // Rebind attack ownership and restore its scalar header; handle target tables below.
            if (CCHR_ATTACK_CTRL_OFFSET + 0x50 <= copySize) {
                *(uint64_t*)(character + CCHR_ATTACK_CTRL_OFFSET + 0x10) =
                    (uint64_t)character;
                std::memcpy(character + CCHR_ATTACK_CTRL_OFFSET + 0x28,
                            savedCharacter + CCHR_ATTACK_CTRL_OFFSET + 0x28,
                            0x28);
            }

            // Restore selected special/combo matcher state and callbacks; rebind character owners.
            // Leave command resources and allocated line storage outside these copies.
            for (uint32_t commandOffset : CCHR_COMMAND_OFFSETS) {
                if (commandOffset + CCHR_COMMAND_SIZE > copySize) continue;
                std::memcpy(character + commandOffset + 0x18,
                            savedCharacter + commandOffset + 0x18, 0x120);
                std::memcpy(character + commandOffset + 0x148,
                            savedCharacter + commandOffset + 0x148, 0x18);
                *(uint64_t*)(character + commandOffset + 0x140) =
                    (uint64_t)character;
                *(uint64_t*)(character + commandOffset + 0x160) =
                    (uint64_t)character;
                std::memcpy(character + commandOffset + 0x170,
                            savedCharacter + commandOffset + 0x170, 0x08);
                std::memcpy(character + commandOffset + 0x180,
                            savedCharacter + commandOffset + 0x180,
                            CCHR_COMMAND_SIZE - 0x180);
            }

            // Restore joint-hold selectors/matrices and rebind the owner.
            // Accept a saved target only if unchanged or present in the roster.
            if (CCHR_JOINT_HOLD_OFFSET + CCHR_JOINT_HOLD_SIZE <= copySize) {
                *(uint64_t*)(character + CCHR_JOINT_HOLD_OFFSET + 0x08) =
                    (uint64_t)character;
                std::memcpy(character + CCHR_JOINT_HOLD_OFFSET + 0x10,
                            savedCharacter + CCHR_JOINT_HOLD_OFFSET + 0x10, 0x08);
                uint64_t savedTarget = *(const uint64_t*)(
                    savedCharacter + CCHR_JOINT_HOLD_OFFSET + 0x18);
                uint64_t liveTarget = *(uint64_t*)(
                    character + CCHR_JOINT_HOLD_OFFSET + 0x18);
                *(uint64_t*)(character + CCHR_JOINT_HOLD_OFFSET + 0x18) =
                    (savedTarget == liveTarget || isTeamCharacter(savedTarget))
                        ? savedTarget : 0;
                std::memcpy(character + CCHR_JOINT_HOLD_OFFSET + 0x20,
                            savedCharacter + CCHR_JOINT_HOLD_OFFSET + 0x20,
                            CCHR_JOINT_HOLD_SIZE - 0x20);
            }

            // Rebind six message records to this character and restore their payloads.
            for (uint32_t message = 0; message < 6; ++message) {
                uint32_t messageOffset = CCHR_MESSAGE_OFFSET +
                    message * CCHR_MESSAGE_STRIDE;
                if (messageOffset + CCHR_MESSAGE_STRIDE > copySize) break;
                *(uint64_t*)(character + messageOffset + 0x08) =
                    (uint64_t)character;
                *(uint32_t*)(character + messageOffset + 0x10) = message + 1;
                std::memcpy(character + messageOffset + 0x18,
                            savedCharacter + messageOffset + 0x18, 0x0C);
            }

            // Restore pause mode/disable bytes, excluding the inline vtable.
            if (CCHR_PAUSE_OFFSET + 0x10 <= copySize) {
                std::memcpy(character + CCHR_PAUSE_OFFSET + 0x08,
                            savedCharacter + CCHR_PAUSE_OFFSET + 0x08, 0x08);
            }

            // Restore the embedded CPU's VM work region as one block after pointer filtering.
            // Its internal layout is not fully resolved; do not assume it is pointer-free.
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

            // Reapply motion playback, vectors, and event masks after pointer filtering.
            // Keep slot vtables and work buffers; handle borrowed event pointers below.
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
                // setMotionEx binds four borrowed sequence pointers from the motion resource.
                // Restore them only while that resource still matches the snapshot.
                if (isTeamCharacter((uint64_t)slot.regions[i].ptr) &&
                    motionBase + 0x140 <= copySize) {
                    const uint8_t* savedCharacter = slot.data + offset;
                    uint8_t* character = (uint8_t*)slot.regions[i].ptr;
                    uint16_t motionNo = *(const uint16_t*)(
                        savedCharacter + motionBase + 0x08);
                    size_t resourceOffset = 0x11B8 + ((motionNo >> 8) & 0xF) * 8;
                    if (motionNo != 0xFFFF && resourceOffset + 8 <= copySize) {
                        uint64_t savedResource = *(const uint64_t*)(
                            savedCharacter + resourceOffset);
                        if (savedResource && savedResource ==
                            *(const uint64_t*)(character + resourceOffset)) {
                            std::memcpy(character + motionBase + 0x120,
                                        savedCharacter + motionBase + 0x120, 0x20);
                        }
                    }
                }
            }

            // Restore five target-info payloads when their reference is null, unchanged, or roster-owned.
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

            // Restore capsule geometry and flags after pointer filtering; parents are remapped separately.
            static constexpr uint32_t capsuleBase = 0x4210;
            static constexpr uint32_t capsuleStride = 0xB0;
            for (uint32_t primitive = 0; primitive < 16; ++primitive) {
                uint32_t entry = capsuleBase + primitive * capsuleStride;
                static constexpr SnapshotRange fields[] = {
                    {0x08, 0x01}, // geometry type
                    {0x10, 0x04}, // primitive kind
                    {0x20, 0x24}, // capsule endpoints and radius
                    {0x50, 0x04}, // hit-primitive flags
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
                        {0x08, 0x01}, // geometry type
                        {0x10, 0x04}, // primitive kind
                        {0x20, 0x10}, // sphere center and radius
                        {0x30, 0x04}, // hit-primitive flags
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

            // Each of three tables has 32 target-hit records: pointer +0x08, count/time +0x10/+0x14.
            // Restore compatible targets with their hit history, leaving entry vtables live.
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

            // Restore embedded input through +0x56F verbatim, bypassing pointer false positives.
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
    RestoreCharacterCollisionParentReferences(slot);
    const uint64_t regionsEnd = Profile_Now();

    /*std::memcpy(liveGlobalRng, (void*)_addr(0x140D765D8), sizeof(liveGlobalRng));
    HookLog("RNGRESTORE frame=%d stage=regions live=%08X,%08X,%08X,%08X\n",
            frame, liveGlobalRng[0], liveGlobalRng[1], liveGlobalRng[2], liveGlobalRng[3]);*/

    Rollback_RestoreCollisionNodeClones(slot);
    const uint64_t collisionClonesEnd = Profile_Now();

    // Restore current owner pointers (and line counts) for allocations skipped as replaced.
    for (int i = 0; i < slot.regionCount; ++i) {
        if (slot.regions[i].ownerSlot != 0 && !ownerStillMatches[i]) {
            *(uint64_t*)slot.regions[i].ownerSlot = liveOwnerPointers[i];
            if (slot.regions[i].commandLine) {
                *(uint32_t*)(slot.regions[i].ownerSlot - 0x08) =
                    liveCommandLineCounts[i];
            }
        }
    }
    const uint64_t ownerPointersEnd = Profile_Now();

    // Clear transient hit work via native reset, which may release its array storage.
    // Reset the additional solver counter/bucket fields used by this restore policy.
    if (sHitSolver) {
        ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(sHitSolver);
        *(uint32_t*)(sHitSolver + 0x30064) = 0; // mHitSolverCnt
        *(uint32_t*)(sHitSolver + 0x30070) = 0; // bucket A header
        *(uint32_t*)(sHitSolver + 0x34078) = 0; // bucket B header
    }
    const uint64_t hitSolverEnd = Profile_Now();

    // Clear invalid character/material references before native code consumes them.
    Rollback_ClearStalePointers();
    const uint64_t stalePointersEnd = Profile_Now();

    // Clear game-effect slot caches before factories create replacements.
    Rollback_ClearGameEffectSlots();
    const uint64_t effectSlotsEnd = Profile_Now();
    RestoreShotAndEffectRecipes(slot);
    const uint64_t shotsEnd = Profile_Now();

    /*std::memcpy(liveGlobalRng, (void*)_addr(0x140D765D8), sizeof(liveGlobalRng));
    HookLog("RNGRESTORE frame=%d stage=recipes live=%08X,%08X,%08X,%08X\n",
            frame, liveGlobalRng[0], liveGlobalRng[1], liveGlobalRng[2], liveGlobalRng[3]);*/

    using SetChildMaterialModeFn = void(__fastcall*)(void*, int, uint8_t);
    auto setChildMaterialMode =
        (SetChildMaterialModeFn)_addr(FN_MODEL_SET_CHILD_MATERIAL_MODE);
    for (const RollbackModelRenderState& saved : slot.modelRenderStates) {
        if (!saved.model ||
            *(uint64_t*)saved.model != saved.expectedVtable) {
            continue;
        }

        uint32_t* objectFlags = (uint32_t*)((uint8_t*)saved.model + 0x10);
        uint32_t* drawLayerMask = (uint32_t*)((uint8_t*)saved.model + 0x11C);
        bool liveCutin = (*drawLayerMask & CUTIN_DRAW_LAYER_BITS) != 0;
        bool savedCutin = (saved.drawLayerMask & CUTIN_DRAW_LAYER_BITS) != 0;
        if (saved.character) {
            *objectFlags = (*objectFlags & ~MTOBJECT_VISIBLE_FLAG) |
                (saved.objectFlags & MTOBJECT_VISIBLE_FLAG);
        }
        *drawLayerMask = saved.drawLayerMask;
        *(float*)((uint8_t*)saved.model + 0x180) = saved.transparency;
        if (liveCutin != savedCutin) {
            // Reapply child material mode when crossing cut-in/normal render layers.
            setChildMaterialMode(saved.model, -1, savedCutin ? 2 : 3);
        }
    }

    // Reapply selected character state after recipe/model repair.
    offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        size_t copySize = slot.regions[i].size;
        if (offset + copySize > slot.size)
            copySize = slot.size - offset;
        if (slot.regions[i].skipVtable &&
            isTeamCharacter((uint64_t)slot.regions[i].ptr)) {
            for (const SnapshotRange& range : CCHR_POST_RESTORE_TAIL_RANGES) {
                if ((size_t)range.offset + range.size <= copySize) {
                    std::memcpy((uint8_t*)slot.regions[i].ptr + range.offset,
                                slot.data + offset + range.offset,
                                range.size);
                }
            }

            uint8_t* character = (uint8_t*)slot.regions[i].ptr;
            const uint8_t* savedCharacter = slot.data + offset;
            restoreCnsHelper(character, savedCharacter, copySize, 0x3BF0);
            restoreCnsHelper(character, savedCharacter, copySize, 0x3D60);

            // Reapply capsule geometry/flags late; parent references are restored below.
            constexpr uint32_t capsuleBase = 0x4210;
            constexpr uint32_t capsuleStride = 0xB0;
            for (uint32_t primitive = 0; primitive < 16; ++primitive) {
                uint32_t entry = capsuleBase + primitive * capsuleStride;
                static constexpr SnapshotRange fields[] = {
                    {0x08, 0x01},
                    {0x10, 0x04},
                    {0x20, 0x24},
                    {0x50, 0x04},
                    {0x68, 0x0C},
                    {0x80, 0x20},
                };
                for (const SnapshotRange& field : fields) {
                    if ((size_t)entry + field.offset + field.size <= copySize) {
                        std::memcpy(character + entry + field.offset,
                                    savedCharacter + entry + field.offset,
                                    field.size);
                    }
                }
            }

            constexpr uint32_t sphereBases[] = {0x4E20, 0x5530, 0x5C40};
            constexpr uint32_t sphereStride = 0x70;
            for (uint32_t base : sphereBases) {
                for (uint32_t primitive = 0; primitive < 16; ++primitive) {
                    uint32_t entry = base + primitive * sphereStride;
                    if ((size_t)entry + 0x40 > copySize) continue;
                }
            }

            // Keep saved character relationships only when unchanged or roster-owned.
            constexpr uint32_t RELATIONSHIP_OFFSETS[] = {
                0x6718, 0x6720, 0x6728, 0x6730,
            };
            for (uint32_t relationshipOffset : RELATIONSHIP_OFFSETS) {
                if ((size_t)relationshipOffset + sizeof(uint64_t) > copySize)
                    continue;
                uint64_t savedRelationship = *(const uint64_t*)(
                    slot.data + offset + relationshipOffset);
                uint64_t* liveRelationship = (uint64_t*)(
                    (uint8_t*)slot.regions[i].ptr + relationshipOffset);
                *liveRelationship =
                    (savedRelationship == *liveRelationship ||
                     isTeamCharacter(savedRelationship))
                        ? savedRelationship : 0;
            }

            Rollback_RemapCharacterShotLinks(
                (void*)character, slot.regions[i].expectedVtable,
                savedCharacter, copySize, slot.shotRecipes);
        }
        offset += slot.regions[i].size;
    }
    RestoreCharacterCollisionParentReferences(slot);
    const uint64_t characterReferencesLateStart = Profile_Now();
    Rollback_RestoreCharacterShotReferences(slot);
    const uint64_t characterShotReferencesLateEnd = Profile_Now();
    Effect_RestoreCharacterReferences(slot);
    if (g_activeRollbackProfile) {
        g_activeRollbackProfile->restoreCharacterShotReferencesLate +=
            characterShotReferencesLateEnd - characterReferencesLateStart;
        g_activeRollbackProfile->restoreCharacterEffectReferencesLate +=
            Profile_Now() - characterShotReferencesLateEnd;
    }

    // Reapply camera state after recipe/model repair, including retained references.
    uint64_t liveCamera = *(uint64_t*)_addr(PTR_SCAMERA);
    uint64_t liveCameraUnit = liveCamera
        ? *(uint64_t*)(liveCamera + 0x58) : 0;
    if (liveCameraUnit) {
        offset = 0;
        for (int i = 0; i < slot.regionCount; ++i) {
            size_t copySize = slot.regions[i].size;
            if (offset + copySize > slot.size)
                copySize = slot.size - offset;
            if ((uint64_t)slot.regions[i].ptr == liveCameraUnit) {
                restoreActionSideCamera(liveCameraUnit, slot.data + offset,
                                        copySize);
                break;
            }
            offset += slot.regions[i].size;
        }
    }

    // Restore the current uCharacterPost's center and fighter cache used for position correction.
    restoreCharacterPostState();

    traceRestoreState("after");

    /*std::memcpy(liveGlobalRng, (void*)_addr(0x140D765D8), sizeof(liveGlobalRng));
    HookLog("RNGRESTORE frame=%d stage=complete live=%08X,%08X,%08X,%08X\n",
            frame, liveGlobalRng[0], liveGlobalRng[1], liveGlobalRng[2], liveGlobalRng[3]);*/

    if (g_activeRollbackProfile) {
        const uint64_t restoreEnd = Profile_Now();
        g_activeRollbackProfile->restoreTotal += restoreEnd - profileStart;
        g_activeRollbackProfile->restoreSetup += setupEnd - profileStart;
        g_activeRollbackProfile->restoreRegions += regionsEnd - setupEnd;
        g_activeRollbackProfile->restoreCollisionClones += collisionClonesEnd - regionsEnd;
        g_activeRollbackProfile->restoreOwnerPointers += ownerPointersEnd - collisionClonesEnd;
        g_activeRollbackProfile->restoreHitSolver += hitSolverEnd - ownerPointersEnd;
        g_activeRollbackProfile->restoreStalePointers += stalePointersEnd - hitSolverEnd;
        g_activeRollbackProfile->restoreEffectSlots += effectSlotsEnd - stalePointersEnd;
        g_activeRollbackProfile->restoreShots += shotsEnd - effectSlotsEnd;
        g_activeRollbackProfile->restoreUi += restoreEnd - shotsEnd;
    }
    LiveBenchmark_Record(LiveBenchmarkMetric::Restore, benchmarkStart);
}
