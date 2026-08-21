# Concrete Character Field Audit

**Status: working Ghidra evidence ledger, 2026-08-14.**

This document records concrete character bytes at and after `cChr+0x68F0`.
`generated_character_tail_fields.inc` is a createProperty export, not an
ownership proof: several generated `mWallStickTime` entries are actually the
low word of a live pointer in this build. Historical helper, effect, shot,
resource, `MtArray`, `cRef`, and embedded-object storage must not be copied
flat during rollback restoration.

## Restore Policy

`ConcreteCharacterSnapshot_RestoreFields` first preserves detected live
references and complete embedded `MtObject` ranges, then restores known scalar
ranges. Explicit per-DTI ranges are needed for packed scalar state adjacent to
preserved references. Generated entries which overlap a known live reference
must be skipped entirely; restoring only their low dword corrupts the pointer.

Only exact, gameplay-facing scalar/POD fields belong in the verified range
table. Broad "apparently scalar" spans and untyped constructor clears remain
unrestored until their layout and gameplay use are proven.

## Verified Scalar/POD Fields

The following fields are absent from the generated property export and are
backed by a concrete constructor or gameplay method. All offsets are relative
to the concrete character object.

| DTI | Character | Range | Gameplay state | Evidence | Confidence |
|---:|---|---|---|---|---|
| `0x140D44F18` | `uChunli` | `+0x6900/4`, `+0x6904/4` | charge-ready flag, voice timer | `uChunLi__uChunLi` | High |
| `0x140D44F60` | `uCViper` | `+0x6908/4` | Optic Laser charge timer | `uCViper__updateOpticLaserCharge` | High |
| `0x140D45020` | `uDormammu` | `+0x68F4/4`, `+0x68F8/4`, `+0x6920/1` | second magic stock, Stalking Flare counter/lock | `uDormammu__ctor` | High |
| `0x140D45068` | `uDrDoom` | `+0x68F8/4` | single-animation action latch | `uDoctorDoom__createProperty` | High |
| `0x140D45200` | `uFrank` | `+0x6990/0x10`, `+0x69A0/0x10`, `+0x69B0/0x10`, `+0x69C4/4`, `+0x69DC/4` | Kobun scale, offset, rotation, level, experience | `uFrankWest__uFrankWest` | High |
| `0x140D45200` | `uFrank` | `+0x69EC/8`, `+0x6A48/8` | weapon speed/throw ID and throw/action wait timers | `uFrankWest__createProperty` | High |
| `0x140D45408` | `uHawkEye` | `+0x6918/4` | standing-S charge counter | `uHawkeye__tick` | High |
| `0x140D45450` | `uHiryu` | `+0x6900/1`, `+0x6920/5`, `+0x6928/4` | team-state latch, Ouroboros state/latch, voice timer | ctor, `tick`, `actionTransition_3510` | High |
| `0x140D45488` | `uHulk` | `+0x6900/1` | Gamma Wave fired latch | `uHulk__ctor` | High |
| `0x140D45578` | `uJill` | `+0x68F8/1` | Feral-install state | `uJill__uJill` | High |
| `0x140D456C0` | `uMayoi` | `+0x68F4/4` | cached initial life | `uMayoi__uMayoi` | Medium |
| `0x140D45700` | `uModok` | `+0x68F4/0x18` | Psionic level, effect-shot timing, animation state, fly height | `uMODOK__uMODOK` | High |
| `0x140D458D0` | `uNova` | `+0x6908/0x09` | Nova Force timer/stage and Centurion latch | `uNova__uNova` | High |
| `0x140D45A10` | `uRyu` | `+0x68F4/8`, `+0x6920/1`, `+0x6938/8` | Shinku range/state and Ranbu voice state | `uRyu__uRyu` | High |
| `0x140D45C58` | `uSentinel` | `+0x6909/1`, `+0x690C/4`, `+0x6910/4`, `+0x6914/1` | drone locks, phase, and spawn timer | `uSentinel__ctor` | High |
| `0x140D45D80` | `uSpiderMan` | `+0x68FC/4` | web-throw target ID | `uSpiderMan__uSpiderMan` | High |
| `0x140D45E18` | `uSuperSkrull` | `+0x690C/4`, `+0x6910/4` | Rolling Skrull height/state | `uSuperSkrull__uSuperSkrull` | High |
| `0x140D45E58` | `uTaskMaster` | `+0x6904/0x0C` | arrow selection, weapon mode, animation latch | ctor and `uTaskmaster__setWeaponModeFlags` | High/Medium |
| `0x140D45EA0` | `uThor` | `+0x6908/4`, `+0x6910/8` | speech progression, variant, latch | `uThor__uThor` | Medium |
| `0x140D45FF8` | `uWesker` | `+0x690C/4` | life-change tracker | `uWesker__uWesker` | High |
| `0x140D46038` | `uWolverine` | `+0x68F8/1` | Berserker install latch | `uWolverine__uWolverine` | High |
| `0x140D460C0` | `uZero` | `+0x6910/1` | Sogenmu install latch | `uZero__uZero` | High |
| `0x140D45958` | `uRedArremer` | `+0x68F4/1`, `+0x68FC/2`, `+0x6928/4` | ability, sync/install latches, flight-idle timer | ctor, `tick`, `sync` | High |
| `0x140D459D8` | `uRRaccoon` | `+0x6954/9`, `+0x6960/4` | weapon action, movement state/latch, voice timer | gameplay callbacks | High |

`uFrank` also has three `0x30`-byte constructor-initialized tables at
`+0x68F8`, `+0x6928`, and `+0x6958`. They are pointer-free by the current
evidence but have only medium confidence as gameplay state, so they are not
promoted until the targeted pass establishes their tick/action consumers.

## Unsafe References And Owners

These fields must remain live or be restored through a dedicated remap/rebuild
recipe. A range listed as an embedded owner includes its own vtable, reference
count, or heap backing and must never be raw-copied.

| Character | DTI | Unsafe range | Kind | Evidence |
|---|---:|---|---|---|
| `uArthur` | `0x140D44E48` | `+0x68F0/8`, `+0x6910/0x10` | trap/install unit pointer, embedded `cRef` | `uArthur__ctor`, `uArthur__tickAndArmorSfx` |
| `uChunli` | `0x140D44F18` | `+0x68F8/8` | Kikoken effect pointer | `uChunLi__uChunLi` |
| `uCViper` | `0x140D44F60` | `+0x68F0/8`, `+0x6900/8` | Burning Kick and Optic Laser effect pointers | `uCViper__uCViper`, `uCViper__tickAndSpawnBurningKick`, `uCViper__updateOpticLaserCharge` |
| `uDante` | `0x140D44FA0` | `+0x68F0/8`, `+0x6908/0x18`, `+0x6928/0x10` | Devil Trigger aura/helper pointers, embedded `cRef` | `uDante__uDante`, `uDante__cmd_spawnDtAura`, `uDante__replaceDevilTriggerAuraEffect` |
| `uDeadPool` | `0x140D44FE0` | `+0x6900/0x30` | heap-backed `MtArray` | `uDeadpool__uDeadpool` |
| `uDormammu` | `0x140D45020` | `+0x6900/0x20` | Purification/spell effect pointers | `uDormammu__ctor` |
| `uDrDoom` | `0x140D45068` | `+0x68F0/8` | active missile/emplacement shot pointer | `uDoctorDoom__tick` |
| `uFelicia` | `0x140D45138` | `+0x68F0/8` | kitten-install unit pointer | `uFelicia__uFelicia`, `uFelicia__tickAndCatHelper` |
| `uCapAmerica` | `0x140D44E90` | `+0x68F0/8` | Shield Slash option-unit pointer | ctor and `uCaptainAmerica__tick` |
| `uGhostRider` | `0x140D452B0` | `+0x68F0/0x20` | chain and Hellfire effect/slot pointers | `uGhostRider__uGhostRider` |
| `uIronFist` | `0x140D454D0` | `+0x6910/0x10` | heap-backed `MtArray` | `uIronFist__uIronFist` |
| `uIronMan` | `0x140D45518` | `+0x6910/0x10` | heap-backed `MtArray` | `uIronMan__uIronMan` |
| `uJill` | `0x140D45578` | `+0x68F0/8`, `+0x6900/0x18` | effects/cache and command reference | `uJill__uJill` |
| `uMorrigan` | `0x140D45748` | `+0x68F0/8` | Astral Vision `uMorriganSh` owner pointer | `uMorrigan__setup`, `cChr__setField_26864` |
| `uNova` | `0x140D458D0` | `+0x68F0/0x18` | Nova Force/Centurion/Super Nova effects | `uNova__uNova` |
| `uPhoenix` | `0x140D45910` | `+0x6920/0x10` | embedded `cRef` | `uPhoenix__uPhoenix` |
| `uHiryu` | `0x140D45450` | `+0x68F8/8`, `+0x6910/0x10` | option unit and frozen-target `MtArray` | ctor and `tick` |
| `uRedArremer` | `0x140D45958` | `+0x6900/8`, `+0x6908/0x20` | Chaos Tide effect and reference slots | ctor and `tick` |
| `uRRaccoon` | `0x140D459D8` | `+0x68F0/0x60` | twelve trap/effect unit pointers | ctor, `tick`, gameplay callbacks |
| `uRyu` | `0x140D45A10` | `+0x6918/8`, `+0x6928/0x10` | Shinku beam and command reference | `uRyu__uRyu` |
| `uSentinel` | `0x140D45C58` | `+0x68F0/8`, `+0x6900/8` | Hyper/drone effects | `uSentinel__ctor` |
| `uShuma` | `0x140D45CF8` | `+0x68F8/0x30`, `+0x6930/0xB58` | helper-unit ring and embedded material object | `uShumaGorath__uShumaGorath` |
| `uSpiderMan` | `0x140D45D80` | `+0x6900/8` | Web Hyper effect | `uSpiderMan__uSpiderMan` |
| `uStorm` | `0x140D45DD8` | `+0x68F0/0x18` | Hail Storm, Lightning, Wind Flight units | `uStorm__uStorm` |
| `uTaskMaster` | `0x140D45E58` | `+0x6910/8` | drone effect pointer | `uTaskMaster__ctor` |
| `uThor` | `0x140D45EA0` | `+0x68F0/0x10` | Hammer/Mighty Tornado effects | `uThor__uThor` |
| `uTrish` | `0x140D45ED8` | `+0x68F0/0x18` | Low Voltage and trap pointers | `uTrish__uTrish` |
| `uVergil` | `0x140D45F68` | `+0x6920/8`, `+0x6940/0x10` | helper pointer and embedded `cRef` | `uVergil__uVergil` |
| `uWesker` | `0x140D45FF8` | `+0x68F0/0x20` | teleport cache `MtArray`, charge effect, resource pointers | `uWesker__uWesker` |
| `uWolverine` | `0x140D46038` | `+0x68F0/8`, `+0x6900/0x28` | Berserker effect, `MtArray`, command reference | `uWolverine__uWolverine` |
| `uX23` | `0x140D46080` | `+0x68F0/8` | Trigger Install effect | `uX23__uX23` |
| `uZero` | `0x140D460C0` | `+0x68F8/0x18`, `+0x6920/8`, `+0x69A8/0x20` | effects and embedded command references | `uZero__uZero` |
| `uZeroSh` | `0x140D46118` | `+0x68F0` onward | embedded `cChrTrace`, exact extent pending | constructor | Medium |

## Generated Range Conflicts

The generated `+0x68F0/4` entry overlaps a known live reference or embedded
owner for `uArthur`, `uCapAmerica`, `uCViper`, `uDante`, `uDrDoom`, `uFelicia`,
`uGhostRider`, `uIronFist`, `uIronMan`, `uJill`, `uMorrigan`, `uNova`, `uSentinel`,
`uStorm`, `uThor`, `uTrish`, `uTron`, `uWesker`, `uWolverine`, `uX23`, `uZeroSh`,
and `uRRaccoon`. In each confirmed case it targets the low dword of a
reference/owner instead of the generated field's inferred `mWallStickTime`
label.

`RestoreCharacterGeneratedRanges` checks every generated character range
against the live and saved eight-byte words at four-byte alignment. If either
word is an accessible pointer, it does not restore that generated range. This
keeps the policy correct for the confirmed DTIs above and for any equivalent
layout reached by a different character state without maintaining a fragile
hard-coded skip list.

The Morrigan pass corrected an earlier shorthand: `uMorrigan +0x68F0` stores
the live `uMorriganSh` Astral Vision owner created by `uMorrigan__setup`.
Soul Fist effect slots are indexed storage at `cChr+0x6748`, not part of this
concrete tail. C. Viper's `+0x6908/4` is a separate Optic Laser charge timer
and is restored as a scalar after the two effect owners are preserved.

`uShuma +0x68F0/4` is a scalar (`mNumAttachedEyeBalls`), not
`mWallStickTime`; it remains safe to restore despite the generated label.

## Final Six-Request Pass

Each class that needed follow-up received no more than six sequential Ghidra
requests across all passes. This final table supersedes earlier partial notes
where it provides a more exact range. Ranges marked unresolved are intentionally
not restored explicitly.

| Character | Exact scalar/POD additions or complete scalar tail | Exact unsafe ranges | Final state |
|---|---|---|---|
| `uAmaterasu` | none beyond generated fields | none proven | constructor found but not decompiled within cap |
| `uArthur` | `+0x68F8/4` flags; `+0x68FC/0x14` EX/head motion | `+0x68F0/8` trap/install unit; `+0x6910/0x10` `cRef` | high confidence |
| `uCapAmerica` | none beyond generated fields | `+0x68F0/8` Shield Slash option-unit pointer | complete `0x6900` layout; generated range is pointer-filtered |
| `uChunli` | `+0x6900/8` charge-ready/voice timer | `+0x68F8/8` Kikoken effect | high confidence |
| `uCViper` | `+0x6908/4` Optic Laser charge timer | `+0x68F0/8`, `+0x6900/8` Burning Kick/Optic Laser effects | high confidence |
| `uDante` | generated `+0x68FC/8` charge fields | `+0x68F0/8`, `+0x6908/0x18` helper/effect references; `+0x6928/0x10` `cRef` | remaining bytes unresolved |
| `uDeadPool` | `+0x68F0/8` teleport state | `+0x6900/0x30` `MtArray` | remaining tail unresolved |
| `uDormammu` | `+0x68F4/8`, `+0x6920/1` | `+0x6900/0x20` effect pointers | high confidence |
| `uDrDoom` | `+0x68F8/4` single-animation latch | `+0x68F0/8` active missile/emplacement | high confidence pointer conflict |
| `uDrStrange`, `uDrStrangeSh` | `uDrStrangeSh +0x6950/1` enable flag | none proven | inherited base tail unresolved |
| `uFelicia` | none beyond generated fields | `+0x68F0/8` kitten-install unit pointer | high confidence pointer conflict |
| `uFeliciaC` | `+0x68F4/4` family/type POD | none proven | medium confidence |
| `uFeliciaF` | generated `+0x68F0/4` family type; `+0x68F4/4` POD | none proven | high confidence |
| `uFrank` | `+0x6990/0x30`, `+0x69C4/4`, `+0x69DC/4`, `+0x69EC/8`, `+0x6A48/8` | `+0x6A00/0x10` `MtArray` | tables outside these ranges unresolved |
| `uGalactus` | generated scalar tail plus embedded vector state | none proven | vector padding unresolved |
| `uGhostRider` | none | `+0x68F0/0x20` chain/Hellfire pointers | high confidence pointer conflict |
| `uHawkEye` | `+0x68FC/0x18` standing-S motion tuning; `+0x6918/4` charge counter | none proven | `+0x68F4/8`, `+0x6914/4` unresolved |
| `uHiryu` | `+0x6900/1`, `+0x6920/5`, `+0x6928/4` | `+0x68F8/8` option unit; `+0x6910/0x10` `MtArray` | complete `0x6930` layout |
| `uHulk` | `+0x6900/1` Gamma Wave latch | none | high confidence |
| `uIronFist` | generated chain/count fields | `+0x68F0/0x10` effects; `+0x6910/0x10` `MtArray` | high confidence pointer conflict |
| `uIronMan` | `+0x68F8/2` effect latches | `+0x68F0/8`, `+0x6900/8`, `+0x6920/8` effects; `+0x6910/0x10` `MtArray` | high confidence pointer conflict |
| `uJill` | `+0x68F8/1` Feral latch | `+0x68F0/8`, `+0x6900/0x18` | high confidence pointer conflict |
| `uLeilei` | generated fields only | none proven | `+0x68F0/0x40` unresolved |
| `uMagneto` | scalar-only `+0x68F0..+0x690B` | none | individual fields not all typed |
| `uMayoi` | `+0x68F4/4` initial-life cache | none | high confidence |
| `uModok` | `+0x68F4/0x1C` Psionic/flight state | `+0x6910/0x18` tracked effects | high confidence |
| `uNemesis` | all `+0x68F0..+0x6950` scalar/POD | none | high confidence safety classification |
| `uMorrigan` | none beyond generated fields | `+0x68F0/8` Astral Vision `uMorriganSh` owner | high confidence pointer conflict |
| `uNova` | `+0x6908/9` Nova Force/Centurion state | `+0x68F0/0x18` effects | high confidence pointer conflict |
| `uPhoenix` | generated fields | `+0x6920/0x10` command `cRef` | other bytes unresolved |
| `uRedArremer` | `+0x68F4/1`, `+0x68FC/2`, `+0x6928/4` | `+0x6900/8` effect; `+0x6908/0x20` references | `+0x68F8/4` reset-only state unresolved |
| `uRRaccoon` | `+0x6954/9`, `+0x6960/4` action/movement/voice state | `+0x68F0/0x60` unit pointers | `+0x6950/2` reset-only flags unresolved; generated range is pointer-filtered |
| `uRedArremerSh` | `+0x68F0/1`, `+0x68F4/4`, `+0x68F8/2`, `+0x6908/1`, `+0x6938/4` | `+0x6910/8` effect | remaining bytes unresolved |
| `uRyu` | special existing policy restores Shinku/Ranbu scalar state | `+0x6918/8`, `+0x6928/0x10` rebuilt/cleared by recipe | high confidence |
| `uSentinel` | `+0x6909/1`, `+0x690C/9` drone state | `+0x68F0/8`, `+0x6900/8` effects | high confidence pointer conflict |
| `uShuma` | generated `+0x68F0/4` attached-eyeball count | `+0x68F8/0x30`, `+0x6930/0xB58` | high confidence |
| `uSpiderMan` | `+0x68FC/4` web target | `+0x6900/8` Web Hyper effect | high confidence |
| `uStorm` | generated `+0x6914/4` Lightning Attack count | `+0x68F0/0x18` unit pointers | high confidence pointer conflict |
| `uSuperSkrull` | `+0x690C/8` height/state; generated rolling motion fields | none | high confidence |
| `uTaskMaster` | `+0x68F0/0x0C`, `+0x6900/0x10` arrow/action state | `+0x6910/8` drone effect | high confidence |
| `uThor` | `+0x6900/1`, `+0x6908/4`, `+0x6910/8` | `+0x68F0/0x10` effects | high confidence pointer conflict |
| `uTrish` | generated `+0x6910/0x14` shot position/animation state | `+0x68F0/0x18` effect/traps | high confidence pointer conflict |
| `uTron` | none proven | `+0x68F0/8` live unit pointer | high confidence pointer conflict |
| `uVergil` | generated flags/charge; `+0x691C/4`, `+0x6930/0x0C` | `+0x6920/8`, `+0x6940/0x10` | high confidence |
| `uWesker` | `+0x690C/4`; generated sunglasses/aim ranges | `+0x68F0/0x10`, `+0x6900/8`, `+0x6910/0x10` | high confidence pointer conflict |
| `uWolverine` | `+0x68F8/1` Berserker latch | `+0x68F0/8`, `+0x6900/0x10`, `+0x6910/0x10` | high confidence pointer conflict |
| `uX23` | none proven | `+0x68F0/8` Trigger Install effect | high confidence pointer conflict |
| `uZero` | `+0x6910/1`; generated charge/saber fields | effects `+0x68F8/0x18`, `+0x6920/8`; refs `+0x69A8/0x20` | high confidence |
| `uZeroSh` | none proven | embedded `cChrTrace` begins `+0x68F0` | exact extent unresolved |
| `uZombie`, `uVJoe` | none proven | none proven | constructor body unavailable at cap |

## Coverage And Remaining Work

The first bounded pass touched every class represented by
`generated_character_tail_fields.inc`: 58 unique concrete DTIs, including
special forms and boss/helper characters. It confirmed no additional exact
field in the first pass for `uAmaterasu`, `uArthur`, `uCViper`, `uCapAmerica`,
`uChris`, `uDrStrange`, `uDrStrangeSh`, `uFelicia`, `uFeliciaC`, `uFeliciaF`,
`uGalactus`, `uGouki`, `uHaggar`, `uHawkEye`, `uHiryu`, `uIronFist`,
`uIronMan`, `uLeilei`, `uMagneto`, `uMorrigan`, `uNaruhodo`, `uNemesis`,
`uPhoenix`, `uRRaccoon`, `uRedArremer`, `uRedArremerSh`, `uSheHulk`,
`uShuma`, `uSpencer`, `uTron`, `uVJoe`, `uVergil`, `uX23`, `uZombie`, and
`uZeroSh`.

This is not final for classes whose concrete constructor or update method was
not exposed within the initial two-request budget. The next targeted Ghidra
pass is limited to two further requests per unresolved class and will either
record exact fields/pointers or leave the class explicitly unresolved rather
than infer a broad range.

## Eight-Request Follow-Up

The per-character cap was raised once, from six to eight sequential Ghidra
requests. The following findings refine the final-pass table:

| Character | Newly resolved fields | Newly resolved unsafe ownership | Evidence |
|---|---|---|---|
| `uAmaterasu` | DTI size `0x6920`; generated `+0x68F0/4`, `+0x6900/4`, `+0x6910/1` confirmed | none proven | `uAmaterasu__ctor` |
| `uCapAmerica` | DTI size `0x6900` | `+0x68F0/8` `uShotOptionShieldSlash*` | shot-owner store |
| `uDrDoom` | DTI size `0x6910` | `+0x68F0/8` active missile/emplacement | constructor and tick |
| `uDrStrangeSh` | DTI size `0x6960`; `+0x6950/1` enable | none proven | `uDrStrangeSh__tick` |
| `uFrank` | `+0x6A50/1` summon latch | `+0x6A28/0x18` photo-entity pointers | ctor and tick |
| `uGalactus` | DTI size `0x6950`; vectors `+0x6930/0x10`, `+0x6940/0x10` | none proven | ctor and tick |
| `uMagneto` | DTI size `0x6910`; complete scalar/POD tail `+0x68F0/0x20` | none proven | ctor and tick |
| `uPhoenix` | DTI size `0x6940`; generated scalar fields reconfirmed | `+0x68F8/0x20` four Fallen-Phoenix effect pointers | ctor and tick |
| `uTaskMaster` | complete scalar/POD `+0x68F0/0x10` | `+0x6910/8` drone effect | ctor |
| `uWolverine` | DTI size `0x6928` | command/reference owner is `+0x6910/0x18` | ctor |
| `uZombie` | DTI size `0x6920`; `+0x68F4/8` action timer and voice toggle | none proven | ctor and tick |

The eight-request cap leaves the concrete layouts for `uDrStrange`, `uGouki`,
`uHiryu`, `uLeilei`, `uNaruhodo`, `uRRaccoon`, `uRedArremer`, `uVJoe`, and
`uZeroSh` partially or wholly unresolved. Their unproven ranges remain under
the existing pointer/embedded-object preservation policy and are not promoted
to explicit scalar restoration.

## Twelve-Request Resolution

A final four-request pass per remaining class resolved all but the internal
layout and final allocation size of `uZeroSh::cChrTrace`. That object already
remains live through embedded-object detection; only its separately consumed
post-trace latch is restored explicitly.

| Character | DTI size | Scalar/POD state | Unsafe owner/reference state |
|---|---:|---|---|
| `uDrStrange` | `0x6950` | constructor-zeroed `+0x68F0..+0x694B`; no gameplay semantics or ownership type proven | none proven |
| `uGouki` / `uAkuma` | `0x6900` | `+0x68F0/2` tag/team-switch flags | none |
| `uHiryu` | `0x6930` | `+0x6900/1` team-state latch; `+0x6920/4`, `+0x6924/1`, `+0x6928/4` Ouroboros state | `+0x68F8/8` option unit; `+0x6910/0x10` Ouroboros frozen-target `MtArray` |
| `uLeilei` / `uHsienKo` | `0x6930` | `+0x6900/4` reserved state, `+0x6908/0x0C` base position, `+0x6918/4` pendulum length, `+0x691C/4` state | `+0x68F0/0x10` helper units; `+0x6928/8` command reference |
| `uNaruhodo` / `uPhoenixWright` | `0x69B0` | generated keep-item/reversal state `+0x6918/0x0D`; remaining helper state bank is not semantically typed | evidence slots `+0x68F0/0x28`, helper `+0x6928/8`, command reference `+0x6930/8` |
| `uRRaccoon` / `uRocketRaccoon` | `0x6970` | gameplay action/movement/voice state `+0x6954/9`, `+0x6960/4`; reset-only `+0x6950/2` unresolved | twelve bomb/mine unit pointers `+0x68F0/0x60` |
| `uRedArremer` / `uFirebrand` | `0x6930` | `+0x68F4/1` ability latch, `+0x68FC/2` sync/Chaos Tide latches, `+0x6928/4` flight-idle timer; reset-only `+0x68F8/4` unresolved | Chaos Tide effect `+0x6900/8`; four reference slots `+0x6908/0x20` |
| `uVJoe` / `uViewtifulJoe` | `0x6910` | generated `+0x68F0/1` Shocking Pink latch; `+0x6900/0x0C` vector | none |
| `uZeroSh` | size unavailable | `+0x7090/4` trace status/dispatch latch | embedded `cChrTrace` begins `+0x68F0`; internal extent remains live/unrestored |

The previous eight-request unresolved summary is superseded by this table.

`cChrTrace__ctor` confirms that the embedded `uZeroSh +0x68F0` owner contains
an owning vtable, `mpChr`, delay state, and two heap-backed `MtArray` trace
buffers (each initialized for sixteen 16-byte elements). Its final outer extent
could not be recovered from the available DTI metadata, so it must remain under
the generic complete-embedded-object preservation policy. `uZeroSh__tick`
consumes the separate `+0x7090` latch, which is why that four-byte range is
explicitly restored after owner preservation.

## Playable Roster Field-Pass Ledger

The following ledger records the completed direct Ghidra pass for the internal
roster IDs `1..50`. Each entry was checked through its constructor and the
available tick, move, state, action, or gameplay consumer. The pass did not
promote broad constructor clears; it records only exact scalar evidence and
unsafe ownership findings.

| ID | Character | Concrete DTI | Pass result |
|---:|---|---:|---|
| 1 | Ryu | `0x140D45A10` | existing Shinku/Ranbu scalar recipe and owner policy |
| 2 | Chun-Li | `0x140D44F18` | verified charge/voice scalars; Kikoken owner |
| 3 | Akuma | `0x140D452F0` | verified tag/team flags; manual layout |
| 4 | Chris | `0x140D44ED0` | generated tail coverage confirmed |
| 5 | Wesker | `0x140D45FF8` | verified life tracker; cache/effect owners |
| 6 | Viewtiful Joe | `0x140D45FB0` | verified Shocking Pink/vector state |
| 7 | Dante | `0x140D44FA0` | added `+0x68F0/8` aura owner; existing helper/ref owners |
| 8 | Trish | `0x140D45ED8` | generated shot state; effect/trap owners |
| 9 | Frank West | `0x140D45200` | verified level/weapon/timer state; photo/MtArray owners |
| 10 | Spencer | `0x140D45D40` | existing concrete scalar recipe |
| 11 | Arthur | `0x140D44E48` | added `+0x68F0/8` trap/install owner; `mIsCrossSword` covered |
| 12 | Amaterasu | `0x140D44E10` | generated ability/magata/install fields confirmed |
| 13 | Zero | `0x140D460C0` | verified install latch; effect/reference owners |
| 14 | Tron | `0x140D45F20` | `+0x68F0/8` live unit owner; generated range filtered |
| 15 | Morrigan | `0x140D45748` | added `+0x68F0/8` Astral Vision owner; Soul Fist slots are indexed |
| 16 | Hsien-Ko | `0x140D455F8` | existing scalar/vector recipe; helper/ref owners |
| 17 | Felicia | `0x140D45138` | added `+0x68F0/8` kitten-install owner |
| 18 | C. Viper | `0x140D44F60` | added effect owners `+0x68F0/8`, `+0x6900/8`; charge timer `+0x6908/4` |
| 19 | Haggar | `0x140D45338` | generated scalar tail confirmed |
| 20 | Jill | `0x140D45578` | verified install latch; effect/cache/ref owners |
| 21 | Strider Hiryu | `0x140D45450` | verified Ouroboros/team state; option/MtArray owners |
| 22 | Vergil | `0x140D45F68` | generated flags/charge; vector/helper/ref owners |
| 23 | Phoenix Wright | `0x140D457C0` | generated keep-item/reversal state; helper/ref owners |
| 24 | Firebrand | `0x140D45958` | verified ability/sync/flight state; effect/ref owners; reset flag unresolved |
| 25 | Nemesis | `0x140D45890` | scalar/POD tail safety confirmed |
| 26 | Spider-Man | `0x140D45D80` | verified web target; Web Hyper owner |
| 27 | Captain America | `0x140D44E90` | manual layout; Shield Slash owner |
| 28 | Wolverine | `0x140D46038` | verified install latch; effect/MtArray/ref owners |
| 29 | Magneto | `0x140D45680` | complete scalar/POD tail confirmed |
| 30 | Hulk | `0x140D45488` | generated motion fields; Gamma Wave latch |
| 31 | She-Hulk | `0x140D45CB8` | generated scalar tail coverage confirmed |
| 32 | Taskmaster | `0x140D45E58` | verified arrow/action state; drone owner |
| 33 | Iron Man | `0x140D45518` | verified effect latches; effect/MtArray owners |
| 34 | Thor | `0x140D45EA0` | verified speech/tornado state; effect owners |
| 35 | Doctor Doom | `0x140D45068` | verified animation latch; missile/emplacement owner |
| 36 | Phoenix | `0x140D45910` | generated scalar state; effect and `cRef` owners |
| 37 | Shuma-Gorath | `0x140D45CF8` | attached-eyeball scalar; helper/material owners |
| 38 | MODOK | `0x140D45700` | verified Psionic/flight state; tracked effects |
| 39 | Dormammu | `0x140D45020` | verified spell stock/flare state; effect owners |
| 40 | Deadpool | `0x140D44FE0` | verified teleport state; `MtArray` owner |
| 41 | Storm | `0x140D45DD8` | verified Lightning count; effect owners |
| 42 | Super-Skrull | `0x140D45E18` | verified Rolling Skrull height/state |
| 43 | Sentinel | `0x140D45C58` | verified drone state; Hyper/drone owners |
| 44 | X-23 | `0x140D46080` | Trigger Install owner; generated range filtered |
| 45 | Nova | `0x140D458D0` | verified Nova Force/Centurion state; effect owners |
| 46 | Rocket Raccoon | `0x140D459D8` | verified action/movement/voice state; trap owners; reset flags unresolved |
| 47 | Ghost Rider | `0x140D452B0` | chain/Hellfire owner range; generated range filtered |
| 48 | Iron Fist | `0x140D454D0` | generated chain state; effect/MtArray owners |
| 49 | Doctor Strange | `0x140D450A8` | constructor tail remains unresolved; no gameplay ownership proven |
| 50 | Hawkeye | `0x140D45408` | verified standing-S motion/charge state; remaining bytes unresolved |

The five newly promoted unsafe rows are now represented in both this ledger
and the policy table above. The generated include remains untouched because it
is regenerated from the property export; runtime pointer filtering handles the
overlapping inferred `mWallStickTime` entries.
