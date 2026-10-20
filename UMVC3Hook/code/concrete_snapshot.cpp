
// "Concrete" just means the bytes past the base class of uCharacter / uShot

#include "../pch.h"

#include "concrete_snapshot.h"
#include "mk10utils.h"
#include "rollback_memory.h"
#include "rollback_model_policy.h"
#include "rollback_profile.h"

#include <array>
#include <cmath>
#include <cstring>

extern void HookLog(const char* fmt, ...);

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
constexpr uintptr_t kLegionTigerDti = 0x140D4ABB8ull;
constexpr uintptr_t kRepulsorBlastSpDti = 0x140D4ADA0ull;
constexpr uintptr_t kDogMissileDti = 0x140D4B888ull;
constexpr uintptr_t kFireGrenadeExplodeDti = 0x140D48BB0ull;
constexpr uintptr_t kItemThrowDti = 0x140D4B980ull;
constexpr uintptr_t kRocketLauncherExplodeDti = 0x140D4BAC8ull;
constexpr uintptr_t kElementalSummonShotDti = 0x140D49DF8ull;
constexpr uintptr_t kVulcanAxeDti = 0x140D48808ull;
constexpr uintptr_t kShikiDanKobunDti = 0x140D4D3D0ull;
constexpr uintptr_t kAttackWireDti = 0x140D4CA20ull;
constexpr uintptr_t kJamSessionDti = 0x140D49288ull;
constexpr uintptr_t kInfernoDti = 0x140D49790ull;
constexpr uintptr_t kShockingPinkBombDti = 0x140D4D6A0ull;
constexpr uintptr_t kOilBalloonGroundOilDti = 0x140D4C478ull;
constexpr uintptr_t kDarkPhoenixFireDti = 0x140D4BDA0ull;
constexpr uintptr_t kVulcanSwingWireDti = 0x140D4CB00ull;
constexpr uintptr_t kVulcanBoomerangDti = 0x140D4D660ull;

uint64_t GetDti(void* object);

constexpr uintptr_t kShotLaserDti = 0x140D482E8ull;
constexpr uintptr_t kEffectDti = 0x140E18698ull;
constexpr uintptr_t kDtiIsKindOf = 0x1400039D0ull;

struct SnapshotRange {
    size_t offset;
    size_t size;
};

struct DtiSnapshotRange {
    uintptr_t dti;
    size_t offset;
    size_t size;
};

constexpr DtiSnapshotRange kGeneratedShotTailRanges[] = {
#include "generated_shot_tail_fields.inc"
};

// Scalar concrete-tail fields verified from constructors and gameplay virtuals
// in Ghidra but absent from the generated createProperty export. Keep these
// separate from the generated file and reapply them after ownership filtering.
constexpr DtiSnapshotRange kVerifiedShotScalarRanges[] = {
    // Target counts, three gem-slot indices, and selected target (native setup
    // 0x140173FD0 / move 0x140173870). Packed {0, 1} resembles a pointer;
    // retaining factory zero loses the gem encounter (frame 2810 -> 2824).
    {kElementalSummonShotDti, 0x21B0, 0x18},
    {0x140D49A08ull, 0x2140, 0x04}, // uShotMissileDoomsMissile: flight parameter
    {0x140D4CA20ull, 0x2140, 0x03}, // uShotOptionAttackWire: state flags
    {0x140D4CA20ull, 0x2144, 0x08}, // uShotOptionAttackWire: mode/timer
    {0x140D49250ull, 0x2140, 0x01}, // uShotOptionGrapple: attached latch
    {0x140D4BD00ull, 0x2140, 0x04}, // uShotOptionHealingFieldGathering: range
    {0x140D4D430ull, 0x21D8, 0x04}, // uShotOptionRoundTrip: owner orientation
    {0x140D4D430ull, 0x22A4, 0x04}, // uShotOptionRoundTrip: owner rotation
    {0x140D4D430ull, 0x22E0, 0x05}, // uShotOptionRoundTrip: rotation/side
    {0x140D48780ull, 0x21C8, 0x04}, // uShotOptionSickleBoomerang: owner rotation
    {0x140D4D118ull, 0x21D0, 0x0C}, // uShotOptionSpadaBoomerang: primary owner rotation
    {0x140D4D118ull, 0x22A0, 0x0C}, // uShotOptionSpadaBoomerang: secondary owner rotation
    {0x140D4D118ull, 0x22E0, 0x20}, // uShotOptionSpadaBoomerang: phase/timing
    {0x140D4CA60ull, 0x2148, 0x08}, // uShotOptionSwingWire: outbound timing
    {0x140D4CA60ull, 0x2150, 0x01}, // uShotOptionSwingWire: outbound latch
    {0x140D4CA60ull, 0x2154, 0x08}, // uShotOptionSwingWire: return timing
    {0x140D4CA60ull, 0x215C, 0x01}, // uShotOptionSwingWire: return latch
    {0x140D4CF60ull, 0x2160, 0x01}, // uShotLaserBigThunder: tail flag
    {0x140D490D0ull, 0x2160, 0x04}, // uShotLaserCrystal: effect variant
    {0x140D4BF38ull, 0x2170, 0x10}, // uShotLaserDarkFire: effect position
    {0x140D49988ull, 0x2230, 0x04}, // uShotLaserDrDoomJS: tail state
    {0x140D4BB08ull, 0x2240, 0x10}, // uShotLaserGMBeam: tail vector
    {0x140D4C870ull, 0x2168, 0x01}, // uShotLaserHyperMysticBlast: update gate
    {0x140D4C870ull, 0x216C, 0x04}, // uShotLaserHyperMysticBlast: state
    {0x140D49118ull, 0x2160, 0x07}, // uShotLaserMillionCarat: timer/latches
    {0x140D4C8B8ull, 0x2168, 0x04}, // uShotLaserMysticBlast: setup scalar
    {0x140D499C8ull, 0x2230, 0x04}, // uShotLaserPlasmaBeam: phase
    {0x140D4AD20ull, 0x2230, 0x04}, // uShotLaserProtonCannon: mode
    {0x140D4C5B0ull, 0x23D0, 0x12}, // uShotLaserReflectSinkuHadouken: reflected state
    {0x140D4D7E0ull, 0x2314, 0x08}, // uShotLaserRekkouha: timer/spawn phase
    {0x140D48AA8ull, 0x2160, 0x01}, // uShotLaserSatelliteLaser: one-shot latch
    {0x140D4C5F0ull, 0x2300, 0x04}, // uShotLaserSinkuHadouken: model offset
    {0x140D49168ull, 0x2238, 0x01}, // uShotLaserThunderBolt: effect latch
    {0x140D4BB90ull, 0x2140, 0x04}, // uShotShieldBodyBeam: collision phase
    {0x140D4A920ull, 0x2140, 0x0D}, // uShotShieldFormationD: phase/motion
    {0x140D4BD40ull, 0x2140, 0x0C}, // uShotShieldHealingField: phase/healing
    {0x140D48F70ull, 0x2140, 0x04}, // uShotShieldKikousyou: collision phase
    {0x140D4A9A8ull, 0x2140, 0x05}, // uShotShieldOuroborosExit: phase/side
    {0x140D48A68ull, 0x2144, 0x0C}, // uShotOptionShieldSlash: rotation state
    {0x140D48780ull, 0x2218, 0x04}, // uShotOptionSickleBoomerang: effect variant
    {0x140D4A968ull, 0x21D0, 0x04}, // uShotShieldOuroboros: orbit rotation
    {0x140D4A968ull, 0x2210, 0x04}, // uShotShieldOuroboros: instance mode
    {0x140D4A968ull, 0x2218, 0x01}, // uShotShieldOuroboros: initial-shot latch
    {0x140D4A968ull, 0x221C, 0x04}, // uShotShieldOuroboros: child count
    {0x140D4A968ull, 0x2220, 0x01}, // uShotShieldOuroboros: owner-state latch
    {0x140D4D470ull, 0x2148, 0x10}, // uShotShieldRaidSword: mode/rotation/timer
    {0x140D4D4C8ull, 0x2214, 0x04}, // uShotShieldSpiralSword: update counter
    {0x140D4D4C8ull, 0x21D4, 0x08}, // uShotShieldSpiralSword: owner rotation/orientation
    {0x140D4D508ull, 0x21D4, 0x04}, // uShotShieldStormSword: owner rotation
    {0x140D4D508ull, 0x2210, 0x06}, // uShotShieldStormSword: index/flags
    {0x140D4CDE0ull, 0x2140, 0x04}, // uShotShieldSuperBurningSkrull: phase
    {0x140D4A098ull, 0x2140, 0x08}, // uShotShieldWeaponCartSpread: variant/time
    {0x140D49310ull, 0x2140, 0x08}, // uShotVulcanAcidRain: spawn state
    {0x140D4AF00ull, 0x21D0, 0x10}, // uShotVulcanAnkihouBomb: owner transform
    {0x140D4AF00ull, 0x2204, 0x04}, // uShotVulcanAnkihouBomb: owner state
    {0x140D4AF00ull, 0x2210, 0x04}, // uShotVulcanAnkihouBomb: phase/variant
    {0x140D4AF00ull, 0x2220, 0x30}, // uShotVulcanAnkihouBomb: trajectory/timing
    {0x140D4AFE0ull, 0x21D0, 0x0C}, // uShotVulcanAnkihouRolling: owner trajectory
    {0x140D4AFE0ull, 0x2210, 0x20}, // uShotVulcanAnkihouRolling: phase/timing
    {0x140D4AFE0ull, 0x2230, 0x01}, // uShotVulcanAnkihouRolling: transition latch
    {0x140D4CEC8ull, 0x2140, 0x05}, // uShotVulcanArrowShot: phase/flag
    {0x140D48808ull, 0x21D0, 0x04}, // uShotVulcanAxe: rotation accumulator
    {0x140D4D178ull, 0x2140, 0x04}, // uShotVulcanBarrier: phase
    {0x140D4D1C8ull, 0x2140, 0x04}, // uShotVulcanBarrierSpread: phase
    {0x140D4D1C8ull, 0x2150, 0x10}, // uShotVulcanBarrierSpread: target position
    {0x140D4A588ull, 0x2140, 0x01}, // uShotVulcanBombArrow: exploded latch
    {0x140D4A588ull, 0x2148, 0x08}, // uShotVulcanBombArrow: timer/subtype
    {0x140D4A588ull, 0x2150, 0x08}, // uShotVulcanBombArrow: trajectory state
    {0x140D4A5E8ull, 0x2140, 0x04}, // uShotVulcanBombArrowExplode: mode
    {0x140D4D660ull, 0x21D4, 0x08}, // uShotVulcanBoomerang: owner rotation
    {0x140D4D660ull, 0x2210, 0x26}, // uShotVulcanBoomerang: homing/return state
    {0x140D49398ull, 0x21D8, 0x04}, // uShotVulcanBulletDanceFinish: owner rotation
    {0x140D4D268ull, 0x2148, 0x01}, // uShotVulcanChushokuRash: tail flag
    {0x140D4C080ull, 0x2140, 0x0D}, // uShotVulcanClaymoreMine: activation state
    {0x140D4C080ull, 0x2150, 0x04}, // uShotVulcanClaymoreMine: detonation timer
    {0x140D488B8ull, 0x2150, 0x10}, // uShotVulcanCrossSword: homing direction
    {0x140D4C100ull, 0x2140, 0x01}, // uShotVulcanDigHole: flag
    {0x140D4C100ull, 0x2144, 0x04}, // uShotVulcanDigHole: state
    {0x140D4C6E0ull, 0x21D0, 0x0C}, // uShotVulcanDirectAttackBomb: owner transform
    {0x140D4B888ull, 0x2140, 0x04}, // uShotVulcanDogMissile: variant
    {0x140D4D220ull, 0x2140, 0x15}, // uShotVulcanEnergyBolt: direction/speed/flag
    {0x140D4BBD8ull, 0x2140, 0x01}, // uShotVulcanEnergySpear: spawn latch
    {0x140D4BC18ull, 0x2140, 0x02}, // uShotVulcanEnergySpearSpread: trigger flags
    {0x140D4BA48ull, 0x2140, 0x01}, // uShotVulcanExatkRocketLauncher: spawn latch
    {0x140D4BA48ull, 0x2144, 0x04}, // uShotVulcanExatkRocketLauncher: life limit
    {0x140D4B658ull, 0x2140, 0x18}, // uShotVulcanFinishingShower: state/timing
    {0x140D4B658ull, 0x2160, 0x10}, // uShotVulcanFinishingShower: vector
    {0x140D4BF78ull, 0x2140, 0x04}, // uShotVulcanFire: effect variant
    {0x140D48BB0ull, 0x21D0, 0x04}, // uShotVulcanFireGrenadeBomb: rotation
    {0x140D48BB0ull, 0x2218, 0x01}, // uShotVulcanFireGrenadeBomb: stop latch
    {0x140D4BFB8ull, 0x2144, 0x08}, // uShotVulcanFirePillar: state/vibration
    {0x140D4AA70ull, 0x2140, 0x01}, // uShotVulcanFormationCBom: spawn latch
    {0x140D4AAF0ull, 0x2140, 0x01}, // uShotVulcanFormationCSpread: spawn latch
    {0x140D4BC60ull, 0x2140, 0x04}, // uShotVulcanGMPulse: variant
    {0x140D4AC98ull, 0x2140, 0x14}, // uShotVulcanGammaSlam: movement state
    {0x140D48900ull, 0x214C, 0x08}, // uShotVulcanGreatSorcery: event state
    {0x140D48CD0ull, 0x21D0, 0x04}, // uShotVulcanHandGrenadeBomb: owner angle
    {0x140D48CD0ull, 0x2214, 0x04}, // uShotVulcanHandGrenadeBomb: state/count
    {0x140D4B020ull, 0x2140, 0x04}, // uShotVulcanHenkyouKi: state
    {0x140D4B228ull, 0x2140, 0x01}, // uShotVulcanHyperGravitation: setup flag
    {0x140D4C918ull, 0x2140, 0x09}, // uShotVulcanHyperMysticSmash: flags/state
    {0x140D4C918ull, 0x21E4, 0x04}, // uShotVulcanHyperMysticSmash: owner gameplay float
    {0x140D4C290ull, 0x2140, 0x0C}, // uShotVulcanHyperTrap: variant/flag/timer
    {0x140D484D0ull, 0x21D8, 0x04}, // uShotVulcanHyoran: owner angle
    {0x140D4B980ull, 0x2230, 0x04}, // uShotVulcanItemThrow: item type
    {0x140D4D550ull, 0x2140, 0x0C}, // uShotVulcanJigenZan: timer/count/variant
    // Dormammu f942: horizontal drift diverges before f943 position/rotation.
    // Reapply after ownership filtering; vfn124 consumes this setup-randomized speed.
    {0x140D49898ull, 0x2140, 0x04}, // uShotVulcanMagicMagmaSpread: horizontal drift
    {0x140D4BA90ull, 0x2140, 0x01}, // uShotVulcanNemesisRocketLauncher: spawn latch
    {0x140D4BA90ull, 0x2144, 0x04}, // uShotVulcanNemesisRocketLauncher: life limit
    {0x140D4A6B0ull, 0x2140, 0x01}, // uShotVulcanNetArrow: deferred-target flag
    {0x140D49E78ull, 0x2140, 0x01}, // uShotVulcanOpticalMagic: state flag
    {0x140D49EB8ull, 0x2140, 0x01}, // uShotVulcanOpticalMagicS: spawn latch
    {0x140D4C4C8ull, 0x2140, 0x01}, // uShotVulcanPendulumLog: trigger latch
    {0x140D4C4C8ull, 0x2144, 0x04}, // uShotVulcanPendulumLog: state
    {0x140D49FC8ull, 0x2140, 0x0A}, // uShotVulcanPleaseHelpMe: state/timer/flags
    {0x140D4A788ull, 0x2140, 0x08}, // uShotVulcanPoisonArrow: install parameters
    {0x140D485E0ull, 0x2140, 0x01}, // uShotVulcanRaikou: one-shot latch
    {0x140D4A7E8ull, 0x2140, 0x05}, // uShotVulcanRangingBladeArrow: timer/latch
    {0x140D4D350ull, 0x21D4, 0x04}, // uShotVulcanRockThrow: owner rotation
    {0x140D48EE8ull, 0x21D4, 0x04}, // uShotVulcanSatelliteLaserLock: owner rotation
    {0x140D4B6B0ull, 0x2140, 0x08}, // uShotVulcanShilhouetteBlade: phase/time
    {0x140D4C998ull, 0x2140, 0x18}, // uShotVulcanMysticStareBomb: phase/timing
    {0x140D48EE8ull, 0x2228, 0x04}, // uShotVulcanSatelliteLaserLock: input timer
    {0x140D4D3D0ull, 0x2140, 0x04}, // uShotVulcanShikiDanKobun: phase
    {0x140D4D3D0ull, 0x2148, 0x0B}, // uShotVulcanShikiDanKobun: timestamp/flags
    {0x140D4D6A0ull, 0x2140, 0x04}, // uShotVulcanShockingPinkBomb: contact count
    {0x140D4D6A0ull, 0x2150, 0x30}, // uShotVulcanShockingPinkBomb: contact vectors
    {0x140D4D6A0ull, 0x2184, 0x0C}, // uShotVulcanShockingPinkBomb: trajectory state
    {0x140D4D6A0ull, 0x2198, 0x05}, // uShotVulcanShockingPinkBomb: end state
    {0x140D4D6A0ull, 0x21A0, 0x04}, // uShotVulcanShockingPinkBomb: effect variant
    {0x140D4D6A0ull, 0x21B8, 0x08}, // uShotVulcanShockingPinkBomb: effect/action state
    {0x140D4D5E8ull, 0x2144, 0x05}, // uShotVulcanStormSwordSpread: timer/latch
    {0x140D49520ull, 0x21D0, 0x04}, // uShotVulcanSomersaultGrenade: owner trajectory
    {0x140D49520ull, 0x2210, 0x0C}, // uShotVulcanSomersaultGrenade: phase/trajectory
    {0x140D495D0ull, 0x2140, 0x01}, // uShotVulcanSomersaultLasso: deferred-target flag
    {0x140D49608ull, 0x21D0, 0x04}, // uShotVulcanSomersaultShuriken: owner trajectory
    {0x140D49608ull, 0x2210, 0x08}, // uShotVulcanSomersaultShuriken: angle state
    {0x140D4B6E8ull, 0x2140, 0x05}, // uShotVulcanSoulFist: mode/flag
    {0x140D49BD0ull, 0x2140, 0x04}, // uShotVulcanSphereFlame: variant
    {0x140D4A840ull, 0x2140, 0x0C}, // uShotVulcanSpreadArrow: mode/count/life
    {0x140D4CB00ull, 0x2158, 0x01}, // uShotVulcanSwingWire: lifecycle flag
    {0x140D4BE38ull, 0x2140, 0x04}, // uShotVulcanTKBind: phase
    {0x140D4B078ull, 0x21D0, 0x04}, // uShotVulcanTenraiha: owner trajectory
    {0x140D4B078ull, 0x2210, 0x24}, // uShotVulcanTenraiha: phase/random ranges
    {0x140D49F88ull, 0x21D4, 0x04}, // uShotVulcanThrowRose: owner rotation
    {0x140D4A120ull, 0x2140, 0x24}, // uShotVulcanThrowWeapon: weapon/movement state
    {0x140D4A160ull, 0x2140, 0x24}, // uShotVulcanThrowWeaponB: weapon/movement state
    {0x140D489E0ull, 0x2214, 0x01}, // uShotVulcanTorch: powered-up flag
    {0x140D4C570ull, 0x2140, 0x05}, // uShotVulcanTrapLandMine: variant/armed state
    {0x140D4C570ull, 0x2148, 0x08}, // uShotVulcanTrapLandMine: state/timer
    {0x140D48700ull, 0x2210, 0x01}, // uShotVulcanTsutamaki: lifecycle flag
    {0x140D4CB40ull, 0x2140, 0x05}, // uShotVulcanWebBall: scalar flags
    {0x140D4CB88ull, 0x2140, 0x09}, // uShotVulcanWebMove: phase/timing
    {0x140D4CBC8ull, 0x2144, 0x04}, // uShotVulcanWebThrow: phase timestamp
    {0x140D4A228ull, 0x2200, 0x08}, // uShotVulcanWildPitch: spawn/variant state
    {0x140D4B0D0ull, 0x2200, 0x20}, // uShotVulcanZireitou: gameplay tail state
};

using GetPrimitiveFn = uint64_t(__fastcall*)(uint64_t, uint32_t);
using GetMetadataFn = uint64_t(__fastcall*)(uint64_t, uint32_t);

bool Accessible(uint64_t address, size_t size = sizeof(uint64_t))
{
    return IsAccessibleRange(address, size);
}

bool IsMappedPointer(uint64_t value)
{
    return IsLikelyPointer(value) && Accessible(value);
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
    // Every caller passes a fresh GetDti result, which already validated this
    // exact size field without any intervening native work.
    if (!dti) return 0;
    return (*(uint32_t*)(dti + 0x30) & 0x7FFFFF) << 2;
}

bool IsDti(uint64_t dti, uintptr_t expected)
{
    return dti == (uint64_t)_addr(expected);
}

bool IsDtiKindOf(uint64_t dti, uintptr_t expected)
{
    return dti && ((uint8_t(__fastcall*)(void*, void*))_addr(kDtiIsKindOf))(
        (void*)dti, (void*)_addr(expected));
}

bool IsLaserOwnedEffect(void* effect)
{
    if (!effect) return false;
    for (size_t offset : {0x40u, 0x280u, 0x288u}) {
        void* owner = *(void**)((uint8_t*)effect + offset);
        uint64_t ownerDti = GetDti(owner);
        if (IsDtiKindOf(ownerDti, kShotLaserDti)) return true;
    }
    return false;
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
        if (IsMappedPointer(liveValue) || IsMappedPointer(savedValue)) {
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
    if (slotIndex >= kShotCollisionSlotCount) return 0;
    uint64_t slot = *(const uint64_t*)(shot + kShotCollisionSlotsOffset +
                                      slotIndex * sizeof(uint64_t));
    if (!slot) return 0;

    uint64_t vtable = *(uint64_t*)slot;
    if (!vtable) return 0;
    GetPrimitiveFn getPrimitive = *(GetPrimitiveFn*)(vtable + 0x88);
    if (!getPrimitive) return 0;

    uint32_t count = *(uint32_t*)(slot + 0x20);
    if (count > kMaxCollisionPrimitives || primitiveIndex >= count) return 0;
    return getPrimitive(slot, primitiveIndex);
}

uint64_t GetCollisionMetadata(const uint8_t* shot, uint32_t slotIndex,
                              uint32_t primitiveIndex)
{
    if (slotIndex >= kShotCollisionSlotCount) return 0;
    uint64_t slot = *(const uint64_t*)(shot + kShotCollisionSlotsOffset +
                                      slotIndex * sizeof(uint64_t));
    if (!slot) return 0;

    uint32_t count = *(uint32_t*)(slot + 0x20);
    if (count > kMaxCollisionPrimitives || primitiveIndex >= count) return 0;

    auto getMetadata = (GetMetadataFn)_addr(0x1405AA900);
    return getMetadata(slot, primitiveIndex);
}

void CaptureCollisionPrimitives(const uint8_t* shot, ConcreteShotSnapshot& out)
{
    for (uint32_t slot = 0; slot < kShotCollisionSlotCount; ++slot) {
        uint64_t collisionSlot = *(const uint64_t*)(
            shot + kShotCollisionSlotsOffset + slot * sizeof(uint64_t));
        if (!collisionSlot) continue;

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
            auto shotOffset = [&](uint64_t parent) {
                uintptr_t shotStart = (uintptr_t)shot;
                uintptr_t shotEnd = shotStart + out.body.size();
                return parent >= shotStart && parent < shotEnd
                    ? (int32_t)(parent - shotStart) : -1;
            };

            if (saved.metadataType == 0) {
                // cHitPrimSphere is 0x70 bytes: world center/radius at +0x20,
                // flags/parent at +0x30/+0x38, and local center at +0x50.
                std::memcpy(saved.geometry, (void*)(primitive + 0x20), 0x10);
                saved.flags = *(uint32_t*)(primitive + 0x30);
                saved.parent0 = *(uint64_t*)(primitive + 0x38);
                std::memcpy(saved.parentState, (void*)(primitive + 0x40),
                            sizeof(saved.parentState));
                std::memcpy(saved.localGeometry, (void*)(primitive + 0x50),
                            0x10);
            } else {
                std::memcpy(saved.geometry, (void*)(primitive + 0x20),
                            sizeof(saved.geometry));
                saved.flags = *(uint32_t*)(primitive + 0x50);
                saved.parent0 = *(uint64_t*)(primitive + 0x58);
                saved.parent1 = *(uint64_t*)(primitive + 0x60);
                std::memcpy(saved.parentState, (void*)(primitive + 0x68),
                            sizeof(saved.parentState));
                std::memcpy(saved.localGeometry, (void*)(primitive + 0x80),
                            sizeof(saved.localGeometry));
            }
            saved.parent0ShotOffset = shotOffset(saved.parent0);
            saved.parent1ShotOffset = shotOffset(saved.parent1);
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
            float* sourceStart = (float*)saved.sourceGeometry;
            float* sourceEnd = (float*)(saved.sourceGeometry + 0x10);
            for (size_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(localStart[axis]) ||
                    !std::isfinite(worldStart[axis]) ||
                    !std::isfinite(shotPos[axis])) {
                    continue;
                }

                float derivedStart = worldStart[axis] - shotPos[axis];
                bool localAxisEmpty = std::fabs(localStart[axis]) < 0.01f;
                bool derivedAxisPresent = std::fabs(derivedStart) >= 0.01f;
                if (localAxisEmpty && derivedAxisPresent) {
                    sourceStart[axis] = derivedStart;
                }

                // Capsules have a second endpoint; spheres end after the first
                // center/radius vector and must not read the following object.
                if (saved.metadataType != 0) {
                    const float* localEnd =
                        (const float*)(saved.localGeometry + 0x10);
                    if (!std::isfinite(localEnd[axis]) ||
                        !std::isfinite(worldEnd[axis])) {
                        continue;
                    }
                    float derivedEnd = worldEnd[axis] - shotPos[axis];
                    if (std::fabs(localEnd[axis]) < 0.01f &&
                        std::fabs(derivedEnd) >= 0.01f) {
                        sourceEnd[axis] = derivedEnd;
                    }
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
        // metadataType selects the concrete allocation layout. Type 0 is a
        // 0x70-byte cHitPrimSphere with one center and one parent; the other
        // captured types use the larger capsule layout with two endpoints and
        // two parent fields. Using capsule offsets for a sphere writes beyond
        // the object into the following allocation.
        if (saved.metadataType == 0) {
            std::memcpy((void*)(primitive + 0x20), saved.geometry, 0x10);
            *(uint32_t*)(primitive + 0x30) = saved.flags;
            *(uint64_t*)(primitive + 0x38) = restoreParent(
                saved.parent0, saved.parent0ShotOffset,
                *(uint64_t*)(primitive + 0x38));
            std::memcpy((void*)(primitive + 0x40), saved.parentState,
                        sizeof(saved.parentState));
            std::memcpy((void*)(primitive + 0x50), saved.localGeometry, 0x10);
        } else {
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
            // Keep historical pointers only for current stable targets.
            // Projectile targets are captured by spawn sequence and rebound by
            // Shot_RestoreBodyReferences after all shots have been reconstructed.
            if (validTarget) {
                *(uint64_t*)(shot + entry + 0x08) = savedTarget;
            }
            std::memcpy(shot + entry + 0x10, saved + entry + 0x10, 0x08);
        }
    }
}

} // namespace

static bool CaptureGeneratorEmissionState(
    void* effect, ConcreteShotSnapshot::CallbackEffectState& state)
{
    state.generatorEmissionStates.clear();
    uint64_t effectDti = GetDti(effect);
    if (!IsDtiKindOf(effectDti, kEffectDti)) return false;

    const size_t expectedCount = *(uint16_t*)((uint8_t*)effect + 0x204);
    if (expectedCount > kMaxCallbackGenerators) return false;
    state.generatorEmissionStates.reserve(expectedCount);
    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    bool sawFinish = false, running = false;
    for (uint32_t count = 0; generator; ++count) {
        if (count >= expectedCount || !IsAccessibleRange(generator, 0x28)) {
            state.generatorEmissionStates.clear();
            return false;
        }

        ConcreteShotSnapshot::CallbackEffectState::GeneratorEmissionState saved{};
        saved.dti = GetObjectDti(generator);
        if (!saved.dti) {
            state.generatorEmissionStates.clear();
            return false;
        }
        const uint32_t status = *(uint32_t*)(generator + 0x20);
        sawFinish |= (status & 0x80000000u) != 0;
        running |= (status & 7u) != 0 && !(status & 0x80000000u);
        saved.ordinary = *(uint64_t*)(*(uint64_t*)generator + 0x88) ==
            (uint64_t)_addr(0x140834F30);
        if (saved.ordinary) {
            if (!IsAccessibleRange(generator, 0x208)) {
                state.generatorEmissionStates.clear();
                return false;
            }
            saved.resource = *(uint64_t*)(generator + 0x68);
            EffectGenerator::CaptureOrdinary((void*)generator, saved);
        }
        state.generatorEmissionStates.push_back(saved);
        generator = *(uint64_t*)(generator + 0x18);
    }
    if (state.generatorEmissionStates.size() != expectedCount) {
        state.generatorEmissionStates.clear();
        return false;
    }
    state.emissionFinished = sawFinish && !running;
    return true;
}

static bool RestoreGeneratorEmissionState(
    void* effect, const ConcreteShotSnapshot::CallbackEffectState& state,
    bool liveShot = false)
{
    if (!state.hasGeneratorEmissionState) return false;

    const size_t generatorCount = state.generatorEmissionStates.size();
    if (generatorCount > kMaxCallbackGenerators) return false;
    std::array<uint64_t, kMaxCallbackGenerators> generators;
    size_t foundCount = 0;
    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    for (size_t index = 0; generator; ++index) {
        if (index >= generatorCount ||
            !IsAccessibleRange(generator, 0x28) ||
            GetObjectDti(generator) != state.generatorEmissionStates[index].dti ||
            (state.generatorEmissionStates[index].ordinary &&
             (!IsAccessibleRange(generator, 0x208) ||
              *(uint64_t*)(*(uint64_t*)generator + 0x88) != (uint64_t)_addr(0x140834F30) ||
              *(uint64_t*)(generator + 0x68) != state.generatorEmissionStates[index].resource ||
              *(uint32_t*)(generator + 0x70) != state.generatorEmissionStates[index].modes))) {
            return false;
        }
        generators[foundCount++] = generator;
        generator = *(uint64_t*)(generator + 0x18);
    }
    if (foundCount != generatorCount) return false;

    // Mode 0 enters cParticleGenerator__shiftHistoryBuffer, whose unsigned
    // loop requires particle+0x9D to be nonzero. A fresh factory graph can
    // contain a not-yet-initialized mode-0 particle; rewinding the generator
    // phase would move it immediately and underflow the history index.
    for (size_t index = 0; index < generatorCount; ++index) {
        uint64_t current = generators[index];
        const auto& saved = state.generatorEmissionStates[index];
        // Only track-capable ordinary particles have history at +0x9C/+0x9D.
        // Never interpret Node/Infinite matrices as ordinary particle lists.
        if (!saved.ordinary ||
            (!IsDti(saved.dti, 0x140E1E2A0ull) && !IsDti(saved.dti, 0x140E1E2D8ull) &&
             !IsDti(saved.dti, 0x140E1E698ull))) continue;
        if (*(uint8_t*)(current + 0x77) ==
            state.generatorEmissionStates[index].phase) {
            continue;
        }
        uint64_t particle = *(uint64_t*)(current + 0xE0);
        uint32_t count = 0;
        while (particle && count++ < 0x10000) {
            if (!IsAccessibleRange(particle, 0xA0) ||
                (*(uint8_t*)(particle + 0x9C) == 0 &&
                 *(uint8_t*)(particle + 0x9D) == 0)) {
                return false;
            }
            particle = *(uint64_t*)(particle + 0x08);
        }
        if (particle) return false;
    }

    for (size_t index = 0; index < generatorCount; ++index) {
        EffectGenerator::RestoreOrdinary((void*)generators[index],
                                         state.generatorEmissionStates[index], liveShot);
    }
    return true;
}

static void RetireRecreatedEffect(void* effect)
{
    if (!effect) return;
    const uint64_t dti = GetDti(effect);
    if (!IsDtiKindOf(dti, kEffectDti) && !IsDtiKindOf(dti, 0x140D46ED0ull)) return;
    auto kill = (void(__fastcall*)(void*))*(uint64_t*)(*(uint64_t*)effect + 0x78);
    kill(effect); // native deferred retirement; never replay a spawning finish wrapper
}

static void RestoreEffectTimeline(void* effect,
    const ConcreteShotSnapshot::CallbackEffectState& state, bool reconstructed)
{
    std::memcpy((uint8_t*)effect + 0x100, state.flags100, sizeof(state.flags100));
    std::memcpy((uint8_t*)effect + 0x110, state.timing110, sizeof(state.timing110));
    std::memcpy((uint8_t*)effect + 0x1D0, state.state1D0, sizeof(state.state1D0));
    std::memcpy((uint8_t*)effect + 0x1E0, state.state1E0, sizeof(state.state1E0));
    // Fresh graphs own their generation token, child census and active counts.
    // An old zero completion count must not finish a newly recreated visual.
    if (!reconstructed)
        std::memcpy((uint8_t*)effect + 0x1F8, state.state1F8, sizeof(state.state1F8));
    std::memcpy((uint8_t*)effect + 0x230, state.state230, sizeof(state.state230));
}

static void RestoreEffectFallbackClock(void* effect,
    const ConcreteShotSnapshot::CallbackEffectState& state)
{
    // A pending/mismatched fresh graph must keep its setup and completion flags.
    // Still preserve an explicit expiry so repeated reconstruction cannot extend it.
    using EffectGenerator::Read;
    using EffectGenerator::Write;
    if (Read<uint16_t>(state.state1D0, 6)) {
        std::memcpy((uint8_t*)effect + 0x1D4, state.state1D0 + 4, 4);
        Write<uint32_t>(effect, 0x130, (Read<uint32_t>(effect, 0x130) & ~0xFu) |
            (Read<uint32_t>(state.timing110, 0x20) & 0xFu));
    }
    Write<uint32_t>(effect, 0x100, (Read<uint32_t>(effect, 0x100) & ~2u) |
        (Read<uint32_t>(state.flags100, 0) & 2u));
}

bool ConcreteShotSnapshot_Capture(void* shot, ConcreteShotSnapshot& out)
{
    out = {};
    if (!shot) return false;

    uint64_t dti = GetDti(shot);
    uint32_t size = DtiSize(dti);
    if (size < kShotBodyStart || size > kMaxObjectSize) {
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
        state.savedEffect = effect;
        uint64_t effectDti = GetDti(effect);
        if (!IsDtiKindOf(effectDti, kEffectDti)) continue;
        state.hasState = true;
        state.unitState = *(uint32_t*)((uint8_t*)effect + 0x10) & 7u;
        state.dti = effectDti;
        state.resource = *(uint64_t*)((uint8_t*)effect + 0x108);
        std::memcpy(state.flags100, (uint8_t*)effect + 0x100,
                    sizeof(state.flags100));
        std::memcpy(state.timing110, (uint8_t*)effect + 0x110,
                    sizeof(state.timing110));
        std::memcpy(state.state1D0, (uint8_t*)effect + 0x1D0,
                    sizeof(state.state1D0));
        std::memcpy(state.state1E0, (uint8_t*)effect + 0x1E0,
                    sizeof(state.state1E0));
        std::memcpy(state.state1F8, (uint8_t*)effect + 0x1F8,
                    sizeof(state.state1F8));
        std::memcpy(state.state230, (uint8_t*)effect + 0x230,
                    sizeof(state.state230));
        state.hasGeneratorEmissionState =
            CaptureGeneratorEmissionState(effect, state);
    }
    return true;
}

bool ConcreteEffectSnapshot_Capture(
    void* effect, ConcreteShotSnapshot::CallbackEffectState& state)
{
    state = {};
    state.occupied = effect != nullptr;
    state.savedEffect = effect;
    if (!effect) return false;
    uint64_t effectDti = GetDti(effect);
    if (!IsDtiKindOf(effectDti, kEffectDti)) return true;
    state.hasState = true;
    state.unitState = *(uint32_t*)((uint8_t*)effect + 0x10) & 7u;
    state.dti = effectDti;
    state.resource = *(uint64_t*)((uint8_t*)effect + 0x108);
    std::memcpy(state.flags100, (uint8_t*)effect + 0x100,
                sizeof(state.flags100));
    std::memcpy(state.timing110, (uint8_t*)effect + 0x110,
                sizeof(state.timing110));
    std::memcpy(state.state1D0, (uint8_t*)effect + 0x1D0,
                sizeof(state.state1D0));
    std::memcpy(state.state1E0, (uint8_t*)effect + 0x1E0,
                sizeof(state.state1E0));
    std::memcpy(state.state1F8, (uint8_t*)effect + 0x1F8,
                sizeof(state.state1F8));
    std::memcpy(state.state230, (uint8_t*)effect + 0x230,
                sizeof(state.state230));
    state.hasGeneratorEmissionState =
        CaptureGeneratorEmissionState(effect, state);
    return true;
}

bool ConcreteEffectSnapshot_Restore(
    void* effect, const ConcreteShotSnapshot::CallbackEffectState& state, bool reconstructed)
{
    if (!effect || !state.hasState ||
        GetObjectDti((uint64_t)effect) != state.dti ||
        *(uint64_t*)((uint8_t*)effect + 0x108) != state.resource)
        return false;
    if (reconstructed && state.CompletedIndependent()) {
        RetireRecreatedEffect(effect);
        return true;
    }
    // Laser strips need to generate fresh history from frame zero. Applying an
    // advanced emission cursor to an empty manager graph leaves them invisible.
    const bool restored = !IsLaserOwnedEffect(effect) &&
        RestoreGeneratorEmissionState(effect, state);
    if (restored || !reconstructed)
        RestoreEffectTimeline(effect, state, reconstructed);
    else
        RestoreEffectFallbackClock(effect, state);
    return true;
}

void ConcreteShotSnapshot_RestoreCallbackEffects(
    void* shot, const ConcreteShotSnapshot& snapshot)
{
    if (!shot) return;

    // Doom's hit/expire handlers already finished callbacks before entering
    // step 2. Reconstruction must not restart those effects and leave the
    // native wait handler (vfn121) waiting forever for its slots to empty.
    const bool waitingForCallbacks = snapshot.body.size() > 0x212C && snapshot.vtable &&
        (snapshot.body[0x212C] == 2 || snapshot.body[0x212C] == 3) &&
        *(uint64_t*)(snapshot.vtable + 0x3C8) == (uint64_t)_addr(0x1401A0050);

    if (IsDti(snapshot.dti, kAttackWireDti) ||
        IsDti(snapshot.dti, kFireGrenadeExplodeDti) ||
        IsDti(snapshot.dti, kJamSessionDti) ||
        IsDti(snapshot.dti, kInfernoDti) ||
        IsDti(snapshot.dti, kShockingPinkBombDti) ||
        IsDti(snapshot.dti, kOilBalloonGroundOilDti) ||
        IsDti(snapshot.dti, kDarkPhoenixFireDti)) {
        //Hacks for all of these to prevent desyncs.
        return;
    }

    for (uint32_t index = 0; index < kShotCallbackEffectCount; ++index) {
        const auto& state = snapshot.callbackEffects[index];
        void** slot = (void**)((uint8_t*)shot + kShotCallbackEffectsOffset +
                              index * sizeof(void*));
        void* effect = *slot;

        // Saved absence/death and native cleanup are authoritative. Leave the
        // pointer in its slot until native preMove clears the retired unit.
        if (!state.occupied || waitingForCallbacks ||
            (state.hasState && state.unitState != 1 && state.unitState != 2)) {
            RetireRecreatedEffect(effect);
            continue;
        }

        // Pending effects can occupy a callback slot before their complete DTI
        // and generator graph becomes queryable. Preserve the factory-created
        // replacement in that case; occupancy is authoritative, while scalar
        // rewind is only possible for a fully captured effect.
        if (!state.hasState)
            continue;

        if (!effect || GetDti(effect) != state.dti ||
            *(uint64_t*)((uint8_t*)effect + 0x108) != state.resource) {
            continue;
        }

        if (!IsDtiKindOf(snapshot.dti, kShotLaserDti) &&
            RestoreGeneratorEmissionState(effect, state, true))
            RestoreEffectTimeline(effect, state, true);
        else
            RestoreEffectFallbackClock(effect, state);
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
    if (liveDti != snapshot.dti || liveSize != snapshot.body.size()) {
        return false;
    }
    const uint64_t validateEnd = Profile_Now();

    const uint8_t* source = snapshot.body.data();
    uint8_t* live = (uint8_t*)shot;
    std::vector<uint8_t> overlay(source, source + snapshot.body.size());
    const uint64_t copyBodyEnd = Profile_Now();

    // Keep the fresh vtable and unit state; Shot_ApplyFlight restores lifecycle bits.
    PreserveRange(overlay.data(), live, liveSize, {0, sizeof(uint64_t)});
    PreserveRange(overlay.data(), live, liveSize, {0x10, 0x08});

    // Keep fresh bytes wherever either copy looks like a pointer.
    // Restore known scalar fields below to override false pointer matches.
    PreserveLivePointers(overlay.data(), live, liveSize);
    const uint64_t pointerScanEnd = Profile_Now();

    // Keep the material table and count together, including the adjacent light group.
    PreserveRange(overlay.data(), live, liveSize, {0x110, 0x10});

    // Keep joint counts/tables and motion controls consistent with the fresh model.
    PreserveRange(overlay.data(), live, liveSize, {0x530, 0x30});

    if (IsDti(snapshot.dti, kLegionTigerDti) ||
        IsDti(snapshot.dti, kRepulsorBlastSpDti)) {
        // Preserve the loaded model and motion setup; rewind actor playback only.
        PreserveRange(overlay.data(), live, liveSize, {0x100, 0x1200});
        PreserveRange(overlay.data(), live, liveSize, {0x1348, 0x128});
        CopyRange(overlay.data(), source, liveSize, {0x1300, 0x48});
    }

    // Keep the actor's four resource references and seven owned material-helper arrays.
    PreserveRange(overlay.data(), live, liveSize, {0x13E8, 0x58});

    // Keep embedded cAtkCtrl/cArm and the following shot data fresh as a block.
    // Restore selected gameplay fields, targets, and collision state separately.
    PreserveRange(overlay.data(), live, liveSize, {0x1470, 0xC08});

    // Restore the saved attack-resource binding, needed by Kikosho's phase-5 hitboxes.
    // Keep the rebuilt binding for Nemesis's explosion and Elemental Summon instead.
    if (!IsDti(snapshot.dti, kRocketLauncherExplodeDti) &&
        !IsDti(snapshot.dti, kElementalSummonShotDti)) {
        CopyRange(overlay.data(), source, liveSize, {0x1490, 0x08});
    }

    // Restore attack selection, flags, combo, timers, and animation number.
    // The pending attack ID is restored with the full scalar header below.
    CopyRange(overlay.data(), source, liveSize, {0x1498, 0x24});

    // Soul Fist stores a live uGameEffect pointer after its mode field.
    if (IsDti(snapshot.dti, kSoulFistDti)) {
        PreserveRange(overlay.data(), live, liveSize, {0x2148, 0x08});
    }

    // Keep fresh bytes for embedded MtObjects detected in either copy.
    PreserveEmbeddedObjectsFromBoth(overlay.data(), live, source, liveSize,
                                    kShotRestoreStart);
    const uint64_t embeddedLiveEnd = Profile_Now();
    const uint64_t embeddedSavedEnd = embeddedLiveEnd;

    // Reapply property-exported and verified scalars after ownership filtering.
    RestoreGeneratedRanges(overlay.data(), source, liveSize, snapshot.dti,
                           kGeneratedShotTailRanges);
    RestoreGeneratedRanges(overlay.data(), source, liveSize, snapshot.dti,
                           kVerifiedShotScalarRanges);

    if (IsDti(snapshot.dti, kVulcanAxeDti)) {
        // Restore the axe's rotation angle even if its bits resemble a pointer.
        CopyRange(overlay.data(), source, liveSize, {0x21D0, 0x04});
    }

    if (IsDti(snapshot.dti, kItemThrowDti)) {
        // Restore item variant, spin, and landing/bounce state after filtering.
        CopyRange(overlay.data(), source, liveSize, {0x2210, 0x20});
    }

    if (IsDti(snapshot.dti, kShikiDanKobunDti)) {
        // Restore the Servbot's voice threshold/latch and periodic sound timer.
        CopyRange(overlay.data(), source, liveSize, {0x2170, 0x10});
    }

    if (IsDti(snapshot.dti, kVulcanBoomerangDti)) {
        // Restore boomerang trajectory/phase and effect/lifetime latches after uCnsTSR.
        CopyRange(overlay.data(), source, liveSize, {0x2210, 0x30});
    }

    // Restore the shot ID to match the rewound ID pool and target-hit records.
    CopyRange(overlay.data(), source, liveSize, {0x1F50, 0x04});

    if (IsDti(snapshot.dti, kDogMissileDti)) {
        // Undo setup's animation restart by restoring the actor playback header.
        CopyRange(overlay.data(), source, liveSize, {0x1300, 0x48});
    }

    if (Rollback_ShouldRestoreShotModelPlayback(snapshot.dti)) {
        // Rewind model playback controls, motion state, and event masks.
        // Keep fresh motion resources and work buffers.
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

    if (IsDti(snapshot.dti, kAttackWireDti)) {
        // Restore attachment/control flags, mode, and return countdown.
        // Leave the effect resource at +0x2150 fresh.
        CopyRange(overlay.data(), source, liveSize, {0x2140, 0x10});
    }

    if (IsDti(snapshot.dti, kVulcanSwingWireDti)) {
        // Restore activation latch, mode, and timer used to signal Spencer.
        // Leave the effect resource at +0x2150 fresh.
        CopyRange(overlay.data(), source, liveSize, {0x2140, 0x10});
    }

    // Apply the filtered body, leaving the unit prefix and intrusive links fresh.
    CopyRange(live, overlay.data(), liveSize,
              {kShotRestoreStart, liveSize - kShotRestoreStart});
    const uint64_t applyBodyEnd = Profile_Now();
    // Reapply the full attack scalar header, including the pending attack ID.
    // Restore target-hit records so reconstruction does not allow duplicate hits.
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

void ConcreteShotSnapshot_RebuildLiveCapsuleWorld(
    void* shot, const ConcreteShotSnapshot& snapshot)
{
    if (!shot) return;
    const float* shotPosition = (const float*)((uint8_t*)shot + 0x50);
    for (const auto& saved : snapshot.collisionPrimitives) {
        if (saved.metadataType != 1 || !saved.hasSourceGeometry) continue;
        uint64_t primitive = GetCollisionPrimitive(
            (const uint8_t*)shot, saved.slot, saved.index);
        if (!primitive) continue;

        const float* localStart = (const float*)saved.sourceGeometry;
        const float* localEnd =
            (const float*)(saved.sourceGeometry + 0x10);
        if (std::fabs(localStart[1] - localEnd[1]) < 0.01f) continue;
        const float* currentWorldStart = (const float*)(primitive + 0x20);
        const float* currentWorldEnd = (const float*)(primitive + 0x30);
        bool dynamicYAxisCollapsed =
            std::fabs(currentWorldStart[1] - shotPosition[1]) < 0.01f &&
            std::fabs(currentWorldEnd[1] - shotPosition[1]) < 0.01f &&
            (std::fabs(localStart[1]) >= 0.01f ||
             std::fabs(localEnd[1]) >= 0.01f);
        if (!dynamicYAxisCollapsed) continue;

        float* liveLocalStart = (float*)(primitive + 0x80);
        float* liveLocalEnd = (float*)(primitive + 0x90);
        float* worldStart = (float*)(primitive + 0x20);
        float* worldEnd = (float*)(primitive + 0x30);
        for (size_t axis = 0; axis < 3; ++axis) {
            liveLocalStart[axis] = localStart[axis];
            liveLocalEnd[axis] = localEnd[axis];
            worldStart[axis] = shotPosition[axis] + localStart[axis];
            worldEnd[axis] = shotPosition[axis] + localEnd[axis];
        }
    }

    if (!Rollback_ConsumePendulumCollisionReset(shot, snapshot.dti))
        return;

    // Native collision observes the reset pose on the frame the asynchronous
    // motion becomes ready; synchronous reconstruction skips that state.
    for (const auto& saved : snapshot.collisionPrimitives) {
        if (saved.metadataType != 1 || saved.geometryType != 6) continue;
        uint64_t primitive = GetCollisionPrimitive(
            (const uint8_t*)shot, saved.slot, saved.index);
        if (!primitive) continue;
        float* worldStart = (float*)(primitive + 0x20);
        float* worldEnd = (float*)(primitive + 0x30);
        for (size_t axis = 0; axis < 3; ++axis)
            worldStart[axis] = worldEnd[axis] = shotPosition[axis];
    }
}
