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
// entry. Embedded reference/collision repair is in rollback_restore.cpp; no
// owner, vtable, lock or callback is copied here. The model prefix below +1450
// comes from the shared table in the header.
void Common(uint8_t* live, const uint8_t* saved, bool roster)
{
    Rollback_RestoreActorFields(live, saved, roster ? ActorRoster : ActorCharacter);
    Copy<0x1480, 0x08>(live, saved); // hip rate/offset
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
    // +6428 mpLastCreateShot is a weak unit reference: resolved by identity after
    // shot restore (RollbackUnit_ResolveUnitReference), never copied here.
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
    {0x140D44F60, 0x68F8, 1}, // Burning Kick spawn-attempt latch (14006D670)
    {0x140D44F60, 0x6908, 4}, // Viper Optic Laser charge
    {0x140D44FA0, 0x68F8, 1}, // Dante DT mode; collision consumer 140070A40
    {0x140D44FA0, 0x68FC, 8}, // Dante charge fields
    {0x140D44FA0, 0x6920, 2}, // effect transition and command eligibility latches
    {0x140D44FE0, 0x68F0, 8}, // Deadpool teleport state
    {0x140D44FE0, 0x68F8, 4}, // command flags (140074EB0)
    {0x140D44FE0, 0x6900, 0x30}, // three inline position records, not an MtArray
    {0x140D44FE0, 0x6930, 1}, // delayed action voice latch
    {0x140D45020, 0x68F0, 0x10}, // Dormammu both stocks, flare counter, mEftUnitDelay
    {0x140D45020, 0x6920, 1}, // Stalking Flare lock
    {0x140D45068, 0x68F8, 4}, // Doom single-animation latch
    {0x140D450A8, 0x6920, 8}, // Strange Grace next slot/count
    {0x140D450A8, 0x6928, 6}, // elapsed counter and two command latches
    {0x140D450A8, 0x6930, 0x0C}, // Grace ages; shot refs handled separately
    {0x140D450A8, 0x6948, 4}, // action-reset voice clock (14007AD00)
    {0x140D450F0, 0x6920, 8}, // StrangeSh inherits Grace slot/count state
    {0x140D450F0, 0x692C, 2}, // inherited command eligibility latches
    {0x140D450F0, 0x6930, 0x0C}, // inherited Grace ages
    {0x140D450F0, 0x6950, 1}, // StrangeSh mbEnable
    {0x140D45138, 0x68F0, 1}, // Felicia install latch (14007FBE0)
    {0x140D45180, 0x68F0, 1}, // FeliciaC move-active latch (tick 140080F50)
    {0x140D45180, 0x68F4, 4}, // FeliciaC family/type, initialized to 6
    {0x140D45180, 0x68F8, 2}, // turnaround and parent defeat/install-expiry latches
    {0x140D451C0, 0x68F0, 8}, // FeliciaF mFamilyType and adjacent type POD
    {0x140D45200, 0x68F0, 1}, // Frank constructor's boolean, NOT a float wall timer
    {0x140D45200, 0x6990, 0x30}, // Frank Kobun scale/offset/rotation
    {0x140D45200, 0x69C4, 4}, // level
    {0x140D45200, 0x69DC, 4}, // experience
    {0x140D45200, 0x69E0, 6}, // experience multiplier, install and award latches
    {0x140D45200, 0x69EC, 8}, // weapon speed/throw ID
    {0x140D45200, 0x6A00, 0x0C}, // cached positioning anchor (140083B50)
    {0x140D45200, 0x6A48, 9}, // throw/action wait timers, summon latch
    {0x140D45248, 0x68F0, 0x28}, // Galactus depth/life/damage/position/random quotas
    {0x140D45248, 0x6920, 0x30}, // mAbsoluteMove and two concrete vectors
    {0x140D452F0, 0x68F0, 2}, // Akuma tag/team switch flags
    {0x140D45338, 0x68F0, 8}, // Haggar wall timer and button-repeat count
    {0x140D45408, 0x68F0, 4}, // Hawkeye mWallStickTime
    {0x140D45408, 0x68F4, 4}, // mutable standing-S range minimum (1400C8A50)
    {0x140D45408, 0x68FC, 0x18}, // Hawkeye standing-S motion tuning
    {0x140D45408, 0x6918, 4}, // standing-S charge
    {0x140D45450, 0x68F0, 4}, // Hiryu mWallStickTime, not option owner +68F8
    {0x140D45450, 0x6900, 1}, // Hiryu team-state latch
    {0x140D45450, 0x6910, 0x0C}, // cached position (140092E70), not an MtArray
    {0x140D45450, 0x6920, 5}, // Ouroboros state/latch
    {0x140D45450, 0x6928, 4}, // voice timer
    {0x140D45488, 0x68F0, 0x10}, // Hulk reserved speed, rate/max, Tornado Rolling
    {0x140D45488, 0x6900, 1}, // Hulk Gamma Wave latch
    {0x140D45488, 0x6904, 4}, // elapsed-animation checkpoint (140095C70)
    {0x140D454D0, 0x6900, 0x0C}, // Iron Fist chain counts and attack timer
    {0x140D454D0, 0x6910, 0x0C}, // cached position (140097D40), not an MtArray
    {0x140D45518, 0x68F8, 2}, // Iron Man effect latches
    {0x140D45518, 0x6910, 0x0C}, // cached position (14009A590)
    {0x140D45518, 0x6928, 1}, // command eligibility latch
    {0x140D45578, 0x68F8, 1}, // Jill Feral install latch
    {0x140D45578, 0x6908, 1}, // previous move-active edge latch
    {0x140D455F8, 0x68FC, 4}, // reserved Anki variant (1400D46A0)
    {0x140D455F8, 0x6900, 0x18}, // position, pendulum length, pending action
    // Hsien-Ko +6918 is Linlin; +6920 is a sound cRef. Neither is POD.
    {0x140D45680, 0x68F0, 0x20}, // Magneto reviewed scalar/POD tail
    {0x140D456C0, 0x68F0, 8}, // Mayoi mButtobiLife and initial-life cache
    {0x140D45700, 0x68F0, 0x20}, // MODOK Psionic level/header and flight state
    {0x140D45748, 0x68F8, 1}, // Morrigan command eligibility (1400AC470)
    // cChrTrace ctor/clear/update/recordRingSlot: +0 vtable and +8 source
    // stay live; +10 delay/clock, +18 animation IDs, +40/+140 vectors,
    // +240..59F flags, stun, motion and ability/request history are inline POD.
    {0x140D45788, 0x6900, 0x590}, // Morrigan shadow delayed gameplay history
    {0x140D45788, 0x6E90, 1}, // shadow continuation latch (1400AE4A0)
    {0x140D457C0, 0x6918, 0x0D}, // Wright kept-item/reversal state
    {0x140D457C0, 0x6940, 2}, // UI notification and voice-once latches
    {0x140D45890, 0x68F0, 0x60}, // Nemesis reviewed scalar/POD tail
    {0x140D458D0, 0x6908, 9}, // Nova Force/Centurion state
    {0x140D45910, 0x68F0, 8}, // Phoenix shot count and gauge consumption
    {0x140D45910, 0x6930, 1}, // resurrection-used latch; status consumer 140031FDB
    {0x140D45910, 0x6934, 4}, // Phoenix single-animation flag; +6920 cRef stays live
    {0x140D45958, 0x68F0, 4}, // Firebrand constructor's 32-bit state header
    {0x140D45958, 0x68F4, 1}, // Firebrand ability latch
    {0x140D45958, 0x68F8, 4}, // elapsed-animation checkpoint (1400BEC90)
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
    {0x140D45A10, 0x6914, 4}, // directional mode (140090BB0 -> 1400C5F90)
    {0x140D45A10, 0x6920, 1}, // Ranbu latch; +6928 cRef stays live
    {0x140D45A10, 0x6938, 8}, // Ranbu voice state
    {0x140D45C58, 0x6908, 2}, // command eligibility and move-active latches
    {0x140D45C58, 0x690C, 9}, // drone phase/timer/latch
    {0x140D45CB8, 0x68F0, 4}, // She-Hulk mCrouchingTime
    {0x140D45CF8, 0x68F0, 4}, // Shuma attached-eyeball count, NOT wall timer
    {0x140D45CF8, 0x6928, 4}, // material-animation selector (1400CD8BD)
    {0x140D45CF8, 0x7494, 0x10}, // material flags, clock, end time, delta coefficient
    {0x140D45CF8, 0x74A8, 1}, // material-animation repeat byte
    {0x140D45D40, 0x68F0, 0x10}, // Spencer wire latch/animation/user flags
    {0x140D45D80, 0x68F0, 0x10}, // Spider-Man MS counters, anime flag, web target ID
    {0x140D45DD8, 0x6908, 2}, // Storm effect spawn-attempt latches
    {0x140D45DD8, 0x690C, 0x11}, // motion selector, drift, LA count, RNG voice clock, command latch
    {0x140D45DD8, 0x6920, 0x0E}, // target movement floats and phase latches
    {0x140D45E18, 0x68F0, 0x0C}, // Skrull inline movement vector, not a wall timer
    {0x140D45E18, 0x6900, 0x14}, // Skrull rolling speed/inertia/frame/height/state
    {0x140D45E58, 0x68F0, 0x20}, // Taskmaster arrow/action state
    {0x140D45EA0, 0x6900, 1}, // Thor tornado latch
    {0x140D45EA0, 0x6904, 8}, // Thor speech time and stage
    {0x140D45EA0, 0x690C, 1}, // command eligibility latch
    {0x140D45EA0, 0x6910, 8}, // speech variant/latch
    {0x140D45ED8, 0x6910, 0x14}, // Trish shot position and single-animation flag
    {0x140D45F68, 0x68F0, 0x12}, // Vergil spiral/raid/storm sword occupancy flags
    {0x140D45F68, 0x6910, 1}, // DT latch: outside the 18-byte sword bank
    {0x140D45F68, 0x6914, 0x0C}, // Vergil charge time/level and action state
    {0x140D45F68, 0x6930, 0x0C}, // vector
    {0x140D45FB0, 0x68F0, 1}, // Joe Shocking Pink latch
    {0x140D45FB0, 0x6900, 0x0C}, // vector
    {0x140D45FF8, 0x68F0, 0x0C}, // Wesker teleport position (1400EEF50)
    {0x140D45FF8, 0x6908, 8}, // Wesker sunglasses life and life tracker
    {0x140D45FF8, 0x6920, 0x20}, // Wesker left/right/up/down aiming limits
    {0x140D46038, 0x68F8, 1}, // Wolverine Berserker latch
    {0x140D46038, 0x6900, 0x0C}, // cached position (1400F0F60)
    {0x140D46080, 0x68F8, 1}, // X-23 command eligibility latch
    {0x140D460C0, 0x68F0, 8}, // Zero charge time/level
    {0x140D460C0, 0x68F8, 1}, // pending charge reset: 1400F7AA0 reads, 1400F8089 sets
    {0x140D460C0, 0x6910, 1}, // Zero Sogenmu latch
    {0x140D460C0, 0x6914, 8}, // Zero saber alpha and alpha delta
    {0x140D460C0, 0x6928, 1}, // command eligibility: 1400F80A0 writes, 140099E20 reads
    {0x140D46118, 0x6900, 0x590}, // Zero shadow cChrTrace delayed gameplay history
    {0x140D46118, 0x6E90, 4}, // tick reads trace+5A0 dispatch latch, not +7090
    {0x140D46158, 0x68F0, 2}, // Zombie constructor's two latches, not wall timer
    {0x140D46158, 0x68F4, 8}, // Zombie timer/voice toggle
    {0x140D46158, 0x6900, 0x0C}, // previous-parent position
    {0x140D46158, 0x6910, 4}, // previous-parent action
};

// These known layouts have no confirmed scalar tail in the property export.
// Their generated +68F0 dword is the low half of an owner, not a wall timer.
// Common state still needs restoration, while reference recipes own the tail.
constexpr uint64_t referenceOnlyLayouts[] = {
    0x140D44A30, // uCharacter common base
    0x140D44E90, // Captain America: Shield Slash owner
    0x140D452B0, // Ghost Rider: chain/Hellfire owners
    0x140D45F20, // Tron: live unit owner
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

std::span<const uint32_t> Rollback_CharacterShotTailOffsets(uint64_t dti)
{
    static constexpr uint32_t oneSpark[] = {0x6908};
    static constexpr uint32_t webOrCarpet[] = {0x6900};
    static constexpr uint32_t barriers[] = {0x68F8, 0x6900};
    static constexpr uint32_t phoenix[] = {0x68F8, 0x6900, 0x6908};
    static constexpr uint32_t modok[] = {0x6910, 0x6918};
    static constexpr uint32_t eyes[] = {0x68F8, 0x6900, 0x6908, 0x6910, 0x6918, 0x6920};
    // uDoctorDoom__tick (140079EB0) alive-checks its missile emplacement here;
    // Captain America keeps his Shield Slash option shot in the same slot.
    static constexpr uint32_t emplacement[] = {0x68F0};
    // uRocketRaccoon__retireFixedShotSlot (1400C49C0): four tracked trap shots.
    static constexpr uint32_t traps[] = {0x6908, 0x6910, 0x6918, 0x6920};
    switch (dti) {
    case 0x140D44E10: return oneSpark;
    case 0x140D44E90: case 0x140D45068: return emplacement;
    case 0x140D459D8: return traps;
    case 0x140D45020: case 0x140D45D80: return webOrCarpet;
    case 0x140D45700: return modok;
    case 0x140D45910: return phoenix;
    case 0x140D45CF8: return eyes;
    case 0x140D45ED8: return barriers;
    default: return {};
    }
}

CharacterRestoreStatus Rollback_RestoreAuxiliaryActor(
    void* destination, size_t destinationSize, const uint8_t* saved,
    size_t savedSize, uint64_t dti)
{
    if (dti != 0x140D455B0 && dti != 0x140D45638)
        return CharacterRestoreStatus::UnsupportedLayout;
    if (!destination || !saved || destinationSize < 0x14D0 || savedSize < 0x14D0)
        return CharacterRestoreStatus::InvalidBuffer;
    auto* live = static_cast<uint8_t*>(destination);
    // The owning fighter slot and vtable are validated by integration. These
    // actors have no character constraints/ability at the same offsets. King
    // Kobun factories hide and position this retained actor, so its visibility
    // and pose are in its table rows.
    Rollback_RestoreActorFields(live, saved, ActorAuxiliary);
    Copy<0x1470, 9>(live, saved); // horizontal velocity, acceleration, crossing latch
    Copy<0x147C, 9>(live, saved); // vertical equivalents; +1488 parent stays live
    Copy<0x1490, 0x3C>(live, saved); // tuning, timers, state and parent-action history
    Copy<0x14CD, 1>(live, saved); // animation-completion latch
    if (dti == 0x140D45638) Copy<0x14CE, 1>(live, saved); // Linlin active latch
    return CharacterRestoreStatus::ReviewedSubset;
}

std::span<const uint32_t> Rollback_CharacterEffectTailOffsets(uint64_t dti)
{
    static constexpr uint32_t first[] = {0x68F0};
    static constexpr uint32_t second[] = {0x68F8};
    static constexpr uint32_t third[] = {0x6900};
    static constexpr uint32_t viper[] = {0x68F0, 0x6900};
    static constexpr uint32_t dante[] = {0x68F0, 0x6908, 0x6910, 0x6918};
    static constexpr uint32_t dormammu[] = {0x6908, 0x6910, 0x6918};
    static constexpr uint32_t frank[] = {0x6A10, 0x6A18, 0x6A20};
    static constexpr uint32_t ghost[] = {0x68F0, 0x68F8, 0x6900, 0x6908, 0x6910};
    static constexpr uint32_t ironMan[] = {0x68F0, 0x6900, 0x6920};
    static constexpr uint32_t wright[] = {0x6928};
    static constexpr uint32_t modok[] = {0x6920};
    static constexpr uint32_t phoenix[] = {0x6910};
    // Twelve unit slots at +68F0..+6948; +6908..+6920 are the trap shots.
    static constexpr uint32_t rocket[] = {0x68F0, 0x68F8, 0x6900, 0x6928, 0x6930, 0x6938, 0x6940, 0x6948};
    static constexpr uint32_t ryu[] = {0x6918};
    static constexpr uint32_t storm[] = {0x68F0, 0x68F8, 0x6900};
    static constexpr uint32_t thor[] = {0x68F0, 0x68F8};
    static constexpr uint32_t vergil[] = {0x6908, 0x6920};
    static constexpr uint32_t zero[] = {0x68F8, 0x6900, 0x6908};
    // Found by scanning character code for tail fields passed to the native
    // alive test or assigned from sGameEffect spawn (140131660):
    // uDrStrange__updatePersistentSpellEffects (14007B9E0), uIronFist__tick
    // (140097740), uNova__updateActionEffects (1400B91E0), uSentinel__tick
    // (1400CA240), uWesker__tick (1400EDF80). Nova's three effects, Trish's
    // Low Voltage effect, Zero's +68F8 and Rocket's remaining slots come from
    // the constructor audit in the removed doc/chrfields.md (git 7c19da4^).
    static constexpr uint32_t strange[] = {0x68F0, 0x68F8, 0x6900};
    static constexpr uint32_t ironFist[] = {0x68F0, 0x68F8};
    static constexpr uint32_t sentinel[] = {0x68F0, 0x6900};
    switch (dti) {
    case 0x140D450A8: case 0x140D458D0: return strange; // Doctor Strange, Nova
    case 0x140D454D0: return ironFist;
    case 0x140D45FF8: return third; // Wesker
    case 0x140D45C58: return sentinel;
    case 0x140D44E10: case 0x140D44F18: case 0x140D45450: return second;
    case 0x140D44E48: case 0x140D45ED8: case 0x140D46038: case 0x140D46080: return first;
    case 0x140D44F60: case 0x140D45578: return viper;
    case 0x140D44FA0: return dante;
    case 0x140D45020: return dormammu;
    case 0x140D45200: return frank;
    case 0x140D452B0: return ghost;
    case 0x140D45518: return ironMan;
    case 0x140D45700: return modok;
    case 0x140D457C0: return wright;
    case 0x140D45910: case 0x140D45998: case 0x140D45E58: return phoenix;
    case 0x140D45958: return third;
    case 0x140D459D8: return rocket;
    case 0x140D45A10: return ryu;
    case 0x140D45DD8: return storm;
    case 0x140D45EA0: return thor;
    case 0x140D45F68: return vergil;
    case 0x140D460C0: return zero;
    default: return {};
    }
}

std::span<const uint32_t> Rollback_CharacterHelperTailOffsets(uint64_t dti)
{
    static constexpr uint32_t first[] = {0x68F0};
    static constexpr uint32_t linlin[] = {0x6918};
    static constexpr uint32_t mayoi[] = {0x6910};
    static constexpr uint32_t zero[] = {0x6920};
    static constexpr uint32_t photos[] = {0x6A28, 0x6A30, 0x6A38};
    static constexpr uint32_t source[] = {0x6900};
    switch (dti) {
    case 0x140D45200: return photos;
    case 0x140D455F8: return linlin;
    case 0x140D45748: case 0x140D45F20: return first;
    case 0x140D457C0: return mayoi;
    case 0x140D45998: return source;
    case 0x140D460C0: return zero;
    default: return {};
    }
}

CharacterRestoreStatus Rollback_RestoreCharacterFields(
    void* destination, size_t destinationSize, const uint8_t* saved,
    size_t savedSize, uint64_t dti, bool roster)
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
    Common(live, saved, roster);
    if (dti == 0x140D45CF8 &&
        Load<uint64_t>(live, 0x6970) == Load<uint64_t>(saved, 0x6970)) {
        // Embedded uMaterialVjobAnimation at +6930. Preserve its target,
        // controller parents and StringHandles; only rewind matching bindings.
        Copy<0x6978, 1>(live, saved);
        for (size_t controller = 0x6980; controller < 0x6E80; controller += 0x28) {
            if (Load<uint64_t>(live, controller + 8) != Load<uint64_t>(saved, controller + 8) ||
                Load<uint64_t>(live, controller + 0x18) != Load<uint64_t>(saved, controller + 0x18))
                continue;
            Copy<0x10, 4>(live + controller, saved + controller);
            Copy<0x20, 8>(live + controller, saved + controller);
        }
    }
    for (size_t helper : {size_t(0x3BF0), size_t(0x3D60)}) {
        // Constructor 14010B3B0 uses state 0 + updateLockAngle; setter
        // 14010CBE0 uses state 0 + updateFromReference. State alone cannot
        // reconstruct the callback. Only accept the two native implementations.
        const uint64_t callback = Load<uint64_t>(saved, helper + 0x128);
        if (callback == 0x14010BB40 || callback == 0x14010BC20)
            Copy<0x128, 8>(live + helper, saved + helper);
    }
    for (size_t i = first; i < end; ++i)
        std::memcpy(live + tails[i].offset, saved + tails[i].offset, tails[i].size);
    return CharacterRestoreStatus::ReviewedSubset;
}
