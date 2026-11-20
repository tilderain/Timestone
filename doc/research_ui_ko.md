# Battle UI Resync And KO-On-Mispredicted-Frame Research

Ghidra 8192 (`umvc3.exe.unpacked.exe`), 2026-11-14. Static analysis only; no
runtime trace. "Verified" = read in decompilation; "Hypothesis" = inferred.
Plugin behavior below is taken from the existing design notes (headless resim
runs categories 3, 7-16, 18, 21; `sAction.compact` restores `+28..+10F`,
`+140/8`, `+150/8`, `+170/1`, `+218/4`), not re-read from source.

## Where The Battle UI Lives (verified)

- `sAction__local_1402bfb50` (phase 3/4 entry) creates `uBattleUi` and
  `uBtStatusGaugeManager` with `sUnit__addBottom(0x1C, ...)`.
- `uBattleUi__setup @ 0x1402D29E0` adds every child at `moveLine + 1` (0x1D):
  2x `uBtGaugeLife`, 2x `uBtGaugeHc`, `uBattleUiTime`, `uBtGaugeCardIcon`,
  `uBattleUiHit` (stored in `mpHitUi`), `uBtKeyDisp`, 2x `uBtChrIcon`,
  2x `uBtAssistChrIcon`, `uBtChr2Icon`, `uBattleUiNaruhodo`; `uBtScore` and two
  `uViewDrawUi` go to 0x18. None of these lanes run during headless resim, so
  UI sees only live frames and one jump per rollback.

## UI Elements: Live-Read vs Integrated

| Element | Source | Rollback behavior |
|---|---|---|
| Life bars `uBtGaugeLife__updateLifeGaugeDisplay @ 0x1402F53F0` | `cChrStatus` life/red-life ratio, `checkDefeatStatus`, X-Factor ratio read every frame | Self-correcting. Small event timers (`+1B9+k*C0+BB` flash, `mXFactorFrame`) are cosmetic. |
| **Life-bar member order** `uBtGaugeLife__local_1402F48A0` | Consumes one-shot request `sGameUi+0x5960[team]`, swaps its slot order `+0xB4[0]<->+0xB4[req]` and plays colour transitions, then writes 0 back via `sGameUI__setTeamCharacterId` | **Integrated, edge-driven. Not self-correcting.** |
| Hyper gauge `uBtGaugeHc__updateHyperComboGauge @ 0x1402F2EF0` | `sCharacter__getTeamGaugeInBars` each frame; cached `mLv` | Bars self-correct. Level edges against cached `mLv` replay the level-up voice (`sGSound__playOrdinaryVoice` 0x56..0x59, `queueGroup4Voice`) when a rolled-back gain is earned again. |
| Timer `uBattleUiTime__updateTimerDisplay @ 0x1402E4BB0` | `sAction::mRTimeLimit` (+0xF8, restored) | Self-correcting. |
| Combo counter `uHitModel__move @ 0x1402D7D30` | Team combo count (`cChrTeam+0x90`) read each frame; 4-state machine (show / 15f hold / fade) | Count self-corrects while showing; a rollback across combo end can show a stale "N hits" fade for ~15f+fade. Destroyed by the step-6 edge (below). |
| Portraits `uBtChrIcon__move @ 0x1402EF120`, `uBtAssistChrIcon__move @ 0x1402EEF70` | Active character and availability polled each frame | Self-correcting state machines. |
| Phase/KO banners `uBattleUi__moveAfter @ 0x1402D2410` | Edge on `sAction::mStep` (cached at `uBattleUiBase+0x48+4`) | **One-shot. Not undone.** |
| KO finish portraits `uFinishScreen__move @ 0x14031B980` (`sGameUi+0x5970`) | Polls `checkDefeatStatus` per member; sets sticky bit in `+0x49C` and renders that character's finish texture | **Sticky.** A rolled-back KO stays marked. |

## Root Causes (ranked)

1. **`Rollback_SyncActiveCharacterUi` misreads `sGameUi+0x5960` (verified).**
   `sGameUI__setTeamCharacterId @ 0x140022610` has only two callers:
   - `cChrTeam__commitPendingCharacterSwap @ 0x1400215E2` writes the incoming
     member's old slot (1 or 2).
   - The life gauge (`0x1402F4901`) writes 0 after consuming it.

   The point character's member id (`+0x2DB0`, `cChr__setMemberId @ 0x1400226F0`)
   is 0 after a swap. The current repair compares the request against the active
   member id and writes it whenever they differ. In practice that **clears every
   pending swap request**:
   - A tag committed during resim but not live never reaches the gauge, so the
     bars and portraits stay in the old order.
   - A tag predicted live, consumed by the gauge, then rolled back leaves the
     order swapped. Nothing re-swaps it, because order changes only through these
     relative swap edges.

   Both directions leave the life-bar order inverted until another swap.
2. **Predicted KO fires irreversible UI at step 6 (verified).**
   - `stepFighting` calls `setNextStep(6)`.
   - `executeMatchPhase(6)` runs in the same sim lane (headless resim also runs
     `sAction`; that is an assumption from the design notes).
   - The next live `uBattleUi__moveAfter` kills `mpHitUi` and creates a
     `uBattleUiPhase`: kind 1 = K.O., 0xE = other KO variant, 3 = Time Over.
   - `uBattleUiPhase__loadPhaseAnimationAndVoice @ 0x1402E4520` queues the
     announcer (`queueGroup4Voice`, `playOrdinaryVoice 0x69`).

   After rollback `mStep` returns to 5, and the moveAfter edge to 5 does nothing.
   - The K.O. banner and voice stay ("KO trolling").
   - The combo counter is gone for the rest of the round.
   - The real KO later creates a second banner.

   `uFinishScreen` likewise keeps the dead-portrait bit of the falsely KO'd character.
3. **`sAction::mFinishWatch` (+0x110) is outside the restored range (verified
   layout; effect is a hypothesis).**
   - The range `+28,+E8` ends at `+10F`.
   - `mFinishWatch` is reset only by `executeMatchPhase` (to -1), or set to 0 when
     it is negative. Otherwise `stepFighting` adds to it while a defeated team
     waits for KO flight (`fighter flag 0x1000`, cap 900) or during time-over.

   So a rolled-back KO or time-over leaves a stale positive value on one peer only:
   - The next finish wait (and so the frame of step 6) can differ between peers.
   - Any checksum over the whole `sAction.compact` region diverges. This explains
     desync reports around round end.

   Also unrestored and written in this flow:
   - `mWinTeam +0x114`, `mRestartMode +0x118`, `mBtnRestart +0x11C`
   - `+0x214`
   - `+0x22C`: the KO swap member index, written by `stepFighting`.
4. **Sound side effects of a predicted KO or tag (verified call sites; for the
   sound fork).**
   - `commitPendingCharacterSwap` calls `sAction__playAnnouncerEvent`,
     `playCharacterSwapVoice` and `updateUiBitfieldAndPlaySoundOnChange` from
     gameplay, so they also fire during resim.
   - Entering step 8 plays the system cue and resets listener slots.
5. Cosmetic: the duplicate hyper-gauge level-up voice; and `cSPCData` per-character
   counters (`cChr__updatePostContactEventCounters @ 0x140046E20`) are not
   restored. The counters matter only in Heroes & Heralds.

## Fixes (with addresses)

1. **Replace `Rollback_SyncActiveCharacterUi`.**
   - After resim, write 0 to `sGameUi+0x5960[0..1]`. Any request produced in resim
     is superseded by a canonical rebuild.
   - Then, for each `uBtGaugeLife` (child of `uBattleUi`; team id at `+0xB0`),
     set the slot-order array `+0xB4[0..2]` from authoritative team state: entry k
     = default member index of the character whose `+0x2DB0 == k`.
   - Hypothesis: verify the array semantics with a runtime dump of `+0xB4..+0xBC`
     before and after one tag.
   - Alternative: snapshot `+0x5960[2]` and each gauge's `+0xB4[3]` per frame;
     restore both after resim and let live frames consume.
2. **Gate step-6/7 UI on confirmed frames (presentation only, so no desync).**
   - Hook `uBattleUi__moveAfter @ 0x1402D2410`. While the step that changed was
     entered on an unconfirmed frame (frame > last frame with both peers' input),
     return without updating the cached step at `+0x48+4`.
   - The edge then fires once the KO is confirmed. Do the same for the
     `uFinishScreen__move` slot assignment (`0x14031B980`).
   - Repair if not gating: when `mStep` rolls back below 6, kill
     `mpResultPhaseUi` and the live `uBattleUiPhase` units, and recreate
     `mpHitUi`. To recreate it, mirror `uBattleUi__setup`: `uBattleUiHit` new/ctor
     `0x1402D6B30/0x1402D67D0`, `addBottom(line+1)`, `MtAnim__setFrameIndex(0x80)`.
   - Also clear the sticky `uFinishScreen +0x49C` bits for members not defeated.
3. **Snapshot and restore `sAction +0x110..+0x11F`** (`mFinishWatch`,
   `mWinTeam`, `mRestartMode`, `mBtnRestart`), plus `+0x214` and `+0x22C..+0x22F`.
   These are plain scalars written by the round-end flow.
4. **Do not gate the gameplay transition** (`setNextStep(6)` /
   `executeMatchPhase`) on network confirmation. Confirmation time differs between
   peers, so that would break determinism. Gate only the UI and audio consumers.
   If the irreversible network/replay side effects of `executeMatchPhase(6..8)`
   become a problem, defer only those to confirmed frames:
   - `sReplay__clearAllReplayFlags`, `sNetPad`/`nNet__sSession` calls
   - phase 7 `cChrTeam__commitBattleResultStats`
   - phase 8 fade/kill/scene changes

## Ghidra Annotations Added

Plate comments on `sGameUI__setTeamCharacterId`, `uBattleUi__moveAfter`, and
`sAction__stepFighting` (KO/finish-watch flow).

## Hyper Finish Backdrop On A Rolled-Back K.O. (2026-11-20)

Static analysis, Ghidra 8192; not yet confirmed in a running match.

- The full-screen backdrop of a K.O. by hyper combo is `uAkebonoFinishEffect`
  (ctor `0x140135780`, DTI `0x140D46948`, a `uGameEffect` that only overrides
  `updateWorldMatrix 0x140136250` to stay on the camera).
- An animation command spawns it (`0x1401228A6`: `0x140131660` with effect
  0x68, `sUnit::addPending` category 13, announcer voice 0x6A) and stores it
  with `sAction__mpAkebonoFinishEffect__setter 0x14014F000` in **`sAction+0x158`**.
  The command does nothing while the slot is set
  (`SharedProperty__getObjectAt158 0x14073EA30`), and the two hyper attack-space
  commands at `0x1401224E1` / `0x14012260C` skip `sAction__createByLevelAndAttach
  0x14014DB70` for the same reason.
- `sAction__subStepHelper 0x14014F410` kills the attack space in `+0x148` and
  the auxiliary object in `+0x160` every frame the slot is set. Nothing but the
  setter and the round reset (`sAction__resetExAtkSpaceState 0x14014EFA0`)
  clears it.
- `sAction.compact` does not restore `+0x158`, and the unit transaction only
  takes the slot when it holds one of the nine attack-space vtables
  (`ActionAttackSpaceFields`, `IsAttackSpace`). So a backdrop spawned by a
  predicted K.O. stayed up after the rollback, and for as long as it lived no
  hyper background could be created.

Fix: after the effect and shot restore, the snapshot's own `sAction+0x158`
(the region is saved whole) is compared with the live slot. A different live
object is killed through vfn `+0x78` when still pending or running, as the
native setter does to a superseded one, and the slot is cleared. If the replay
reaches the K.O. again the command spawns a new one.
