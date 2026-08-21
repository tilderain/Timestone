#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// A concrete uShot body captured using the object's runtime DTI size. The
// snapshotter deliberately restores only subclass storage; the common uShot
// body is still restored by ShotFlightState and the native factory keeps its
// ownership/topology pointers valid.
struct ConcreteShotSnapshot {
    struct CallbackEffectState {
        struct GeneratorState {
            uint64_t dti = 0;
            uint64_t resource = 0;
            uint8_t state20[0x08] = {};
            uint8_t state30[0x08] = {};
            uint8_t state70[0x14] = {};
            uint8_t stateB8[0x10] = {};
            uint8_t state110[0x2C] = {};
            uint8_t state19C[0x24] = {};
            uint8_t state1F0[0x10] = {};
            uint8_t state238 = 0;
        };

        bool occupied = false;
        bool hasState = false;
        bool hasGeneratorState = false;
        uint64_t dti = 0;
        uint64_t resource = 0;
        uint8_t flags100[0x08] = {};
        uint8_t timing110[0x28] = {};
        uint8_t state1D0[0x08] = {};
        uint8_t state1E0[0x10] = {};
        uint8_t state1F8[0x0C] = {};
        uint8_t state230[0x12] = {};
        std::vector<GeneratorState> generators;
    };

    struct CollisionPrimitive {
        uint32_t slot = 0;
        uint32_t index = 0;
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
    CallbackEffectState callbackEffects[13];
};

bool ConcreteShotSnapshot_Capture(void* shot, ConcreteShotSnapshot& out);
bool ConcreteShotSnapshot_Restore(void* shot, const ConcreteShotSnapshot& snapshot,
                                   const void* const* validTargets,
                                   size_t validTargetCount);
void ConcreteShotSnapshot_RestoreCallbackEffects(
    void* shot, const ConcreteShotSnapshot& snapshot);
bool ConcreteEffectSnapshot_Capture(
    void* effect, ConcreteShotSnapshot::CallbackEffectState& state);
bool ConcreteEffectSnapshot_Restore(
    void* effect, const ConcreteShotSnapshot::CallbackEffectState& state);
// Reapply reconstructed capsule sources after the native shot hitbox update.
// Reconstructed parent transforms can collapse data-derived world endpoints,
// so this restores them directly at the outer update boundary.
void ConcreteShotSnapshot_ReapplyCapsules(void* shot,
                                           const ConcreteShotSnapshot& snapshot);
void ConcreteShotSnapshot_RebuildLiveCapsuleWorld(
    void* shot, const ConcreteShotSnapshot& snapshot);

// Restore detected scalar spans from concrete character tails while retaining
// live pointer/embedded-object ownership records, then apply verified overrides.
void ConcreteCharacterSnapshot_RestoreFields(void* character,
                                             const uint8_t* savedBody,
                                             size_t bodySize,
                                             uint64_t dti);
