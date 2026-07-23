#include "../pch.h"

#include "concrete_snapshot.h"
#include "mk10utils.h"

#include <cstring>

namespace {

constexpr uintptr_t kMinGameAddress = 0x140000000ull;
constexpr size_t kShotBodyStart = 0x2140;
constexpr size_t kShotRestoreStart = 0x50;
constexpr size_t kMaxObjectSize = 0x40000;
constexpr size_t kMaxEmbeddedObjectSize = 0x1000;
constexpr size_t kShotCollisionSlotsOffset = 0x2058;
constexpr uint32_t kShotCollisionSlotCount = 4;
constexpr uint32_t kMaxCollisionPrimitives = 128;
constexpr uint32_t kAttackTargetTableCount = 3;
constexpr uint32_t kAttackTargetCount = 32;
constexpr size_t kAttackTargetStride = 0x18;
constexpr size_t kAttackTargetTables[kAttackTargetTableCount] = {
    0x18C8, 0x1BC8, 0x1EC8
};

// These DTI addresses were verified from the class DTI getters in Ghidra.
// They are used only to select the exceptional subclass policy; the object
// size itself always comes from the live object's DTI metadata.
constexpr uintptr_t kSoulFistDti = 0x140D4B6E8ull;
constexpr uintptr_t kRyuDti = 0x140D45A10ull;
constexpr uintptr_t kHsienKoDti = 0x140D455F8ull;

struct SnapshotRange {
    size_t offset;
    size_t size;
};

using GetPrimitiveFn = uint64_t(__fastcall*)(uint64_t, uint32_t);

bool Accessible(uint64_t address, size_t size = sizeof(uint64_t))
{
    if (address < 0x10000 || address >= 0x0000800000000000ull || size == 0)
        return false;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }

    const uint64_t regionEnd = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    return address <= regionEnd && size <= regionEnd - address;
}

bool Executable(uint64_t address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }

    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executable) != 0;
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

void CopyRange(void* destination, const uint8_t* source, size_t objectSize,
               SnapshotRange range)
{
    if (range.offset >= objectSize) return;
    size_t size = range.size;
    if (range.offset + size > objectSize) size = objectSize - range.offset;
    std::memcpy((uint8_t*)destination + range.offset, source + range.offset, size);
}

void PreserveLivePointers(uint8_t* overlay, const uint8_t* live,
                          size_t objectSize)
{
    for (size_t offset = kShotRestoreStart;
         offset + sizeof(uint64_t) <= objectSize;
         offset += sizeof(uint64_t)) {
        uint64_t liveValue = *(const uint64_t*)(live + offset);
        uint64_t savedValue = *(const uint64_t*)(overlay + offset);
        if (Accessible(liveValue) || Accessible(savedValue)) {
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
                                               size_t objectSize)
{
    std::vector<SnapshotRange> ranges;
    for (size_t offset = kShotBodyStart;
         offset + sizeof(uint64_t) <= objectSize;
         offset += sizeof(uint64_t)) {
        uint64_t candidate = *(const uint64_t*)(live + offset);
        if (candidate < kMinGameAddress || !Accessible(candidate + 0x28))
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
            saved.active = *(uint8_t*)(primitive + 0x08);
            saved.kind = *(uint32_t*)(primitive + 0x10);
            std::memcpy(saved.geometry, (void*)(primitive + 0x20),
                        sizeof(saved.geometry));
            saved.flags = *(uint32_t*)(primitive + 0x50);
            std::memcpy(saved.parentState, (void*)(primitive + 0x68),
                        sizeof(saved.parentState));
            std::memcpy(saved.localGeometry, (void*)(primitive + 0x80),
                        sizeof(saved.localGeometry));
            out.collisionPrimitives.push_back(saved);
        }
    }
}

void RestoreCollisionPrimitives(uint8_t* shot,
                                const ConcreteShotSnapshot& snapshot)
{
    for (const auto& saved : snapshot.collisionPrimitives) {
        if (saved.slot >= kShotCollisionSlotCount) continue;
        uint64_t primitive = GetCollisionPrimitive(shot, saved.slot, saved.index);
        if (!primitive) continue;

        *(uint8_t*)(primitive + 0x08) = saved.active;
        *(uint32_t*)(primitive + 0x10) = saved.kind;
        std::memcpy((void*)(primitive + 0x20), saved.geometry,
                    sizeof(saved.geometry));
        *(uint32_t*)(primitive + 0x50) = saved.flags;
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
    return true;
}

bool ConcreteShotSnapshot_Restore(void* shot,
                                  const ConcreteShotSnapshot& snapshot,
                                  const void* const* validTargets,
                                  size_t validTargetCount)
{
    if (!shot || snapshot.body.size() < kShotBodyStart) return false;

    uint64_t liveDti = GetDti(shot);
    uint32_t liveSize = DtiSize(liveDti);
    if (liveDti != snapshot.dti || liveSize != snapshot.body.size() ||
        !Accessible((uint64_t)shot, liveSize)) {
        return false;
    }

    const uint8_t* source = snapshot.body.data();
    uint8_t* live = (uint8_t*)shot;
    std::vector<uint8_t> overlay(source, source + snapshot.body.size());

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

    // uModel's joint count and allocation pointers describe the freshly-built
    // native model. A saved count with a null live index array makes rendering
    // call through a null pointer during joint-order traversal.
    PreserveRange(overlay.data(), live, liveSize, {0x530, 0x30});

    // Soul Fist stores a live uGameEffect pointer after its mode field.
    if (IsDti(snapshot.dti, kSoulFistDti)) {
        PreserveRange(overlay.data(), live, liveSize, {0x2148, 0x08});
    }

    // Concrete projectile tails can contain embedded MtObjects with live
    // ownership/list links. Discover those objects from the fresh instance
    // rather than maintaining a projectile-class allowlist. Their complete
    // live ranges must survive restoration; otherwise a later destructor can
    // follow an owner pointer from the old timeline.
    for (const SnapshotRange& range : FindEmbeddedObjects(live, liveSize)) {
        PreserveRange(overlay.data(), live, liveSize, range);
    }

    // Copy the complete gameplay tail. The prefix contains cUnit ownership and
    // intrusive links and remains owned by the freshly-created object.
    CopyRange(live, overlay.data(), liveSize,
              {kShotRestoreStart, liveSize - kShotRestoreStart});
    // A fresh shot has empty cAtkCtrl target tables. Restore the saved
    // per-target hit count/time so reconstructing an already-consumed attack
    // cannot apply the same projectile to the same character a second time.
    RestoreAttackTargets(live, source, liveSize, validTargets, validTargetCount);
    RestoreCollisionPrimitives(live, snapshot);

    for (const auto& saved : snapshot.collisionPrimitives) {
        if (!GetCollisionPrimitive(live, saved.slot, saved.index))
            return false;
    }
    return true;
}

void ConcreteCharacterSnapshot_RestoreFields(void* character,
                                             const uint8_t* savedBody,
                                             size_t bodySize,
                                             uint64_t dti)
{
    if (!character || !savedBody) return;

    if (IsDti(dti, kRyuDti)) {
        // uRyu's concrete tail is scalar except for mpBeamObj at +0x6918.
        // Keep that external reference live until shot/effect fixups can map it.
        CopyRange(character, savedBody, bodySize, {0x68F0, 0x28});
        CopyRange(character, savedBody, bodySize, {0x6920, 0x20});
    }

    if (IsDti(dti, kHsienKoDti)) {
        // Ghidra verified these as mReservedAnki, mBasePos, mPendulumLength,
        // and mUsrF. The following +0x6918 field is a tracked helper pointer
        // and is intentionally left on the live timeline.
        CopyRange(character, savedBody, bodySize, {0x68FC, 0x1C});
    }
}
