#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Full bodies are still captured. This policy never discovers ownership from
// the bit pattern of a saved or live word. Unlisted bytes in a reviewed layout
// remain live. UnsupportedLayout and InvalidBuffer never mutate destination.
enum class CharacterRestoreStatus {
    InvalidBuffer,
    UnsupportedLayout,
    ReviewedSubset, // NOT a claim of complete deterministic layout coverage
};

constexpr size_t ROLLBACK_CHARACTER_COMMON_SIZE = 0x68F0;

// The one list of plain-byte fields in the model prefix (+0..+1450) that
// characters, their auxiliary actors and weapons, shots and shot-driven effect
// models share. Each row names the owners that rewind it. A row an owner lacks
// is a pointer it resolves another way, a field its scheduler stage owns, or
// state that owner leaves live; the comments say which.
enum ActorOwner : uint8_t {
    ActorCharacter = 1 << 0, // uCharacter helper that is not on a team roster.
    ActorRoster    = 1 << 1, // Roster uCharacter.
    ActorAuxiliary = 1 << 2, // Tron and Hsien-Ko auxiliary actors.
    ActorWeapon    = 1 << 3, // uChrWeapon: uModel prefix only, no actor block.
    ActorShot      = 1 << 4, // uShot and shot-driven uGameEffectModel.
};
struct ActorFieldRow { uint16_t offset, size; uint8_t owners; };
namespace rollback_actor_detail {
constexpr uint8_t Chr = ActorCharacter | ActorRoster | ActorAuxiliary; // uActorGameModel on the character side.
constexpr uint8_t Act = Chr | ActorShot;                                // Every uActorGameModel.
constexpr uint8_t Mdl = Act | ActorWeapon;                              // Every uModel.
constexpr uint8_t Leg = Chr | ActorWeapon;                              // Not copied for shots (see Motion rows).
}
inline constexpr ActorFieldRow ActorFieldRows[] = {
    {0x0008, 0x08, rollback_actor_detail::Act}, // cUnit mRno, mUnitParam. A weapon's lifecycle stays live.
    {0x0010, 0x04, ActorRoster | ActorAuxiliary}, // Unit flags. Helpers keep lifecycle live; shots: scheduler stage.
    {0x0028, 0x04, rollback_actor_detail::Act}, // Unit delta time.
    {0x002C, 0x0C, rollback_actor_detail::Chr}, // Through the group mask at +30. Shots: scheduler stage.
    // +18/+20/+38 list links stay live.
    {0x0040, 0x08, ActorShot},                  // Coordinate parent. Leased for shots, live for the rest.
    {0x0048, 0xB8, rollback_actor_detail::Mdl}, // uCoord selectors, vectors and matrices.
    // uBaseModel +100..510 is live render and resource ownership, except:
    {0x011C, 0x04, ActorAuxiliary},             // Draw layer mask (characters and weapons: render-state pass).
    {0x0180, 0x04, ActorAuxiliary},             // Transparency (likewise).
    {0x0510, 0x20, ActorWeapon},                // uModel flags and motion evaluation state.
    {0x0558, 0x04, rollback_actor_detail::Leg}, // Motion calculation flags. Shots: storage stage, with the counts.
    {0x055C, 0x04, rollback_actor_detail::Mdl}, // Motion interpolation attributes.
    // +560..+1160: eight Motion records, ActorMotionRows below.
    {0x1160, 0x18, ActorWeapon},                // uModel motion/envelope state before its live owner pointer.
    {0x1180, 0x30, rollback_actor_detail::Mdl}, // Null scale, root-motion delta position and quaternion.
    {0x1240, 0x10, rollback_actor_detail::Act}, // Morph weights, not the shader owners at +1250.
    {0x1260, 0x3F, rollback_actor_detail::Act}, // Plane, waist/foot/IK scalar parameters.
    // cToonParam writer 140451410 / shader consumer 140451210, excluding the
    // vtable and the unknown +12E4..12EF gap.
    {0x12C8, 0x1C, rollback_actor_detail::Chr}, // Rim-light height limits, color factor, power.
    {0x12F0, 0x10, rollback_actor_detail::Chr}, // Cut-in factor.
    {0x1300, 0x08, rollback_actor_detail::Act}, // Delta time coefficients.
    {0x1308, 0x08, ActorShot},                  // mpOwner. Leased for shots, live for characters.
    {0x1310, 0x34, rollback_actor_detail::Act}, // Animation requests, time/rate/wait/serial.
    // cActorGameModelAnm: vtable +1348 and owner +1380 are never copied.
    {0x1350, 0x10, rollback_actor_detail::Act}, // Animation numbers and execution flags.
    {0x1360, 0x18, ActorShot},                  // Resource mirrors: retained for shots, permanent for characters.
    // Throw setup (140040754 -> 140042460) borrows the thrower's script in slot
    // 3 without refcount changes: a mutable binding, rewound including null.
    {0x1378, 0x08, rollback_actor_detail::Act},
    // Type/key and resource cursors. Characters copy these only while their
    // permanent resources are unchanged (Rollback_RestoreActorFields).
    {0x1388, 0x30, ActorShot},
    // +13B8..13BF remains unresolved.
    {0x13C0, 0x01, rollback_actor_detail::Act}, // mCngKey.
    {0x13C4, 0x10, rollback_actor_detail::Act}, // Compare registers, jump count, return flags.
    {0x13D8, 0x08, ActorShot},                  // Command return unit. Leased for shots; characters resolve by identity.
    {0x13E0, 0x08, rollback_actor_detail::Act}, // Loop count and start frame.
};
// One uModel::Motion record (+560 + n * 180). +000 vtable, +160 mJoint and +178
// mpKeyFrameData stay live. The rows shots omit are alignment gaps and bytes
// whose meaning is unknown (doc/uCharacter_model_member_audit.md); the
// character side has always copied them.
inline constexpr ActorFieldRow ActorMotionRows[] = {
    {0x008, 0x06, rollback_actor_detail::Mdl}, // Playback state.
    {0x00E, 0x02, rollback_actor_detail::Leg},
    {0x010, 0x64, rollback_actor_detail::Mdl}, // Playback and root-motion history.
    {0x074, 0x0C, rollback_actor_detail::Leg},
    {0x080, 0x60, rollback_actor_detail::Mdl},
    // initVectorTrackSampler 14079F7E0 / sample 14079F5D0: two samplers that
    // own no storage but borrow track +00 and segment +10 from rMotionList.
    {0x0E0, 0x0C, rollback_actor_detail::Mdl}, {0x0EC, 0x04, rollback_actor_detail::Leg},
    {0x0F0, 0x0C, rollback_actor_detail::Mdl}, {0x0FC, 0x04, rollback_actor_detail::Leg},
    {0x100, 0x0C, rollback_actor_detail::Mdl}, {0x10C, 0x04, rollback_actor_detail::Leg},
    {0x110, 0x0C, rollback_actor_detail::Mdl}, {0x11C, 0x04, rollback_actor_detail::Leg},
    {0x120, 0x40, rollback_actor_detail::Mdl}, // Four event bindings and sequence history.
    // setupMotionParam 14052A880 binds count and descriptor together;
    // calcKeyFrame 1405260B0 consumes both with the retained work slice.
    {0x168, 0x04, rollback_actor_detail::Mdl}, // Key count.
    {0x16C, 0x04, rollback_actor_detail::Leg},
    {0x170, 0x08, rollback_actor_detail::Mdl}, // Borrowed mpKeyFrameInfo.
};

// Caller guarantees valid bodies of at least +1450 (+1300 for a weapon),
// retained resources and compatible work allocations. No native calls.
inline void Rollback_RestoreActorFields(void* destination, const uint8_t* saved, ActorOwner owner)
{
    auto* live = static_cast<uint8_t*>(destination);
    for (const ActorFieldRow& row : ActorFieldRows)
        if (row.owners & owner) std::memcpy(live + row.offset, saved + row.offset, row.size);
    for (size_t motion = 0x560; motion < 0x1160; motion += 0x180)
        for (const ActorFieldRow& row : ActorMotionRows)
            if (row.owners & owner) std::memcpy(live + motion + row.offset, saved + motion + row.offset, row.size);
    if (owner & rollback_actor_detail::Chr) {
        bool sameResources = true;
        for (size_t offset = 0x1360; offset < 0x1378; offset += 8)
            sameResources &= !std::memcmp(live + offset, saved + offset, 8);
        if (sameResources) std::memcpy(live + 0x1388, saved + 0x1388, 0x30);
    }
}

// Exact weak-reference slots. Consumers resolve identities, never memcpy them.
std::span<const uint32_t> Rollback_CharacterShotTailOffsets(uint64_t dti);
std::span<const uint32_t> Rollback_CharacterHelperTailOffsets(uint64_t dti);
std::span<const uint32_t> Rollback_CharacterEffectTailOffsets(uint64_t dti);
CharacterRestoreStatus Rollback_RestoreAuxiliaryActor(
    void* destination, size_t destinationSize, const uint8_t* saved,
    size_t savedSize, uint64_t dti);

// Caller validates allocation/vtable identity and passes the actual destination
// capacity, independently of the available snapshot bytes. Pass the runtime DTI;
// policy constants use the same fixed-address convention as mk10utils.h::_addr.
CharacterRestoreStatus Rollback_RestoreCharacterFields(
    void* destination, size_t destinationSize, const uint8_t* saved,
    size_t savedSize, uint64_t dti, bool roster);
