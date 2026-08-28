# Heroes And Heralds Rollback State

**Status: Ghidra research, 2026-08-28. No implementation yet.**

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

The safest prototype is to capture `cSPCData+0x3BC00..+0x48A70`, about 53 KiB,
as one explicit region while H&H/card mode is active. This avoids copying the
roughly 245 KiB static parameter prefix on every snapshot.

Before shipping that range:

1. Audit all writes to `cSPCData` during representative H&H matches.
2. Confirm the tail contains no live pointers, resource references, locks, or
   peer-local UI ownership that require preservation.
3. Narrow or split the region if the tail mixes gameplay state with unsafe UI
   state.
4. Add the region to checksums and snapdump naming, not only save/restore.
5. Verify restore across hits that activate, expire, stack, copy, or reverse a
   card condition.

Capturing the complete `0x48A70` object (skipping the vtable) is a useful debug
experiment, but it adds about 291 KiB per frame and copies a large immutable
database. It should not be the final design unless the write audit proves that
selective coverage is unreliable.

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
projectile/card-effect card. Snapdump agreement from that matrix will determine
whether the generic runtime-tail strategy is sound before investing in launch
UI or campaign behavior.
