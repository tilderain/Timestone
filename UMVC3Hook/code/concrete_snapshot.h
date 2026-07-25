#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// A concrete uShot body captured using the object's runtime DTI size. The
// snapshotter deliberately restores only subclass storage; the common uShot
// body is still restored by ShotFlightState and the native factory keeps its
// ownership/topology pointers valid.
struct ConcreteShotSnapshot {
    struct CollisionPrimitive {
        uint32_t slot = 0;
        uint32_t index = 0;
        uint64_t metadata = 0;
        uint32_t metadataType = 0;
        uint8_t geometryType = 0;
        uint32_t collisionKind = 0;
        uint8_t geometry[0x24] = {};
        uint32_t flags = 0;
        uint64_t parent0 = 0;
        uint64_t parent1 = 0;
        int32_t parent0ShotOffset = -1;
        int32_t parent1ShotOffset = -1;
        uint8_t parentState[0x0C] = {};
        uint8_t localGeometry[0x20] = {};
        uint8_t sourceGeometry[0x20] = {};
        bool hasSourceGeometry = false;
    };

    void* savedObject = nullptr;
    uint64_t dti = 0;
    uint64_t vtable = 0;
    std::vector<uint8_t> body;
    std::vector<CollisionPrimitive> collisionPrimitives;
};

bool ConcreteShotSnapshot_Capture(void* shot, ConcreteShotSnapshot& out);
bool ConcreteShotSnapshot_Restore(void* shot, const ConcreteShotSnapshot& snapshot,
                                  const void* const* validTargets,
                                  size_t validTargetCount);
void ConcreteShotSnapshot_DispatchCapsule(
    void* shot, const ConcreteShotSnapshot& snapshot, void* metadata,
    void* primitive, void* cameraOrigin,
    void (__fastcall* dispatchCapsule)(void*, void*, void*, void*));

// Reapply reconstructed capsule sources after the native shot hitbox update.
// The native update can overwrite the dispatcher result later in the same
// tick, so this runs at the outer update boundary.
void ConcreteShotSnapshot_ReapplyCapsules(
    void* shot, const ConcreteShotSnapshot& snapshot,
    void* cameraOrigin,
    void (__fastcall* dispatchCapsule)(void*, void*, void*, void*));

// Restore detected scalar spans from concrete character tails while retaining
// live pointer/embedded-object ownership records, then apply verified overrides.
void ConcreteCharacterSnapshot_RestoreFields(void* character,
                                             const uint8_t* savedBody,
                                             size_t bodySize,
                                             uint64_t dti);
