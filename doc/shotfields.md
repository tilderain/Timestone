# Concrete Shot Field Audit

**Status: Working evidence ledger, 2026-08-14.**

This document collates the Ghidra evidence returned by the three shot-audit
workers. It deliberately distinguishes completed function-by-function audits,
exact DTI-size proofs, preliminary constructor findings, and work that remains
incomplete. It is not a claim that the expanded audit is finished.

## Scope

The first audit covered all 58 unique DTIs represented by
`generated_shot_tail_fields.inc`. The expanded inventory found 299 concrete shot
DTIs:

| Partition | Count |
|---|---:|
| `uShotVulcanA*` through `uShotVulcanM*` | 145 |
| `uShotVulcanN*` through `uShotVulcanZ*` | 78 |
| Laser, Missile, Option, and Shield | 76 |
| Total | 299 |

The non-Vulcan inventory consists of 37 Laser, 11 Missile, 10 Option, and 18
Shield classes. No additional concrete direct-`uShot` family was found.

An exact registered size of `0x2140` proves that a class has no concrete bytes
at or after the concrete-tail boundary. Such a class needs no concrete scalar or
pointer rule even if it has class-specific gameplay virtuals.

## Restore Rule

`ConcreteShotSnapshot_Restore` preserves words that look like accessible
pointers and preserves complete embedded `MtObject` owners. Consequently, an
otherwise flat body overlay does not reliably restore:

- Packed scalar qwords that happen to resemble mapped addresses.
- Scalar state inside a preserved embedded owner.
- Historical pointers, resources, child shots, effects, or reference handles.

Pointer-free fields in the first two categories require explicit per-DTI scalar
ranges. Pointer fields require reconstruction, remapping, or lifecycle-aware
rebinding; the consolidated reference ledger is below.

## Common Embedded Owner

Many extended shots embed a `uCnsTSR`-style owner of size `0xD0`. For an owner
beginning at `B`, the workers consistently recovered this split:

| Owner-relative range | Classification |
|---|---|
| `+0x00` | Vtable |
| `+0x18`, `+0x20` | Scheduler links |
| `+0x40` | Embedded constraint vtable |
| `+0x48` | Self-reference |
| `+0x50`, `+0x58`, `+0x60` | Particle parent/sibling links |
| `+0x08..+0x17` | Pointer-free state |
| `+0x28..+0x3F` | Pointer-free state |
| `+0x68..+0xCF` | Pointer-free transform/control state |
| `+0x90`, `+0xA0`, `+0xB0` | Translation, scale, and rotation vectors |
| `+0xC0`, `+0xC4` | Mode/order scalars |

The native destructor unlinks particle state and destroys the constraint base.
Historical links must not be copied. Gameplay methods do directly mutate some
of the pointer-free transform fields, so those bytes may need explicit restore
even though the containing owner remains fresh.

## Generated-DTI Audit

The initial function-by-function audit covered every DTI emitted by the generated
property schema. The following ranges were found outside that schema and were
added to `kVerifiedShotScalarRanges`.

| DTI | Shot | Additional pointer-free ranges |
|---:|---|---|
| `0x140D4CF60` | `uShotLaserBigThunder` | `+0x2160/1` tail flag |
| `0x140D490D0` | `uShotLaserCrystal` | `+0x2160/4` effect variant |
| `0x140D4BF38` | `uShotLaserDarkFire` | `+0x2170/0x10` effect position |
| `0x140D49988` | `uShotLaserDrDoomJS` | `+0x2230/4` state |
| `0x140D4BB08` | `uShotLaserGMBeam` | `+0x2240/0x10` vector |
| `0x140D4C870` | `uShotLaserHyperMysticBlast` | `+0x2168/1`, `+0x216C/4` gate/state |
| `0x140D49118` | `uShotLaserMillionCarat` | `+0x2160/7` timer and wave latches |
| `0x140D4C8B8` | `uShotLaserMysticBlast` | `+0x2168/4` setup scalar |
| `0x140D499C8` | `uShotLaserPlasmaBeam` | `+0x2230/4` phase |
| `0x140D4AD20` | `uShotLaserProtonCannon` | `+0x2230/4` mode |
| `0x140D4C5B0` | `uShotLaserReflectSinkuHadouken` | `+0x23D0/0x12` reflected state |
| `0x140D4D7E0` | `uShotLaserRekkouha` | `+0x2314/8` timer and spawn phase |
| `0x140D48AA8` | `uShotLaserSatelliteLaser` | `+0x2160/1` one-shot latch |
| `0x140D4C5F0` | `uShotLaserSinkuHadouken` | `+0x2300/4` model offset |
| `0x140D49168` | `uShotLaserThunderBolt` | `+0x2238/1` effect latch |
| `0x140D48A68` | `uShotOptionShieldSlash` | `+0x2144/0x0C` rotation state |
| `0x140D48780` | `uShotOptionSickleBoomerang` | `+0x2218/4` effect variant |
| `0x140D4A968` | `uShotShieldOuroboros` | `+0x2210/4`, `+0x2218/1`, `+0x221C/4`, `+0x2220/1` |
| `0x140D4D470` | `uShotShieldRaidSword` | `+0x2148/0x10` mode, rotation, timer |
| `0x140D4D4C8` | `uShotShieldSpiralSword` | `+0x2214/4` update counter |
| `0x140D48808` | `uShotVulcanAxe` | `+0x21D0/4` rotation accumulator |
| `0x140D4A588` | `uShotVulcanBombArrow` | `+0x2140/1`, `+0x2148/8` latch/timer/subtype |
| `0x140D488B8` | `uShotVulcanCrossSword` | `+0x2150/0x10` homing direction |
| `0x140D48BB0` | `uShotVulcanFireGrenadeBomb` | `+0x21D0/4`, `+0x2218/1` rotation/stop latch |
| `0x140D48900` | `uShotVulcanGreatSorcery` | `+0x214C/8` event countdown/index |
| `0x140D4C998` | `uShotVulcanMysticStareBomb` | `+0x2140/0x18` phase/timing |
| `0x140D48EE8` | `uShotVulcanSatelliteLaserLock` | `+0x2228/4` input timer |
| `0x140D4D3D0` | `uShotVulcanShikiDanKobun` | `+0x2140/4`, `+0x2148/0x0B` phase/timestamp/flags |
| `0x140D4D6A0` | `uShotVulcanShockingPinkBomb` | `+0x2140/4`, `+0x2150/0x30`, `+0x2184/0x0C`, `+0x2198/5`, `+0x21A0/4`, `+0x21B8/8` |
| `0x140D4D5E8` | `uShotVulcanStormSwordSpread` | `+0x2144/5` timer/latch |
| `0x140D489E0` | `uShotVulcanTorch` | `+0x2214/1` powered flag |
| `0x140D4CBC8` | `uShotVulcanWebThrow` | `+0x2144/4` phase timestamp |

### Generated-DTI Function Evidence

Representative methods that establish the gameplay use of these fields:

| Shot | Audited evidence |
|---|---|
| `uShotLaserMillionCarat` | ctor `0x140163510`, `vfn121` `0x140163690` |
| `uShotLaserHyperMysticBlast` | ctor `0x1401AB360`, move `0x1401AB5E0`, setup `0x1401AB6D0` |
| `uShotLaserReflectSinkuHadouken` | move `0x1401A7D00`, `vfn91` `0x1401A7880`, `vfn124` `0x1401A83E0`, `vfn126` `0x1401A8530` |
| `uShotLaserRekkouha` | move `0x1401C49C0`, `vfn120` `0x1401C4DF0`, setup `0x1401C4F90` |
| `uShotOptionShieldSlash` | setup `0x14015C0F0`, `vfn91` `0x14015BC90`, helper `0x14015BE20` |
| `uShotShieldOuroboros` | `vfn91` `0x140183900`, move `0x140184010`, setup `0x140184110` |
| `uShotShieldRaidSword` | `vfn91` `0x1401BE3E0`, `vfn118` `0x1401BEB60`, setup `0x1401BECA0` |
| `uShotVulcanBombArrow` | `vfn99` `0x14017F1C0`, `vfn118` `0x14017F3E0`, setup `0x14017F6A0` |
| `uShotVulcanMysticStareBomb` | move `0x1401AE270`, `vfn118` `0x1401AE2B0`, setup `0x1401AE400` |
| `uShotVulcanShockingPinkBomb` | `vfn94` `0x1401C2380`, move `0x1401C2AC0`, `vfn118` `0x1401C2B90`, helper `0x1401C2F80` |

## Expanded Completed Findings

The ranges below come from completed expanded audits and are not yet all present
in `kVerifiedShotScalarRanges`.

### Missile

All 11 Missile classes are complete. Nine end at `0x2140`.

| DTI | Shot | Size | Concrete result | Evidence |
|---:|---|---:|---|---|
| `0x140D49A08` | `uShotMissileDoomsMissile` | `0x2150` | Scalar `+0x2140/4`; no pointer | ctor `0x14016E610`, move `0x14016E850`, setup `0x14016E910` |
| `0x140D4B398` | `uShotMissileJammingBomb` | `0x2150` | Embedded `cRef +0x2140/0x10`; no POD | ctor `0x140192300`, dtor `0x140192360`, setup `0x140192520` |

Size `0x2140`: `uShotMissileArrow`, `FloatBomb`, `GravitySqueeze`,
`GrenadeLauncher`, `HomingEnergyGun`, `Hysteric`, `OpticalMagicSpread`,
`TKShot`, and `TrishBullet`.

### Option

All 10 Option classes are complete.

| DTI | Shot | Size | Pointer-free class state | Pointer/owner state |
|---:|---|---:|---|---|
| `0x140D4CA20` | `uShotOptionAttackWire` | `0x2160` | `+0x2140..+0x2142`, `+0x2144/4`, `+0x2148/4` | Resource `+0x2150` |
| `0x140D49250` | `uShotOptionGrapple` | `0x2150` | `+0x2140/1` | None |
| `0x140D4BD00` | `uShotOptionHealingFieldGathering` | `0x2150` | `+0x2140/4` | None |
| `0x140D4D430` | `uShotOptionRoundTrip` | `0x22F0` | Owner POD ranges plus `+0x22E0/4`, `+0x22E4/1` | Two `uCnsTSR` owners `+0x2140`, `+0x2210` |
| `0x140D48A68` | `uShotOptionShieldSlash` | `0x2300` | `+0x2140/0x10` plus owner POD | Resource `+0x2150`; owners `+0x2160`, `+0x2230` |
| `0x140D48780` | `uShotOptionSickleBoomerang` | `0x2220` | Owner POD, `+0x2210/4`, `+0x2214/1`, `+0x2218/4` | Owner `+0x2140` |
| `0x140D4D118` | `uShotOptionSpadaBoomerang` | `0x2300` | Owner POD plus `+0x22E0/0x20` | Owners `+0x2140`, `+0x2210` |
| `0x140D4CA60` | `uShotOptionSwingWire` | `0x2160` | `+0x2148/8`, `+0x2150/1`, `+0x2154/8`, `+0x215C/1` | Resource `+0x2140` |

`uShotOptionPsionicCubeDamage` and `uShotOptionSoulFistAbsorb` have size
`0x2140` and no concrete tail.

Important owner-side references were also found outside the shot bodies:

- Captain stores `uShotOptionShieldSlash*` at character `+0x68F0`.
- The corresponding Dante path stores `uShotOptionSpadaBoomerang*` at character
  `+0x68F0`.

These references require saved-to-restored shot remapping.

### Shield

All 18 Shield classes are complete.

| DTI | Shot | Size | Pointer-free concrete state | Pointer/owner state |
|---:|---|---:|---|---|
| `0x140D4BB90` | `uShotShieldBodyBeam` | `0x2150` | `+0x2140/4` phase | None |
| `0x140D4A920` | `uShotShieldFormationD` | `0x2150` | `+0x2140/0x0D` | None |
| `0x140D4BD40` | `uShotShieldHealingField` | `0x2160` | `+0x2140/0x0C` | Embedded `cRef +0x2150/0x10` |
| `0x140D48F70` | `uShotShieldKikousyou` | `0x2150` | `+0x2140/4` phase | None |
| `0x140D487C0` | `uShotShieldKnightShield` | `0x2150` | `+0x2140/4` mode | Resource `+0x2148` |
| `0x140D4A968` | `uShotShieldOuroboros` | `0x2230` | Owner POD; `+0x2210/8`, `+0x2218/1`, `+0x221C/4`, `+0x2220/1` | Owner `+0x2140` |
| `0x140D4A9A8` | `uShotShieldOuroborosExit` | `0x2150` | `+0x2140/4`, `+0x2144/1` | None |
| `0x140D4D470` | `uShotShieldRaidSword` | `0x2160` | `+0x2140/0x18` | None |
| `0x140D4D4C8` | `uShotShieldSpiralSword` | `0x2220` | Owner POD; `+0x2210/8` | Owner `+0x2140` |
| `0x140D4D508` | `uShotShieldStormSword` | `0x2220` | Owner POD; `+0x2210/4`, `+0x2214/2` | Owner `+0x2140` |
| `0x140D4CDE0` | `uShotShieldSuperBurningSkrull` | `0x2150` | `+0x2140/4` phase | None |
| `0x140D4A098` | `uShotShieldWeaponCartSpread` | `0x2150` | `+0x2140/8` | None |

Size `0x2140`: `uShotShieldFlameGun`, `HellFire`, `JamSession`,
`RepulsarBlastSP`, `Tempest`, and `WildPitchHit`.

### Vulcan A-C

The assigned A-C batch is complete. Significant extended tails:

| DTI | Shot | Size | Pointer-free state | Pointer/owner state | Evidence |
|---:|---|---:|---|---|---|
| `0x140D49310` | `uShotVulcanAcidRain` | `0x2150` | `+0x2140/8` | None | spawn `0x140166150`, setup `0x140166540` |
| `0x140D4AF00` | `uShotVulcanAnkihouBomb` | `0x2260` | `+0x21D0/0x10`, `+0x2204/4`, `+0x2210/4`, `+0x2220/0x30` | Owner `+0x2140`; `cRef +0x2250` | ctor `0x14018AC60`, dtor `0x14018AD10`, setup `0x14018B930` |
| `0x140D4AFE0` | `uShotVulcanAnkihouRolling` | `0x2240` | `+0x21D0/0x0C`, `+0x2210/0x20`, `+0x2230/1` | Owner `+0x2140` | ctor `0x14018C840`, setup `0x14018D110` |
| `0x140D4CEC8` | `uShotVulcanArrowShot` | `0x2150` | `+0x2140/4`, `+0x2144/1` | None | `vfn91 0x1401B5030` |
| `0x140D48808` | `uShotVulcanAxe` | `0x2220` | `+0x21D0/4`, `+0x2210/2` | Owner `+0x2140`; resource `+0x2218` | `vfn115 0x1401581F0`, `vfn118 0x1401583E0` |
| `0x140D4D178` | `uShotVulcanBarrier` | `0x2160` | `+0x2140/4` | `cRef +0x2148/0x10` | setup `0x1401B83E0` |
| `0x140D4D1C8` | `uShotVulcanBarrierSpread` | `0x2160` | `+0x2140/4`, `+0x2150/0x10` | None | `vfn110 0x1401B88D0` |
| `0x140D4A588` | `uShotVulcanBombArrow` | `0x2160` | `+0x2140/1`, `+0x2144/0x14` | None | `vfn118 0x14017F3E0` |
| `0x140D4A5E8` | `uShotVulcanBombArrowExplode` | `0x2150` | `+0x2140/4` | None | `vfn92 0x14017FB50` |
| `0x140D4D660` | `uShotVulcanBoomerang` | `0x2240` | `+0x21D4/8`, `+0x2210/0x26` | Owner `+0x2140` | `vfn91 0x1401C1510`, `vfn118 0x1401C16F0` |
| `0x140D49398` | `uShotVulcanBulletDanceFinish` | `0x2210` | `+0x21D8/4` | Owner `+0x2140` | `vfn118 0x140166D20` |
| `0x140D4D268` | `uShotVulcanChushokuRash` | `0x2150` | `+0x2148/1` | Child reference `+0x2140` | move `0x1401B9A60` |
| `0x140D4C080` | `uShotVulcanClaymoreMine` | `0x2160` | `+0x2140/0x14` with packed flags | None | `vfn118 0x1401A25E0` |
| `0x140D488B8` | `uShotVulcanCrossSword` | `0x2170` | `+0x2140/0x0C`, `+0x2150/0x10`, `+0x2160/4` | None | `vfn124 0x140159310` |

Size `0x2140` in this batch: `AnkihouBombExplode`, `AnkihouKobun`,
`AntmanArrow`, `ArrowRain`, `BigFirePillar`, `BigTornado`, `BodyPressFinish`,
`BreakClock`, `BreakFireChain`, `BreakVase`, `BrownEnvelope`,
`BrownEnvelopeExplode`, `Bullet`, `BurningKick`, `Car`, and `ClaymoreSpread`.

### Vulcan D-H

The D-H worker completed exact size enumeration for all assigned classes and a
full audit for the following extended classes:

| DTI | Shot | Size | Pointer-free state | Pointer/owner state |
|---:|---|---:|---|---|
| `0x140D4C100` | `uShotVulcanDigHole` | `0x2150` | `+0x2140/1`, `+0x2144/4` | None |
| `0x140D4C6E0` | `uShotVulcanDirectAttackBomb` | `0x2210` | `+0x21D0/0x0C` | Owner `+0x2140` |
| `0x140D4B888` | `uShotVulcanDogMissile` | `0x2150` | `+0x2140/4` | None |
| `0x140D4D220` | `uShotVulcanEnergyBolt` | `0x2160` | `+0x2140/0x15` | None |
| `0x140D4BBD8` | `uShotVulcanEnergySpear` | `0x2150` | `+0x2140/1` | None |
| `0x140D4BC18` | `uShotVulcanEnergySpearSpread` | `0x2150` | `+0x2140/2` | None |
| `0x140D4BA48` | `uShotVulcanExatkRocketLauncher` | `0x2150` | `+0x2140/1`, `+0x2144/4` | None |
| `0x140D4BF78` | `uShotVulcanFire` | `0x2150` | `+0x2140/4` | None |
| `0x140D49750` | `uShotVulcanFireCarpet` | `0x2150` | None | `cRef +0x2140/0x10` |
| `0x140D48BB0` | `uShotVulcanFireGrenadeBomb` | `0x2220` | `+0x21D0/4`; property-backed `+0x2214/4`, `+0x2218/1` | Owner `+0x2140` |
| `0x140D4BFB8` | `uShotVulcanFirePillar` | `0x2150` | `+0x2144/8` | None |
| `0x140D4AA70` | `uShotVulcanFormationCBom` | `0x2150` | `+0x2140/1` | None |
| `0x140D4AAF0` | `uShotVulcanFormationCSpread` | `0x2150` | `+0x2140/1` | None |

Size `0x2140` was proven for 29 assigned D-H classes, including
`DarkPhoenixFire`, `DirectAttackBombExplode`, `DivePunch`, `EnergyGun`,
`FallRock`, `FileStorm`, `FireGrenadeExplode`, `FireGrenadeGroundFlame`,
`FireWall`, `FireWorks`, all size-zero Formation children listed by the worker,
`FurisosoguShi`, `GammaQuake`, `GatlingGun`, `GenmuZero`, `GouHadouken`,
`GrenadeLauncherExplode`, `Hadangeki`, `Hadouken`, `HammerImpact`,
`HandGrenadeExplode`, `HandGun`, both HomingEnergyGun explosions,
`HyperAimArrow`, and `HyperTrapSpread`.

The following D-H classes have exact DTI and size evidence but still lacked a
complete gameplay-method pass when the results were collated:

| DTI | Shot | Size |
|---:|---|---:|
| `0x140D4B658` | `uShotVulcanFinishingShower` | `0x2170` |
| `0x140D4BC60` | `uShotVulcanGMPulse` | `0x2150` |
| `0x140D4AC98` | `uShotVulcanGammaSlam` | `0x2160` |
| `0x140D48900` | `uShotVulcanGreatSorcery` | `0x2160` |
| `0x140D48CD0` | `uShotVulcanHandGrenadeBomb` | `0x2220` |
| `0x140D4B020` | `uShotVulcanHenkyouKi` | `0x2150` |
| `0x140D4B228` | `uShotVulcanHyperGravitation` | `0x2150` |
| `0x140D4C918` | `uShotVulcanHyperMysticSmash` | `0x22F0` |
| `0x140D4C290` | `uShotVulcanHyperTrap` | `0x2150` |
| `0x140D484D0` | `uShotVulcanHyoran` | `0x2210` |

### Vulcan N-P

All 22 requested N-P classes in the completed batch are closed.

| DTI | Shot | Size | Pointer-free state | Pointer state |
|---:|---|---:|---|---|
| `0x140D4BA90` | `uShotVulcanNemesisRocketLauncher` | `0x2150` | `+0x2140/1`, `+0x2144/4` | None |
| `0x140D4A6B0` | `uShotVulcanNetArrow` | `0x2150` | `+0x2140/1` | Non-owning target `+0x2148` |
| `0x140D48558` | `uShotVulcanOneSpark` | `0x2150` | `+0x2140/2`, `+0x2144/4` | None |
| `0x140D49E78` | `uShotVulcanOpticalMagic` | `0x2150` | `+0x2140/1` | None |
| `0x140D49EB8` | `uShotVulcanOpticalMagicS` | `0x2150` | `+0x2140/1` | None |
| `0x140D4C4C8` | `uShotVulcanPendulumLog` | `0x2150` | `+0x2140/1`, `+0x2144/4` | None |
| `0x140D49FC8` | `uShotVulcanPleaseHelpMe` | `0x2150` | `+0x2140/0x0A` | None |
| `0x140D4A788` | `uShotVulcanPoisonArrow` | `0x2150` | `+0x2140/8` | None |

Size `0x2140`: `NaruhodoKnife`, `NormalArrow`, all four OilBalloon classes,
`OneSparkZan`, `OpticalMagicSpread`, `OuroborosShot`, `PhotonShot`,
`PierceArrow`, `PlasmaStorm`, `PrickleBoard`, and `PsionicCube`.

### Vulcan R-S

The completed 19-class R-S batch found:

| DTI | Shot | Size | Pointer-free state | Pointer/owner state |
|---:|---|---:|---|---|
| `0x140D485E0` | `uShotVulcanRaikou` | `0x2150` | `+0x2140/1` | None |
| `0x140D4A7E8` | `uShotVulcanRangingBladeArrow` | `0x2150` | `+0x2140/4`, `+0x2144/1` | None |
| `0x140D4D350` | `uShotVulcanRockThrow` | `0x2210` | Owner transform/control POD, including `+0x21D4/4` | Owner `+0x2140` |
| `0x140D48EE8` | `uShotVulcanSatelliteLaserLock` | `0x2230` | Owner POD; `+0x2218/8`, `+0x2228/8` | Owner `+0x2140`, resource `+0x2210`, child `+0x2220` |
| `0x140D4D3D0` | `uShotVulcanShikiDanKobun` | `0x2180` | `+0x2140/0x13`, `+0x2160/0x10`, `+0x2170/5`, `+0x2178/4` | None |
| `0x140D4B6B0` | `uShotVulcanShilhouetteBlade` | `0x2150` | `+0x2140/8` | None |
| `0x140D4D6A0` | `uShotVulcanShockingPinkBomb` | `0x21D0` | Detailed POD ranges through `+0x21BF` | Resource `+0x21A8`, parent `+0x21B0`, `cRef +0x21C0` |

Size `0x2140`: `RaikouSub`, `RapidSlash`, `RepulsarBlastSPExpload`,
`RocketLauncher`, `RocketLauncherExplode`, `SandSplash`, `SeismoHammer`,
`ShikiDan`, `ShockingPinkExplode`, `ShotGun`, `SixCannon`, and `SmartBomb`.

### Vulcan S-T

All 27 requested S-T classes are complete.

| DTI | Shot | Size | Pointer-free state | Pointer/owner state |
|---:|---|---:|---|---|
| `0x140D49520` | `uShotVulcanSomersaultGrenade` | `0x2220` | Owner POD; `+0x2210/0x0C` | Owner `+0x2140` |
| `0x140D495D0` | `uShotVulcanSomersaultLasso` | `0x2150` | `+0x2140/1` | Target `+0x2148` |
| `0x140D49608` | `uShotVulcanSomersaultShuriken` | `0x2220` | Owner POD; `+0x2210/8` | Owner `+0x2140` |
| `0x140D4B6E8` | `uShotVulcanSoulFist` | `0x2150` | `+0x2140/5` | Effect `+0x2148` |
| `0x140D49BD0` | `uShotVulcanSphereFlame` | `0x2150` | `+0x2140/4` | None |
| `0x140D4A840` | `uShotVulcanSpreadArrow` | `0x2150` | `+0x2140/0x0C` | None |
| `0x140D4D5E8` | `uShotVulcanStormSwordSpread` | `0x2150` | `+0x2140/9` | None |
| `0x140D4CB00` | `uShotVulcanSwingWire` | `0x2160` | `+0x2140/1`, `+0x2144/8`, `+0x2158/1` | Resource `+0x2150` |
| `0x140D4BE38` | `uShotVulcanTKBind` | `0x2160` | `+0x2140/4` | `cRef +0x2148` |
| `0x140D4B078` | `uShotVulcanTenraiha` | `0x2240` | Owner POD; `+0x2210/0x24` | Owner `+0x2140` |
| `0x140D49F88` | `uShotVulcanThrowRose` | `0x2210` | Owner transform/control POD | Owner `+0x2140` |
| `0x140D4A120` | `uShotVulcanThrowWeapon` | `0x2170` | `+0x2140/0x24` with packed flags | None |
| `0x140D4A160` | `uShotVulcanThrowWeaponB` | `0x2170` | `+0x2140/0x24` with packed flag | None |
| `0x140D489E0` | `uShotVulcanTorch` | `0x2220` | Owner POD; `+0x2210/5` | Owner `+0x2140` |
| `0x140D4C570` | `uShotVulcanTrapLandMine` | `0x2150` | `+0x2140/5`, `+0x2148/8` | None |
| `0x140D48700` | `uShotVulcanTsutamaki` | `0x2220` | Owner POD; `+0x2210/1` | Owner `+0x2140` |

Size `0x2140`: `SomersaultGrenadeExplode`, `SpellOfVishanti`,
`SpiralArrow`, `TKBindSpread`, `TKShotExplode`, `Tekadama`,
`TekadamaExplode`, `TenmaGouZanku`, both ThunderCartridge classes, and
`TorchFirePillar`.

## Gameplay-Relevant References

The following concrete references were established by completed audits. They
must not be copied as scalar bytes.

| Shot | Offset | Reference/lifecycle evidence |
|---|---:|---|
| `uShotLaserDarkFire` | `+0x2160` | Child effect used by setup/kill |
| `uShotLaserHyperMysticBlast` | `+0x2160` | Child effect checked by move |
| `uShotLaserMysticBlast` | `+0x2160` | Child effect used by aim/move |
| `uShotLaserThunderBolt` | `+0x2230` | Command effect used by spawn/move/kill |
| `uShotLaserRekkouha` | `+0x2300`, `+0x2308` | Ref-counted EFL resources |
| `uShotMissileJammingBomb` | `+0x2140` | Embedded `cRef` cookie handle |
| `uShotOptionAttackWire` | `+0x2150` | Ref-counted resource |
| `uShotOptionShieldSlash` | `+0x2150` | Ref-counted resource |
| `uShotOptionSwingWire` | `+0x2140` | Ref-counted resource |
| `uShotShieldHealingField` | `+0x2150` | Embedded `cRef` |
| `uShotShieldKnightShield` | `+0x2148` | Ref-counted EFL resource |
| `uShotVulcanAxe` | `+0x2218` | EFL resource reused by hit behavior |
| `uShotVulcanBarrier` | `+0x2148` | Embedded sound/command `cRef` |
| `uShotVulcanChushokuRash` | `+0x2140` | Non-owning child/object reference |
| `uShotVulcanFireCarpet` | `+0x2140` | Embedded sound/command `cRef` |
| `uShotVulcanLandMine` | `+0x2218` | Ref-counted EFL resource |
| `uShotVulcanMysticStareBomb` | `+0x2158` | Attached helper object |
| `uShotVulcanNetArrow` | `+0x2148` | Deferred non-owning target |
| `uShotVulcanSatelliteLaserLock` | `+0x2210` | Owned EFL resource |
| `uShotVulcanSatelliteLaserLock` | `+0x2220` | Non-owning child shot |
| `uShotVulcanShockingPinkBomb` | `+0x21A8` | Owned effect resource |
| `uShotVulcanShockingPinkBomb` | `+0x21B0` | Non-owning callback parent/model |
| `uShotVulcanShockingPinkBomb` | `+0x21C0` | Embedded `cRef` |
| `uShotVulcanSomersaultLasso` | `+0x2148` | Deferred non-owning target |
| `uShotVulcanSoulFist` | `+0x2148` | Managed child effect |
| `uShotVulcanSwingWire` | `+0x2150` | Owned EFL resource |
| `uShotVulcanTKBind` | `+0x2148` | Embedded `cRef` |

The common callback array at `uShot+0x1F78..+0x1FDF` remains separately handled
by factory reconstruction and pointer-free effect timeline restoration.

Do not flat-restore the references in this ledger. Historical effect, resource,
child-shot, and helper addresses must instead use native factory reconstruction,
saved-to-restored shot remapping, or a validated resource/effect recipe.
Embedded `uCnsTSR`, `rGameEffectModel`, `cAimVec`, and `cRef` objects likewise
retain their fresh ownership links even where some internal scalar animation
state is gameplay-facing.

## Conflicting Evidence

One expanded worker reported `uShotVulcanFireGrenadeBomb`'s post-owner fields as
`+0x220C` and `+0x2210`. The generated `createProperty` evidence and the earlier
independent gameplay audit place `mBoundCount` at `+0x2214` and the terminal flag
at `+0x2218`, with the embedded owner extending through `+0x220F`. The
property-backed offsets are treated as authoritative in current source. The
eight-byte discrepancy must be rechecked directly before adopting the expanded
worker's alternate offsets.

The second expanded pass also did not always distinguish an embedded owner's
full pointer-free state from the subset proven gameplay-facing. This ledger
records broad owner POD only where the worker explicitly audited it; ownership
links remain excluded regardless.

## Remaining Coverage

At collation time:

- The complete 299-class inventory had been established.
- Missile, Option, Shield, Vulcan N-P, Vulcan R-S, and Vulcan S-T batches listed
  above were complete.
- Vulcan A-C was complete.
- Ten extended Vulcan D-H classes listed above still needed completion.
- Later Vulcan I-M and W-Z continuation jobs had not yet returned usable final
  reports.
- The independent second Laser pass had started but had not returned its final
  report. The earlier generated-DTI Laser audit remains complete.

Do not convert preliminary constructor-only findings into restore ranges until
the corresponding gameplay methods and ownership destructor have been checked.

## Fast Follow-Up Pass

After the initial collation, three new workers performed bounded, tail-focused
passes over the remaining extended Vulcan classes. Each used one Ghidra request
at a time and was limited to four requests per class, so these results prioritize
coverage over deep helper reconstruction. High-confidence scalar fields may be
promoted after a direct verification; medium and low confidence observations are
evidence only and are not patch-ready.

### D-H Follow-Up

All ten formerly incomplete D-H classes were visited. Exact DTI allocation sizes
were not recovered in this bounded pass, but every constructor writes beyond
`+0x2140`.

| Shot | Fast-pass tail evidence | Confidence |
|---|---|---|
| `uShotVulcanFinishingShower` | POD dwords `+0x2140`, `+0x2144`; floats `+0x2148..+0x2157`; vector `+0x2160` | High |
| `uShotVulcanGMPulse` | Variant/index dword `+0x2140`, read by move | High |
| `uShotVulcanGammaSlam` | POD `+0x2140`; floats `+0x2144`, `+0x2148`, `+0x214C`, `+0x2150` | High |
| `uShotVulcanGreatSorcery` | Radius `+0x2140`; countdown `+0x214C`; spawn/index `+0x2150` | High |
| `uShotVulcanHandGrenadeBomb` | Embedded model/container at `+0x2140`; angle `+0x21D0`; state/count near `+0x2214` | Medium |
| `uShotVulcanHenkyouKi` | Zeroed dword `+0x2140`; no tail gameplay access found | High |
| `uShotVulcanHyperGravitation` | Zeroed/setup flag `+0x2140` | Medium |
| `uShotVulcanHyperMysticSmash` | POD flags/state `+0x2140..+0x2148`; embedded two-element `0xD0`-stride array from `+0x2150`; gameplay float `+0x21E4` | Medium |
| `uShotVulcanHyperTrap` | Resource-derived dword `+0x2140`; enable flag `+0x2144`; timer `+0x2148`, expired at ten seconds | High |
| `uShotVulcanHyoran` | Embedded model/container at `+0x2140`; randomized angle `+0x21D8` | Medium |

Fast-pass function evidence: FinishingShower `0x140195EB0`, `0x1401961D0`,
`0x140196690`; GMPulse `0x14019D6A0`, `0x14019DAF0`, `0x14019DA40`;
GammaSlam `0x140187B10`, `0x140187E80`, `0x140187C20`; GreatSorcery
`0x140159750`, `0x140159840`, `0x140159BB0`; HandGrenadeBomb `0x14015E3F0`,
`0x14015E860`, `0x14015E720`; HenkyouKi `0x14018E530`, `0x14018E880`,
`0x14018E790`; HyperGravitation `0x140190A50`, `0x140190F50`, `0x140190B40`;
HyperMysticSmash `0x1401AC160`, `0x1401AC600`, `0x1401AC960`; HyperTrap
`0x1401A44F0`, `0x1401A47A0`, `0x1401A7200`; Hyoran `0x1401541F0`,
`0x140154420`, `0x140154300`.

### I-M Follow-Up

High-confidence size proofs establish no concrete tail for: `IceArrow`,
`IceStorm`, `JDBomb`, `JDBombExplode`, `JammingBombExplode`, `Javelin`,
`JillMachineGun`, `Kikouken`, and `Knife`.

| Shot | DTI size | Fast-pass tail evidence | Confidence |
|---|---:|---|---|
| `uShotVulcanIronPipeRolling` | `0x2230` | Embedded pointer-bearing model/effect owner from `+0x2140`; constructor initializes scalar floats through `+0x2227` | High |
| `uShotVulcanItemThrow` | `0x2240` | Embedded resource/model state through `+0x222F`; item-type scalar `+0x2230` selects one of twelve resources and landing sign | High |
| `uShotVulcanJammingBomb` | `0x2150` | Embedded `cRef +0x2140/0x10`; no POD tail | High |
| `uShotVulcanJigenZan` | `0x2150` | Timer `+0x2140`; spawn state `+0x2144`; variant `+0x2148` | High |
| `uShotVulcanKaiserPhoenix` | `0x2210` | Untyped class POD/resource state `+0x2140..+0x220F`; no named gameplay method recovered | Low |
| `uShotVulcanKingKobun` | `0x2170` | Tail exists but move only exposed a helper/liveness path; direct offsets not recovered | Medium |
| `uShotVulcanKobunLauncher` | `0x2160` | Launcher POD/state exists; setup accesses it through local accessors, offsets unresolved | Medium |

### W-Z Follow-Up

High-confidence no-tail proofs: `uShotVulcanWeskerHandGun`,
`uShotVulcanWhirlWind`, and `uShotVulcanZeroBuster` have DTI size `0x2140`.

| Shot | Fast-pass tail evidence | Confidence |
|---|---|---|
| `uShotVulcanWebBall` | Inferred extent `0x2150`; POD/flags `+0x2140..+0x2144`; likely pointer `+0x2148` | Medium |
| `uShotVulcanWebMove` | Inferred extent `0x2150`; phase/float POD `+0x2140..+0x2148` | Medium |
| `uShotVulcanWebThrow` | Inferred extent `0x2148`; local step `+0x2140`, float `+0x2144`; no gameplay-tail function inspected under cap | Medium-low |
| `uShotVulcanWeaponCart` | Inferred extent `0x22F0`; owner-managed/array region `+0x2140..+0x21FF`; POD `+0x2210..+0x228F` and `+0x2290..+0x22EF`; resource/reference pointers `+0x2280`, `+0x22A0`; model pointers `+0x2190..+0x21E8` | Medium |
| `uShotVulcanWeaponCartSpread` | Inferred extent `0x2280`; owner-managed region `+0x2140..+0x21FF`; POD `+0x2210`, `+0x2268/4` | Medium |
| `uShotVulcanWildPitch` | Inferred extent `0x2220`; owner-managed region `+0x2140..+0x21FF`; spawn latch `+0x2200`, variant `+0x2204` | Medium |
| `uShotVulcanZireitou` | Inferred extent `0x2220`; owner-managed region `+0x2140..+0x21FF`; POD `+0x2200..+0x221F`, including float `+0x2218` and state `+0x221C` | Medium |

The W-Z extents are explicitly inferred from highest constructor-owned bytes, not
direct DTI registration reads. Do not use them to define a restore range without
direct size and gameplay-method verification.
