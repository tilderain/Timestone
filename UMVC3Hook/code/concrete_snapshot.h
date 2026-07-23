#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

// A concrete uShot body captured using the object's runtime DTI size. The
// snapshotter deliberately restores only subclass storage; the common uShot
// body is still restored by ShotFlightState and the native factory keeps its
// ownership/topology pointers valid.
struct ConcreteShotSnapshot {
    void* savedObject = nullptr;
    uint64_t dti = 0;
    uint64_t vtable = 0;
    std::vector<uint8_t> body;
};

bool ConcreteShotSnapshot_Capture(void* shot, ConcreteShotSnapshot& out);
void ConcreteShotSnapshot_Restore(void* shot, const ConcreteShotSnapshot& snapshot);

// Reapply Ghidra-verified pointer-free subclass fields after rollback.cpp's
// conservative character pointer-preservation pass.
void ConcreteCharacterSnapshot_RestoreFields(void* character,
                                             const uint8_t* savedBody,
                                             size_t bodySize,
                                             uint64_t dti);
