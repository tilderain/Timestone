#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>
#include "effect_generator_state.h"

// A concrete uShot body captured using the object's runtime DTI size. The
// snapshotter deliberately restores only subclass storage; the common uShot
// body is still restored by ShotFlightState and the native factory keeps its
// ownership/topology pointers valid.
struct ConcreteShotSnapshot {
    struct CallbackEffectState {
        using GeneratorEmissionState = EffectGenerator::State;

        bool occupied = false;
        void* savedEffect = nullptr;
        uint8_t unitState = 0;
        bool emissionFinished = false;
        bool hasState = false;
        bool hasGeneratorEmissionState = false;
        uint64_t dti = 0;
        uint64_t resource = 0;
        uint8_t flags100[0x08] = {};
        uint8_t timing110[0x28] = {};
        uint8_t state1D0[0x08] = {};
        uint8_t state1E0[0x10] = {};
        uint8_t state1F8[0x10] = {};
        uint8_t state230[0x12] = {};
        std::vector<GeneratorEmissionState> generatorEmissionStates;

        void ResetForCapture() {
            auto storage = std::move(generatorEmissionStates);
            *this = {};
            generatorEmissionStates = std::move(storage);
            generatorEmissionStates.clear();
        }

        bool CompletedIndependent() const {
            // Native isAlive accepts states 1/2. Finished emission can still
            // have live particles/children; only captured inactivity proves
            // that the independent effect should not be recreated.
            return hasState && unitState != 1 && unitState != 2;
        }
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

    void ResetForCapture() {
        savedObject = nullptr;
        dti = vtable = 0;
        body.clear();
        collisionPrimitives.clear();
        for (auto& callback : callbackEffects)
            callback.ResetForCapture();
    }
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
    void* effect, const ConcreteShotSnapshot::CallbackEffectState& state,
    bool reconstructed = true);
// Reapply reconstructed capsule sources after the native shot hitbox update.
// Reconstructed parent transforms can collapse data-derived world endpoints,
// so this restores them directly at the outer update boundary.
void ConcreteShotSnapshot_ReapplyCapsules(void* shot,
                                           const ConcreteShotSnapshot& snapshot);
void ConcreteShotSnapshot_RebuildLiveCapsuleWorld(
    void* shot, const ConcreteShotSnapshot& snapshot);
