#include "../pch.h"

#include "concrete_snapshot.h"
#include "mk10utils.h"
#include "rollback_memory.h"
#include "rollback_profile.h"

#include <cmath>
#include <cstring>

namespace {

constexpr uintptr_t kMinGameAddress = 0x140000000ull;
constexpr size_t kShotBodyStart = 0x2140;
constexpr size_t kShotRestoreStart = 0x50;
constexpr size_t kCharacterConcreteTailStart = 0x68F0;
constexpr size_t kMaxObjectSize = 0x40000;
constexpr size_t kMaxEmbeddedObjectSize = 0x1000;
constexpr size_t kShotCollisionSlotsOffset = 0x2058;
constexpr uint32_t kShotCollisionSlotCount = 4;
constexpr uint32_t kMaxCollisionPrimitives = 128;
constexpr uint32_t kAttackTargetTableCount = 3;
constexpr uint32_t kAttackTargetCount = 32;
constexpr size_t kAttackTargetStride = 0x18;
constexpr size_t kShotCallbackEffectsOffset = 0x1F78;
constexpr uint32_t kShotCallbackEffectCount = 13;
constexpr uint32_t kMaxCallbackGenerators = 64;
constexpr size_t kAttackTargetTables[kAttackTargetTableCount] = {
    // uShot embeds cAtkCtrl at +0x1470. Its three 0x300-byte TargetInfo
    // tables begin at cAtkCtrl +0x50/+0x350/+0x650.
    0x14C0, 0x17C0, 0x1AC0
};

// These DTI addresses were verified from the class DTI getters in Ghidra.
// They are used only to select the exceptional subclass policy; the object
// size itself always comes from the live object's DTI metadata.
constexpr uintptr_t kSoulFistDti = 0x140D4B6E8ull;
constexpr uintptr_t kKingKobunDti = 0x140D4D2B0ull;
constexpr uintptr_t kMissileEmplacementDti = 0x140D49A60ull;
constexpr uintptr_t kVulcanAxeDti = 0x140D48808ull;
constexpr uintptr_t kRyuDti = 0x140D45A10ull;
constexpr uintptr_t kHsienKoDti = 0x140D455F8ull;
constexpr uintptr_t kSpencerDti = 0x140D45D40ull;
constexpr uintptr_t kAttackWireDti = 0x140D4CA20ull;

struct SnapshotRange {
    size_t offset;
    size_t size;
};

struct DtiSnapshotRange {
    uintptr_t dti;
    size_t offset;
    size_t size;
};

constexpr DtiSnapshotRange kGeneratedCharacterTailRanges[] = {
#include "generated_character_tail_fields.inc"
};

constexpr DtiSnapshotRange kGeneratedShotTailRanges[] = {
#include "generated_shot_tail_fields.inc"
};

using GetPrimitiveFn = uint64_t(__fastcall*)(uint64_t, uint32_t);
using GetMetadataFn = uint64_t(__fastcall*)(uint64_t, uint32_t);

bool Accessible(uint64_t address, size_t size = sizeof(uint64_t))
{
    return IsAccessibleRange(address, size);
}

bool Executable(uint64_t address)
{
    return IsExecutableAddress(address);
}

bool HasLocalGeometry(const uint8_t* geometry)
{
    constexpr size_t components[] = {0x00, 0x04, 0x08, 0x10, 0x14, 0x18};
    for (size_t offset : components) {
        float value = 0.0f;
        std::memcpy(&value, geometry + offset, sizeof(value));
        if (!std::isfinite(value)) return false;
        if (std::fabs(value) >= 0.01f) return true;
    }
    return false;
}

float EndpointSpanSquared(const uint8_t* geometry)
{
    const float* start = (const float*)geometry;
    const float* end = (const float*)(geometry + 0x10);
    float span = 0.0f;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(start[axis]) || !std::isfinite(end[axis]))
            return 0.0f;
        float delta = end[axis] - start[axis];
        span += delta * delta;
    }
    return span;
}

uint64_t GetDti(void* object)
{
    if (!object || !Accessible((uint64_t)object)) return 0;
    uint64_t vtable = *(uint64_t*)object;
    if (!vtable || !Accessible(vtable + 0x28)) return 0;
    uint64_t getDti = *(uint64_t*)(vtable + 0x20);
    if (!getDti || getDti < kMinGameAddress || !Executable(getDti)) return 0;
    uint64_t dti = ((uint64_t(__fastcall*)(uint64_t))getDti)((uint64_t)object);
    return dti >= kMinGameAddress && Accessible(dti + 0x30, sizeof(uint32_t))
        ? dti : 0;
}

uint32_t DtiSize(uint64_t dti)
{
    if (!dti || !Accessible(dti + 0x30, sizeof(uint32_t))) return 0;
    return (*(uint32_t*)(dti + 0x30) & 0x7FFFFF) << 2;
}

bool IsDti(uint64_t dti, uintptr_t expected)
{
    return dti == (uint64_t)_addr(expected);
}

bool IsGameImageAddress(uint64_t address)
{
    static const uint64_t imageBase = (uint64_t)GetMKXEntryPoint();
    static const uint64_t imageEnd = []() {
        const auto* dos = (const IMAGE_DOS_HEADER*)GetMKXEntryPoint();
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return (uint64_t)0;
        const auto* nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)dos + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return (uint64_t)0;
        return (uint64_t)dos + nt->OptionalHeader.SizeOfImage;
    }();
    return address >= imageBase && address < imageEnd;
}

void CopyRange(void* destination, const uint8_t* source, size_t objectSize,
               SnapshotRange range)
{
    if (range.offset >= objectSize) return;
    size_t size = range.size;
    if (range.offset + size > objectSize) size = objectSize - range.offset;
    std::memcpy((uint8_t*)destination + range.offset, source + range.offset, size);
}

template <size_t Count>
void RestoreGeneratedRanges(void* destination, const uint8_t* source,
                            size_t objectSize, uint64_t dti,
                            const DtiSnapshotRange (&ranges)[Count])
{
    for (const DtiSnapshotRange& range : ranges) {
        if (IsDti(dti, range.dti)) {
            CopyRange(destination, source, objectSize,
                      {range.offset, range.size});
        }
    }
}

void PreserveLivePointers(uint8_t* overlay, const uint8_t* live,
                          size_t objectSize)
{
    for (size_t offset = kShotRestoreStart;
         offset + sizeof(uint64_t) <= objectSize;
         offset += sizeof(uint64_t)) {
        uint64_t liveValue = *(const uint64_t*)(live + offset);
        uint64_t savedValue = *(const uint64_t*)(overlay + offset);
        if ((IsLikelyPointer(liveValue) && Accessible(liveValue)) ||
            (IsLikelyPointer(savedValue) && Accessible(savedValue))) {
            std::memcpy(overlay + offset, live + offset, sizeof(uint64_t));
        }
    }
}

void PreserveRange(uint8_t* saved, const uint8_t* live, size_t objectSize,
                   SnapshotRange range)
{
    if (range.offset >= objectSize) return;
    size_t size = range.size;
    if (range.offset + size > objectSize) size = objectSize - range.offset;
    std::memcpy(saved + range.offset, live + range.offset, size);
}

std::vector<SnapshotRange> FindEmbeddedObjects(const uint8_t* live,
                                               size_t objectSize,
                                               size_t searchStart);

void PreserveEmbeddedObjectsFromBoth(uint8_t* overlay, const uint8_t* live,
                                     const uint8_t* saved, size_t objectSize,
                                     size_t searchStart)
{
    size_t liveRangeEnd = searchStart;
    size_t savedRangeEnd = searchStart;
    for (size_t offset = searchStart;
         offset + sizeof(uint64_t) <= objectSize;
         offset += sizeof(uint64_t)) {
        auto preserveCandidate = [&](const uint8_t* body, size_t& rangeEnd) {
            if (offset < rangeEnd) return;
            uint64_t candidate = *(const uint64_t*)(body + offset);
            if (!IsGameImageAddress(candidate)) return;

            uint64_t dti = GetDti((void*)(body + offset));
            uint32_t size = DtiSize(dti);
            if (!dti || size < sizeof(uint64_t) ||
                size > kMaxEmbeddedObjectSize || offset + size > objectSize) {
                return;
            }

            PreserveRange(overlay, live, objectSize, {offset, size});
            rangeEnd = offset + size;
        };

        preserveCandidate(live, liveRangeEnd);
        preserveCandidate(saved, savedRangeEnd);
    }
}

void RestoreDetectedCharacterTailScalars(void* character,
                                         const uint8_t* savedBody,
                                         size_t bodySize)
{
    if (bodySize <= kCharacterConcreteTailStart) return;

    uint8_t* live = (uint8_t*)character;
    std::vector<uint8_t> preserve(bodySize - kCharacterConcreteTailStart, 0);
    auto preserveRange = [&](size_t start, size_t end) {
        start = std::max(start, kCharacterConcreteTailStart);
        end = std::min(end, bodySize);
        if (start >= end) return;
        std::memset(preserve.data() + start - kCharacterConcreteTailStart, 1,
                    end - start);
    };

    // Inline MtObjects own vtables, references, and destructor state. Their
    // complete native ranges remain on the live timeline.
    for (const SnapshotRange& range : FindEmbeddedObjects(
             live, bodySize, kCharacterConcreteTailStart)) {
        preserveRange(range.offset, range.offset + range.size);
    }

    // Character subclasses use both 8-byte and 4-byte packing. Scan each
    // four-byte boundary so an unaligned helper pointer is not split into two
    // apparent scalar words. Preserve the neighboring selector fields with the
    // pointer: attachment records commonly store parent/joint modes immediately
    // before or after the reference, and mixing saved selectors with a live
    // object can dispatch a virtual method that object does not implement.
    for (size_t offset = kCharacterConcreteTailStart;
         offset + sizeof(uint64_t) <= bodySize;
         offset += alignof(uint32_t)) {
        uint64_t liveValue = 0;
        uint64_t savedValue = 0;
        std::memcpy(&liveValue, live + offset, sizeof(liveValue));
        std::memcpy(&savedValue, savedBody + offset, sizeof(savedValue));
        bool livePointer = IsLikelyPointer(liveValue) && Accessible(liveValue);
        bool savedPointer = IsLikelyPointer(savedValue) && Accessible(savedValue);
        bool staleTransition =
            (liveValue == 0 && savedValue >= 0x10000 &&
             savedValue < 0x0000800000000000ull && (savedValue & 7) == 0) ||
            (savedValue == 0 && liveValue >= 0x10000 &&
             liveValue < 0x0000800000000000ull && (liveValue & 7) == 0);
        if (livePointer || savedPointer || staleTransition) {
            size_t recordStart = offset >= 8 ? offset - 8 : 0;
            preserveRange(recordStart, offset + sizeof(uint64_t) + 16);
        }
    }

    size_t offset = kCharacterConcreteTailStart;
    while (offset < bodySize) {
        while (offset < bodySize &&
               preserve[offset - kCharacterConcreteTailStart] != 0) {
            ++offset;
        }
        size_t scalarStart = offset;
        while (offset < bodySize &&
               preserve[offset - kCharacterConcreteTailStart] == 0) {
            ++offset;
        }
        if (scalarStart < offset) {
            std::memcpy(live + scalarStart, savedBody + scalarStart,
                        offset - scalarStart);
        }
    }
}

std::vector<SnapshotRange> FindEmbeddedObjects(const uint8_t* live,
                                               size_t objectSize,
                                               size_t searchStart)
{
    std::vector<SnapshotRange> ranges;
    for (size_t offset = searchStart;
         offset + sizeof(uint64_t) <= objectSize;
         offset += sizeof(uint64_t)) {
        uint64_t candidate = *(const uint64_t*)(live + offset);
        // An inline MtObject begins with a game-owned vtable. Reject ordinary
        // heap/data pointers before any VirtualQuery or DTI virtual dispatch.
        if (!IsGameImageAddress(candidate))
            continue;

        uint64_t dti = GetDti((void*)(live + offset));
        uint32_t size = DtiSize(dti);
        if (!dti || size < sizeof(uint64_t) ||
            size > kMaxEmbeddedObjectSize || offset + size > objectSize)
            continue;

        bool overlaps = false;
        for (const SnapshotRange& range : ranges) {
            if (offset < range.offset + range.size &&
                range.offset < offset + size) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps)
            ranges.push_back({offset, size});
    }
    return ranges;
}

uint64_t GetCollisionPrimitive(const uint8_t* shot, uint32_t slotIndex,
                               uint32_t primitiveIndex)
{
    uint64_t slot = *(const uint64_t*)(shot + kShotCollisionSlotsOffset +
                                      slotIndex * sizeof(uint64_t));
    if (!Accessible(slot, 0x24)) return 0;

    uint64_t vtable = *(uint64_t*)slot;
    if (!Accessible(vtable + 0x90)) return 0;
    GetPrimitiveFn getPrimitive = *(GetPrimitiveFn*)(vtable + 0x88);
    if (!getPrimitive || !Executable((uint64_t)getPrimitive)) return 0;

    uint32_t count = *(uint32_t*)(slot + 0x20);
    if (count > kMaxCollisionPrimitives || primitiveIndex >= count) return 0;
    uint64_t primitive = getPrimitive(slot, primitiveIndex);
    return Accessible(primitive, 0xA0) ? primitive : 0;
}

uint64_t GetCollisionMetadata(const uint8_t* shot, uint32_t slotIndex,
                              uint32_t primitiveIndex)
{
    uint64_t slot = *(const uint64_t*)(shot + kShotCollisionSlotsOffset +
                                      slotIndex * sizeof(uint64_t));
    if (!Accessible(slot, 0x24)) return 0;

    uint32_t count = *(uint32_t*)(slot + 0x20);
    if (count > kMaxCollisionPrimitives || primitiveIndex >= count) return 0;

    auto getMetadata = (GetMetadataFn)_addr(0x1405AA900);
    uint64_t metadata = getMetadata(slot, primitiveIndex);
    return Accessible(metadata, 0x60) ? metadata : 0;
}

void CaptureCollisionPrimitives(const uint8_t* shot, ConcreteShotSnapshot& out)
{
    for (uint32_t slot = 0; slot < kShotCollisionSlotCount; ++slot) {
        uint64_t collisionSlot = *(const uint64_t*)(
            shot + kShotCollisionSlotsOffset + slot * sizeof(uint64_t));
        if (!Accessible(collisionSlot, 0x24)) continue;

        uint32_t count = *(uint32_t*)(collisionSlot + 0x20);
        if (count > kMaxCollisionPrimitives) continue;
        for (uint32_t index = 0; index < count; ++index) {
            uint64_t primitive = GetCollisionPrimitive(shot, slot, index);
            if (!primitive) continue;

            ConcreteShotSnapshot::CollisionPrimitive saved{};
            saved.slot = slot;
            saved.index = index;
            uint64_t metadata = GetCollisionMetadata(shot, slot, index);
            if (metadata)
                saved.metadataType = *(uint32_t*)(metadata + 0x14);
            saved.geometryType = *(uint8_t*)(primitive + 0x08);
            saved.collisionKind = *(uint32_t*)(primitive + 0x10);
            std::memcpy(saved.geometry, (void*)(primitive + 0x20),
                        sizeof(saved.geometry));
            saved.flags = *(uint32_t*)(primitive + 0x50);
            saved.parent0 = *(uint64_t*)(primitive + 0x58);
            saved.parent1 = *(uint64_t*)(primitive + 0x60);
            auto shotOffset = [&](uint64_t parent) {
                uintptr_t shotStart = (uintptr_t)shot;
                uintptr_t shotEnd = shotStart + out.body.size();
                return parent >= shotStart && parent < shotEnd
                    ? (int32_t)(parent - shotStart) : -1;
            };
            saved.parent0ShotOffset = shotOffset(saved.parent0);
            saved.parent1ShotOffset = shotOffset(saved.parent1);
            std::memcpy(saved.parentState, (void*)(primitive + 0x68),
                        sizeof(saved.parentState));
            std::memcpy(saved.localGeometry, (void*)(primitive + 0x80),
                        sizeof(saved.localGeometry));
            std::memcpy(saved.sourceGeometry, saved.localGeometry,
                        sizeof(saved.sourceGeometry));

            // Resource-driven capsules can retain only part of their local
            // cache. Tron swarm capsules, for example, keep Y=30 while both X
            // endpoints collapse to floating-point residue. Repair an empty
            // endpoint axis independently from the saved world capsule rather
            // than treating one valid component as proof that the whole cache
            // is authoritative.
            const float* worldStart = (const float*)saved.geometry;
            const float* worldEnd = (const float*)(saved.geometry + 0x10);
            const float* shotPos = (const float*)(shot + 0x50);
            const float* localStart = (const float*)saved.localGeometry;
            const float* localEnd = (const float*)(saved.localGeometry + 0x10);
            float* sourceStart = (float*)saved.sourceGeometry;
            float* sourceEnd = (float*)(saved.sourceGeometry + 0x10);
            for (size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(localStart[axis]) ||
                    !std::isfinite(localEnd[axis]) ||
                    !std::isfinite(worldStart[axis]) ||
                    !std::isfinite(worldEnd[axis]) ||
                    !std::isfinite(shotPos[axis])) {
                    continue;
                }

                float derivedStart = worldStart[axis] - shotPos[axis];
                float derivedEnd = worldEnd[axis] - shotPos[axis];
                bool localAxisEmpty = std::fabs(localStart[axis]) < 0.01f &&
                    std::fabs(localEnd[axis]) < 0.01f;
                bool derivedAxisPresent = std::fabs(derivedStart) >= 0.01f ||
                    std::fabs(derivedEnd) >= 0.01f;
                if (localAxisEmpty && derivedAxisPresent) {
                    sourceStart[axis] = derivedStart;
                    sourceEnd[axis] = derivedEnd;
                }
            }
            saved.hasSourceGeometry = HasLocalGeometry(saved.sourceGeometry);
            out.collisionPrimitives.push_back(saved);
        }
    }
}

void RestoreCollisionPrimitives(uint8_t* shot,
                                 const ConcreteShotSnapshot& snapshot,
                                 const void* const* validTargets,
                                 size_t validTargetCount)
{
    auto restoreParent = [&](uint64_t savedParent, int32_t shotOffset,
                             uint64_t liveParent) {
        if (shotOffset >= 0 && (size_t)shotOffset < snapshot.body.size())
            return (uint64_t)shot + (uint32_t)shotOffset;
        for (size_t target = 0; target < validTargetCount; ++target) {
            if (savedParent == (uint64_t)validTargets[target])
                return savedParent;
        }
        return liveParent;
    };

    for (const auto& saved : snapshot.collisionPrimitives) {
        if (saved.slot >= kShotCollisionSlotCount) continue;
        uint64_t primitive = GetCollisionPrimitive(shot, saved.slot, saved.index);
        if (!primitive) continue;

        *(uint8_t*)(primitive + 0x08) = saved.geometryType;
        *(uint32_t*)(primitive + 0x10) = saved.collisionKind;
        std::memcpy((void*)(primitive + 0x20), saved.geometry,
                    sizeof(saved.geometry));
        *(uint32_t*)(primitive + 0x50) = saved.flags;
        *(uint64_t*)(primitive + 0x58) = restoreParent(
            saved.parent0, saved.parent0ShotOffset,
            *(uint64_t*)(primitive + 0x58));
        *(uint64_t*)(primitive + 0x60) = restoreParent(
            saved.parent1, saved.parent1ShotOffset,
            *(uint64_t*)(primitive + 0x60));
        std::memcpy((void*)(primitive + 0x68), saved.parentState,
                    sizeof(saved.parentState));
        std::memcpy((void*)(primitive + 0x80), saved.localGeometry,
                    sizeof(saved.localGeometry));

    }
}

void RestoreAttackTargets(uint8_t* shot, const uint8_t* saved,
                          size_t objectSize, const void* const* validTargets,
                          size_t validTargetCount)
{
    for (size_t table : kAttackTargetTables) {
        for (uint32_t index = 0; index < kAttackTargetCount; ++index) {
            size_t entry = table + index * kAttackTargetStride;
            if (entry + kAttackTargetStride > objectSize) return;

            uint64_t savedTarget = *(const uint64_t*)(saved + entry + 0x08);
            uint64_t liveTarget = *(uint64_t*)(shot + entry + 0x08);
            bool validTarget = savedTarget == 0 || savedTarget == liveTarget;
            for (size_t target = 0; !validTarget && target < validTargetCount;
                 ++target) {
                validTarget = savedTarget == (uint64_t)validTargets[target];
            }
            // TargetInfo only owns character references. Restrict historical
            // pointers to the current roster instead of accepting an unrelated
            // live MtObject that happens to reuse the old address.
            if (validTarget) {
                *(uint64_t*)(shot + entry + 0x08) = savedTarget;
            }
            std::memcpy(shot + entry + 0x10, saved + entry + 0x10, 0x08);
        }
    }
}

} // namespace

bool ConcreteShotSnapshot_Capture(void* shot, ConcreteShotSnapshot& out)
{
    out = {};
    if (!shot) return false;

    uint64_t dti = GetDti(shot);
    uint32_t size = DtiSize(dti);
    if (size < kShotBodyStart || size > kMaxObjectSize ||
        !Accessible((uint64_t)shot, size)) {
        return false;
    }

    out.savedObject = shot;
    out.dti = dti;
    out.vtable = *(uint64_t*)shot;
    out.body.resize(size);
    std::memcpy(out.body.data(), shot, size);
    CaptureCollisionPrimitives((const uint8_t*)shot, out);
    for (uint32_t index = 0; index < kShotCallbackEffectCount; ++index) {
        void* effect = *(void**)((uint8_t*)shot + kShotCallbackEffectsOffset +
                                index * sizeof(void*));
        auto& state = out.callbackEffects[index];
        state.occupied = effect != nullptr;
        if (!effect || !IsEffectObject(effect) ||
            !Accessible((uint64_t)effect, 0x138)) {
            continue;
        }
        state.hasState = true;
        state.dti = GetObjectDti((uint64_t)effect);
        std::memcpy(state.flags100, (uint8_t*)effect + 0x100,
                    sizeof(state.flags100));
        std::memcpy(state.timing110, (uint8_t*)effect + 0x110,
                    sizeof(state.timing110));
        if (!Accessible((uint64_t)effect, 0x242)) continue;
        std::memcpy(state.state1D0, (uint8_t*)effect + 0x1D0,
                    sizeof(state.state1D0));
        std::memcpy(state.state1E0, (uint8_t*)effect + 0x1E0,
                    sizeof(state.state1E0));
        std::memcpy(state.state1F8, (uint8_t*)effect + 0x1F8,
                    sizeof(state.state1F8));
        std::memcpy(state.state230, (uint8_t*)effect + 0x230,
                    sizeof(state.state230));

        uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
        for (uint32_t count = 0; generator && count < kMaxCallbackGenerators;
             ++count) {
            if (!Accessible(generator, 0x240)) break;
            ConcreteShotSnapshot::CallbackEffectState::GeneratorState saved{};
            saved.dti = GetObjectDti(generator);
            if (!saved.dti) break;
            std::memcpy(saved.state20, (void*)(generator + 0x20),
                        sizeof(saved.state20));
            std::memcpy(saved.state30, (void*)(generator + 0x30),
                        sizeof(saved.state30));
            std::memcpy(saved.state70, (void*)(generator + 0x70),
                        sizeof(saved.state70));
            std::memcpy(saved.stateB8, (void*)(generator + 0xB8),
                        sizeof(saved.stateB8));
            std::memcpy(saved.state110, (void*)(generator + 0x110),
                        sizeof(saved.state110));
            std::memcpy(saved.state19C, (void*)(generator + 0x19C),
                        sizeof(saved.state19C));
            std::memcpy(saved.state1F0, (void*)(generator + 0x1F0),
                        sizeof(saved.state1F0));
            saved.state238 = *(uint8_t*)(generator + 0x238);
            state.generators.push_back(saved);
            generator = *(uint64_t*)(generator + 0x18);
        }
    }
    return true;
}

bool ConcreteEffectSnapshot_Capture(
    void* effect, ConcreteShotSnapshot::CallbackEffectState& state)
{
    state = {};
    state.occupied = effect != nullptr;
    if (!effect || !IsEffectObject(effect) ||
        !Accessible((uint64_t)effect, 0x138))
        return state.occupied;
    state.hasState = true;
    state.dti = GetObjectDti((uint64_t)effect);
    std::memcpy(state.flags100, (uint8_t*)effect + 0x100,
                sizeof(state.flags100));
    std::memcpy(state.timing110, (uint8_t*)effect + 0x110,
                sizeof(state.timing110));
    if (!Accessible((uint64_t)effect, 0x242)) return true;
    std::memcpy(state.state1D0, (uint8_t*)effect + 0x1D0,
                sizeof(state.state1D0));
    std::memcpy(state.state1E0, (uint8_t*)effect + 0x1E0,
                sizeof(state.state1E0));
    std::memcpy(state.state1F8, (uint8_t*)effect + 0x1F8,
                sizeof(state.state1F8));
    std::memcpy(state.state230, (uint8_t*)effect + 0x230,
                sizeof(state.state230));
    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    for (uint32_t count = 0; generator && count < kMaxCallbackGenerators;
         ++count) {
        if (!Accessible(generator, 0x240)) break;
        ConcreteShotSnapshot::CallbackEffectState::GeneratorState saved{};
        saved.dti = GetObjectDti(generator);
        if (!saved.dti) break;
        std::memcpy(saved.state20, (void*)(generator + 0x20), sizeof(saved.state20));
        std::memcpy(saved.state30, (void*)(generator + 0x30), sizeof(saved.state30));
        std::memcpy(saved.state70, (void*)(generator + 0x70), sizeof(saved.state70));
        std::memcpy(saved.stateB8, (void*)(generator + 0xB8), sizeof(saved.stateB8));
        std::memcpy(saved.state110, (void*)(generator + 0x110), sizeof(saved.state110));
        std::memcpy(saved.state19C, (void*)(generator + 0x19C), sizeof(saved.state19C));
        std::memcpy(saved.state1F0, (void*)(generator + 0x1F0), sizeof(saved.state1F0));
        saved.state238 = *(uint8_t*)(generator + 0x238);
        state.generators.push_back(saved);
        generator = *(uint64_t*)(generator + 0x18);
    }
    return true;
}

bool ConcreteEffectSnapshot_Restore(
    void* effect, const ConcreteShotSnapshot::CallbackEffectState& state)
{
    if (!effect || !state.hasState || !IsEffectObject(effect) ||
        !Accessible((uint64_t)effect, 0x242) || GetObjectDti((uint64_t)effect) != state.dti)
        return false;
    std::memcpy((uint8_t*)effect + 0x100, state.flags100, sizeof(state.flags100));
    std::memcpy((uint8_t*)effect + 0x110, state.timing110, sizeof(state.timing110));
    std::memcpy((uint8_t*)effect + 0x1D0, state.state1D0, sizeof(state.state1D0));
    std::memcpy((uint8_t*)effect + 0x1E0, state.state1E0, sizeof(state.state1E0));
    std::memcpy((uint8_t*)effect + 0x1F8, state.state1F8, sizeof(state.state1F8));
    std::memcpy((uint8_t*)effect + 0x230, state.state230, sizeof(state.state230));
    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    for (const auto& saved : state.generators) {
        if (!generator || !Accessible(generator, 0x240) ||
            GetObjectDti(generator) != saved.dti) return false;
        std::memcpy((void*)(generator + 0x20), saved.state20, sizeof(saved.state20));
        std::memcpy((void*)(generator + 0x30), saved.state30, sizeof(saved.state30));
        std::memcpy((void*)(generator + 0x70), saved.state70, sizeof(saved.state70));
        std::memcpy((void*)(generator + 0xB8), saved.stateB8, sizeof(saved.stateB8));
        std::memcpy((void*)(generator + 0x110), saved.state110, sizeof(saved.state110));
        std::memcpy((void*)(generator + 0x19C), saved.state19C, sizeof(saved.state19C));
        std::memcpy((void*)(generator + 0x1F0), saved.state1F0, sizeof(saved.state1F0));
        *(uint8_t*)(generator + 0x238) = saved.state238;
        generator = *(uint64_t*)(generator + 0x18);
    }
    return true;
}

void ConcreteShotSnapshot_RestoreCallbackEffects(
    void* shot, const ConcreteShotSnapshot& snapshot)
{
    if (!shot) return;

    for (uint32_t index = 0; index < kShotCallbackEffectCount; ++index) {
        const auto& state = snapshot.callbackEffects[index];
        void** slot = (void**)((uint8_t*)shot + kShotCallbackEffectsOffset +
                              index * sizeof(void*));
        void* effect = *slot;

        // Do not destroy a fresh native callback during reconstruction. The
        // factory has already established its ownership and category links;
        // clearing or killing it here leaves a live orphan or makes the shot
        // lose its callback on the next uShot::preMove.
        if (!state.occupied) {
            continue;
        }

        // Pending effects can occupy a callback slot before their complete DTI
        // and generator graph becomes queryable. Preserve the factory-created
        // replacement in that case; occupancy is authoritative, while scalar
        // rewind is only possible for a fully captured effect.
        if (!state.hasState)
            continue;

        if (!effect || !IsEffectObject(effect) ||
            !Accessible((uint64_t)effect, 0x138) ||
            GetObjectDti((uint64_t)effect) != state.dti) {
            continue;
        }

        // Preserve the fresh generator/resource graph and native ownership,
        // but rewind the pointer-free uEffect flags and timeline counters.
        std::memcpy((uint8_t*)effect + 0x100, state.flags100,
                    sizeof(state.flags100));
        std::memcpy((uint8_t*)effect + 0x110, state.timing110,
                    sizeof(state.timing110));
        if (!Accessible((uint64_t)effect, 0x242)) continue;
        std::memcpy((uint8_t*)effect + 0x1D0, state.state1D0,
                    sizeof(state.state1D0));
        std::memcpy((uint8_t*)effect + 0x1E0, state.state1E0,
                    sizeof(state.state1E0));
        std::memcpy((uint8_t*)effect + 0x1F8, state.state1F8,
                    sizeof(state.state1F8));
        std::memcpy((uint8_t*)effect + 0x230, state.state230,
                    sizeof(state.state230));

        uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
        for (const auto& saved : state.generators) {
            if (!generator || !Accessible(generator, 0x240) ||
                GetObjectDti(generator) != saved.dti) {
                break;
            }
            std::memcpy((void*)(generator + 0x20), saved.state20,
                        sizeof(saved.state20));
            std::memcpy((void*)(generator + 0x30), saved.state30,
                        sizeof(saved.state30));
            std::memcpy((void*)(generator + 0x70), saved.state70,
                        sizeof(saved.state70));
            std::memcpy((void*)(generator + 0xB8), saved.stateB8,
                        sizeof(saved.stateB8));
            std::memcpy((void*)(generator + 0x110), saved.state110,
                        sizeof(saved.state110));
            std::memcpy((void*)(generator + 0x19C), saved.state19C,
                        sizeof(saved.state19C));
            std::memcpy((void*)(generator + 0x1F0), saved.state1F0,
                        sizeof(saved.state1F0));
            *(uint8_t*)(generator + 0x238) = saved.state238;
            generator = *(uint64_t*)(generator + 0x18);
        }
    }
}

bool ConcreteShotSnapshot_Restore(void* shot,
                                  const ConcreteShotSnapshot& snapshot,
                                  const void* const* validTargets,
                                  size_t validTargetCount)
{
    if (!shot || snapshot.body.size() < kShotBodyStart) return false;

    const uint64_t profileStart = Profile_Now();

    uint64_t liveDti = GetDti(shot);
    uint32_t liveSize = DtiSize(liveDti);
    if (liveDti != snapshot.dti || liveSize != snapshot.body.size() ||
        !Accessible((uint64_t)shot, liveSize)) {
        return false;
    }
    const uint64_t validateEnd = Profile_Now();

    const uint8_t* source = snapshot.body.data();
    uint8_t* live = (uint8_t*)shot;
    std::vector<uint8_t> overlay(source, source + snapshot.body.size());
    const uint64_t copyBodyEnd = Profile_Now();

    // The vtable and common unit state remain owned by the freshly-created
    // object. ShotFlightState restores the gameplay lifecycle bits safely.
    PreserveRange(overlay.data(), live, liveSize, {0, sizeof(uint64_t)});
    PreserveRange(overlay.data(), live, liveSize, {0x10, 0x08});

    // Restore the complete common uShot gameplay body rather than the sparse
    // ShotFlightState ranges alone. Keep allocations, resources and external
    // references from the freshly-created object wherever either timeline's
    // qword resolves to committed memory. This also lets a fresh null clear a
    // stale saved pointer. Confirmed scalar flight ranges are applied after
    // this overlay by the caller.
    PreserveLivePointers(overlay.data(), live, liveSize);
    const uint64_t pointerScanEnd = Profile_Now();

    // uBaseModel's material-cache pointer and count form one native-owned
    // header. Preserve both from the fresh object so a null fresh allocation
    // cannot be paired with the saved object's nonzero count during teardown.
    PreserveRange(overlay.data(), live, liveSize, {0x110, 0x10});

    // uModel's joint count and allocation pointers describe the freshly-built
    // native model. A saved count with a null live index array makes rendering
    // call through a null pointer during joint-order traversal.
    PreserveRange(overlay.data(), live, liveSize, {0x530, 0x30});

    // uActorGameModel owns four resource references followed by seven helper
    // allocations in +0x13E8..+0x143F. Its destructor releases this complete
    // block. Preserve it atomically; mixing saved adjacent ownership state with
    // fresh pointers can make two fields release the same helper or dispatch
    // through an object whose vtable was already cleared.
    PreserveRange(overlay.data(), live, liveSize, {0x13E8, 0x58});

    // cAtkCtrl and cArm are inline native owners. Their scalar fields include
    // vtable-backed collision slots and target-info storage, so preserving only
    // the pointer-looking words is insufficient: a saved destructor state can
    // leave a live slot non-null with a zeroed vtable. Keep the complete fresh
    // owner region; the explicitly restored target/collision gameplay state is
    // applied below through native accessors.
    PreserveRange(overlay.data(), live, liveSize, {0x1470, 0xC08});

    // cAtkCtrl+0x20 is its attack-resource map. Kikosho phase 5 resolves the
    // current attack-info entry through this binding before it creates its
    // expanded explosion primitive. The factory's fresh map can describe a
    // later resource state even when the restored mAtkInfoId matches, so keep
     // the saved binding while leaving the surrounding inline owner fresh.
     CopyRange(overlay.data(), source, liveSize, {0x1490, 0x08});

    // Restore all known pointer-free cAtkCtrl scalars: mAtkInfoId, mFlag,
    // mTotalCombo, mDelayTime, mLifeTime, mCountTime, mEndTime, and mAnmNo.
    // The target-table MtArray headers immediately follow this range and stay
    // owned by the fresh object; their validated entries are restored below.
    CopyRange(overlay.data(), source, liveSize, {0x1498, 0x24});

    // Soul Fist stores a live uGameEffect pointer after its mode field.
    if (IsDti(snapshot.dti, kSoulFistDti)) {
        PreserveRange(overlay.data(), live, liveSize, {0x2148, 0x08});
    }

    // Scan the common body and concrete tail once, preserving inline MtObjects
    // found in either timeline. Saved-only owners still retain fresh bytes so
    // historical generator and particle ownership cannot enter the live heap.
    PreserveEmbeddedObjectsFromBoth(overlay.data(), live, source, liveSize,
                                    kShotRestoreStart);
    const uint64_t embeddedLiveEnd = Profile_Now();
    const uint64_t embeddedSavedEnd = embeddedLiveEnd;

    // createProperty value fields exported from properties_static.json are
    // ownership-free gameplay state. Reapply them after the pointer and
    // embedded-object filters so a packed scalar cannot be retained merely
    // because it resembles an address.
    RestoreGeneratedRanges(overlay.data(), source, liveSize, snapshot.dti,
                           kGeneratedShotTailRanges);

    if (IsDti(snapshot.dti, kVulcanAxeDti)) {
        // Arthur's axe rotates its model and collision geometry from this
        // accumulator. Values such as 18.0f resemble mapped addresses when
        // read as a qword, so the generic ownership filter keeps factory zero.
        CopyRange(overlay.data(), source, liveSize, {0x21D0, 0x04});
    }

    // mId is scalar allocator state inside the otherwise fresh-owned cArm
    // block. A reconstructed Kikosho kept its factory ID here while the ID
    // pool was rewound, changing per-target hit consumption on the next tick.
    CopyRange(overlay.data(), source, liveSize, {0x1F50, 0x04});

    if (IsDti(snapshot.dti, kKingKobunDti) ||
        IsDti(snapshot.dti, kMissileEmplacementDti)) {
        // These shots consume model animation state in gameplay. Reconstruction
        // keeps fresh motion owners and resources, but must rewind pointer-free
        // playback state or joint-driven phases and launch positions restart.
        CopyRange(overlay.data(), source, liveSize, {0x550, 0x10});
        static constexpr SnapshotRange motionRestoreRanges[] = {
            {0x08, 0x118},
            {0x140, 0x20},
            {0x168, 0x08},
        };
        for (uint32_t motion = 0; motion < 8; ++motion) {
            size_t motionBase = 0x560 + motion * 0x180;
            for (const SnapshotRange& range : motionRestoreRanges) {
                CopyRange(overlay.data(), source, liveSize,
                          {motionBase + range.offset, range.size});
            }
        }
    }

    /*if (IsDti(snapshot.dti, kAttackWireDti)) {
        // This unlabeled attack-wire block was separately verified from its
        // constructor and collision callbacks. Its owned effect resource starts
        // at +0x2150 and intentionally remains on the reconstructed timeline.
        CopyRange(overlay.data(), source, liveSize, {0x2140, 0x10});
    }*/

    // Copy the complete gameplay tail. The prefix contains cUnit ownership and
    // intrusive links and remains owned by the freshly-created object.
    CopyRange(live, overlay.data(), liveSize,
              {kShotRestoreStart, liveSize - kShotRestoreStart});
    const uint64_t applyBodyEnd = Profile_Now();
    // A fresh shot has empty cAtkCtrl target tables. Restore the saved
    // per-target hit count/time so reconstructing an already-consumed attack
    // cannot apply the same projectile to the same character a second time.
    // Keep the fresh vtable, mpUnit and mpResource ownership at +0x00..+0x27,
    // but rewind mAtkInfoId, flags, combo and timing state through mAnmNo.
    CopyRange(live, source, liveSize, {0x1498, 0x28});
    RestoreAttackTargets(live, source, liveSize, validTargets, validTargetCount);
    const uint64_t targetsEnd = Profile_Now();
    RestoreCollisionPrimitives(live, snapshot, validTargets, validTargetCount);

    for (const auto& saved : snapshot.collisionPrimitives) {
        if (!GetCollisionPrimitive(live, saved.slot, saved.index))
            return false;
    }
    const uint64_t collisionEnd = Profile_Now();
    if (g_activeRollbackProfile) {
        g_activeRollbackProfile->restoreConcreteValidate += validateEnd - profileStart;
        g_activeRollbackProfile->restoreConcreteCopyBody += copyBodyEnd - validateEnd;
        g_activeRollbackProfile->restoreConcretePointerScan += pointerScanEnd - copyBodyEnd;
        g_activeRollbackProfile->restoreConcreteEmbeddedLive += embeddedLiveEnd - pointerScanEnd;
        g_activeRollbackProfile->restoreConcreteEmbeddedSaved += embeddedSavedEnd - embeddedLiveEnd;
        g_activeRollbackProfile->restoreConcreteApplyBody += applyBodyEnd - embeddedSavedEnd;
        g_activeRollbackProfile->restoreConcreteTargets += targetsEnd - applyBodyEnd;
        g_activeRollbackProfile->restoreConcreteCollision += collisionEnd - targetsEnd;
    }
    return true;
}

void ConcreteShotSnapshot_ReapplyCapsules(
    void* shot, const ConcreteShotSnapshot& snapshot)
{
    if (!shot) return;

    const ConcreteShotSnapshot::CollisionPrimitive* widest = nullptr;
    float widestSpan = 0.0f;
    for (const auto& saved : snapshot.collisionPrimitives) {
        if (!saved.hasSourceGeometry || saved.metadataType != 1) continue;
        const float* start = (const float*)saved.sourceGeometry;
        const float* end = (const float*)(saved.sourceGeometry + 0x10);
        if (std::fabs(start[0] + end[0]) >= 0.01f) continue;
        float span = EndpointSpanSquared(saved.sourceGeometry);
        if (span > widestSpan) {
            widest = &saved;
            widestSpan = span;
        }
    }
    if (!widest) return;

    for (const auto& saved : snapshot.collisionPrimitives) {
        if (!saved.hasSourceGeometry || saved.metadataType != 1) continue;
        uint64_t primitive = GetCollisionPrimitive(
            (const uint8_t*)shot, saved.slot, saved.index);
        uint64_t metadata = GetCollisionMetadata(
            (const uint8_t*)shot, saved.slot, saved.index);
        if (!primitive || !metadata) continue;

        uint32_t liveMetadataType = *(uint32_t*)(metadata + 0x14);
        if (liveMetadataType != 1) continue;

        // Native swept capsules can intentionally span the current and prior
        // shot positions. Only replace geometry that actually collapsed.
        if (EndpointSpanSquared((const uint8_t*)(primitive + 0x20)) > 0.0001f)
            continue;

        const float* start = (const float*)saved.sourceGeometry;
        const float* end = (const float*)(saved.sourceGeometry + 0x10);
        bool centered = std::fabs(start[0] + end[0]) < 0.01f;
        const auto* selected = centered ? &saved : widest;
        if (EndpointSpanSquared(selected->sourceGeometry) <= 0.0001f)
            continue;

        // The reconstructed primitive's live parent transforms can cancel the
        // restored local X offsets inside virtual +0x1C8, collapsing both world
        // endpoints to the shot origin. This source was recovered at capture as
        // world endpoint minus shot position, so invert that operation directly
        // at the outer hitbox-update boundary without touching shared metadata.
        const float* shotPosition = (const float*)((uint8_t*)shot + 0x50);
        const float* selectedStart = (const float*)selected->sourceGeometry;
        const float* selectedEnd =
            (const float*)(selected->sourceGeometry + 0x10);
        float* localStart = (float*)(primitive + 0x80);
        float* localEnd = (float*)(primitive + 0x90);
        float* worldStart = (float*)(primitive + 0x20);
        float* worldEnd = (float*)(primitive + 0x30);
        for (size_t axis = 0; axis < 3; ++axis) {
            localStart[axis] = selectedStart[axis];
            localEnd[axis] = selectedEnd[axis];
            worldStart[axis] = shotPosition[axis] + selectedStart[axis];
            worldEnd[axis] = shotPosition[axis] + selectedEnd[axis];
        }
    }
}

void ConcreteCharacterSnapshot_RestoreFields(void* character,
                                             const uint8_t* savedBody,
                                             size_t bodySize,
                                             uint64_t dti)
{
    if (!character || !savedBody) return;

    // Preserve unknown character-tail scalars while retaining detected live
    // helper/resource ownership. Known createProperty fields are then applied
    // explicitly so packed values cannot be mistaken for pointers.
    RestoreDetectedCharacterTailScalars(character, savedBody, bodySize);

    RestoreGeneratedRanges(character, savedBody, bodySize, dti,
                           kGeneratedCharacterTailRanges);

    if (IsDti(dti, kRyuDti)) {
        // Restore retires every live effect without reconstructing or remapping
        // this weak reference. Do not let uRyu::tick release the retired object.
        CopyRange(character, savedBody, bodySize, {0x68F0, 0x28});
        if (bodySize >= 0x6920)
            *(uint64_t*)((uint8_t*)character + 0x6918) = 0;
        CopyRange(character, savedBody, bodySize, {0x6920, 0x20});
    }

    if (IsDti(dti, kHsienKoDti)) {
        // Ghidra verified these as mReservedAnki, mBasePos, mPendulumLength,
        // and mUsrF. The following +0x6918 field is a tracked helper pointer
        // and is intentionally left on the live timeline.
        CopyRange(character, savedBody, bodySize, {0x68FC, 0x1C});
    }

    /*if (IsDti(dti, kSpencerDti)) {
        // uSpencer's complete 16-byte concrete tail is pointer-free. It holds
        // the wire-active latch, wire animation number and mUsrF consumed when
        // the grapple branches to its facing-specific action.
        CopyRange(character, savedBody, bodySize, {0x68F0, 0x10});
    }*/

}
