#pragma once

#include "rollback.h"
#include "rollback_snapdump.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>

struct LifetimeSnapshot;

static constexpr int ROLLBACK_MAX_REGIONS = 2048;

struct RollbackSnapshotRange {
    uint32_t offset;
    uint32_t size;
};

struct RollbackRegion {
    void* ptr;
    size_t size;
    uint64_t ownerSlot;
    uint64_t expectedVtable;
    bool skipVtable;
    bool commandLine;
    int dependentRegion = -1; // auxiliary work buffer: validate its actor before dereferencing ownerSlot
#if ROLLBACK_SNAPDUMP_ENABLED
    const char* snapDumpName;
#endif
};

#if ROLLBACK_SNAPDUMP_ENABLED
static_assert(offsetof(RollbackRegion, snapDumpName) >
              offsetof(RollbackRegion, skipVtable));
static_assert(sizeof(RollbackRegion) <= 64);
#endif

enum class RollbackCharacterCollisionParentKind : uint8_t {
    None,
    Character,
    Weapon,
};

struct RollbackCharacterCollisionParentReference {
    void* character = nullptr;
    uint64_t expectedCharacterVtable = 0;
    uint32_t offset = 0;
    uint16_t weaponIndex = 0;
    RollbackCharacterCollisionParentKind kind =
        RollbackCharacterCollisionParentKind::None;
};

struct RollbackCollisionNodeClone {
    uint64_t ownerSlot;
    void* savedPtr;
    void* clone;
    uint64_t wrapperDti;
    uint32_t childCount;
    uint8_t scalarState[0xE0];
    std::vector<uint64_t> childDtis;
};

struct RollbackModelRenderState {
    void* model;
    uint64_t expectedVtable;
    uint32_t objectFlags;
    uint32_t drawLayerMask;
    float transparency;
    bool character;
};

struct RollbackActionSideCameraState {
    void* motionResource = nullptr;
    void* motionParent = nullptr;
    void* requestResource = nullptr;
    void* requestParent = nullptr;
};

struct RollbackCharacterPostState {
    // Gameplay-relevant inherited cUnit state. The remaining cUnit fields are
    // scheduler/list ownership, visual flags, or per-tick derived delta time.
    int32_t rno = 0;
    int32_t unitParam = 0;
    bool moveEnabled = false;
    uint64_t unitGroup = 0;
    // Complete class-specific scalar payload at uCharacterPost+0x40..+0x4f.
    // The two following pointers are remapped by roster index below.
    uint8_t scalarState[0x10] = {};
    uint8_t cachedCharacterMembers[2] = {UINT8_MAX, UINT8_MAX};
    bool valid = false;
};

struct RollbackCharacterMotionResource {
    int regionIndex;
    void* resource; // retained mpMotionList[2], including saved null
};

struct RollbackSnapshotSlot {
    int32_t frame = -1;
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    bool ownsData = true;
    RollbackRegion regions[ROLLBACK_MAX_REGIONS];
    int regionCount = 0;
    uint32_t localInput = 0;
    uint32_t remoteInput = 0;
    std::vector<RollbackCharacterCollisionParentReference>
        characterCollisionParentReferences;
    std::vector<RollbackCollisionNodeClone> collisionNodeClones;
    std::vector<RollbackModelRenderState> modelRenderStates;
    RollbackActionSideCameraState actionSideCamera;
    RollbackCharacterPostState characterPost;
    std::vector<RollbackCharacterMotionResource> characterMotionResources;
    std::shared_ptr<LifetimeSnapshot> lifetime;
};

// Encode the slot ABI into a required linker symbol. Translation units built
// against a different slot stride or input offset cannot link together.
template <size_t SlotSize, size_t LocalInputOffset, size_t RemoteInputOffset>
struct RollbackSnapshotLayoutGuard {
    static void Require();
};

using CurrentRollbackSnapshotLayoutGuard = RollbackSnapshotLayoutGuard<
    sizeof(RollbackSnapshotSlot),
    offsetof(RollbackSnapshotSlot, localInput),
    offsetof(RollbackSnapshotSlot, remoteInput)>;

inline void Rollback_RequireCurrentSnapshotLayout()
{
    CurrentRollbackSnapshotLayoutGuard::Require();
}

extern RollbackSnapshotSlot g_rollbackSnapshotSlots[ROLLBACK_WINDOW];

int Rollback_BuildRegions(RollbackRegion* regions, int maxRegions);
