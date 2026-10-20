#include "rollback_character_restore.h"

#include <cstring>
#include <initializer_list>

namespace {

template <size_t Offset, size_t Size>
void Copy(uint8_t* live, const uint8_t* saved)
{
    std::memcpy(live + Offset, saved + Offset, Size);
}

template <typename T>
T Load(const uint8_t* bytes, size_t offset)
{
    T value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

// One common kernel, independent of concrete DTI. Bounds are validated once at
// entry. Embedded reference/collision repair remains at its existing phase in
// rollback_restore.cpp; no owner, vtable, lock or callback is copied here.
void CommonEarly(uint8_t* live, const uint8_t* saved, bool roster)
{
    Copy<0x08, 0x08>(live, saved); // cUnit mRno, mUnitParam
    if (roster) Copy<0x10, 0x04>(live, saved); // helpers keep lifecycle live
    Copy<0x28, 0x10>(live, saved); // delta time and unit-group mask
    // +18/+20/+38 list links, +40 coordinate parent remain live.
    Copy<0x48, 0xB8>(live, saved); // uCoord selectors, vectors and matrices
    // uBaseModel +100..510 is live render/resource ownership.
    Copy<0x558, 0x08>(live, saved); // motion calculation/interpolation flags
    Copy<0x1180, 0x30>(live, saved); // null scale, delta position/quaternion
    Copy<0x1240, 0x10>(live, saved); // morph weights, not shader owners +1250
    Copy<0x1260, 0x3F>(live, saved); // plane, waist/foot/IK scalar parameters
    // cToonParam writer 140451410 / shader consumer 140451210. Inline visual
    // parameters, excluding the vtable and the unknown +12E4..12EF gap.
    Copy<0x12C8, 0x1C>(live, saved); // rim-light height limits, color factor, power
    Copy<0x12F0, 0x10>(live, saved); // cut-in factor
    Copy<0x1300, 0x08>(live, saved); // delta coefficients; +1308 mpOwner live
    Copy<0x1310, 0x34>(live, saved); // animation requests, time/rate/wait/serial
    // cActorGameModelAnm: vtable +1348, resources +1360..1380,
    // owner +1380 and borrowed command cursors +1390..13B0 are not scalars.
    Copy<0x1350, 0x10>(live, saved); // animation numbers and execution flags
    bool sameResources = true;
    for (size_t offset = 0x1360; offset < 0x1380; offset += 8)
        sameResources &= Load<uint64_t>(live, offset) == Load<uint64_t>(saved, offset);
    if (sameResources) {
        Copy<0x1388, 0x30>(live, saved); // type/key and borrowed resource cursors
    }
    // +13B8..13BF remains unresolved. Interpreter +90 is character +13D8.
    // Restore saved null without guessing whether a nonzero value is a pointer.
    if (Load<uint64_t>(saved, 0x13D8) == 0) Copy<0x13D8, 8>(live, saved);
    Copy<0x13C0, 0x01>(live, saved); // mCngKey
    Copy<0x13C4, 0x10>(live, saved); // compare registers, jump count, return flags
    Copy<0x13E0, 0x08>(live, saved); // loop count/start frame
    Copy<0x1480, 0x08>(live, saved); // hip rate/offset, reapplied late
    Copy<0x14A0, 0x04>(live, saved); // constraint share flags
    // cChrNode +14B0..14EF is live intrusive topology.
    Copy<0x14F0, 0x40>(live, saved); // character/action/program/material flags
    Copy<0x1548, 0x18>(live, saved); // status mode, life/red-life and deltas
    Copy<0x1870, 0x01>(live, saved); // ability updated-this-move latch
    Copy<0x21D8, 0x560>(live, saved); // input history, excludes vtable and owner
    for (size_t message = 0x2C50; message < 0x2D70; message += 0x30) {
        Copy<0x14, 1>(live + message, saved + message); // cChrMessage stop gate
        // 140030AB0 queues impulse; 1400303C0 dispatches to mISpd/mIAcl/mIDir.
        Copy<0x18, 0x18>(live + message, saved + message); // flags, recipient/action, speed/accel/direction
    }
    Copy<0x2D80, 0x10>(live, saved); // per-character MtXorShift
    Copy<0x2D94, 0x04>(live, saved); // moveAfter counter, not setup/type ID
    Copy<0x2D9C, 0x2C>(live, saved); // player/team/member/body/assist settings
    // +2DC8..3630 resources and cRefs stay live. cFollowEyes' material
    // controller +36A0..3BEF is NOT a pointer-free byte array.
    Copy<0x3638, 0x64>(live, saved); // follow-eye angles, limits and speed
    // trans/applyPatternAnimation (14054A430/14054A960) consume selectors and
    // clocks. Preserve StringHandle ownership and require identical bindings.
    if (Load<uint64_t>(live, 0x36E0) == Load<uint64_t>(saved, 0x36E0) &&
        Load<uint64_t>(live, 0x100) == Load<uint64_t>(saved, 0x100) &&
        Load<uint64_t>(live, 0x110) == Load<uint64_t>(saved, 0x110)) {
        Copy<0x36E8, 1>(live, saved); // mEyeMatCtrl.mControlAllSlot
        for (size_t controller = 0x36F0; controller < 0x3BF0; controller += 0x28) {
            if (Load<uint64_t>(live, controller + 8) != Load<uint64_t>(saved, controller + 8) ||
                Load<uint64_t>(live, controller + 0x18) != Load<uint64_t>(saved, controller + 0x18))
                continue;
            Copy<0x10, 4>(live + controller, saved + controller); // mControlID
            Copy<0x20, 8>(live + controller, saved + controller); // mFrame, mSlot
        }
    }
    for (size_t helper : {size_t(0x3BF0), size_t(0x3D60)}) {
        Copy<0x68, 0x10>(live + helper, saved + helper); // uConstraint blend weight, ID, order/priority
        Copy<0x80, 0x10>(live + helper, saved + helper); // blend speed/value, ignore-rate/enable and scalar padding
    }
    Copy<0x3ED0, 0x04>(live, saved); // face-part selector, not MtArrays +3ED8
    // mWeaponNum is an allocation bound. Only rewind with matching storage.
    if (Load<uint64_t>(live, 0x3F40) == Load<uint64_t>(saved, 0x3F40))
        Copy<0x3F38, 0x04>(live, saved);
    Copy<0x3F48, 0x148>(live, saved); // animation requests, EP counts, combo IDs
    Copy<0x4090, 0x10>(live, saved); // final combo IDs, push selector/reserve ID
    // +40A0..40AF unknown, deliberately not inferred from constructor zeros.
    Copy<0x40B0, 0x13D>(live, saved); // movement, hit/block counters and flags
    Copy<0x41F0, 0x09>(live, saved); // super-armor stop and reserve time-scale
    Copy<0x41FC, 0x04>(live, saved); // team material-effect state
    Copy<0x4D10, 0x100>(live, saved); // capsule position history
    Copy<0x6340, 0xE8>(live, saved); // push vectors, collision sets, damage state
    // +6428 mpLastCreateShot is remapped after reconstruction.
    Copy<0x6430, 0x2C>(live, saved); // shot group/serial/hit latches
    // +6460 critical section, +6488 unknown, target references +6490 onward,
    // callbacks +6738/+6740, auxiliary owners +6748 and CPU +67F0 stay live.
    Copy<0x67F8, 0x16>(live, saved); // shot defense, voice selectors and latches
    Copy<0x6810, 0x11>(live, saved); // throw cooldowns and bone-draw flag
    Copy<0x6828, 0x09>(live, saved); // profile selectors, local-matrix enable
    Copy<0x6840, 0x80>(live, saved); // local override and post-transform matrices
    Copy<0x68C4, 0x02>(live, saved); // throw-link validation/height latches
    Copy<0x68C8, 0x14>(live, saved); // throw heights, charge/lifetime/stiff rates
    Copy<0x68E0, 0x10>(live, saved); // material-effect vector

    for (size_t command : {size_t(0x2738), size_t(0x2950)}) {
        uint8_t* dst = live + command;
        const uint8_t* src = saved + command;
        Copy<0x18, 0x120>(dst, src); // limit predicates; retain limit owner/vtable
        Copy<0x148, 0x18>(dst, src); // button charge payload; retain owner/vtable
        if (Load<uint64_t>(dst, 0x178) == Load<uint64_t>(src, 0x178))
            Copy<0x170, 0x04>(dst, src); // line count requires matching allocation
        if (Load<uint64_t>(dst, 0x168) == Load<uint64_t>(src, 0x168))
            Copy<0x180, 0x08>(dst, src); // successful entry borrowed from resource
        // 1400419C0 saves +19C into +1A0; anmchrCmd1 opcodes 3/4/7 read it.
        Copy<0x188, 0x1C>(dst, src); // successful IDs, delay/work, mSavedSucWork
        Copy<0x1A4, 0x40>(dst, src); // per-line usage counts
        // +1E4 unresolved; six callbacks at +1E8 remain live.
    }
}

void CommonLate(uint8_t* live, const uint8_t* saved)
{
    // These writes intentionally repeat early state across native reconstruction.
    for (size_t helper : {size_t(0x3BF0), size_t(0x3D60)}) {
        Copy<0x68, 0x10>(live + helper, saved + helper); // uConstraint blend/order state
        Copy<0x80, 0x10>(live + helper, saved + helper); // uConstraint blend/enable state
    }
    Copy<0x1480, 0x08>(live, saved);
    Copy<0x1518, 0x18>(live, saved);
    Copy<0x2AD8, 0x18>(live, saved); // combo matcher success scalars, no pointers
    Copy<0x3F84, 0x18>(live, saved);
    Copy<0x4D10, 0x100>(live, saved);
    Copy<0x6350, 0x20>(live, saved);
    Copy<0x6430, 0x2C>(live, saved);
    for (size_t record = 0x6490; record < 0x6710; record += 0x80)
        std::memcpy(live + record + 0x10, saved + record + 0x10, 0x70);
    Copy<0x67F8, 0x10>(live, saved);
    Copy<0x680D, 0x01>(live, saved);
    Copy<0x6810, 0x11>(live, saved);
    Copy<0x6828, 0x09>(live, saved);
    Copy<0x6840, 0x80>(live, saved);
    Copy<0x68C4, 0x02>(live, saved);
    Copy<0x68C8, 0x14>(live, saved);
    Copy<0x68E0, 0x10>(live, saved);
    // No universal +68F0 write: this is a pointer for many concrete classes.
}

struct TailRange { uint64_t dti; uint32_t offset; uint32_t size; };
// Reviewed constructor/consumer evidence in doc/chrfields.md, plus the Grace
// registry policy already in production. This is NOT the generated property
// export: inferred mWallStickTime fields can be low halves of owner pointers.
constexpr TailRange tails[] = {
    {0x140D44E10, 0x68F0, 4}, // Amaterasu ability
    {0x140D44E10, 0x6900, 4}, // magata
    {0x140D44E10, 0x6910, 1}, // install
    {0x140D44E48, 0x68F8, 0x18}, // Arthur flags and EX/head motion
    {0x140D44ED0, 0x68F0, 4}, // Chris mWallStickTime (roster field-pass ledger)
    {0x140D44F18, 0x68F0, 4}, // Chun-Li mWallStickTime; effect starts at +68F8
    {0x140D44F18, 0x6900, 8}, // Chun-Li charge ready/voice timer
    {0x140D44F60, 0x6908, 4}, // Viper Optic Laser charge
    {0x140D44FA0, 0x68FC, 8}, // Dante charge fields
    {0x140D44FE0, 0x68F0, 8}, // Deadpool teleport state
    {0x140D45020, 0x68F0, 0x10}, // Dormammu both stocks, flare counter, mEftUnitDelay
    {0x140D45020, 0x6920, 1}, // Stalking Flare lock
    {0x140D45068, 0x68F8, 4}, // Doom single-animation latch
    {0x140D450A8, 0x6920, 8}, // Strange Grace next slot/count
    {0x140D450A8, 0x6930, 0x0C}, // Grace ages; shot refs handled separately
    {0x140D450F0, 0x6920, 8}, // StrangeSh inherits Grace slot/count state
    {0x140D450F0, 0x6930, 0x0C}, // inherited Grace ages
    {0x140D450F0, 0x6950, 1}, // StrangeSh mbEnable
    {0x140D45180, 0x68F0, 1}, // FeliciaC move-active latch (tick 140080F50)
    {0x140D45180, 0x68F4, 4}, // FeliciaC family/type, initialized to 6
    {0x140D45180, 0x68F9, 1}, // FeliciaC parent defeat/install-expiry latch
    {0x140D451C0, 0x68F0, 8}, // FeliciaF mFamilyType and adjacent type POD
    {0x140D45200, 0x68F0, 1}, // Frank constructor's boolean, NOT a float wall timer
    {0x140D45200, 0x6990, 0x30}, // Frank Kobun scale/offset/rotation
    {0x140D45200, 0x69C4, 4}, // level
    {0x140D45200, 0x69DC, 4}, // experience
    {0x140D45200, 0x69EC, 8}, // weapon speed/throw ID
    {0x140D45200, 0x6A48, 9}, // throw/action wait timers, summon latch
    {0x140D45248, 0x68F0, 0x28}, // Galactus depth/life/damage/position/random quotas
    {0x140D45248, 0x6920, 0x30}, // mAbsoluteMove and two concrete vectors
    {0x140D452F0, 0x68F0, 2}, // Akuma tag/team switch flags
    {0x140D45338, 0x68F0, 8}, // Haggar wall timer and button-repeat count
    {0x140D45408, 0x68F0, 4}, // Hawkeye mWallStickTime
    {0x140D45408, 0x68FC, 0x18}, // Hawkeye standing-S motion tuning
    {0x140D45408, 0x6918, 4}, // standing-S charge
    {0x140D45450, 0x68F0, 4}, // Hiryu mWallStickTime, not option owner +68F8
    {0x140D45450, 0x6900, 1}, // Hiryu team-state latch
    {0x140D45450, 0x6920, 5}, // Ouroboros state/latch
    {0x140D45450, 0x6928, 4}, // voice timer
    {0x140D45488, 0x68F0, 0x10}, // Hulk reserved speed, rate/max, Tornado Rolling
    {0x140D45488, 0x6900, 1}, // Hulk Gamma Wave latch
    {0x140D454D0, 0x6900, 0x0C}, // Iron Fist chain counts and attack timer
    {0x140D45518, 0x68F8, 2}, // Iron Man effect latches
    {0x140D45578, 0x68F8, 1}, // Jill Feral install latch
    {0x140D455F8, 0x6900, 0x14}, // Hsien-Ko reserved state, scalar padding, base position
    {0x140D455F8, 0x6918, 8}, // pendulum length/state
    {0x140D45680, 0x68F0, 0x20}, // Magneto reviewed scalar/POD tail
    {0x140D456C0, 0x68F0, 8}, // Mayoi mButtobiLife and initial-life cache
    {0x140D45700, 0x68F0, 0x20}, // MODOK Psionic level/header and flight state
    // cChrTrace ctor/clear/update/recordRingSlot: +0 vtable and +8 source
    // stay live; +10 delay/clock, +18 animation IDs, +40/+140 vectors,
    // +240..59F flags, stun, motion and ability/request history are inline POD.
    {0x140D45788, 0x6900, 0x590}, // Morrigan shadow delayed gameplay history
    {0x140D457C0, 0x6918, 0x0D}, // Wright kept-item/reversal state
    {0x140D45890, 0x68F0, 0x60}, // Nemesis reviewed scalar/POD tail
    {0x140D458D0, 0x6908, 9}, // Nova Force/Centurion state
    {0x140D45910, 0x68F0, 8}, // Phoenix shot count and gauge consumption
    {0x140D45910, 0x6934, 4}, // Phoenix single-animation flag; +6920 cRef stays live
    {0x140D45958, 0x68F0, 4}, // Firebrand constructor's 32-bit state header
    {0x140D45958, 0x68F4, 1}, // Firebrand ability latch
    {0x140D45958, 0x68FC, 2}, // sync/install latches
    {0x140D45958, 0x6928, 4}, // flight-idle timer
    {0x140D45998, 0x68F0, 1}, // Firebrand shadow active latch, not a float timer
    {0x140D45998, 0x68F4, 4}, // shadow state
    {0x140D45998, 0x68F8, 2}, // shadow sync/install latches
    {0x140D45998, 0x6908, 1}, // shadow action latch
    {0x140D45998, 0x6938, 4}, // shadow timer; effect owner +6910 stays live
    {0x140D459D8, 0x6950, 2}, // Rocket airborne phase flags
    {0x140D459D8, 0x6954, 9}, // action/movement state, cleanup latch
    {0x140D459D8, 0x6960, 4}, // voice timer
    {0x140D45A10, 0x68F0, 0x24}, // Ryu Shinku rate/range/state and six speed/accel fields
    {0x140D45A10, 0x6920, 1}, // Ranbu latch; +6928 cRef stays live
    {0x140D45A10, 0x6938, 8}, // Ranbu voice state
    {0x140D45C58, 0x6909, 1}, // Sentinel drone lock
    {0x140D45C58, 0x690C, 9}, // drone phase/timer/latch
    {0x140D45CB8, 0x68F0, 4}, // She-Hulk mCrouchingTime
    {0x140D45CF8, 0x68F0, 4}, // Shuma attached-eyeball count, NOT wall timer
    {0x140D45D40, 0x68F0, 0x10}, // Spencer wire latch/animation/user flags
    {0x140D45D80, 0x68F0, 0x10}, // Spider-Man MS counters, anime flag, web target ID
    {0x140D45DD8, 0x6914, 4}, // Storm mLACount; owners occupy +68F0..6907
    {0x140D45E18, 0x68F0, 4}, // Skrull mWallStickTime
    {0x140D45E18, 0x6900, 0x14}, // Skrull rolling speed/inertia/frame/height/state
    {0x140D45E58, 0x68F0, 0x20}, // Taskmaster arrow/action state
    {0x140D45EA0, 0x6900, 1}, // Thor tornado latch
    {0x140D45EA0, 0x6904, 8}, // Thor speech time and stage
    {0x140D45EA0, 0x6910, 8}, // speech variant/latch
    {0x140D45ED8, 0x6910, 0x14}, // Trish shot position and single-animation flag
    {0x140D45F68, 0x68F0, 0x12}, // Vergil spiral/raid/storm sword occupancy flags
    {0x140D45F68, 0x6914, 0x0C}, // Vergil charge time/level and action state
    {0x140D45F68, 0x6930, 0x0C}, // vector
    {0x140D45FB0, 0x68F0, 1}, // Joe Shocking Pink latch
    {0x140D45FB0, 0x6900, 0x0C}, // vector
    {0x140D45FF8, 0x6908, 8}, // Wesker sunglasses life and life tracker
    {0x140D45FF8, 0x6920, 0x20}, // Wesker left/right/up/down aiming limits
    {0x140D46038, 0x68F8, 1}, // Wolverine Berserker latch
    {0x140D460C0, 0x68F0, 8}, // Zero charge time/level; effect starts at +68F8
    {0x140D460C0, 0x6910, 1}, // Zero Sogenmu latch
    {0x140D460C0, 0x6914, 8}, // Zero saber alpha and alpha delta
    {0x140D46118, 0x6900, 0x590}, // Zero shadow cChrTrace delayed gameplay history
    {0x140D46118, 0x6E90, 4}, // tick reads trace+5A0 dispatch latch, not +7090
    {0x140D46158, 0x68F0, 2}, // Zombie constructor's two latches, not wall timer
    {0x140D46158, 0x68F4, 8}, // Zombie timer/voice toggle
};

// These known layouts have no confirmed scalar tail in the property export.
// Their generated +68F0 dword is the low half of an owner, not a wall timer.
// Common state still needs restoration, while reference recipes own the tail.
constexpr uint64_t referenceOnlyLayouts[] = {
    0x140D44A30, // uCharacter common base
    0x140D44E90, // Captain America: Shield Slash owner
    0x140D45138, // Felicia: kitten-install owner
    0x140D452B0, // Ghost Rider: chain/Hellfire owners
    0x140D45748, // Morrigan: Astral Vision owner
    0x140D45F20, // Tron: live unit owner
    0x140D46080, // X-23: Trigger Install owner
};

static_assert([] {
    uint64_t previousDti = 0;
    size_t previousEnd = 0;
    for (const auto& range : tails) {
        if (range.offset < ROLLBACK_CHARACTER_COMMON_SIZE || !range.size)
            return false;
        if (range.dti < previousDti ||
            (range.dti == previousDti && range.offset < previousEnd))
            return false;
        previousDti = range.dti;
        previousEnd = size_t(range.offset) + range.size;
    }
    return true;
}());

} // namespace

CharacterRestoreStatus Rollback_RestoreCharacterFields(
    void* destination, size_t destinationSize, const uint8_t* saved,
    size_t savedSize, uint64_t dti, bool roster, CharacterRestorePhase phase)
{
    if (!destination || !saved || destinationSize < ROLLBACK_CHARACTER_COMMON_SIZE ||
        savedSize < ROLLBACK_CHARACTER_COMMON_SIZE)
        return CharacterRestoreStatus::InvalidBuffer;
    // Reject a truncated concrete tail before writing any common state.
    size_t first = 0, end = sizeof(tails) / sizeof(tails[0]);
    while (first < end) {
        const size_t middle = first + (end - first) / 2;
        if (tails[middle].dti < dti) first = middle + 1;
        else end = middle;
    }
    while (end < sizeof(tails) / sizeof(tails[0]) && tails[end].dti == dti)
        ++end;
    bool knownLayout = first != end;
    for (uint64_t referenceOnly : referenceOnlyLayouts)
        knownLayout |= referenceOnly == dti;
    // An unsupported DTI must not partially rewind even the common body.
    if (!knownLayout) return CharacterRestoreStatus::UnsupportedLayout;
    for (size_t i = first; i < end; ++i) {
        const auto& range = tails[i];
        if (range.offset > destinationSize || range.size > destinationSize - range.offset ||
            range.offset > savedSize || range.size > savedSize - range.offset)
            return CharacterRestoreStatus::InvalidBuffer;
    }
    auto* live = static_cast<uint8_t*>(destination);
    if (phase == CharacterRestorePhase::Early) CommonEarly(live, saved, roster);
    else CommonLate(live, saved);
    // Preserve the existing phase contract: tails were early-only except for
    // the old universal +68F0 dword. Reapply only its reviewed scalar portion,
    // never the reference at that offset in a different concrete layout.
    for (size_t i = first; i < end; ++i) {
        const auto& range = tails[i];
        if (phase == CharacterRestorePhase::Early)
            std::memcpy(live + range.offset, saved + range.offset, range.size);
        else if (range.offset == 0x68F0)
            std::memcpy(live + range.offset, saved + range.offset,
                        range.size < 4 ? range.size : 4);
    }
    return CharacterRestoreStatus::ReviewedSubset;
}
