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
constexpr uintptr_t kWeaponCartDti = 0x140D4A1A8ull;
constexpr uintptr_t kLegionTigerDti = 0x140D4ABB8ull;
constexpr uintptr_t kRepulsorBlastSpDti = 0x140D4ADA0ull;
constexpr uintptr_t kRocketLauncherExplodeDti = 0x140D4BAC8ull;
constexpr uintptr_t kPendulumLogDti = 0x140D4C4C8ull;
constexpr uintptr_t kVulcanAxeDti = 0x140D48808ull;
constexpr uintptr_t kShikiDanKobunDti = 0x140D4D3D0ull;
constexpr uintptr_t kRyuDti = 0x140D45A10ull;
constexpr uintptr_t kHsienKoDti = 0x140D455F8ull;
constexpr uintptr_t kSpencerDti = 0x140D45D40ull;
constexpr uintptr_t kAttackWireDti = 0x140D4CA20ull;
constexpr uintptr_t kJamSessionDti = 0x140D49288ull;
constexpr uintptr_t kInfernoDti = 0x140D49790ull;
constexpr uintptr_t kShockingPinkBombDti = 0x140D4D6A0ull;
constexpr uintptr_t kVulcanSwingWireDti = 0x140D4CB00ull;
constexpr uintptr_t kVulcanBoomerangDti = 0x140D4D660ull;
constexpr uintptr_t kShotLaserDti = 0x140D482E8ull;
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

constexpr DtiSnapshotRange kGeneratedCharacterTailRanges[] = {
#include "generated_character_tail_fields.inc"
};

constexpr DtiSnapshotRange kGeneratedShotTailRanges[] = {
#include "generated_shot_tail_fields.inc"
};

// Scalar concrete-tail fields verified from constructors and gameplay virtuals
// in Ghidra but absent from the generated createProperty export. Keep these
// separate from the generated file and reapply them after ownership filtering.
constexpr DtiSnapshotRange kVerifiedShotScalarRanges[] = {
    {0x140D49A08ull, 0x2140, 0x04}, // uShotMissileDoomsMissile: flight parameter
    {0x140D4CA20ull, 0x2140, 0x03}, // uShotOptionAttackWire: state flags
    {0x140D4CA20ull, 0x2144, 0x08}, // uShotOptionAttackWire: mode/timer
    {0x140D49250ull, 0x2140, 0x01}, // uShotOptionGrapple: attached latch
    {0x140D4BD00ull, 0x2140, 0x04}, // uShotOptionHealingFieldGathering: range
    {0x140D4D430ull, 0x21D8, 0x04}, // uShotOptionRoundTrip: owner orientation
    {0x140D4D430ull, 0x22A4, 0x04}, // uShotOptionRoundTrip: owner rotation
    {0x140D4D430ull, 0x22E0, 0x05}, // uShotOptionRoundTrip: rotation/side
    {0x140D48780ull, 0x21C8, 0x04}, // uShotOptionSickleBoomerang: owner rotation
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

// Concrete character-tail scalar/POD fields verified in Ghidra but absent from
// the generated createProperty export. These are applied after reference
// filtering so nearby live helper/effect ownership is never flattened.
constexpr DtiSnapshotRange kVerifiedCharacterScalarRanges[] = {
    {0x140D44F18ull, 0x6900, 0x08}, // uChunli: charge-ready and voice timer
    {0x140D45020ull, 0x68F4, 0x08}, // uDormammu: spell stock and flare counter
    {0x140D45020ull, 0x6920, 0x01}, // uDormammu: Stalking Flare lock
    {0x140D45068ull, 0x68F8, 0x04}, // uDrDoom: single-animation action latch
    {0x140D44FE0ull, 0x68F0, 0x08}, // uDeadPool: teleport state
    {0x140D45200ull, 0x6990, 0x30}, // uFrank: Kobun scale, offset, rotation
    {0x140D45200ull, 0x69C4, 0x04}, // uFrank: level
    {0x140D45200ull, 0x69DC, 0x04}, // uFrank: experience
    {0x140D45200ull, 0x69EC, 0x08}, // uFrank: weapon speed and throw ID
    {0x140D45200ull, 0x6A48, 0x08}, // uFrank: throw and action wait timers
    {0x140D45200ull, 0x6A50, 0x01}, // uFrank: summon latch
    {0x140D45248ull, 0x6930, 0x20}, // uGalactus: concrete vector state
    {0x140D45408ull, 0x68FC, 0x18}, // uHawkEye: standing-S motion tuning
    {0x140D45408ull, 0x6918, 0x04}, // uHawkEye: standing-S charge counter
    {0x140D44F60ull, 0x6908, 0x04}, // uCViper: Optic Laser charge timer
    {0x140D45450ull, 0x6900, 0x01}, // uHiryu: team-state transition latch
    {0x140D45450ull, 0x6920, 0x05}, // uHiryu: Ouroboros state/latch
    {0x140D45450ull, 0x6928, 0x04}, // uHiryu: Ouroboros state scalar
    {0x140D45488ull, 0x6900, 0x01}, // uHulk: Gamma Wave fired latch
    {0x140D45518ull, 0x68F8, 0x02}, // uIronMan: effect-state latches
    {0x140D45578ull, 0x68F8, 0x01}, // uJill: Feral install latch
    {0x140D45680ull, 0x68F0, 0x20}, // uMagneto: complete scalar tail
    {0x140D456C0ull, 0x68F4, 0x04}, // uMayoi: initial-life cache
    {0x140D45700ull, 0x68F4, 0x1C}, // uModok: Psionic and flight state
    {0x140D458D0ull, 0x6908, 0x09}, // uNova: Nova Force and Centurion state
    {0x140D45C58ull, 0x6909, 0x01}, // uSentinel: drone active lock
    {0x140D45C58ull, 0x690C, 0x09}, // uSentinel: drone phase, timer, latch
    {0x140D45D80ull, 0x68FC, 0x04}, // uSpiderMan: web-throw target ID
    {0x140D45E18ull, 0x690C, 0x08}, // uSuperSkrull: Rolling Skrull height/state
    {0x140D45E58ull, 0x68F0, 0x10}, // uTaskMaster: arrow velocity/action state
    {0x140D45E58ull, 0x6900, 0x10}, // uTaskMaster: arrow/action state
    {0x140D45EA0ull, 0x6900, 0x01}, // uThor: tornado-active latch
    {0x140D45EA0ull, 0x6908, 0x04}, // uThor: speech stage
    {0x140D45EA0ull, 0x6910, 0x08}, // uThor: speech variant/latch
    {0x140D45F68ull, 0x691C, 0x04}, // uVergil: action state
    {0x140D45F68ull, 0x6930, 0x0C}, // uVergil: concrete vector state
    {0x140D45958ull, 0x68F4, 0x01}, // uRedArremer: active-ability latch
    {0x140D45958ull, 0x68FC, 0x02}, // uRedArremer: sync and Chaos Tide latches
    {0x140D45958ull, 0x6928, 0x04}, // uRedArremer: flight idle timer
    {0x140D459D8ull, 0x6954, 0x09}, // uRRaccoon: weapon action and movement state
    {0x140D459D8ull, 0x6960, 0x04}, // uRRaccoon: voice timer
    {0x140D45FB0ull, 0x6900, 0x0C}, // uVJoe: concrete vector state
    {0x140D45FF8ull, 0x690C, 0x04}, // uWesker: life-change tracker
    {0x140D46038ull, 0x68F8, 0x01}, // uWolverine: Berserker install latch
    {0x140D460C0ull, 0x6910, 0x01}, // uZero: Sogenmu installed latch
    {0x140D46118ull, 0x7090, 0x04}, // uZeroSh: trace status/dispatch latch
    {0x140D46158ull, 0x68F4, 0x08}, // uZombie: action timer and voice toggle
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

bool IsDtiKindOf(uint64_t dti, uintptr_t expected)
{
    return dti && ((uint8_t(__fastcall*)(void*, void*))_addr(kDtiIsKindOf))(
        (void*)dti, (void*)_addr(expected));
}

bool IsLaserOwnedEffect(void* effect)
{
    if (!effect) return false;
    for (size_t offset : {0x40u, 0x280u, 0x288u}) {
        if (!Accessible((uint64_t)effect + offset, sizeof(void*))) continue;
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

bool RangeOverlapsCharacterReference(const uint8_t* live,
                                     const uint8_t* saved, size_t objectSize,
                                     SnapshotRange range)
{
    if (!live || !saved || range.offset >= objectSize) return false;
    size_t rangeEnd = std::min(range.offset + range.size, objectSize);
    size_t wordStart = range.offset & ~(size_t)(alignof(uint32_t) - 1);
    for (size_t offset = wordStart;
         offset + sizeof(uint64_t) <= objectSize && offset < rangeEnd;
         offset += alignof(uint32_t)) {
        if (offset + sizeof(uint64_t) <= range.offset) continue;

        uint64_t liveValue = 0;
        uint64_t savedValue = 0;
        std::memcpy(&liveValue, live + offset, sizeof(liveValue));
        std::memcpy(&savedValue, saved + offset, sizeof(savedValue));
        if ((IsLikelyPointer(liveValue) && Accessible(liveValue)) ||
            (IsLikelyPointer(savedValue) && Accessible(savedValue))) {
            return true;
        }
    }
    return false;
}

void RestoreCharacterGeneratedRanges(void* destination, const uint8_t* source,
                                     size_t objectSize, uint64_t dti)
{
    const uint8_t* live = (const uint8_t*)destination;
    for (const DtiSnapshotRange& range : kGeneratedCharacterTailRanges) {
        if (!IsDti(dti, range.dti)) continue;

        // Generated fields can be inferred from a subclass schema even when
        // this build uses the same bytes as a helper/effect pointer. Preserve
        // the whole live reference instead of copying only its low scalar word.
        if (RangeOverlapsCharacterReference(
                live, source, objectSize, {range.offset, range.size})) {
            continue;
        }
        CopyRange(destination, source, objectSize, {range.offset, range.size});
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

static bool CaptureGeneratorEmissionState(
    void* effect, ConcreteShotSnapshot::CallbackEffectState& state)
{
    state.generatorEmissionStates.clear();
    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    for (uint32_t count = 0; generator; ++count) {
        if (count >= kMaxCallbackGenerators || !Accessible(generator, 0x200)) {
            state.generatorEmissionStates.clear();
            return false;
        }

        ConcreteShotSnapshot::CallbackEffectState::GeneratorEmissionState saved{};
        saved.dti = GetObjectDti(generator);
        if (!saved.dti) {
            state.generatorEmissionStates.clear();
            return false;
        }
        saved.resource = *(uint64_t*)(generator + 0x68);
        saved.phase = *(uint8_t*)(generator + 0x77);
        std::memcpy(saved.timing78, (void*)(generator + 0x78),
                    sizeof(saved.timing78));
        saved.elapsed = *(uint32_t*)(generator + 0xB8);
        std::memcpy(saved.spawnCounters, (void*)(generator + 0x1F4),
                    sizeof(saved.spawnCounters));
        state.generatorEmissionStates.push_back(saved);
        generator = *(uint64_t*)(generator + 0x18);
    }
    return true;
}

static bool RestoreGeneratorEmissionState(
    void* effect, const ConcreteShotSnapshot::CallbackEffectState& state)
{
    if (!state.hasGeneratorEmissionState) return false;

    std::vector<uint64_t> generators;
    generators.reserve(state.generatorEmissionStates.size());
    uint64_t generator = *(uint64_t*)((uint8_t*)effect + 0x218);
    for (size_t index = 0; generator; ++index) {
        if (index >= state.generatorEmissionStates.size() ||
            !Accessible(generator, 0x200) ||
            GetObjectDti(generator) != state.generatorEmissionStates[index].dti ||
            *(uint64_t*)(generator + 0x68) !=
                state.generatorEmissionStates[index].resource) {
            return false;
        }
        generators.push_back(generator);
        generator = *(uint64_t*)(generator + 0x18);
    }
    if (generators.size() != state.generatorEmissionStates.size()) return false;

    for (size_t index = 0; index < generators.size(); ++index) {
        generator = generators[index];
        const auto& saved = state.generatorEmissionStates[index];
        *(uint8_t*)(generator + 0x77) = saved.phase;
        std::memcpy((void*)(generator + 0x78), saved.timing78,
                    sizeof(saved.timing78));
        *(uint32_t*)(generator + 0xB8) = saved.elapsed;
        std::memcpy((void*)(generator + 0x1F4), saved.spawnCounters,
                    sizeof(saved.spawnCounters));
    }
    return true;
}

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
        state.resource = *(uint64_t*)((uint8_t*)effect + 0x108);
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
    if (!effect || !IsEffectObject(effect) ||
        !Accessible((uint64_t)effect, 0x138))
        return state.occupied;
    state.hasState = true;
    state.dti = GetObjectDti((uint64_t)effect);
    state.resource = *(uint64_t*)((uint8_t*)effect + 0x108);
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
    state.hasGeneratorEmissionState =
        CaptureGeneratorEmissionState(effect, state);
    return true;
}

bool ConcreteEffectSnapshot_Restore(
    void* effect, const ConcreteShotSnapshot::CallbackEffectState& state)
{
    if (!effect || !state.hasState || !IsEffectObject(effect) ||
        !Accessible((uint64_t)effect, 0x242) ||
        GetObjectDti((uint64_t)effect) != state.dti ||
        *(uint64_t*)((uint8_t*)effect + 0x108) != state.resource)
        return false;
    std::memcpy((uint8_t*)effect + 0x100, state.flags100, sizeof(state.flags100));
    std::memcpy((uint8_t*)effect + 0x110, state.timing110, sizeof(state.timing110));
    std::memcpy((uint8_t*)effect + 0x1D0, state.state1D0, sizeof(state.state1D0));
    std::memcpy((uint8_t*)effect + 0x1E0, state.state1E0, sizeof(state.state1E0));
    std::memcpy((uint8_t*)effect + 0x1F8, state.state1F8, sizeof(state.state1F8));
    std::memcpy((uint8_t*)effect + 0x230, state.state230, sizeof(state.state230));
    // Laser strips need to generate fresh history from frame zero. Applying an
    // advanced emission cursor to an empty manager graph leaves them invisible.
    if (!IsLaserOwnedEffect(effect))
        RestoreGeneratorEmissionState(effect, state);
    return true;
}

void ConcreteShotSnapshot_RestoreCallbackEffects(
    void* shot, const ConcreteShotSnapshot& snapshot)
{
    if (!shot) return;

    if (IsDti(snapshot.dti, kAttackWireDti) ||
        IsDti(snapshot.dti, kJamSessionDti) ||
        IsDti(snapshot.dti, kInfernoDti) ||
        IsDti(snapshot.dti, kShockingPinkBombDti)) {
        // Attack Wire's factory effect owns track buffers initialized for the
        // fresh generator graph. Rewinding its presentation timeline can make
        // a zero-length track active and underflow shiftHistoryBuffer.
        // Jam Session likewise uses three factory callbacks. Rewinding their
        // timelines made all three finish during client re-simulation and
        // marked the still-live shot for removal; shot lifetime is restored
        // independently by Shot_ApplyFlight.
        // Dormammu's Inferno uses the same factory callback path in slot 1.
        // Rewinding it made the client remove the assist shot after seven
        // re-simulated frames while the host kept it alive.
        // Shocking Pink rebuilds callback slot 2 with a fresh particle manager
        // and track table. Its captured timeline can activate a descriptor from
        // the retired graph, so leave that callback on its native timeline.
        return;
    }

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
            GetObjectDti((uint64_t)effect) != state.dti ||
            *(uint64_t*)((uint8_t*)effect + 0x108) != state.resource) {
            continue;
        }

        // DTI alone does not identify the EFL resource that defines generator
        // tracks. Preserve the fresh graph unless it uses the captured resource,
        // and stop scalar restoration before the live list count at +0x208.
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
        if (!IsDtiKindOf(snapshot.dti, kShotLaserDti))
            RestoreGeneratorEmissionState(effect, state);
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

    if (IsDti(snapshot.dti, kLegionTigerDti) ||
        IsDti(snapshot.dti, kRepulsorBlastSpDti)) {
        // These shots drive collision from asynchronously loaded models. Once
        // setup succeeds, do not overwrite the fresh model and Motion graph.
        // Restore only uActorGameModel's pointer-free animation playback state;
        // native move evaluation rebuilds the matching joint pose from it.
        PreserveRange(overlay.data(), live, liveSize, {0x100, 0x1200});
        PreserveRange(overlay.data(), live, liveSize, {0x1348, 0x128});
        CopyRange(overlay.data(), source, liveSize, {0x1300, 0x48});
    }

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
    // Nemesis's explosion is rebuilt from its remapped parent rocket; its fresh
    // binding carries that parent's attack variant, while the saved client
    // binding can still resolve the factory-default launch parameters.
    if (!IsDti(snapshot.dti, kRocketLauncherExplodeDti)) {
        CopyRange(overlay.data(), source, liveSize, {0x1490, 0x08});
    }

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
    RestoreGeneratedRanges(overlay.data(), source, liveSize, snapshot.dti,
                           kVerifiedShotScalarRanges);

    if (IsDti(snapshot.dti, kVulcanAxeDti)) {
        // Arthur's axe rotates its model and collision geometry from this
        // accumulator. Values such as 18.0f resemble mapped addresses when
        // read as a qword, so the generic ownership filter keeps factory zero.
        CopyRange(overlay.data(), source, liveSize, {0x21D0, 0x04});
    }

    if (IsDti(snapshot.dti, kShikiDanKobunDti)) {
        // Beacon Bomb's Servbot state machine keeps its random-selection phase
        // and timer immediately after reflected mAttachOfs. Factory zero here
        // reaches intInRange on a different tick and advances global RNG.
        CopyRange(overlay.data(), source, liveSize, {0x2170, 0x10});
    }

    if (IsDti(snapshot.dti, kVulcanBoomerangDti)) {
        // The embedded uCnsTSR ends at +0x2210. The remaining concrete tail is
        // pointer-free homing direction, variant, return mode and phase state.
        CopyRange(overlay.data(), source, liveSize, {0x2210, 0x30});
    }

    // mId is scalar allocator state inside the otherwise fresh-owned cArm
    // block. A reconstructed Kikosho kept its factory ID here while the ID
    // pool was rewound, changing per-target hit consumption on the next tick.
    CopyRange(overlay.data(), source, liveSize, {0x1F50, 0x04});

    if (IsDti(snapshot.dti, kKingKobunDti) ||
        IsDti(snapshot.dti, kMissileEmplacementDti) ||
        IsDti(snapshot.dti, kWeaponCartDti) ||
        IsDti(snapshot.dti, kPendulumLogDti)) {
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

    if (IsDti(snapshot.dti, kAttackWireDti)) {
        // Attack Wire's hit/attached latches, mode, and retract timer. Leaving
        // these at factory values makes one peer signal Spencer to retract.
        // The owned effect resource starts at +0x2150 and remains live-owned.
        CopyRange(overlay.data(), source, liveSize, {0x2140, 0x10});
    }

    if (IsDti(snapshot.dti, kVulcanSwingWireDti)) {
        // Swing Wire's activation latch, variant and travel-start timer. Forced
        // reconstruction otherwise resets the timer before its 10-frame branch
        // can signal Spencer through cChr+0x1510. Effect ownership starts at
        // +0x2150 and remains native to the fresh shot.
        CopyRange(overlay.data(), source, liveSize, {0x2140, 0x10});
    }

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

    RestoreCharacterGeneratedRanges(character, savedBody, bodySize, dti);
    RestoreGeneratedRanges(character, savedBody, bodySize, dti,
                            kVerifiedCharacterScalarRanges);

    if (IsDti(dti, kRyuDti)) {
        // Restore retires every live effect without reconstructing or remapping
        // this weak reference. Do not let uRyu::tick release the retired object.
        CopyRange(character, savedBody, bodySize, {0x68F0, 0x28});
        if (bodySize >= 0x6920)
            *(uint64_t*)((uint8_t*)character + 0x6918) = 0;
        CopyRange(character, savedBody, bodySize, {0x6920, 0x20});
    }

    if (IsDti(dti, kHsienKoDti)) {
        // +0x68F0/+0x68F8 are helper units. Restore the intervening scalar
        // state, base position, and pendulum state without overwriting either.
        CopyRange(character, savedBody, bodySize, {0x6900, 0x20});
    }

    if (IsDti(dti, kSpencerDti)) {
        // uSpencer's complete 16-byte concrete tail is pointer-free. It holds
        // the wire-active latch, wire animation number and mUsrF consumed when
        // the grapple branches to its facing-specific action.
        CopyRange(character, savedBody, bodySize, {0x68F0, 0x10});
    }

}
