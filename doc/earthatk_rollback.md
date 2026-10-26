# Heroes And Heralds Rollback State

**Status: Ghidra research, 2026-08-28. No implementation yet.**

## 2026-10-26 scoped recheck (research only)

The implementation status and estimates below are historical research, not a
fresh implementation audit or runtime result. This update directly inspected
the known snapshot source and a bounded set of native card methods. No code,
database layout/name/signature, build, game, deck setup, or dump comparison was
changed/run. Existing useful types were used read-only; misleading decompiler
casts were recorded, not propagated into shared types.

Live Marvel target: **port 8193**, display program
`umvc3.exe.unpacked.exe`, project file `/umvc3xmario5.exe`, Windows
x64, base `0x140000000`. Verify identity again
and explicitly specify the verified port before future research.

| Native anchor | Rechecked body/metadata | Evidence boundary |
|---|---|---|
| `0x1400049F0` | Returns `sGameConfig+0x1E30`; existing receiver/return labels identify `cSPCData`. | Not an alias for standalone `sBattleSetting`, disk INI preferences or the retail serialized payload. |
| `0x140228930` | Constructor initializes large inline ranges and clears two bytes starting at `+0x48A6C`; current `cSPCData` type size is `0x48A70`. | The body suggests inline storage, not a complete ownership/pointer-free proof of every byte or an independent DTI-size audit. Some current field labels/decompiler pointer arithmetic are awkward. |
| `0x140243E90` | ORs flags starting at `+0x3BE8C + (index&15)*0xC`; slots 0–2 select one DWORD, slot >2 updates all three. | The arithmetic does not by itself prove every parameter's high-level team/card meaning. |
| `0x140246940` | Stores a float at `+0x3BCCC + ((index&15)*3 + slot%3)*4`, clamping to `[0,3]`. | This is mutable card-runtime storage, not immutable settings; all callers were not reconstructed here. |
| `0x140244300` | After a successful predicate, writes explicit record argument DWORDs `+0x18/+0x1C`, clears byte `+0x49`, returns success. | The current cast of `this` to `sBattleSetting *` for `0x140242320` is not reliable ownership/alias evidence. This small body alone does not establish table base/count/stride. |

The other `sEarthAtk`, scoring, campaign, life/gauge and attack-path entries below
remain dated 2026-08-28 findings, **not newly reverified exhaustively**. Existing
names/comments are supporting evidence; no donor declaration or exact emitted
symbol retrieval was performed, and no donor provenance is claimed for the
descriptive native method names in this note.

The current inspected `rollback_snapshot.cpp` explicitly adds ten bytes named
`sGameConfig.shotSlotFlags` at `+0x4A89C` (equivalently `cSPCData+0x48A6C`).
**Boundary question:** a ten-byte extent from `+0x48A6C` ends at exclusive
`+0x48A76`, six bytes beyond the current nominal `0x48A70` `cSPCData` type.
The constructor's two-byte clear is not evidence validating all ten flags.
Resolve concrete storage/containing-object extent and native slot writers before
using this capture as proof of the `cSPCData` boundary or copying it to another
layout. This note neither changes the capture nor certifies it unsafe/safe.

Consequently the proposed `[+0x3BC00,+0x48A70)` runtime-tail region remains an
**investigation candidate**, not a shipping-safe restore contract. Audit writes
on both sides of the proposed cut (the constructor clears `0x3BC00` bytes from
`+0x8`, reaching `+0x3BC08`), ownership, resources, callbacks, mode transitions,
and excluded state. Byte agreement over tested snapshots cannot prove all cards,
setup/restart transitions or unobserved allocations correct. Any implementation,
new hook, transport change or shared-layout application needs separate scope and
review; no blanket cosmetic classification or native-online predicate change is
authorized by this research.

## Conclusion

Implementing rollback for Heroes and Heralds combat is feasible, but the main
missing state is not the `sEarthAtk` singleton. It is the mutable card runtime
inside `cSPCData`, embedded at `sGameConfig+0x1E30`.

There are two materially different projects:

1. H&H card battles using the plugin's existing ENet transport and rollback.
   This is a medium-to-large extension of the current system.
2. Reproducing Capcom's complete online H&H campaign, including faction state,
   tournament/result progression, native room messages, and Earth Attack
   scoring. This is a separate, much larger feature and should not be required
   for card-enabled rollback matches.

## `sEarthAtk`

The singleton is created by `sMvc3Manager__createEarthAtk @ 0x14025A270` and
returned by `sEarthAtk__getInstance @ 0x140004720`. Its DTI size is `0x688`.

Most of the object is persistent campaign/profile state:

- tournament side, wins, losses, draws, points, and streaks;
- daily/weekly participation and calendar timestamps;
- downloaded user data and result records;
- campaign board/menu state;
- result and Earth Attack point accumulation.

The large `uEarthAtkVingo` object is the H&H campaign board/menu. Its setup at
`0x140318C10` allocates menu/effect units and loads Earth UI resources. It is not
fight simulation state and must not be added to rollback snapshots.

Battle-relevant `sEarthAtk` behavior found in Ghidra is limited:

| Address | Function | Behavior |
|---|---|---|
| `0x140003AC0` | `sAction__calcHeraldsEarthAtkPoints` | At match start in online H&H, computes two scoring values and writes `sEarthAtk+0x230/+0x238`; one bonus path increments `+0xA8`. |
| `0x140277FA0` | `sEarthAtk__applyTeamLifeModifier` | During fight setup, applies faction bonus life and gauge rates to one or both teams. |
| `0x1402781E0` | `sEarthAtk__isTeamAffectedByBonus` | Chooses the affected team from H&H bonus flags and native local-slot ownership. It explicitly requires online H&H (`BattleType == 9`). |
| `0x140048240` | `sEarthAtk__getAtkRateEAHB` | Supplies the faction attack multiplier during damage calculation. |
| `0x140275EC0` | `sEarthAtk__selectRandomRoundEvent` | Uses global XorShift state for campaign/round event selection. The global RNG is already captured. |
| `0x140278360` | `sEarthAtk__move` | Decodes network timestamps into calendar fields; it is service/profile work, not fight simulation. |

The life, gauge, and attack rates and bonus-selection flags are configuration
read during combat. The resulting fighter life/gauge values are already stored
in captured character/team state. The Earth Attack point writes occur at phase
entry, not continuously during ordinary fighting.

Therefore, flat-copying all `0x688` bytes of `sEarthAtk` is not recommended. It
contains locks and persistent campaign state that rollback should not rewind.
If tests show a rollback window can cross the H&H phase-entry calculation, add a
small explicit scalar snapshot for the proven scoring/bonus fields instead.

## `cSPCData`: The Missing Gameplay State

`sGameConfig__getSPCData @ 0x1400049F0` returns `sGameConfig+0x1E30`. The
embedded `cSPCData` is `0x48A70` bytes (`297,584`) and is constructed at
`cSPCData__cSPCData @ 0x140228930`.

The current rollback region builder captures only ten projectile slot flags at
`sGameConfig+0x4A89C`, corresponding to `cSPCData+0x48A6C`. It does not capture
the H&H runtime.

The constructor strongly suggests an inline, allocation-free layout. Important
ranges are:

| `cSPCData` range | Approx. size | Role |
|---|---:|---|
| `+0x00008..+0x3BBFF` | `0x3BBF8` | Large card/combat parameter database; mostly configuration after match setup. |
| `+0x3BC00..+0x3C027` | `0x428` | Runtime counters, flags, old trigger values, and per-team/per-card floats. |
| `+0x3C028..+0x3E4E7` | `0x24C0` | Additional inline runtime/card records. |
| `+0x3E4E8..+0x3E567` | `0x80` | Parent/deck IDs. |
| `+0x3E568..+0x404E7` | `0x1F80` | Six `0x54`-byte records per row, 16 rows (`0x1F8` per row). Card condition transitions mutate these records during combat. |
| `+0x404E8..+0x48A6F` | `0x8588` | Remaining inline card/UI/runtime tail, including `UseSPC` and projectile flags. Needs write classification. |

Proven gameplay mutations include:

- `cSPCData__setCardRuntimeFlag @ 0x140243E90` sets per-team and
  per-card condition flags near `cSPCData+0x3BE8C`.
- `cSPCData__setCardRuntimeValueClamped @ 0x140246940` writes and clamps a
  per-team/per-card float at
  `cSPCData+0x3BCCC + index*4`.
- `cSPCData__setTeamRuntimeValue @ 0x1402469A0` writes a per-team value at
  `cSPCData+0x3BE0C`.
- `cSPCData__commitCardConditionRecord @ 0x140244300` changes state words at
  record offsets `+0x18/+0x1C` and clears byte `+0x49`.
- `cSPCData__updateLifeTimeCardConditions @ 0x140245100` evaluates life and
  synchronized action time, accumulates record timers, and commits card-state
  transitions.
- `cSPCData__applyCardEffectsForCharacter @ 0x140236A70` applies card effects,
  mutates card flags/counters, updates fighter life and team gauge, and can
  change roster record states.
- Damage and contact processing repeatedly queries and mutates this subsystem.
  `cChr__applyDamageAndHitEffects @ 0x140049D80` is one visible path.

This state is prediction-dependent. If a predicted hit is rolled back, the
card trigger records, counters, and timers must rewind with the fighters. The
full native re-simulation path should then reapply the card logic correctly.

## Snapshot Strategy

The proposed prototype candidate is to capture `[cSPCData+0x3BC00,+0x48A70)`, about 53 KiB,
as one explicit region while H&H/card mode is active, only after the safety
checks below. This would avoid copying the roughly 245 KiB parameter prefix;
its immutability during all relevant transitions remains to be established.

Before shipping that range:

1. Audit all writes to `cSPCData` during representative H&H matches.
2. Confirm the tail contains no live pointers, resource references, locks, or
   peer-local UI ownership that require preservation.
3. Narrow or split the region if the tail mixes gameplay state with unsafe UI
   state.
4. Add the region to checksums and snapdump naming, not only save/restore.
5. Verify restore across hits that activate, expire, stack, copy, or reverse a
   card condition.

Capturing the complete `0x48A70` object for **read-only diagnostics** (skipping
the vtable) could add about 291 KiB per frame. Capture is not permission to
flat-restore it: unknown ownership, pointers and UI state still require audit.
The large parameter prefix may be setup-stable, but that is not a complete
immutability proof. A final design requires evidence for both coverage and safe
restoration, not merely failure of selective coverage.

## Match Setup And Transport

The plugin currently launches normal/training/dual-training modes and exchanges
characters, colors, assists, simple mode, and stage. It does not exchange H&H
side, deck, or card selections.

The plugin also intentionally runs custom ENet matches under an offline battle
type. Setting native `BattleType == 9` globally would activate broad native
online behavior and is unsafe. The lower-risk design is:

- use offline H&H (`BattleType == 8`) for card battle rules;
- keep the custom ENet input/barrier/restart transport;
- exchange the six selected card IDs, deck/side metadata needed to construct
  identical `cSPCData`, and a hash of the initialized card configuration;
- initialize both peers through the same native card setup routines before the
  battle-ready barrier;
- keep native campaign result updates disabled or outside rollback.

Offline H&H does not execute the online-only faction bonus and Earth Attack
scoring paths. If those mechanics are desired in custom matches, reproduce only
their small deterministic setup effects rather than changing the global online
predicate.

## Work Estimate

### Card-enabled rollback battle prototype

Estimated engineering time: **4-8 focused days**.

- Add a temporary `cSPCData` runtime-tail snapshot and checksum coverage.
- Add launch/config plumbing for an H&H battle with fixed or locally prepared
  decks.
- Exchange card IDs and verify identical initialized card state.
- Exercise rollback across basic card triggers and inspect snapdumps.

This can establish feasibility but is not enough to claim all cards correct.

### Usable H&H rollback mode

Estimated engineering time: **2-4 weeks**.

- Complete the `cSPCData` write/ownership audit and implement safe selective
  restore.
- Add launch UI or saved-deck selection and protocol/version changes.
- Handle battle type, restart, rematch, result-screen, and card UI behavior.
- Add H&H runtime fields to diagnostics and desync dumps.
- Test the card-effect families: timers, hit/block triggers, life/gauge changes,
  assists/tags, X-Factor, projectile effects, copy/reverse, death, and comeback
  conditions.
- Audit card-created shots/effects/helper units against existing recipe and
  snapshot coverage.

The schedule risk is card breadth, not raw snapshot code. The central runtime
is generic, but individual cards enter many character, projectile, and effect
paths that may expose existing rollback coverage gaps.

### Complete native online H&H campaign behavior

Estimated engineering time: **1-3+ months**, with high uncertainty.

This includes native room messages such as
`cNetRoomCmu::MsgEarthAtkPlayerInfo`, faction/session ownership, downloaded
campaign state, tournament/result progression, Earth Attack points, board/menu
flows, and persistence rules. It should be treated as a separate feature, not a
prerequisite for rollback card battles.

## Recommended First Milestone

Implement a developer-only fixed-deck H&H launch using `BattleType == 8`, capture
the tentative `cSPCData` runtime tail, and run two-peer forced rollbacks across
one immediate damage card, one timer card, one life/gauge card, and one
projectile/card-effect card. Snapdump agreement from that matrix can support a
bounded prototype result, not prove the entire runtime-tail strategy sound.
Require executed-case and nonempty comparison coverage, crash/detector checks,
and targeted ownership/write audits before broader launch UI or campaign work.
