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
        uint8_t active = 0;
        uint32_t kind = 0;
        uint8_t geometry[0x24] = {};
        uint32_t flags = 0;
        uint8_t parentState[0x0C] = {};
        uint8_t localGeometry[0x20] = {};
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

// Restore detected scalar bytes from the concrete character tail, then reapply
// Ghidra-verified pointer-free ranges that need explicit overrides.
void ConcreteCharacterSnapshot_RestoreFields(void* character,
                                             const uint8_t* savedBody,
                                             size_t bodySize,
                                             uint64_t dti);
