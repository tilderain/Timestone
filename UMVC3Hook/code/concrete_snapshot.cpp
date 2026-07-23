#include "../pch.h"

#include "concrete_snapshot.h"
#include "mk10utils.h"

#include <cstring>

namespace {

constexpr uintptr_t kMinGameAddress = 0x140000000ull;
constexpr size_t kShotBodyStart = 0x2140;
constexpr size_t kMaxObjectSize = 0x40000;

// These DTI addresses were verified from the class DTI getters in Ghidra.
// They are used only to select the exceptional subclass policy; the object
// size itself always comes from the live object's DTI metadata.
constexpr uintptr_t kSoulFistDti = 0x140D4B6E8ull;
constexpr uintptr_t kLaserSinkuDti = 0x140D4C5F0ull;
constexpr uintptr_t kAnkihouRollingDti = 0x140D4AFE0ull;
constexpr uintptr_t kZireitouDti = 0x140D4B0D0ull;
constexpr uintptr_t kRyuDti = 0x140D45A10ull;
constexpr uintptr_t kHsienKoDti = 0x140D455F8ull;

struct SnapshotRange {
    size_t offset;
    size_t size;
};

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

uint64_t GetDti(void* object)
{
    if (!object || !Accessible((uint64_t)object)) return 0;
    uint64_t vtable = *(uint64_t*)object;
    if (!Accessible(vtable + 0x28)) return 0;
    uint64_t getDti = *(uint64_t*)(vtable + 0x20);
    if (!getDti || getDti < kMinGameAddress || !Accessible(getDti)) return 0;
    return ((uint64_t(__fastcall*)(uint64_t))getDti)((uint64_t)object);
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
    if (range.offset < kShotBodyStart || range.offset >= objectSize) return;
    size_t size = range.size;
    if (range.offset + size > objectSize) size = objectSize - range.offset;
    std::memcpy((uint8_t*)destination + range.offset, source + range.offset, size);
}

void PreserveRange(uint8_t* saved, const uint8_t* live, size_t objectSize,
                   SnapshotRange range)
{
    if (range.offset >= objectSize) return;
    size_t size = range.size;
    if (range.offset + size > objectSize) size = objectSize - range.offset;
    std::memcpy(saved + range.offset, live + range.offset, size);
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
    return true;
}

void ConcreteShotSnapshot_Restore(void* shot,
                                  const ConcreteShotSnapshot& snapshot)
{
    if (!shot || snapshot.body.size() < kShotBodyStart) return;

    uint64_t liveDti = GetDti(shot);
    uint32_t liveSize = DtiSize(liveDti);
    if (liveDti != snapshot.dti || liveSize != snapshot.body.size() ||
        !Accessible((uint64_t)shot, liveSize)) {
        return;
    }

    const uint8_t* source = snapshot.body.data();
    uint8_t* live = (uint8_t*)shot;
    std::vector<uint8_t> overlay(source, source + snapshot.body.size());

    // The vtable and common unit state remain owned by the freshly-created
    // object. ShotFlightState restores the gameplay lifecycle bits safely.
    PreserveRange(overlay.data(), live, liveSize, {0, sizeof(uint64_t)});
    PreserveRange(overlay.data(), live, liveSize, {0x10, 0x08});

    // Soul Fist stores a live uGameEffect pointer after its mode field.
    if (IsDti(snapshot.dti, kSoulFistDti)) {
        PreserveRange(overlay.data(), live, liveSize, {0x2148, 0x08});
    }

    // Shinku Hadouken constructs two embedded constraint/share objects. Their
    // vtables and allocator-owned headers must remain those of the fresh
    // native object; scalar subclass state around them is still restored.
    if (IsDti(snapshot.dti, kLaserSinkuDti)) {
        PreserveRange(overlay.data(), live, liveSize, {0x2160, 0xD0});
        PreserveRange(overlay.data(), live, liveSize, {0x2230, 0xD0});
    }

    if (IsDti(snapshot.dti, kAnkihouRollingDti) ||
        IsDti(snapshot.dti, kZireitouDti)) {
        // Hsien-Ko's projectile embeds a particle-list object at +0x2140.
        // Its prev/next/list-owner fields must come from the fresh instance;
        // restoring them makes destruction follow the old timeline's list.
        PreserveRange(overlay.data(), live, liveSize, {0x2190, 0x68});
    }

    // Only the concrete subclass tail is copied here. The existing flight
    // snapshot remains responsible for the pointer-rich common uShot body.
    CopyRange(live, overlay.data(), liveSize,
              {kShotBodyStart, liveSize - kShotBodyStart});
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
