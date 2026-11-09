#include "../pch.h"
#include "lifetime_trace.h"

#include "rollback_snapshot.h"
#include "rollback_capture_set.h"

#include "controller.h"
#include "rollback_snapdump.h"
#include "mk10utils.h"
#include "rollback_memory.h"
#include "rollback_profile.h"
#include "live_benchmark.h"
#include "lag_diagnostics.h"
#include "rollback_restore.h"
#include "rollback_resim.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern void HookLog(const char* fmt, ...);
extern "C" uint32_t Controller_GetCurrentFrameInput();

static_assert(sizeof(RollbackRegion) <= 64);

static constexpr uintptr_t PTR_SREPLAY = 0x140d4d9d8;
static constexpr uintptr_t PTR_SCHARACTER = 0x140d44a70;
static constexpr uintptr_t PTR_SHITSOLVER = 0x140d44060;
static constexpr uintptr_t PTR_SACTION = 0x140d47e68;
static constexpr uintptr_t PTR_SMAIN = 0x140e177e8;
static constexpr uintptr_t PTR_SMVC3MGR = 0x140d510a0;
static constexpr uintptr_t PTR_SUNIT = 0x140e17698;
static constexpr uintptr_t PTR_SCAMERA = 0x140e17930;
static constexpr uintptr_t PTR_SCOLLISION = 0x140e18250;
static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
static constexpr uint32_t SACTION_MODEL_JOINT_WRAPPER_OFFSET = 0x120;
static constexpr uint32_t SACTION_MODEL_JOINT_ALLOCATION_OFFSET = 0x70;
static constexpr uint32_t SACTION_MODEL_JOINT_DATA_OFFSET = 0x10;
static constexpr size_t SACTION_MODEL_JOINT_DATA_SIZE = 0x70;
static constexpr size_t SACTION_MODEL_JOINT_ALLOCATION_SIZE = SACTION_MODEL_JOINT_DATA_OFFSET + SACTION_MODEL_JOINT_DATA_SIZE;
static constexpr uintptr_t TEAM0_OFFSET = 0x40;
static constexpr uintptr_t TEAM1_OFFSET = 0x300;
static constexpr size_t TEAM_SIZE = 0x2C0;
static constexpr uintptr_t SCHARACTER_CHANGE_ORDER_OFFSET = 0x5C0;
static constexpr size_t SCHARACTER_CHANGE_ORDER_SIZE = 0x40;
static constexpr size_t SCOLLISION_SIZE = 0x239E0;
static constexpr size_t SCOLLISION_BVH_COUNT = 0x1F;
static constexpr size_t SCOLLISION_NODE_SIZE = 0x40;
static constexpr uintptr_t NODE_CHR_OFFSET = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET = 0x10;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr size_t MAX_CHAR_SIZE = 0x40000;
// Shared character base size; the nominal fallback is not valid for every helper subclass.
static constexpr size_t CCHR_MIN_SIZE = 0x68F0;
static constexpr size_t CCHR_NOMINAL_SIZE = 0x6A60;
static constexpr size_t CCHR_MAX_SIZE = 30000;
static constexpr uint32_t CCHR_NUM_JOINTS_OFFSET = 0x530;
static constexpr uint32_t CCHR_JOINT_STATE_PTR_OFFSET = 0x538;
static constexpr uint32_t CCHR_JOINT_INDEX_PTR_OFFSET = 0x548;
static constexpr uint32_t CCHR_MOTION_COUNT_OFFSET = 0x554;
static constexpr uint32_t CCHR_MOTION_BASE = 0x560;
static constexpr uint32_t CCHR_MOTION_STRIDE = 0x180;
static constexpr uint32_t CCHR_BLEND_PTR_OFFSET = 0x160;
static constexpr size_t CCHR_BLEND_BYTES_PER_JOINT = 104;
static constexpr uint32_t CCHR_MOTION_CHANNELS_PTR_OFFSET = 0x6D8;
static constexpr uint32_t CCHR_BLENDED_JOINT_PTR_OFFSET = 0x11B0;
static constexpr uint32_t CCHR_MOTION_CHANNEL_COUNT_OFFSET = 0x1238;
static constexpr size_t CCHR_MOTION_CHANNEL_STRIDE = 0x0C;
static constexpr size_t CCHR_JOINT_STATE_STRIDE = 0xC0;
static constexpr size_t CCHR_BLENDED_JOINT_STRIDE = 0x40;
static constexpr uint32_t CCHR_EXTERNAL_ANIM_OFFSETS[] = {0x4200, 0x4E10, 0x5520, 0x5C30};
static constexpr size_t CCHR_EXTERNAL_ANIM_SIZE = 0xE0;
static constexpr int CCHR_MOTION_COUNT = 8;
static constexpr uint32_t CCHR_WEAPON_COUNT_OFFSET = 0x3F38;
static constexpr uint32_t CCHR_WEAPON_ARRAY_OFFSET = 0x3F40;
static constexpr uint32_t SUNIT_CATEGORY_COUNT_OFFSET = 0xC38;
static constexpr uint32_t SUNIT_CATEGORY_LISTS_OFFSET = 0x50;
static constexpr uint32_t SUNIT_CATEGORY_LIST_STRIDE = 0x30;
static constexpr uint32_t SUNIT_CHARACTER_CATEGORY = 8;
static constexpr uint32_t SUNIT_CHARACTER_POST_CATEGORY = 18;
static constexpr uint32_t UCHARACTER_POST_SCALAR_STATE_OFFSET = 0x40;
static constexpr size_t UCHARACTER_POST_SCALAR_STATE_SIZE = 0x10;
static constexpr uint32_t UCHARACTER_POST_TEAM0_CACHE_OFFSET = 0x50;
static constexpr uint32_t UCHARACTER_POST_TEAM1_CACHE_OFFSET = 0x58;
static constexpr size_t UCHARACTER_POST_SIZE = 0x60;
static constexpr uint32_t CUNIT_RNO_OFFSET = 0x08;
static constexpr uint32_t CUNIT_UNIT_PARAM_OFFSET = 0x0C;
static constexpr uint32_t CUNIT_FLAGS_OFFSET = 0x10;
static constexpr uint32_t CUNIT_MOVE_ENABLED_FLAG = 0x400;
static constexpr uint32_t CUNIT_GROUP_OFFSET = 0x30;
static constexpr size_t SMAIN_SIZE = 0x40080;
static constexpr int MAX_REGIONS = ROLLBACK_MAX_REGIONS;
static constexpr int DYNAMIC_SAFETY = 128;
static constexpr size_t SNAPSHOT_CAPACITY_GRANULARITY = 256 * 1024;
static constexpr size_t SHITSOLVER_SIZE = 0x3808C;
static constexpr uintptr_t FN_MTOBJECT_ADD_REF = 0x14050D100;
static constexpr uintptr_t FN_CRESOURCE_RELEASE_REF = 0x14050D5A0;
static constexpr uint32_t UACTION_CAMERA_MOTION_RESOURCE_OFFSET = 0x290;
static constexpr uint32_t UACTION_CAMERA_MOTION_PARENT_OFFSET = 0x298;
static constexpr uint32_t UACTION_CAMERA_REQUEST_RESOURCE_OFFSET = 0x3C0;
static constexpr uint32_t UACTION_CAMERA_REQUEST_PARENT_OFFSET = 0x3D0;
static constexpr uintptr_t DTI_UACTION_SIDE_CAMERA = 0x140D44268;
static constexpr uintptr_t DTI_UCHARACTER_POST = 0x140D44D60;
static constexpr uintptr_t DTI_UCHRWEAPON = 0x140D44968;

using Region = RollbackRegion;
using SnapshotSlot = RollbackSnapshotSlot;
RollbackSnapshotSlot g_rollbackSnapshotSlots[ROLLBACK_WINDOW];
#define g_slots g_rollbackSnapshotSlots

template <size_t SlotSize, size_t LocalInputOffset, size_t RemoteInputOffset>
void RollbackSnapshotLayoutGuard<SlotSize, LocalInputOffset, RemoteInputOffset>::Require()
{
}

template struct RollbackSnapshotLayoutGuard<
    sizeof(RollbackSnapshotSlot),
    offsetof(RollbackSnapshotSlot, localInput),
    offsetof(RollbackSnapshotSlot, remoteInput)>;

static void ReleaseCharacterMotionResources(RollbackSnapshotSlot& slot)
{
    auto releaseRef = (void(__fastcall*)(void*))_addr(FN_CRESOURCE_RELEASE_REF);
    for (const auto& binding : slot.characterMotionResources)
        if (binding.resource) releaseRef(binding.resource);
    slot.characterMotionResources.clear();
}

static void CaptureCharacterMotionResources(RollbackSnapshotSlot& slot)
{
    ReleaseCharacterMotionResources(slot);
    auto addRef = (void(__fastcall*)(void*))_addr(FN_MTOBJECT_ADD_REF);
    size_t offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        const auto& region = slot.regions[i];
        if (region.skipVtable && region.size >= CCHR_MIN_SIZE) {
            const uint64_t dti = GetObjectDti((uint64_t)region.ptr);
            if (dti && ((bool(__fastcall*)(void*, void*))_addr(0x1400039D0))(
                    (void*)dti, (void*)_addr(0x140D44A30))) {
                void* resource = nullptr;
                std::memcpy(&resource, slot.data + offset + 0x11C8, sizeof(resource));
                if (resource) addRef(resource);
                slot.characterMotionResources.push_back({i, resource});
            }
        }
        offset += region.size;
    }
}

static void ReleaseActionSideCameraState(RollbackSnapshotSlot& slot)
{
    auto releaseRef = (void(__fastcall*)(void*))_addr(FN_CRESOURCE_RELEASE_REF);
    if (slot.actionSideCamera.motionResource)
        releaseRef(slot.actionSideCamera.motionResource);
    if (slot.actionSideCamera.requestResource)
        releaseRef(slot.actionSideCamera.requestResource);
    slot.actionSideCamera = {};
}

static void CaptureActionSideCameraState(RollbackSnapshotSlot& slot)
{
    ReleaseActionSideCameraState(slot);

    uint64_t sCamera = *(uint64_t*)_addr(PTR_SCAMERA);
    uint64_t camera = sCamera ? *(uint64_t*)(sCamera + 0x58) : 0;
    if (!camera ||
        GetObjectDti(camera) != (uint64_t)_addr(DTI_UACTION_SIDE_CAMERA)) {
        return;
    }

    slot.actionSideCamera.motionResource =
        *(void**)(camera + UACTION_CAMERA_MOTION_RESOURCE_OFFSET);
    slot.actionSideCamera.motionParent =
        *(void**)(camera + UACTION_CAMERA_MOTION_PARENT_OFFSET);
    slot.actionSideCamera.requestResource =
        *(void**)(camera + UACTION_CAMERA_REQUEST_RESOURCE_OFFSET);
    slot.actionSideCamera.requestParent =
        *(void**)(camera + UACTION_CAMERA_REQUEST_PARENT_OFFSET);

    auto addRef = (void(__fastcall*)(void*))_addr(FN_MTOBJECT_ADD_REF);
    if (slot.actionSideCamera.motionResource)
        addRef(slot.actionSideCamera.motionResource);
    if (slot.actionSideCamera.requestResource)
        addRef(slot.actionSideCamera.requestResource);
}

static void CaptureCharacterPostState(RollbackSnapshotSlot& slot)
{
    slot.characterPost = {};

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

    // Capture dispatch values and the Move bit, not shared lifecycle flags or list links.
    slot.characterPost.rno = *(int32_t*)(characterPost + CUNIT_RNO_OFFSET);
    slot.characterPost.unitParam =
        *(int32_t*)(characterPost + CUNIT_UNIT_PARAM_OFFSET);
    slot.characterPost.moveEnabled =
        (*(uint32_t*)(characterPost + CUNIT_FLAGS_OFFSET) &
         CUNIT_MOVE_ENABLED_FLAG) != 0;
    slot.characterPost.unitGroup =
        *(uint64_t*)(characterPost + CUNIT_GROUP_OFFSET);

    // Fighter-center vector; cached character references at +0x50/+0x58 are encoded below.
    static_assert(sizeof(slot.characterPost.scalarState) ==
                  UCHARACTER_POST_SCALAR_STATE_SIZE);
    std::memcpy(slot.characterPost.scalarState,
                (const void*)(characterPost + UCHARACTER_POST_SCALAR_STATE_OFFSET),
                UCHARACTER_POST_SCALAR_STATE_SIZE);

    uint64_t cachedCharacters[2] = {
        *(uint64_t*)(characterPost + UCHARACTER_POST_TEAM0_CACHE_OFFSET),
        *(uint64_t*)(characterPost + UCHARACTER_POST_TEAM1_CACHE_OFFSET),
    };
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    for (int team = 0; sCharacter && team < 2; ++team) {
        uint64_t node = sCharacter + (team == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
            TEAM_FIRST_NODE_OFFSET;
        for (uint8_t member = 0; node && member < 8; ++member) {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character == cachedCharacters[team])
                slot.characterPost.cachedCharacterMembers[team] = member;
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }

    // Reject non-null cached characters that could not be mapped to team-member indices.
    for (int team = 0; team < 2; ++team) {
        if (cachedCharacters[team] != 0 &&
            slot.characterPost.cachedCharacterMembers[team] == UINT8_MAX) {
            return;
        }
    }
    slot.characterPost.valid = true;
}

static void CaptureCharacterCollisionParentReferences(RollbackSnapshotSlot& slot)
{
    slot.characterCollisionParentReferences.clear();
    uint32_t noneCount = 0;
    uint32_t characterCount = 0;
    uint32_t weaponCountTotal = 0;
    uint32_t unclassifiedCount = 0;

    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return;

    RollbackCaptureSet<uint64_t, 2 * 16> capturedCharacters;
    auto captureCharacter = [&](uint64_t character) {
        if (!character || !capturedCharacters.insert(character).second) return;

        const uint64_t expectedVtable = *(uint64_t*)character;
        uint32_t weaponCount = *(uint32_t*)(character + CCHR_WEAPON_COUNT_OFFSET);
        uint64_t weaponArray = *(uint64_t*)(character + CCHR_WEAPON_ARRAY_OFFSET);
        if (!weaponArray || weaponCount > 256) weaponCount = 0;

        auto captureParent = [&](uint32_t offset) {
            uint64_t parent = *(uint64_t*)(character + offset);
            RollbackCharacterCollisionParentReference reference{};
            reference.character = (void*)character;
            reference.expectedCharacterVtable = expectedVtable;
            reference.offset = offset;

            if (!parent) {
                reference.kind = RollbackCharacterCollisionParentKind::None;
                ++noneCount;
            } else if (parent == character) {
                reference.kind = RollbackCharacterCollisionParentKind::Character;
                ++characterCount;
            } else {
                uint32_t weaponIndex = 0;
                while (weaponIndex < weaponCount &&
                       *(uint64_t*)(weaponArray +
                           (size_t)weaponIndex * sizeof(uint64_t)) != parent) {
                    ++weaponIndex;
                }
                if (weaponIndex == weaponCount) {
                    ++unclassifiedCount;
                    return;
                }
                reference.kind = RollbackCharacterCollisionParentKind::Weapon;
                reference.weaponIndex = (uint16_t)weaponIndex;
                ++weaponCountTotal;
            }
            slot.characterCollisionParentReferences.push_back(reference);
        };

        constexpr uint32_t capsuleBase = 0x4210;
        constexpr uint32_t capsuleStride = 0xB0;
        for (uint32_t primitive = 0; primitive < 16; ++primitive) {
            uint32_t entry = capsuleBase + primitive * capsuleStride;
            captureParent(entry + 0x58);
            captureParent(entry + 0x60);
        }

        constexpr uint32_t sphereBases[] = {0x4E20, 0x5530, 0x5C40};
        constexpr uint32_t sphereStride = 0x70;
        for (uint32_t base : sphereBases) {
            for (uint32_t primitive = 0; primitive < 16; ++primitive)
                captureParent(base + primitive * sphereStride + 0x38);
        }
    };

    for (int team = 0; team < 2; ++team) {
        uint64_t node = sCharacter + (team == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
            TEAM_FIRST_NODE_OFFSET;
        for (int safety = 16; node && safety-- > 0;) {
            captureCharacter(*(uint64_t*)(node + NODE_CHR_OFFSET));
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }

    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        if (g_activeRollbackProfile) {
            g_activeRollbackProfile->snapshotCharacterCollisionParentReferenceCount +=
                (uint32_t)slot.characterCollisionParentReferences.size();
            g_activeRollbackProfile->snapshotCharacterCollisionParentNoneCount +=
                noneCount;
            g_activeRollbackProfile->snapshotCharacterCollisionParentCharacterCount +=
                characterCount;
            g_activeRollbackProfile->snapshotCharacterCollisionParentWeaponCount +=
                weaponCountTotal;
            g_activeRollbackProfile->snapshotCharacterCollisionParentUnclassifiedCount +=
                unclassifiedCount;
        }
    }
}

struct RegionDependency {
    uintptr_t address = 0;
    uint64_t value = 0;
    uint8_t size = 0;
};

struct RegionDependencies {
    static constexpr size_t MAX_VALUES = 1024;
    RegionDependency values[MAX_VALUES] = {};
    size_t count = 0;
    bool overflow = false;

    template <typename T>
    T read(uintptr_t address)
    {
        static_assert(sizeof(T) <= sizeof(uint64_t));
        T value = *(T*)address;
        if (count < MAX_VALUES) {
            RegionDependency& dependency = values[count++];
            dependency.address = address;
            dependency.size = sizeof(T);
            std::memcpy(&dependency.value, &value, sizeof(T));
        } else {
            overflow = true;
        }
        return value;
    }
};

struct RegionCache {
    Region regions[MAX_REGIONS] = {};
    RegionDependencies dependencies;
    int count = 0;
    bool valid = false;
};

static RegionCache g_regionCache;

static bool RegionDependenciesValid(const RegionDependencies& dependencies)
{
    if (dependencies.overflow) return false;
    for (size_t i = 0; i < dependencies.count; ++i) {
        const RegionDependency& dependency = dependencies.values[i];
        uint64_t value = 0;
        switch (dependency.size) {
        case sizeof(uint64_t):
            std::memcpy(&value, (void*)dependency.address, sizeof(uint64_t));
            break;
        case sizeof(uint32_t): {
            uint32_t value32;
            std::memcpy(&value32, (void*)dependency.address, sizeof(value32));
            value = value32;
            break;
        }
        default:
            std::memcpy(&value, (void*)dependency.address, dependency.size);
            break;
        }
        if (value != dependency.value) return false;
    }
    return true;
}

// Bounded list capture using DTI sizes; the traversal limit does not validate pointers.
static void TraverseAndAddDynamicList(uint64_t listHead, size_t nextOffset,
                                      int& r, Region* regions, int maxRegions)
{
    uint64_t current = listHead;
    int safety = DYNAMIC_SAFETY;

    while (current && r < maxRegions && safety-- > 0) {
        // Resolve size through the object's DTI getter.
        uint64_t dtiPtr = GetObjectDti(current);
        uint32_t objSize = DecodeDtiSize(dtiPtr);
        if (objSize == 0 || objSize > MAX_CHAR_SIZE) {
            objSize = 0x400; // Legacy fallback; actual allocation size is unverified.
        }

        regions[r].ptr  = (void*)current;
        regions[r].size = objSize;
        regions[r].ownerSlot = 0;
        regions[r].expectedVtable = 0;
        regions[r].skipVtable = false;
        regions[r].dependentRegion = -1;
#if ROLLBACK_SNAPDUMP_ENABLED
        regions[r].snapDumpName = "dynamic_list_object";
#endif
        ++r;

        current = *(uint64_t*)(current + nextOffset);
    }
}

// Build configured capture regions and record the fields that invalidate their cache.
[[gnu::noinline, gnu::cold]] static int BuildRegionsUncached(
    Region* regions, int maxRegions, RegionDependencies& dependencies)
{
    if (!regions || maxRegions < 1) return 0;

    int r = 0;
    auto read = [&]<typename T>(uintptr_t address) {
        return dependencies.read<T>(address);
    };
    auto add = [&](void* ptr, size_t sz, uint64_t ownerSlot = 0,
                   uint64_t expectedVtable = 0, bool skipVtable = false
#if ROLLBACK_SNAPDUMP_ENABLED
                   , const char* snapDumpName = "unnamed"
#endif
                   ) {
        if (r < maxRegions && ptr && sz > 0) {
            regions[r].ptr  = ptr;
            regions[r].size = sz;
            regions[r].ownerSlot = ownerSlot;
            regions[r].expectedVtable = expectedVtable;
            regions[r].skipVtable = skipVtable;
            regions[r].commandLine = false;
            regions[r].dependentRegion = -1;
#if ROLLBACK_SNAPDUMP_ENABLED
            regions[r].snapDumpName = snapDumpName;
#endif
            ++r;
        }
    };

    // Capture only the replay frame counter, not its internal buffers.
    uint64_t sReplay = read.operator()<uint64_t>(_addr(PTR_SREPLAY));
    if (sReplay) {
        add((void*)(sReplay + 0x54), 4, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sReplay.frameCounter"
#endif
        );
    }

    // Global gameplay RNG: four 32-bit XorShift state words.
    add((void*)_addr(0x140d765d8), 16, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
        , "globalRng"
#endif
    );

    // Shot/effect rewind is unavailable. Keep their manager ownership and IDs live.

    // Hit solver snapshot is not needed, rebuilds hit solver data after each frame ...
    {
    //    uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
    //    add((void*)sHitSolver, SHITSOLVER_SIZE);
    }

    // Legacy direct-sAction capture is disabled; use the manager-linked region below.
    /*{
        uint64_t sAction = read.operator()<uint64_t>(_addr(PTR_SACTION));
        add((void*)sAction, 0x250, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sAction.full"
#endif
        );
    }*/

    // UI objects and their internal allocations are not captured as raw regions.
    // {
    //     uint64_t sGameUI = *(uint64_t*)_addr(PTR_SGAMEUI);
    //     add((void*)sGameUI, 0x6000);
    // }

    // Main timing/pacing capture is disabled.
    {
    //    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
     //   add((void*)sMain, SMAIN_SIZE);
    }

    // Collision-manager and external-buffer discovery is disabled.
    //{
    /*    uint64_t sCollision = *(uint64_t*)_addr(PTR_SCOLLISION);
        collisionDiag.sCollision = sCollision;
        add((void*)sCollision, SCOLLISION_SIZE);
        if (sCollision) {
            // Subsystem offsets are relative to the complete singleton.
            uint64_t sbc = sCollision + 0x43B8;
            uint64_t moveMatrix = sCollision + 0x7368;

            addSbcObjectBuffers(sbc);
            addMoveMatrixBuffers(moveMatrix);

            // Discover external storage for five inline BVH banks.
            for (uint32_t bank : {0x2B8u, 0xFB8u, 0x1CB8u, 0x29B8u, 0x36B8u}) {
                for (uint32_t index = 0; index < SCOLLISION_BVH_COUNT; ++index)
                    addDynamicBvhBuffers(sCollision + bank + index * 0x68, bank, index);
            }

            // Discover the two SbcObject BVH banks.
            for (uint32_t bank : {0xAA8u, 0x17A8u}) {
                for (uint32_t index = 0; index < SCOLLISION_BVH_COUNT; ++index)
                    addDynamicBvhBuffers(sbc + bank + index * 0x68, bank, index);
            }

            // Legacy wrapper discovery; active wrapper capture is in the character pass.
            uint64_t character = *(uint64_t*)_addr(PTR_SCHARACTER);
            if (character) {
                for (uint32_t teamOffset : {TEAM0_OFFSET, TEAM1_OFFSET}) {
                    uint64_t node = character + teamOffset + TEAM_FIRST_NODE_OFFSET;
                    for (uint32_t n = 0; node && n < 8; ++n) {
                        uint64_t chr = *(uint64_t*)(node + NODE_CHR_OFFSET);
                        if (chr) {
                            for (uint32_t offset : CCHR_EXTERNAL_ANIM_OFFSETS) {
                                uint64_t wrapper = *(uint64_t*)(chr + offset);
                                if (wrapper) {
                                    // Wrapper fields use a region; children use native clones.
                                }
                            }
                        }
                        node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
                    }
                }
            }
        }
        static int lastCollisionExternalRegions = -1;
        static size_t lastCollisionExternalBytes = 0;
        if (collisionExternalRegions != lastCollisionExternalRegions ||
            collisionExternalBytes != lastCollisionExternalBytes) {
            /*HookLog("Rollback: sCollision external discovery regions=%d bytes=%zu\n",
                    collisionExternalRegions, collisionExternalBytes);
            HookLog("Rollback: sCollision discovery summary singleton=0x%llX sbc=0x%llX "
                    "active=%u/%u@0x%llX queues=%u/%u bvh=%u nodes=%u standalone=%u pool=%u/%u/%u "
                    "free=%u/%u move=0x%llX categories=%u@0x%llX queues=%u threads=%u\n",
                    collisionDiag.sCollision, collisionDiag.sbc,
                    collisionDiag.activeCount, collisionDiag.activeCapacity,
                    collisionDiag.activeData,
                    collisionDiag.sbcQueueDataPresent,
                    collisionDiag.sbcQueueHeadersValid,
                    collisionDiag.bvhVisited,
                    collisionDiag.bvhReachableNodes,
                    collisionDiag.bvhStandaloneNodes,
                    collisionDiag.bvhPoolPresent,
                    collisionDiag.bvhPoolHeaderAccessible,
                    collisionDiag.bvhPoolCapacityValid,
                    collisionDiag.bvhFreePresent,
                    collisionDiag.bvhFreeHeaderValid,
                    collisionDiag.moveMatrix,
                    collisionDiag.moveCategoryCount,
                    collisionDiag.moveCategoryArray,
                    collisionDiag.moveQueuesPresent,
                    collisionDiag.moveThreadHeadersValid);*/
            //lastCollisionExternalRegions = collisionExternalRegions;
            //lastCollisionExternalBytes = collisionExternalBytes;
    //    }
    //}

    // Capture the camera manager as a raw region; no vtable exclusion is requested.
    uint64_t camUnit = 0;
    {
        uint64_t sCamera = read.operator()<uint64_t>(_addr(PTR_SCAMERA));
        if (sCamera) {
            add((void*)sCamera, 0xD80, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                , "sCamera"
#endif
            );
            // Viewport 0 starts at +0x50 and stores its camera-unit pointer at +0x08.
            camUnit = read.operator()<uint64_t>(sCamera + 0x58);
        }
    }

    // Capture the uActionSideCamera separately; restore uses its field-specific policy.
    {
        if (camUnit &&
            GetObjectDti(camUnit) == (uint64_t)_addr(DTI_UACTION_SIDE_CAMERA)) {
            read.operator()<uint64_t>(camUnit);
            uint64_t dtiPtr  = GetObjectDti(camUnit);
            uint32_t camSize = DecodeDtiSize(dtiPtr);
            if (camSize < 0x100 || camSize > 0x2000) {
                camSize = 0x590; // known DTI size for uActionSideCamera
            }
            add((void*)camUnit, camSize, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                , "uActionSideCamera"
#endif
            );
        }
    }

    // Capture the manager-linked action state; restore selects its gameplay fields.
    uint64_t sMvc3Mgr = read.operator()<uint64_t>(_addr(PTR_SMVC3MGR));
    if (sMvc3Mgr) {
        uint64_t sAction = read.operator()<uint64_t>(
            sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
        add((void*)sAction, 0x300, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sAction.compact"
#endif
        );
        if (sAction) {
            uint64_t modelJointWrapper = read.operator()<uint64_t>(
                sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET);
            if (modelJointWrapper) {
                uint64_t allocationSlot = modelJointWrapper +
                    SACTION_MODEL_JOINT_ALLOCATION_OFFSET;
                uint64_t allocation = read.operator()<uint64_t>(allocationSlot);
                if (allocation) {
                    // Record the allocation's owner slot; restore applies payload after the header.
                    add((void*)allocation,
                        SACTION_MODEL_JOINT_ALLOCATION_SIZE,
                        allocationSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                        , "sAction.modelJointAllocation"
#endif
                    );
                }
            }
        }
    }

    // Team state and character bodies with their owned capture buffers.
    uint64_t sCharacter = read.operator()<uint64_t>(_addr(PTR_SCHARACTER));
    if (sCharacter) {
        // Tag/assist transition records sit outside the two team bodies.
        add((void*)(sCharacter + SCHARACTER_CHANGE_ORDER_OFFSET),
            SCHARACTER_CHANGE_ORDER_SIZE, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sCharacter.changeOrder"
#endif
        );
        uint64_t team0 = sCharacter + TEAM0_OFFSET;
        uint64_t team1 = sCharacter + TEAM1_OFFSET;
        add((void*)team0, TEAM_SIZE, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "cChrTeam[0]"
#endif
        );
        add((void*)team1, TEAM_SIZE, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "cChrTeam[1]"
#endif
        );

        uint64_t teamCharacters[6] = {};
        int teamCharacterCount = 0;
        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;

            while (node && r < maxRegions) {
                uint64_t character = read.operator()<uint64_t>(
                    node + NODE_CHR_OFFSET);
                if (character) {
                    if (teamCharacterCount < 6) {
                        teamCharacters[teamCharacterCount++] = character;
                    }
                    uint64_t vtable = read.operator()<uint64_t>(character);
                    uint64_t dtiFunc = (vtable >= 0x140000000) ? *(uint64_t*)(vtable + 0x20) : 0;
                    uint64_t dtiPtr = GetObjectDti(character);
                    uint32_t charSize = DecodeDtiSize(dtiPtr);
                    uint32_t finalSize = charSize;
                    if (finalSize < CCHR_MIN_SIZE || finalSize > CCHR_MAX_SIZE) {
                        finalSize = CCHR_NOMINAL_SIZE;
                    }
                    uint32_t* animFields = (uint32_t*)(character + 0x1300);
                    add((void*)character, finalSize, 0, vtable, true
#if ROLLBACK_SNAPDUMP_ENABLED
                        , ti == 0 ? "uCharacter.team0" : "uCharacter.team1"
#endif
                    );

                    // Tron/Linlin auxiliaries are category-9 actor models, not
                    // uCharacters. Capture through their actual owner slot.
                    uint32_t auxiliaryOffset = dtiPtr == _addr(0x140D45F20) ? 0x68F0 :
                        dtiPtr == _addr(0x140D455F8) ? 0x6918 : 0;
                    if (auxiliaryOffset) {
                        const uint64_t ownerSlot = character + auxiliaryOffset;
                        const uint64_t actor = read.operator()<uint64_t>(ownerSlot);
                        const uint64_t expectedDti = _addr(
                            auxiliaryOffset == 0x68F0 ? 0x140D455B0 : 0x140D45638);
                        if (actor && GetObjectDti(actor) == expectedDti &&
                            read.operator()<uint64_t>(actor + 0x1488) == character) {
                            const int actorRegion = r;
                            add((void*)actor, 0x14D0, ownerSlot,
                                read.operator()<uint64_t>(actor), true
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.auxiliaryActor"
#endif
                            );
                            if (r > actorRegion) {
                                auto addActorBuffer = [&](uint32_t field, size_t bytes) {
                                    const uint64_t bufferOwner = actor + field;
                                    const uint64_t buffer = read.operator()<uint64_t>(bufferOwner);
                                    const int before = r;
                                    if (buffer && bytes) add((void*)buffer, bytes, bufferOwner, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                        , "uCharacter.auxiliaryWork"
#endif
                                    );
                                    if (r > before) regions[r - 1].dependentRegion = actorRegion;
                                };
                                const uint32_t joints = read.operator()<uint32_t>(actor + 0x530);
                                if (joints && joints <= 256) {
                                    for (int motion = 0; motion < CCHR_MOTION_COUNT; ++motion)
                                        addActorBuffer(CCHR_MOTION_BASE + motion * CCHR_MOTION_STRIDE +
                                            CCHR_BLEND_PTR_OFFSET, CCHR_BLEND_BYTES_PER_JOINT * joints);
                                    addActorBuffer(0x538, CCHR_JOINT_STATE_STRIDE * joints);
                                    addActorBuffer(0x548, joints);
                                    addActorBuffer(0x11B0, CCHR_BLENDED_JOINT_STRIDE * joints);
                                }
                                const uint32_t channels = read.operator()<uint32_t>(actor + 0x1238);
                                const uint32_t motions = read.operator()<uint32_t>(actor + 0x554);
                                if (channels <= 4096 && motions <= CCHR_MOTION_COUNT)
                                    addActorBuffer(0x6D8, CCHR_MOTION_CHANNEL_STRIDE * channels * motions);
                            }
                        }
                    }

                    // processTarget1 indexes these seven heap-backed tables
                    // using the packed counts at +0x1440 and immediately calls
                    // virtual +0x40 on each entry. Rewind each active table
                    // with its owning slot so its entries stay synchronized
                    // with the restored count.
                    static constexpr uint32_t targetOffsets[] = {
                        0x1408, 0x1410, 0x1418, 0x1420,
                        0x1428, 0x1430, 0x1438,
                    };
                    static constexpr uint32_t targetStrides[] = {
                        0xBB0, 0xBB0, 0xB90, 0xB90,
                        0xBB0, 0xBB0, 0xB80,
                    };
                    uint32_t packedTargetCounts = read.operator()<uint32_t>(
                        character + 0x1440);
                    for (size_t target = 0;
                         target < sizeof(targetOffsets) / sizeof(targetOffsets[0]);
                         ++target) {
                        uint32_t count = (packedTargetCounts >> (target * 4)) & 0xF;
                        uint64_t ownerSlot = character + targetOffsets[target];
                        uint64_t table = read.operator()<uint64_t>(ownerSlot);
                        if (table && count > 0 && count <= 10) {
                            add((void*)table,
                                (size_t)count * targetStrides[target],
                                ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.targetTable"
#endif
                            );
                        }
                    }

                    // Capture each motion slot's external per-joint work buffer.
                    uint32_t numJoints = read.operator()<uint32_t>(
                        character + CCHR_NUM_JOINTS_OFFSET);
                    if (numJoints <= 256) {
                        size_t blendSize = CCHR_BLEND_BYTES_PER_JOINT * numJoints;
                        for (int motion = 0; motion < CCHR_MOTION_COUNT; ++motion) {
                            uint64_t ownerSlot = character + CCHR_MOTION_BASE +
                                motion * CCHR_MOTION_STRIDE + CCHR_BLEND_PTR_OFFSET;
                            uint64_t blend = read.operator()<uint64_t>(ownerSlot);
                            if (blend && blendSize > 0) {
                                add((void*)blend, blendSize, ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                    , "uCharacter.motionBlend"
#endif
                                );
                            }
                        }
                    }

                    // Motion+0x178 pointers share one keyframe-work allocation rooted at +0x6D8.
                    uint32_t channelCount = read.operator()<uint32_t>(character +
                        CCHR_MOTION_CHANNEL_COUNT_OFFSET);
                    uint32_t motionCount = read.operator()<uint32_t>(character +
                        CCHR_MOTION_COUNT_OFFSET);
                    if (channelCount <= 4096 && motionCount <= CCHR_MOTION_COUNT) {
                        size_t channelBytes = CCHR_MOTION_CHANNEL_STRIDE *
                            channelCount * motionCount;
                        uint64_t ownerSlot = character + CCHR_MOTION_CHANNELS_PTR_OFFSET;
                        uint64_t channels = read.operator()<uint64_t>(ownerSlot);
                        if (channels && channelBytes > 0) {
                            add((void*)channels, channelBytes, ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.motionChannels"
#endif
                            );
                        }
                    }

                    // Capture joint state, joint-order bytes, and blended matrices with their owners.
                    if (numJoints <= 256 && numJoints > 0) {
                        uint64_t jointStateOwner = character + CCHR_JOINT_STATE_PTR_OFFSET;
                        uint64_t jointStates = read.operator()<uint64_t>(jointStateOwner);
                        if (jointStates) {
                            add((void*)jointStates,
                                CCHR_JOINT_STATE_STRIDE * numJoints,
                                jointStateOwner, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.jointStates"
#endif
                            );
                        }

                        uint64_t jointIndexOwner = character + CCHR_JOINT_INDEX_PTR_OFFSET;
                        uint64_t jointIndices = read.operator()<uint64_t>(jointIndexOwner);
                        if (jointIndices) {
                            add((void*)jointIndices, numJoints, jointIndexOwner, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.jointIndices"
#endif
                            );
                        }

                        uint64_t blendedOwner = character + CCHR_BLENDED_JOINT_PTR_OFFSET;
                        uint64_t blendedJoints = read.operator()<uint64_t>(blendedOwner);
                        if (blendedJoints) {
                            add((void*)blendedJoints,
                                CCHR_BLENDED_JOINT_STRIDE * numJoints,
                                blendedOwner, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.blendedJoints"
#endif
                            );
                        }
                    }

                    for (uint32_t animOffset : CCHR_EXTERNAL_ANIM_OFFSETS) {
                        uint64_t ownerSlot = character + animOffset;
                        uint64_t anim = read.operator()<uint64_t>(ownerSlot);
                        if (anim) {
                            read.operator()<uint64_t>(anim);
                            add((void*)anim, CCHR_EXTERNAL_ANIM_SIZE, ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.externalAnimation"
#endif
                            );
                        }
                    }

                    // Collision parents can be weapon models; capture their bodies and motion buffers too.
                    uint32_t weaponCount = read.operator()<uint32_t>(
                        character + CCHR_WEAPON_COUNT_OFFSET);
                    uint64_t weaponArray = read.operator()<uint64_t>(
                        character + CCHR_WEAPON_ARRAY_OFFSET);
                    if (weaponArray && weaponCount <= 256) {
                        for (uint32_t weaponIndex = 0;
                             weaponIndex < weaponCount && r < maxRegions;
                             ++weaponIndex) {
                            uint64_t weapon = read.operator()<uint64_t>(
                                weaponArray + (size_t)weaponIndex * sizeof(uint64_t));
                            if (!weapon ||
                                GetObjectDti(weapon) != (uint64_t)_addr(DTI_UCHRWEAPON)) {
                                continue;
                            }

                            uint64_t weaponVtable = read.operator()<uint64_t>(weapon);
                            uint32_t weaponSize = DecodeDtiSize(GetObjectDti(weapon));
                            if (weaponSize < 0x1300 || weaponSize > CCHR_MAX_SIZE)
                                continue;
                            add((void*)weapon, weaponSize, 0, weaponVtable, true
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uChrWeapon"
#endif
                            );

                            uint32_t weaponJoints = read.operator()<uint32_t>(
                                weapon + CCHR_NUM_JOINTS_OFFSET);
                            if (weaponJoints == 0 || weaponJoints > 256)
                                continue;

                            size_t weaponBlendSize =
                                CCHR_BLEND_BYTES_PER_JOINT * weaponJoints;
                            for (int motion = 0; motion < CCHR_MOTION_COUNT; ++motion) {
                                uint64_t ownerSlot = weapon + CCHR_MOTION_BASE +
                                    motion * CCHR_MOTION_STRIDE + CCHR_BLEND_PTR_OFFSET;
                                uint64_t blend = read.operator()<uint64_t>(ownerSlot);
                                if (blend) {
                                    add((void*)blend, weaponBlendSize, ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                        , "uChrWeapon.motionBlend"
#endif
                                    );
                                }
                            }

                            uint32_t channelCount = read.operator()<uint32_t>(
                                weapon + CCHR_MOTION_CHANNEL_COUNT_OFFSET);
                            uint32_t motionCount = read.operator()<uint32_t>(
                                weapon + CCHR_MOTION_COUNT_OFFSET);
                            if (channelCount <= 4096 && motionCount <= CCHR_MOTION_COUNT) {
                                uint64_t ownerSlot =
                                    weapon + CCHR_MOTION_CHANNELS_PTR_OFFSET;
                                uint64_t channels = read.operator()<uint64_t>(ownerSlot);
                                size_t channelBytes = CCHR_MOTION_CHANNEL_STRIDE *
                                    channelCount * motionCount;
                                if (channels && channelBytes > 0) {
                                    add((void*)channels, channelBytes, ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                        , "uChrWeapon.motionChannels"
#endif
                                    );
                                }
                            }

                            struct WeaponJointBuffer {
                                uint32_t ownerOffset;
                                size_t stride;
                                const char* name;
                            };
                            static constexpr WeaponJointBuffer weaponJointBuffers[] = {
                                {CCHR_JOINT_STATE_PTR_OFFSET, CCHR_JOINT_STATE_STRIDE,
                                 "uChrWeapon.jointStates"},
                                {CCHR_JOINT_INDEX_PTR_OFFSET, 1,
                                 "uChrWeapon.jointIndices"},
                                {CCHR_BLENDED_JOINT_PTR_OFFSET, CCHR_BLENDED_JOINT_STRIDE,
                                 "uChrWeapon.blendedJoints"},
                            };
                            for (const WeaponJointBuffer& buffer : weaponJointBuffers) {
                                uint64_t ownerSlot = weapon + buffer.ownerOffset;
                                uint64_t data = read.operator()<uint64_t>(ownerSlot);
                                if (data) {
                                    add((void*)data, buffer.stride * weaponJoints,
                                        ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                        , buffer.name
#endif
                                    );
                                }
                            }
                        }
                    }

                    // Special/combo matcher lines: count +0x170, pointer +0x178, stride 0x20.
                    // Capture the payload, not the preceding allocator header.
                    for (uint64_t cmdOff : {0x2738ull, 0x2950ull}) {
                        uint64_t cmd   = character + cmdOff;
                        uint64_t lines = read.operator()<uint64_t>(cmd + 0x178);
                        int n = read.operator()<int>(cmd + 0x170);
                        if (lines && n > 0 && n < 0x1000) {
                            int previousCount = r;
                            add((void*)lines, (size_t)n * 0x20, cmd + 0x178, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                                , "uCharacter.commandLines"
#endif
                            );
                            if (r > previousCount)
                                regions[r - 1].commandLine = true;
                        }
                    }
                }
                node = read.operator()<uint64_t>(node + NODE_NEXT_OFFSET);
            }
        }

        // Capture category-8 bodies not recorded in the team-character set.
        // This pass does not discover their external model/command buffers.
        uint64_t sUnit = read.operator()<uint64_t>(_addr(PTR_SUNIT));
        uint32_t categoryCount = sUnit ? read.operator()<uint32_t>(
            sUnit + SUNIT_CATEGORY_COUNT_OFFSET) : 0;
        if (sUnit && categoryCount > SUNIT_CHARACTER_CATEGORY) {
            uint64_t helper = read.operator()<uint64_t>(
                sUnit + SUNIT_CATEGORY_LISTS_OFFSET +
                SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
            int safety = 64;
            while (helper && safety-- > 0 && r < maxRegions) {
                bool isTeamCharacter = false;
                for (int i = 0; i < teamCharacterCount; ++i) {
                    if (teamCharacters[i] == helper) {
                        isTeamCharacter = true;
                        break;
                    }
                }
                if (!isTeamCharacter) {
                    bool alreadyCaptured = false;
                    for (int region = 0; region < r; ++region)
                        alreadyCaptured |= regions[region].ptr == (void*)helper;
                    read.operator()<uint64_t>(helper);
                    uint32_t helperSize = DecodeDtiSize(GetObjectDti(helper));
                    // Only character-sized helpers belong in this path. Smaller
                    // controllers such as uChrCpu are intentionally not captured.
                    if (!alreadyCaptured && helperSize >= CCHR_MIN_SIZE && helperSize <= CCHR_MAX_SIZE) {
                        add((void*)helper, helperSize, 0, *(uint64_t*)helper, true
#if ROLLBACK_SNAPDUMP_ENABLED
                            , "uCharacter.helper"
#endif
                        );
                    }
                }
                helper = read.operator()<uint64_t>(helper + 0x18);
            }
        }

        // uCharacterPost uses separate slot state, restored into the current coordinator.
        // Do not add its historical allocation as a raw region.
    }

    // Dynamic shots/effects are not captured until the lifetime-aware replacement exists.

    return r;
}

int Rollback_BuildRegions(Region* regions, int maxRegions)
{
    if (!regions || maxRegions < 1) return 0;

    if (g_regionCache.valid &&
        RegionDependenciesValid(g_regionCache.dependencies)) {
        int count = g_regionCache.count < maxRegions
            ? g_regionCache.count : maxRegions;
        std::memcpy(regions, g_regionCache.regions, count * sizeof(Region));
        return count;
    }

    g_regionCache.dependencies = {};
    int count = BuildRegionsUncached(g_regionCache.regions, MAX_REGIONS,
                                     g_regionCache.dependencies);
    g_regionCache.count = count;
    g_regionCache.valid = count > 0 && !g_regionCache.dependencies.overflow;

    int copyCount = count < maxRegions ? count : maxRegions;
    std::memcpy(regions, g_regionCache.regions, copyCount * sizeof(Region));
    return copyCount;
}

void Rollback_SaveSnapshot(int32_t frame)
{
    LifetimeTrace_DropSnapshot(frame); // Invalidate observation before any slot mutation/failure.
    LifetimeTrace_Marker("snapshot_request", frame);
    LagDiagnosticScope diagnostic(g_isResimulating ? LagMetric::SaveResim : LagMetric::SaveLive);
    LagDiagnosticScope diagnosticStage(LagMetric::SaveRegions);
    const uint64_t benchmarkStart = LiveBenchmark_Now();
    const uint64_t profileStart = Profile_Now();
#if ROLLBACK_SNAPDUMP_ENABLED
    const bool captureSnapDump = SnapDump_CaptureEnabled();
#endif
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    int count = Rollback_BuildRegions(slot.regions, MAX_REGIONS);
    if (count == 0) return;
    const uint64_t buildRegionsEnd = Profile_Now();
    diagnosticStage.Next(LagMetric::SaveAllocate);

    size_t total = 0;
    for (int i = 0; i < count; ++i) {
        total += slot.regions[i].size;
    }

    /*if (frame >= 14 && frame <= 20) {
        uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
        int32_t replayFrame = replay ? *(int32_t*)(replay + 0x54) : -1;
        HookLog("SNAPTRACE phase=begin frame=%d resim=%d replay=%d\n",
                frame, g_isResimulating ? 1 : 0, replayFrame);
    }*/

    bool hasSharedStorage = false;
#if ROLLBACK_SNAPDUMP_ENABLED
    size_t sharedCapacity = 0;
    uint8_t* sharedData = captureSnapDump
        ? SnapDump_AcquireSnapshotStorage(frame, total, count, &sharedCapacity, &slot.lifetime)
        : nullptr;
    if (sharedData) {
        if (slot.ownsData)
            delete[] slot.data;
        slot.data = sharedData;
        slot.capacity = sharedCapacity;
        slot.ownsData = false;
        hasSharedStorage = true;
    }
#endif
    if (!hasSharedStorage && slot.lifetime.use_count() > 1)
        slot.lifetime.reset();
    if (!hasSharedStorage && !slot.ownsData) {
        slot.data = nullptr;
        slot.capacity = 0;
        slot.ownsData = true;
    }
    if (!hasSharedStorage && slot.capacity < total) {
        size_t newCapacity = total;
        if (total <= SIZE_MAX - (SNAPSHOT_CAPACITY_GRANULARITY - 1)) {
            newCapacity = (total + SNAPSHOT_CAPACITY_GRANULARITY - 1) &
                ~(SNAPSHOT_CAPACITY_GRANULARITY - 1);
        }
        delete[] slot.data;
        slot.data = new (std::nothrow) uint8_t[newCapacity];
        if (!slot.data) {
            HookLog("Rollback: failed to allocate %zu-byte snapshot for frame %d\n",
                    newCapacity, frame);
            slot.capacity    = 0;
            slot.size        = 0;
            slot.regionCount = 0;
            return;
        }
        slot.capacity = newCapacity;
    }
    const uint64_t allocationEnd = Profile_Now();
    diagnosticStage.Next(LagMetric::SaveCopy);

    // Copy region payloads; keep this frame's region metadata alongside the bytes.
    size_t offset = 0;
    slot.regionCount = count;
    slot.frame = frame;
    for (int i = 0; i < count; ++i) {
        const Region& region = slot.regions[i];
        std::memcpy(slot.data + offset, region.ptr, region.size);
        offset += region.size;
    }
    slot.size = offset;
    CaptureCharacterMotionResources(slot);
    CaptureActionSideCameraState(slot);
    CaptureCharacterPostState(slot);
    const uint64_t copyRegionsEnd = Profile_Now();
    diagnosticStage.Finish();

    // Disabled RNG snapshot logging.
    uint32_t globalRng[4] = {};
    //std::memcpy(globalRng, (void*)_addr(0x140D765D8), sizeof(globalRng));
    /*HookLog("RNGSNAP frame=%d resim=%d state=%08X,%08X,%08X,%08X\n",
            frame, g_isResimulating ? 1 : 0,
            globalRng[0], globalRng[1], globalRng[2], globalRng[3]);*/

    static int lastSnapshotRegionCount = -1;
    if (count != lastSnapshotRegionCount) {
        int collisionIndex = -1;
        size_t collisionSize = 0;
        for (int i = 0; i < count; ++i) {
            if ((uintptr_t)slot.regions[i].ptr == *(uint64_t*)_addr(PTR_SCOLLISION)) {
                collisionIndex = i;
                collisionSize = slot.regions[i].size;
                break;
            }
        }
        HookLog("Rollback: snapshot frame %d regions=%d bytes=%zu sCollisionIndex=%d sCollisionSize=%zu\n",
                frame, count, offset, collisionIndex, collisionSize);
        lastSnapshotRegionCount = count;
    }
    const uint64_t snapshotMetadataEnd = Profile_Now();

    // Capture collision children through native copyFrom, plus remappable parent identities.
    diagnosticStage.Next(LagMetric::SaveClones);
    Rollback_CaptureCollisionNodeClones(slot);
    diagnosticStage.Next(LagMetric::SaveParents);
    CaptureCharacterCollisionParentReferences(slot);
    const uint64_t collisionClonesEnd = Profile_Now();
    diagnosticStage.Next(LagMetric::SaveRenderState);

    // Save visibility, draw layers, and transparency separately from live model ownership.
    slot.modelRenderStates.clear();
    auto captureModelRenderState = [&slot](void* model, bool character) {
        if (!model) return;
        for (RollbackModelRenderState& saved : slot.modelRenderStates) {
            if (saved.model == model) {
                saved.character = saved.character || character;
                return;
            }
        }
        slot.modelRenderStates.push_back({
            model,
            *(uint64_t*)model,
            *(uint32_t*)((uint8_t*)model + 0x10),
            *(uint32_t*)((uint8_t*)model + 0x11C),
            *(float*)((uint8_t*)model + 0x180),
            character,
        });
    };
    auto captureCharacterRenderState = [&captureModelRenderState](void* character) {
        if (!character) return;
        captureModelRenderState(character, true);
        uint32_t weaponCount = *(uint32_t*)((uint8_t*)character + 0x3F38);
        void** weapons = *(void***)((uint8_t*)character + 0x3F40);
        if (!weapons || weaponCount > 256) {
            return;
        }
        for (uint32_t i = 0; i < weaponCount; ++i)
            captureModelRenderState(weapons[i], false);
    };
    uint64_t renderCharacterManager = *(uint64_t*)_addr(PTR_SCHARACTER);
    for (int teamIndex = 0; renderCharacterManager && teamIndex < 2; ++teamIndex) {
        uint64_t node = renderCharacterManager +
            (teamIndex == 0 ? TEAM0_OFFSET : TEAM1_OFFSET) +
            TEAM_FIRST_NODE_OFFSET;
        for (int member = 0; node && member < 8; ++member) {
            void* character = *(void**)(node + NODE_CHR_OFFSET);
            captureCharacterRenderState(character);
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    uint64_t renderUnitManager = *(uint64_t*)_addr(PTR_SUNIT);
    uint32_t renderCategoryCount = renderUnitManager
        ? *(uint32_t*)(renderUnitManager + SUNIT_CATEGORY_COUNT_OFFSET)
        : 0;
    if (renderCategoryCount > SUNIT_CHARACTER_CATEGORY) {
        void* character = *(void**)(renderUnitManager + SUNIT_CATEGORY_LISTS_OFFSET +
            SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
        for (int safety = 64; character && safety-- > 0;
             character = *(void**)((uint8_t*)character + 0x18)) {
            captureCharacterRenderState(character);
        }
    }
    const uint64_t snapshotEnd = Profile_Now();
    diagnosticStage.Finish();

    Profile_RecordSnapshot(profileStart, buildRegionsEnd, allocationEnd, copyRegionsEnd,
                           snapshotMetadataEnd, collisionClonesEnd, snapshotEnd,
                           offset, (uint32_t)count);

    // Disabled performance-counter trace.
    // LARGE_INTEGER qpc_cap;
    // if (QueryPerformanceCounter(&qpc_cap)) {
    //     g_qpc_history[slotIdx] = qpc_cap.QuadPart;
    // }
    
    // Capture local input from the absolute-frame schedule, not the legacy shift buffer.
    slot.localInput = Controller_GetCurrentFrameInput();

    // Seed remote input from the selected slot; controller updates it when input is resolved.
    slot.remoteInput = Controller_GetUsedRemoteInput(frame);
    /*if (frame >= 14 && frame <= 20) {
        HookLog("SNAPTRACE phase=end frame=%d resim=%d local=0x%08X remote=0x%08X bytes=%zu\n",
                frame, g_isResimulating ? 1 : 0, slot.localInput,
                slot.remoteInput, slot.size);
    }*/
    LifetimeTrace_SaveSnapshot(frame, slot.lifetime);
#if ROLLBACK_SNAPDUMP_ENABLED
    if (captureSnapDump) {
        LagDiagnosticScope dumpDiagnostic(LagMetric::SnapDump);
        SnapDump_CaptureSnapshot(slot);
    }
#endif
    
    //HookLog("Rollback: saved frame %d with %d regions, total size %zu bytes\n", frame, count, offset);
    LiveBenchmark_Record(LiveBenchmarkMetric::Save, benchmarkStart);
}

void Rollback_UpdateRemoteInput(int32_t frame, uint32_t input)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];
    if (slot.data && slot.size > 0 && slot.regionCount > 0 && slot.frame == frame)
    {
        uint32_t previous = slot.remoteInput;
        slot.remoteInput = input;
#if ROLLBACK_SNAPDUMP_ENABLED
        SnapDump_UpdateInputs(frame, slot.localInput, input);
#endif
        /*HookLog("IPSNAPREMOTE frame=%d applied=1 old=%08X new=%08X local=%08X\n",
                frame, previous, input, slot.localInput);*/
    }
    else
    {
        /*HookLog("IPSNAPREMOTE frame=%d applied=0 new=%08X slotFrame=%d\n",
                frame, input, slot.frame);*/
    }
}

extern "C" void Rollback_UpdateSnapshotInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    if (slot.frame == frame && slot.data && slot.size > 0)
    {
        uint32_t previousLocal = slot.localInput;
        uint32_t previousRemote = slot.remoteInput;
        slot.localInput = localInput;
        slot.remoteInput = remoteInput;
#if ROLLBACK_SNAPDUMP_ENABLED
        SnapDump_UpdateInputs(frame, localInput, remoteInput);
#endif
        /*HookLog("IPSNAP frame=%d resim=%d applied=1 oldLocal=%08X oldRemote=%08X "
                "newLocal=%08X newRemote=%08X\n",
                frame, g_isResimulating ? 1 : 0, previousLocal, previousRemote,
                localInput, remoteInput);*/
    }
    else
    {
        /*HookLog("IPSNAP frame=%d resim=%d applied=0 local=%08X remote=%08X slotFrame=%d\n",
                frame, g_isResimulating ? 1 : 0, localInput, remoteInput,
                slot.frame);*/
    }
}

extern "C" bool Rollback_GetSnapshotInputs(int32_t frame, uint32_t* outLocal, uint32_t* outRemote)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    const SnapshotSlot& slot = g_slots[slotIdx];
    
    if (slot.frame == frame && slot.data && slot.size > 0)
    {
        if (outLocal) *outLocal = slot.localInput;
        if (outRemote) *outRemote = slot.remoteInput;
        return true;
    }
    return false;
}

void Rollback_FreeSnapshots()
{
    LifetimeTrace_ClearSnapshots();
    // Restore native calls before releasing snapshot storage.
    Rollback_PatchSleep(false);
    Rollback_PatchPresent(false);
    Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false);
    Rollback_PatchDrawCalls(false); // Redundant restoration; no-op if already unpatched.

    for (int i = 0; i < ROLLBACK_WINDOW; ++i) {
        Rollback_DestroyCollisionNodeClones(g_slots[i]);
        ReleaseActionSideCameraState(g_slots[i]);
        ReleaseCharacterMotionResources(g_slots[i]);
        g_slots[i].lifetime.reset();
        if (g_slots[i].ownsData)
            delete[] g_slots[i].data;
        g_slots[i].data        = nullptr;
        g_slots[i].size        = 0;
        g_slots[i].capacity    = 0;
        g_slots[i].ownsData    = true;
        g_slots[i].regionCount = 0;
        g_slots[i].frame       = -1;
        g_slots[i].characterCollisionParentReferences.clear();
        g_slots[i].modelRenderStates.clear();
    }
    Rollback_ResetReSimulation();
    g_regionCache.valid = false;
    // Keep snapdump history intact when freeing live rollback slots.
}
