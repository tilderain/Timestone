#pragma once

#include "concrete_snapshot.h"
#include "rollback.h"

#include <cstddef>
#include <cstdint>
#include <vector>

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
};

struct RollbackShotFlightState {
    uint32_t unitState;
    uint8_t timeScaleState[0x08];
    uint8_t transformState[0xB0];
    uint8_t runtimeState[0x50];
    uint8_t shotMatrices[0x100];
    float timerThresh;
    float timer;
    float delayTime;
    float autoAddDivStartTime;
    uint8_t resourceScalars[0x90];
    uint8_t attackScalars[0x20];
    uint8_t movementScalars[0x60];
    uint8_t animationScalars[0x14];
    uint32_t step;
    uint32_t randomProgress;
    uint32_t randomSelection;
    uint32_t randomFlags;
    uint32_t randomState;
    bool hasZireitouState;
    uint32_t zireitouState[4];
};

enum class RollbackShotSpawnKind : uint8_t { Standard, Extended };

struct RollbackShotRecipe {
    void* savedPtr;
    void* restoredPtr;
    void* shotElement;
    uint64_t initParam;
    void* posData;
    void* ownerChr;
    void* objectRef1;
    uint64_t objectRef1Dti;
    bool objectRef1WasActiveUnit;
    uint32_t slotId;
    void* objectRef2;
    uint64_t objectRef2Dti;
    bool objectRef2WasActiveUnit;
    RollbackShotSpawnKind kind;
    int exA9;
    void* exA10;
    int exA11;
    void* exA12;
    RollbackShotFlightState flight;
    ConcreteShotSnapshot concrete;
    uint64_t spawnSequence = 0;
    uint8_t preSpawnGlobalRng[16] = {};
    uint8_t preSpawnEffectRng[16] = {};
    uint8_t preSpawnOwnerRng[16] = {};
    bool hasPreSpawnRng = false;
    bool reconstructed = false;
};

struct RollbackEffectRecipe {
    void* savedPtr;
    void* restoredPtr;
    void* sGameEffect;
    int effectId;
    int a3;
    void* attachTo;
    int a5;
    int a6;
    void* a7;
    void* a8;
    int a9;
    void* a10;
    int a11;
    uint32_t a12;
    void* a13;
    uint64_t spawnSequence = 0;
};

struct RollbackCollisionNodeClone {
    uint64_t ownerSlot;
    void* savedPtr;
    void* clone;
};

struct RollbackSnapshotSlot {
    int32_t frame = -1;
    uint8_t* data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    RollbackRegion regions[ROLLBACK_MAX_REGIONS];
    int regionCount = 0;
    uint32_t localInput = 0;
    uint32_t remoteInput = 0;
    std::vector<RollbackShotRecipe> shotRecipes;
    std::vector<RollbackEffectRecipe> effectRecipes;
    std::vector<RollbackCollisionNodeClone> collisionNodeClones;
};

extern RollbackSnapshotSlot g_rollbackSnapshotSlots[ROLLBACK_WINDOW];

int Rollback_BuildRegions(RollbackRegion* regions, int maxRegions);
