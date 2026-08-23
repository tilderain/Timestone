#include "../pch.h"

#include "rollback_snapshot.h"

#include "controller.h"
#include "rollback_snapdump.h"
#include "mk10utils.h"
#include "rollback_effect.h"
#include "rollback_memory.h"
#include "rollback_model_policy.h"
#include "rollback_profile.h"
#include "rollback_restore.h"
#include "rollback_resim.h"
#include "rollback_shot.h"

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
static constexpr uintptr_t PTR_SSHOT = 0x140d47f40;
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
// cChr's shared body ends where concrete character tails begin. Some helper
// subclasses, including uFeliciaF, are smaller than the historical 0x6A60
// fallback; expanding them overwrites unrelated allocations placed after them.
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

    // createProperty reflects mRno, mUnitParam, mDeltaTime, and mUnitGroup from
    // cUnit. Preserve the two generic gameplay values, callback enablement, and
    // unit-group dispatch mask. Do not copy the complete cUnit flags dword:
    // lifecycle/category, Draw/Fix, and view bits share it with Move. List links
    // at +0x18/+0x20 remain owned by the current sUnit allocation, while
    // mDeltaTime at +0x28 is derived by sUnit immediately before every move.
    slot.characterPost.rno = *(int32_t*)(characterPost + CUNIT_RNO_OFFSET);
    slot.characterPost.unitParam =
        *(int32_t*)(characterPost + CUNIT_UNIT_PARAM_OFFSET);
    slot.characterPost.moveEnabled =
        (*(uint32_t*)(characterPost + CUNIT_FLAGS_OFFSET) &
         CUNIT_MOVE_ENABLED_FLAG) != 0;
    slot.characterPost.unitGroup =
        *(uint64_t*)(characterPost + CUNIT_GROUP_OFFSET);

    // Ghidra constructor and access audit: +0x40..+0x4f is the complete
    // class-specific scalar state. The only remaining class fields are the two
    // cached character pointers at +0x50/+0x58.
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

    // The cache is defined as one point character per team. Avoid carrying an
    // unresolved prediction-timeline address into a future live allocation.
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

    std::unordered_set<uint64_t> capturedCharacters;
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
        std::memcpy(&value, (void*)dependency.address, dependency.size);
        if (value != dependency.value) return false;
    }
    return true;
}

// ===========================================================================
// Safe Dynamic List Traversal
//
// Walks an intrusive linked list starting at |listHead|, reading each node's
// DTI to determine object size.  Bounded by DYNAMIC_SAFETY to prevent
// infinite loops on corrupted lists.
// ===========================================================================
static void TraverseAndAddDynamicList(uint64_t listHead, size_t nextOffset,
                                      int& r, Region* regions, int maxRegions)
{
    uint64_t current = listHead;
    int safety = DYNAMIC_SAFETY;

    while (current && r < maxRegions && safety-- > 0) {
        // Reject obviously-bogus pointers
        if (current < 0x10000) break;

        // Read object size from DTI (accessed via vtable[4], not object+0x08)
        uint64_t dtiPtr = GetObjectDti(current);
        uint32_t objSize = DecodeDtiSize(dtiPtr);
        if (objSize == 0 || objSize > MAX_CHAR_SIZE) {
            objSize = 0x400; // conservative fallback for typical nodes
        }

        regions[r].ptr  = (void*)current;
        regions[r].size = objSize;
        regions[r].ownerSlot = 0;
        regions[r].expectedVtable = 0;
        regions[r].skipVtable = false;
#if ROLLBACK_SNAPDUMP_ENABLED
        regions[r].snapDumpName = "dynamic_list_object";
#endif
        ++r;

        current = *(uint64_t*)(current + nextOffset);
    }
}

// ===========================================================================
// Region List Builder — enumerates all snapshot-worthy memory
// ===========================================================================
static int BuildRegionsUncached(Region* regions, int maxRegions,
                                RegionDependencies& dependencies)
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
#if ROLLBACK_SNAPDUMP_ENABLED
            regions[r].snapDumpName = snapDumpName;
#endif
            ++r;
        }
    };

    int collisionExternalRegions = 0;
    size_t collisionExternalBytes = 0;
    struct CollisionDiscoveryDiag {
        uint64_t sCollision = 0;
        uint64_t sbc = 0;
        uint64_t moveMatrix = 0;
        uint64_t activeData = 0;
        uint32_t activeCount = 0;
        uint32_t activeCapacity = 0;
        uint64_t moveCategoryArray = 0;
        uint32_t moveCategoryCount = 0;
        uint32_t bvhVisited = 0;
        uint32_t bvhPoolPresent = 0;
        uint32_t bvhPoolHeaderAccessible = 0;
        uint32_t bvhPoolCapacityValid = 0;
        uint32_t bvhFreePresent = 0;
        uint32_t bvhFreeHeaderValid = 0;
        uint32_t bvhReachableNodes = 0;
        uint32_t bvhStandaloneNodes = 0;
        uint32_t sbcQueueHeadersValid = 0;
        uint32_t sbcQueueDataPresent = 0;
        uint32_t moveQueuesPresent = 0;
        uint32_t moveThreadHeadersValid = 0;
    } collisionDiag;

    auto logOwnedBufferRejection = [&](const char* label, uint64_t ownerSlot,
                                       uint64_t ptr, size_t size) {
        return;
        static std::unordered_map<std::string, std::string> lastReasons;
        const char* reason = nullptr;
        if (!ownerSlot) reason = "null owner slot";
        else if (!ptr) reason = "null data pointer";
        else if (!size) reason = "zero size";
        else if (size > 0x1000000) reason = "size exceeds limit";
        else if (!IsAccessibleRange(ptr, size)) reason = "range inaccessible";
        if (!reason) return;

        char state[192];
        std::snprintf(state, sizeof(state),
                      "%s owner=0x%llX ptr=0x%llX size=%zu",
                      reason, ownerSlot, ptr, size);
        std::string& previous = lastReasons[label];
        if (previous != state) {
            HookLog("Rollback: sCollision discovery reject %s: %s\n", label, state);
            previous = state;
        }
    };

    auto addOwnedBuffer = [&](uint64_t ownerSlot, uint64_t ptr, size_t size
#if ROLLBACK_SNAPDUMP_ENABLED
                              , const char* snapDumpName = "owned_buffer"
#endif
                              ) {
        if (!ownerSlot || !ptr || !size || size > 0x1000000 ||
            !IsAccessibleRange(ptr, size)) {
            return;
        }
        if (r >= maxRegions) return;
        add((void*)ptr, size, ownerSlot, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , snapDumpName
#endif
        );
        //++collisionExternalRegions;
        //collisionExternalBytes += size;
    };

    std::unordered_set<uint64_t> capturedStandaloneBvhNodes;
    auto addDynamicBvhBuffers = [&](uint64_t bvh, uint32_t bank, uint32_t index) {
        //++collisionDiag.bvhVisited;
        if (!bvh || !IsAccessiblePointer(bvh)) {
            if (index == 0)
                HookLog("Rollback: sCollision discovery BVH bank=0x%X base=0x%llX inaccessible\n",
                        bank, bvh);
            return;
        }

        // resizeNodePool allocates a 0x10-byte allocator header followed by
        // 0x40-byte nodes and stores the first node in +0x18. Do not rewind
        // allocator metadata; capture the node payload itself. The +0x20
        // array contains pointers into that pool and defines free-list order.
        uint64_t pool = *(uint64_t*)(bvh + 0x18);
        //if (pool) ++collisionDiag.bvhPoolPresent;
        uint32_t poolCapacity = 0;
        if (pool && IsAccessiblePointer(pool - 0x10)) {
            //++collisionDiag.bvhPoolHeaderAccessible;
            poolCapacity = *(uint32_t*)(pool - 0x10);
        }
        if (poolCapacity > 0 && poolCapacity <= 0x10000) {
            //++collisionDiag.bvhPoolCapacityValid;
            addOwnedBuffer(bvh + 0x18, pool,
                           (size_t)poolCapacity * SCOLLISION_NODE_SIZE);
        } else if (pool && index == 0) {
        //    HookLog("Rollback: sCollision discovery BVH bank=0x%X pool=0x%llX capacity=%u headerAccessible=%d\n",
        //            bank, pool, poolCapacity, IsAccessiblePointer(pool - 0x10));
        }

        uint32_t freeCount = *(uint32_t*)(bvh + 0x28);
        uint32_t freeCapacity = *(uint32_t*)(bvh + 0x2C);
        uint64_t freeNodes = *(uint64_t*)(bvh + 0x20);
        //if (freeNodes) ++collisionDiag.bvhFreePresent;
        if (freeCount <= freeCapacity && freeCapacity <= 0x10000) {
        //    ++collisionDiag.bvhFreeHeaderValid;
            addOwnedBuffer(bvh + 0x20, freeNodes, (size_t)freeCapacity * 8);
        } else if (index == 0) {
        //    HookLog("Rollback: sCollision discovery BVH bank=0x%X freeNodes=0x%llX count=%u capacity=%u\n",
        //            bank, freeNodes, freeCount, freeCapacity);
        }

        // A second MtArray at +0x30 tracks nodes not currently assigned to
        // the reserved pool. It has its own external pointer array at +0x48.
        uint32_t noUseCount = *(uint32_t*)(bvh + 0x38);
        uint32_t noUseCapacity = *(uint32_t*)(bvh + 0x3C);
        uint64_t noUseNodes = *(uint64_t*)(bvh + 0x48);
        if (noUseCount <= noUseCapacity && noUseCapacity <= 0x10000) {
            addOwnedBuffer(bvh + 0x48, noUseNodes,
                           (size_t)noUseCapacity * 8);
        }

        // popNodeOrAlloc falls back to individually allocated 0x40-byte nodes
        // after both pointer arrays are exhausted. Walk the live tree and
        // capture every reachable node outside this BVH's fixed pool. The
        // parent/root field is the owner slot, so restore is skipped if the
        // live topology no longer references the saved allocation.
        struct PendingBvhNode { uint64_t ownerSlot; uint64_t node; };
        std::vector<PendingBvhNode> pending;
        uint64_t root = *(uint64_t*)(bvh + 0x08);
        if (root) pending.push_back({bvh + 0x08, root});
        std::unordered_set<uint64_t> visited;
        while (!pending.empty() && visited.size() < 0x10000) {
            PendingBvhNode current = pending.back();
            pending.pop_back();
            if (!current.node || !IsAccessiblePointer(current.node) ||
                !visited.insert(current.node).second) {
                continue;
            }
            //++collisionDiag.bvhReachableNodes;

            bool inFixedPool = pool && poolCapacity > 0 &&
                current.node >= pool &&
                current.node < pool + (size_t)poolCapacity * SCOLLISION_NODE_SIZE &&
                ((current.node - pool) % SCOLLISION_NODE_SIZE) == 0;
            if (!inFixedPool && capturedStandaloneBvhNodes.insert(current.node).second) {
                addOwnedBuffer(current.ownerSlot, current.node, SCOLLISION_NODE_SIZE);
            //    ++collisionDiag.bvhStandaloneNodes;
            }

            uint64_t child0 = *(uint64_t*)(current.node + 0x10);
            uint64_t child1 = *(uint64_t*)(current.node + 0x18);
            if (child0) pending.push_back({current.node + 0x10, child0});
            if (child1) pending.push_back({current.node + 0x18, child1});
        }
    };

    auto addNodeListBuffers = [&](uint64_t nodeList) {
        if (!nodeList || !IsAccessiblePointer(nodeList)) return;

        // NodeList contains seven 41-entry MtArray groups, four singleton
        // MtArrays, and two embedded dynamic BVHs. The object itself is
        // captured separately; these are its heap-backed pointer arrays.
        static constexpr uint32_t arrayBases[] = {
            0x18, 0x538, 0xA58, 0x1028, 0x1548, 0x1A68, 0x1F88
        };
        for (uint32_t base : arrayBases) {
            for (uint32_t category = 0; category < 0x29; ++category) {
                uint64_t header = nodeList + base + category * 0x20;
                uint32_t count = *(uint32_t*)(header + 0x08);
                uint32_t capacity = *(uint32_t*)(header + 0x0C);
                uint64_t data = *(uint64_t*)(header + 0x18);
                if (count <= capacity && capacity <= 0x10000) {
                    addOwnedBuffer(header + 0x18, data, (size_t)capacity * 8);
                }
            }
        }

        for (uint32_t headerOffset : {0xF78u, 0xF98u, 0x24A8u, 0x24C8u}) {
            uint64_t header = nodeList + headerOffset;
            uint32_t count = *(uint32_t*)(header + 0x08);
            uint32_t capacity = *(uint32_t*)(header + 0x0C);
            uint64_t data = *(uint64_t*)(header + 0x18);
            if (count <= capacity && capacity <= 0x10000) {
                addOwnedBuffer(header + 0x18, data, (size_t)capacity * 8);
            }
        }

        addDynamicBvhBuffers(nodeList + 0xFC0, 0xFC0, 0);
        addDynamicBvhBuffers(nodeList + 0x24E8, 0x24E8, 0);
    };

    auto addSbcObjectBuffers = [&](uint64_t sbc) {
        //collisionDiag.sbc = sbc;
        if (!sbc || !IsAccessiblePointer(sbc)) {
            HookLog("Rollback: sCollision discovery SbcObject=0x%llX inaccessible\n", sbc);
            return;
        }

        // Active registration array: MtArray header at +0x40, data at +0x58.
        uint32_t activeCount = *(uint32_t*)(sbc + 0x48);
        uint32_t activeCapacity = *(uint32_t*)(sbc + 0x4C);
        uint64_t activeData = *(uint64_t*)(sbc + 0x58);
        //collisionDiag.activeCount = activeCount;
        //collisionDiag.activeCapacity = activeCapacity;
        //collisionDiag.activeData = activeData;
        if (activeCount <= activeCapacity && activeCapacity <= 0x10000) {
            addOwnedBuffer(sbc + 0x58, activeData,
                           (size_t)activeCapacity * 8);
            if (activeCapacity > 0)
                logOwnedBufferRejection("SbcObject.active", sbc + 0x58, activeData,
                                        (size_t)activeCapacity * 8);
        } else {
            HookLog("Rollback: sCollision discovery active invalid count=%u capacity=%u data=0x%llX\n",
                    activeCount, activeCapacity, activeData);
        }

        // Dirty and merge queues are 41 MtArray-derived NodeList headers with
        // a 0x20-byte stride. These offsets start at the count field: capacity
        // is +0x04, ownership is +0x08, and data is +0x10.
        for (uint32_t base : {0x70u, 0x590u}) {
            for (uint32_t category = 0; category < 0x29; ++category) {
                uint64_t header = sbc + base + category * 0x20;
                uint32_t count = *(uint32_t*)(header + 0x00);
                uint32_t capacity = *(uint32_t*)(header + 0x04);
                uint64_t data = *(uint64_t*)(header + 0x10);
                if (count <= capacity && capacity <= 0x10000) {
                    //++collisionDiag.sbcQueueHeadersValid;
                    //if (data) ++collisionDiag.sbcQueueDataPresent;
                    addOwnedBuffer(header + 0x10, data, (size_t)capacity * 8);
                    if (capacity > 0 && category == 0)
                        logOwnedBufferRejection(base == 0x70 ? "SbcObject.dirty[0]" :
                                                 "SbcObject.merge[0]",
                                                 header + 0x10, data,
                                                 (size_t)capacity * 8);
                } else if (category == 0) {
                   //HookLog("Rollback: sCollision discovery queue base=0x%X count=%u capacity=%u data=0x%llX\n",
                            //base, count, capacity, data);
                }
                if (data && count <= capacity && capacity <= 0x10000) {
                    for (uint32_t entry = 0; entry < count; ++entry) {
                        uint64_t recordSlot = data + (size_t)entry * 8;
                        uint64_t record = *(uint64_t*)recordSlot;
                        addOwnedBuffer(recordSlot, record, 0x28);
                    }
                }
            }
        }
    };

    auto addMoveMatrixBuffers = [&](uint64_t moveMatrix) {
        //collisionDiag.moveMatrix = moveMatrix;
        if (!moveMatrix || !IsAccessiblePointer(moveMatrix)) {
            //HookLog("Rollback: sCollision discovery moveMatrix=0x%llX inaccessible\n",
            //        moveMatrix);
            return;
        }
        uint32_t categoryCount = *(uint32_t*)(moveMatrix + 0x10);
        uint32_t categoryCapacity = *(uint32_t*)(moveMatrix + 0x14);
        uint64_t categoryArray = *(uint64_t*)(moveMatrix + 0x20);
        //collisionDiag.moveCategoryCount = categoryCount;
        //collisionDiag.moveCategoryArray = categoryArray;
        if (categoryCount > categoryCapacity || categoryCapacity > 0x40 ||
            !categoryArray ||
            !IsAccessiblePointer(categoryArray)) {
            //HookLog("Rollback: sCollision discovery move categories count=%u capacity=%u array=0x%llX accessible=%d\n",
            //        categoryCount, categoryCapacity, categoryArray,
            //        IsAccessiblePointer(categoryArray));
            return;
        }

        addOwnedBuffer(moveMatrix + 0x20, categoryArray,
                       (size_t)categoryCapacity * 8);

        // Each category points to an MtArray<NodeList*>. Its backing array
        // selects one full 0x2550-byte NodeList per frame/thread slot.
        for (uint32_t category = 0; category < categoryCount; ++category) {
            uint64_t categorySlot = categoryArray + (size_t)category * 8;
            uint64_t queue = *(uint64_t*)categorySlot;
            if (!queue || !IsAccessiblePointer(queue)) continue;
            //++collisionDiag.moveQueuesPresent;
            uint32_t threadCount = *(uint32_t*)(queue + 8);
            uint32_t threadCapacity = *(uint32_t*)(queue + 0xC);
            uint64_t threadData = *(uint64_t*)(queue + 0x18);
            if (threadCount > threadCapacity || threadCapacity > 0x40) {
                if (category == 0)
                    //HookLog("Rollback: sCollision discovery move queue=0x%llX threadCount=%u capacity=%u\n",
                    //        queue, threadCount, threadCapacity);
                continue;
            }
            addOwnedBuffer(queue + 0x18, threadData,
                           (size_t)threadCapacity * 8);
            for (uint32_t thread = 0; thread < threadCount; ++thread) {
                uint64_t nodeListSlot = threadData + (size_t)thread * 8;
                uint64_t nodeList = *(uint64_t*)nodeListSlot;
                if (!nodeList || !IsAccessiblePointer(nodeList)) continue;
                //++collisionDiag.moveThreadHeadersValid;
                addOwnedBuffer(nodeListSlot, nodeList, 0x2550);
                addNodeListBuffers(nodeList);
            }
        }
    };

    // ---- 1. sReplay: frame counter only (sReplay+0x54). Full replay buffer
    //     has internal pointers that go stale. ---
    uint64_t sReplay = read.operator()<uint64_t>(_addr(PTR_SREPLAY));
    if (sReplay) {
        add((void*)(sReplay + 0x54), 4, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sReplay.frameCounter"
#endif
        );
    }

    // ---- 1a. Global XorShift RNG state (16 bytes, 4 × uint32) ----
    //     Used by 64/73 call sites across gameplay (hit roll, damage variance,
    //     AI decisions, projectile patterns).  Without restoring this, re-simulation
    //     with different inputs consumes RNG differently, causing desyncs in all
    //     RNG-derived behavior between the original and re-simulated timelines.
    //     Address confirmed from LEA RAX,[0x140d765d8] in MtXorShift__getGlobalState.
    add((void*)_addr(0x140d765d8), 16, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
        , "globalRng"
#endif
    );

    // sEffect+0x3C is peer-local presentation RNG. Shot recipes capture the
    // exact pre-spawn value they need; it is not authoritative frame state.

    // ---- 2. sShot manager header and shot ID pool ----
    // Keep intrusive sentinel topology live; recipes reconcile shot lifetime.
    // The 256-entry ID pool starts at +0x38 and its allocation cursor is at
    // +0x438. Capture it separately from the manager header so repeated recipe
    // construction cannot move the client's projectile-ID namespace forward.
    uint64_t sShot = read.operator()<uint64_t>(_addr(PTR_SSHOT));
    if (sShot) {
        add((void*)sShot, 0x38, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sShot"
#endif
        );
        add((void*)(sShot + 0x38), 0x404, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sShot.idPool"
#endif
        );
    }

    // ---- 2a. Slot active flags (NOT in sShot — stored in sGameConfig+0x4A89C) ---
    //     sShot__setSlotActiveFlag writes to uHaggar__getter_49F0(sGameConfig) + 0x48A6c.
    //     uHaggar__getter_49F0 returns sGameConfig + 0x1E30, so the flags live at
    //     sGameConfig + 0x1E30 + 0x48A6c = sGameConfig + 0x4A89C.  10 slots × 1 byte.
    //     Without restoring these the game sees no active projectiles and frees them,
    //     causing stale-address corruption on the next restore.
    {
        uint64_t sGameConfig = read.operator()<uint64_t>(_addr(0x140D50F88));
        if (sGameConfig) {
            add((void*)(sGameConfig + 0x4A89C), 10, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                , "sGameConfig.shotSlotFlags"
#endif
            );
        }
    }

    // uShot heap bodies are not copied. Their intrusive links and owned effect
    // pointers cannot safely survive object destruction; native spawn recipes
    // below reconstruct missing shots and their dependent objects.

    // ---- 3. sHitSolver — collision pairs + result buffer (static array) ----
    //     Full body is 0x3808C (sHitSolver_field_map.md §2): 192KB cHitNode pool,
    //     counters at +0x30060, SortBucketA @+0x30070, SortBucketB @+0x34078,
    //     mWorkCnt/B/Final tail @+0x38080.  The previous 0x34080 size truncated
    //     mid-SortBucketB, dropping ~16KB of bucket B and the work counters.
    {

        ///Disabled sHitsolver
        /*- sHitSolver__dispatchResolvedContacts at 0x140011EA0 invokes every resolver callback and then resets the active-node list.
- sHitSolver__resetActiveNodes at 0x140011320 clears the active array and resets the embedded node-pool cursor at +0x30060.
- New contacts are recreated through sHitSolver__addNode at 0x140010F20.
- Project snapshots occur before the next native tick in rollback_hooks.cpp:177 and before each replay tick in rollback_resim.cpp:225. Therefore snapshots are taken after the previous frame's hit nodes were consumed and reset.
- The current restore already discards restored hit work in rollback_restore.cpp:2159, making the bulk solver copy redundant.*/

    //    uint64_t sHitSolver = *(uint64_t*)_addr(PTR_SHITSOLVER);
    //    add((void*)sHitSolver, SHITSOLVER_SIZE);
    }

    // ---- 4. sAction — battle state machine (static manager) ----
    // Defeat replacement uses state outside the compact ranges restored below.
    // Removing this region caused peers to select the next point character on
    // different frames after a rollback crossed a character defeat.
    /*{
        uint64_t sAction = read.operator()<uint64_t>(_addr(PTR_SACTION));
        add((void*)sAction, 0x250, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sAction.full"
#endif
        );
    }*/

    // ---- 5. sGameUI — SKIPPED: has internal ptrs (ui elements, text buffers) ---
    // {
    //     uint64_t sGameUI = *(uint64_t*)_addr(PTR_SGAMEUI);
    //     add((void*)sGameUI, 0x6000);
    // }

    // ---- 6. sMain — physics, timing, frame pacing (needed for re-sim consistency) ----
    {
    //    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN);
     //   add((void*)sMain, SMAIN_SIZE);
    }

    // ---- 6a. sCollision — inline broadphase, move-matrix and geometry state ----
    // Restore has a dedicated path that retains cSystem synchronization,
    // mPrivateAllocator, and live pointers to externally owned allocations.
    //{
    /*    uint64_t sCollision = *(uint64_t*)_addr(PTR_SCOLLISION);
        collisionDiag.sCollision = sCollision;
        add((void*)sCollision, SCOLLISION_SIZE);
        if (sCollision) {
            // Ghidra's sCollision methods address these subsystems directly
            // from the complete singleton. The documented offsets already
            // include the 0x38-byte cSystem base.
            uint64_t sbc = sCollision + 0x43B8;
            uint64_t moveMatrix = sCollision + 0x7368;

            addSbcObjectBuffers(sbc);
            addMoveMatrixBuffers(moveMatrix);

            // cSbcArrayBP has five independent banks of 31 dynamic BVHs.
            // Their headers are inline in sCollision, but their node pools and
            // free-node arrays are allocated externally.
            for (uint32_t bank : {0x2B8u, 0xFB8u, 0x1CB8u, 0x29B8u, 0x36B8u}) {
                for (uint32_t index = 0; index < SCOLLISION_BVH_COUNT; ++index)
                    addDynamicBvhBuffers(sCollision + bank + index * 0x68, bank, index);
            }

            // cSbcObject owns two banks of 31 dynamic BVHs. The same helper
            // captures their node pools and free-node pointer arrays.
            for (uint32_t bank : {0xAA8u, 0x17A8u}) {
                for (uint32_t index = 0; index < SCOLLISION_BVH_COUNT; ++index)
                    addDynamicBvhBuffers(sbc + bank + index * 0x68, bank, index);
            }

            // Character collision attachment wrappers own dynamic BVHs and
            // NodeList-backed child state outside the singleton. Their child
            // graph is already cloned; capture the allocation's scalar image
            // as well when the live owner still points to it.
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
                                    // Wrapper scalar state is already covered
                                    // by the 0xE0 region. Its child arrays are
                                    // intentionally left to the deep clone.
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

    // ---- 7. sCamera — view matrices, 8-slot camera stack, cutin state ----
    //     3456 B total; skip vtable @+0 .. +7 on restore (handled via flat
    //     memcpy — the vtable pointer is the same at save and restore time
    //     for a singleton, so restoring it is safe here).
    uint64_t camUnit = 0;
    {
        uint64_t sCamera = read.operator()<uint64_t>(_addr(PTR_SCAMERA));
        if (sCamera) {
            add((void*)sCamera, 0xD80, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
                , "sCamera"
#endif
            );
            // First viewport slot at +0x50, stride 0x1A0, camera unit ptr at slot+8.
            // This is the uActionSideCamera gameplay anchor unit.
            camUnit = read.operator()<uint64_t>(sCamera + 0x58);
        }
    }

    // ---- 7a. uActionSideCamera — gameplay camera anchor/smoothing unit ----
    //     cUnit-derived object (DTI 0x140d44268, size 0x590) that computes
    //     camera anchor position, smoothing trajectory, and screen-edge
    //     clamps each frame.  During host-only resim its internal state
    //     drifts from the live client, causing permanent character position
    //     drift (desync) that never self-corrects.  sCamera viewport[0]+8
    //     holds the pointer (confirmed via sCamera__refreshViewsForBoundary).
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

    // ---- 8. sGameEffect — SKIPPED: individual effect nodes are captured ---
    //     via the sUnit category traversal (categories 13-16) below.  Flat-
    //     restoring sGameEffect would crash due to stale effect pointers.

    // ---- 10. sAction (fighting phase state machine) ----
    uint64_t sMvc3Mgr = read.operator()<uint64_t>(_addr(PTR_SMVC3MGR));
    if (sMvc3Mgr) {
        uint64_t sAction = read.operator()<uint64_t>(
            sMvc3Mgr + SMVC3MGR_ACTION_OFFSET);
        add((void*)sAction, 0x300, 0, 0, false
#if ROLLBACK_SNAPDUMP_ENABLED
            , "sAction.compact"
#endif
        );
        if (sAction && IsAccessibleAddress(
                sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET)) {
            uint64_t modelJointWrapper = read.operator()<uint64_t>(
                sAction + SACTION_MODEL_JOINT_WRAPPER_OFFSET);
            if (modelJointWrapper && IsAccessibleAddress(
                    modelJointWrapper + SACTION_MODEL_JOINT_ALLOCATION_OFFSET)) {
                uint64_t allocationSlot = modelJointWrapper +
                    SACTION_MODEL_JOINT_ALLOCATION_OFFSET;
                uint64_t allocation = read.operator()<uint64_t>(allocationSlot);
                if (allocation && IsAccessibleAddress(
                        allocation + SACTION_MODEL_JOINT_ALLOCATION_SIZE - 1)) {
                    // sAction__getModelJoint returns allocation+0x10. Capture
                    // the allocation for ownership validation; restore skips
                    // its header and applies only the scalar payload.
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

    // ---- 11-16. Per-character uCharacter + 17-18. cChrTeam ----
    uint64_t sCharacter = read.operator()<uint64_t>(_addr(PTR_SCHARACTER));
    if (sCharacter) {
        // Two 0x20-byte transition records drive tag/assist entry and eventually
        // populate cChrTeam's pending swap IDs. They sit outside cChrTeam, so
        // restoring only the team and character bodies leaves prediction-time
        // transition progress active after a rollback.
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
                   // HookLog("DIAG Character[team %d]: addr=0x%llX vtable=0x%llX dtiFunc=0x%llX dtiPtr=0x%llX rawSize=%u finalSize=%u\n"
                 //           "DIAG   anim[+0x1300]: %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X\n",
                 //           ti, character, vtable, dtiFunc, dtiPtr, (unsigned)charSize, (unsigned)finalSize,
                 //           animFields[0], animFields[1], animFields[2], animFields[3],
                //            animFields[4], animFields[5], animFields[6], animFields[7],
                 //           animFields[8], animFields[9], animFields[10], animFields[11],
                 //           animFields[12], animFields[13], animFields[14], animFields[15]);
                    add((void*)character, finalSize, 0, vtable, true
#if ROLLBACK_SNAPDUMP_ENABLED
                        , ti == 0 ? "uCharacter.team0" : "uCharacter.team1"
#endif
                    );

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

                    // Restore motion state and its external interpolation data
                    // together. Keeping one live while rewinding the other
                    // leaves animation/root-motion state split across frames.
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

                    // Motion+0x178 fields are slices into one allocation owned
                    // by uModel+0x6D8, not independent allocations. Rewind the
                    // complete allocation through its actual owner slot.
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

                    // These heap buffers are outputs and working state of the
                    // same motion evaluation pass. Leaving them ahead while
                    // rewinding Motion/channel state splits root and joint
                    // transforms across timelines.
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

                    // Character collision chains can use a uChrWeapon as their
                    // parent model. Capture each weapon's motion and joint
                    // outputs so rollback does not build sword hitboxes from
                    // prediction-timeline weapon transforms.
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

                    // ---- cChrCmd line buffers (heap-allocated per-command match state) ----
                    // Two embedded cChrCmd objects at uCharacter+0x2738 and +0x2950.
                    // Each has a heap-allocated line array at cmd+0x178 of n×0x20 B entries
                    // (where n = cmd+0x170). The eight bytes before the payload
                    // are allocator metadata and remain owned by the live allocation.
                    // Without this, double-tap dash / back-dash / charge / combo command
                    // matching progress is lost on snapshot restore → resim eats different
                    // action transitions.  Verified via Ghidra cross-refs from cChrCmd__setOwner.
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

        // Spawned helper uCharacters live in sUnit category 8 but are not in
        // either team roster. Capture their DTI-sized bodies at saved addresses;
        // blend and external animation buffers remain team-fighter-only.
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
                    read.operator()<uint64_t>(helper);
                    uint32_t helperSize = DecodeDtiSize(GetObjectDti(helper));
                    if (helperSize >= 8 && helperSize <= CCHR_MAX_SIZE) {
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

        // Do not snapshot uCharacterPost's camera state. It is an unowned
        // dynamic object and has no owner slot in the snapshot. Restoring its
        // saved address after native list teardown can write into a replaced
        // object and corrupt the model/joint graph.

        // uCharacterPost is captured separately in the snapshot slot. Its
        // scalar payload is copied into the current category-18 object and its
        // two character references are remapped by roster member index.
    }

    // ---- 19-22. sUnit effect categories — SKIPPED (heap corruption) ---
    //     Walking effect categories 13-16 and restoring individual nodes at
    //     their heap addresses corrupts the pool allocator's free-list metadata
    //     when nodes were freed between save and restore (MtHeapAllocator crash).
    //     Instead, we zero the sGameEffect slot tables post-restore to allow
    //     fresh effect allocations without conflicting with old slots.

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
    const uint64_t profileStart = Profile_Now();
    int slotIdx = frame % ROLLBACK_WINDOW;
    if (slotIdx < 0) slotIdx += ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    int count = Rollback_BuildRegions(slot.regions, MAX_REGIONS);
    if (count == 0) return;
    const uint64_t buildRegionsEnd = Profile_Now();

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

    if (slot.capacity < total) {
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

    // Copy data AND save per-region metadata for safe restore
    size_t offset = 0;
    slot.regionCount = count;
    slot.frame = frame;
    for (int i = 0; i < count; ++i) {
        const Region& region = slot.regions[i];
        std::memcpy(slot.data + offset, region.ptr, region.size);
        offset += region.size;
    }
    slot.size = offset;
    CaptureActionSideCameraState(slot);
    CaptureCharacterPostState(slot);
    const uint64_t copyRegionsEnd = Profile_Now();

    // Record the exact state captured at the rollback boundary. RNG hooks only
    // observe advances; this also exposes direct writes and snapshot timing.
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

    // Native clones keep reference-managed geometry alive after attack-end
    // clears the live node's child array.
    Rollback_CaptureCollisionNodeClones(slot);
    CaptureCharacterCollisionParentReferences(slot);
    const uint64_t collisionClonesEnd = Profile_Now();

    // Cut-ins independently change character visibility, model draw routing,
    // and material mode. The raw character restore deliberately preserves some
    // live model ownership state, so keep the small render contract explicitly.
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
    Shot_CaptureRecipes(slot);
    const uint64_t shotRecipesCapturedEnd = Profile_Now();
    Rollback_CaptureCharacterShotReferences(slot);
    const uint64_t characterShotReferencesEnd = Profile_Now();
    Effect_CaptureRecipes(slot);
    const uint64_t shotRecipesEnd = Profile_Now();

    if constexpr (ROLLBACK_PROFILING_ENABLED) {
        if (g_activeRollbackProfile)
            g_activeRollbackProfile->snapshotShotRecipeCount +=
                (uint32_t)slot.shotRecipes.size();
        if (g_activeRollbackProfile) {
            g_activeRollbackProfile->saveCharacterShotReferences +=
                characterShotReferencesEnd - shotRecipesCapturedEnd;
            g_activeRollbackProfile->snapshotCharacterShotReferenceCount +=
                (uint32_t)slot.characterShotReferences.size();
        }
    }

    Profile_RecordSnapshot(profileStart, buildRegionsEnd, allocationEnd, copyRegionsEnd,
                           snapshotMetadataEnd, collisionClonesEnd, shotRecipesEnd,
                           offset, (uint32_t)count);

    // [EXPERIMENTAL] QPC capture — see mvc3/rollback W3-C2
    // LARGE_INTEGER qpc_cap;
    // if (QueryPerformanceCounter(&qpc_cap)) {
    //     g_qpc_history[slotIdx] = qpc_cap.QuadPart;
    // }
    
    // Save the local player's input for this frame in the snapshot slot.
    // At this point g_oldInputs[MAX_HISTORY - g_delayFrames] holds the
    // delayed input that the imminent game tick will use.
    slot.localInput = Controller_GetCurrentFrameInput();

    // Controller_OnFrameStart resolves and republishes the exact input before
    // the tick. Initialize with the prior prediction rather than stale received
    // ring data in case this frame is not confirmed yet.
    slot.remoteInput = Controller_GetUsedRemoteInput(frame);
    /*if (frame >= 14 && frame <= 20) {
        HookLog("SNAPTRACE phase=end frame=%d resim=%d local=0x%08X remote=0x%08X bytes=%zu\n",
                frame, g_isResimulating ? 1 : 0, slot.localInput,
                slot.remoteInput, slot.size);
    }*/
#if ROLLBACK_SNAPDUMP_ENABLED
    SnapDump_CaptureSnapshot(slot);
#endif
    
    //HookLog("Rollback: saved frame %d with %d regions, total size %zu bytes\n", frame, count, offset);
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
    // Make sure patched calls are restored before tearing down
    Rollback_PatchSleep(false);
    Rollback_PatchPresent(false);
    Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false);
    Rollback_PatchDrawCalls(false); // restore draw calls (no-op if unpatched)

    for (int i = 0; i < ROLLBACK_WINDOW; ++i) {
        Rollback_DestroyCollisionNodeClones(g_slots[i]);
        ReleaseActionSideCameraState(g_slots[i]);
        delete[] g_slots[i].data;
        g_slots[i].data        = nullptr;
        g_slots[i].size        = 0;
        g_slots[i].capacity    = 0;
        g_slots[i].regionCount = 0;
        g_slots[i].frame       = -1;
        g_slots[i].shotRecipes.clear();
        g_slots[i].effectRecipes.clear();
        g_slots[i].characterEffectStates.clear();
        g_slots[i].characterEffectReferences.clear();
        g_slots[i].characterCollisionParentReferences.clear();
        g_slots[i].modelRenderStates.clear();
    }
    Shot_Reset();
    Effect_Reset();
    Rollback_ResetReSimulation();
    g_regionCache.valid = false;
    // Keep snapdump history independent of the live rollback slots. New captures
    // replace entries modulo its own 255-frame ring instead of losing pre-reset
    // diagnostic context when the live snapshot storage is recreated.
}
