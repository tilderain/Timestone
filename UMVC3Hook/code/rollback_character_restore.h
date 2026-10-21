#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Full bodies are still captured. This policy never discovers ownership from
// the bit pattern of a saved or live word. Unlisted bytes in a reviewed layout
// remain live. UnsupportedLayout and InvalidBuffer never mutate destination.
enum class CharacterRestoreStatus {
    InvalidBuffer,
    UnsupportedLayout,
    ReviewedSubset, // NOT a claim of complete deterministic layout coverage
};

enum class CharacterRestorePhase { Early, Late };

constexpr size_t ROLLBACK_CHARACTER_COMMON_SIZE = 0x68F0;

// Shared by fighter and weapon integration. Caller guarantees valid model
// bodies, resource lifetimes and compatible work allocations. No validation.
// External work bytes are restored separately; allocation pointers stay live.
void Rollback_RestoreModelMotions(void* destination, const uint8_t* saved);

// Exact weak-reference slots. Consumers resolve identities, never memcpy them.
std::span<const uint32_t> Rollback_CharacterShotTailOffsets(uint64_t dti);
std::span<const uint32_t> Rollback_CharacterHelperTailOffsets(uint64_t dti);
std::span<const uint32_t> Rollback_CharacterEffectTailOffsets(uint64_t dti);
CharacterRestoreStatus Rollback_RestoreAuxiliaryActor(
    void* destination, size_t destinationSize, const uint8_t* saved,
    size_t savedSize, uint64_t dti,
    CharacterRestorePhase phase = CharacterRestorePhase::Early);

// Caller validates allocation/vtable identity and passes the actual destination
// capacity, independently of the available snapshot bytes. Pass the runtime DTI;
// policy constants use the same fixed-address convention as mk10utils.h::_addr.
CharacterRestoreStatus Rollback_RestoreCharacterFields(
    void* destination, size_t destinationSize, const uint8_t* saved,
    size_t savedSize, uint64_t dti, bool roster,
    CharacterRestorePhase phase);
