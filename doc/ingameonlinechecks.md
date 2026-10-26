# Native Online Battle Checks

**Status: Native reference. Reviewed 2026-10-26.** `g__isOnlineBattle @ 0x140006AF0` returns true
for:

- `BattleType == 3`: live standard online
- `BattleType == 4`: online spectator/watch
- `BattleType == 9`: live online Heroes & Heralds

The current Ghidra xref query reports **91 external `UNCONDITIONAL_CALL`
references**. The older total of 92 included an internal branch and is not the
current tool result. All 91 external call-site addresses are retained below.

## Evidence and coverage

Read-only review used port **8193**, `umvc3.exe.unpacked.exe`.
Bodies at `0x140006A60`, `0x140006AB0`, and
`0x140006AF0` confirm live types 3/9 plus spectator type 4; an old plate comment
saying "ordinary or ranked" is misleading. The settings predicates read
`BattleType` under the object's critical section. The 91-address inventory was
re-queried; missing containing-function names in that result do not invalidate
a call address. Function names below are analyst labels, not original symbols.

Table purposes retain earlier static interpretations, **not 91 newly
re-decompiled bodies**. Time behavior was independently checked in
`0x1402895C0` (including assembly), advancement at `0x14028BC10`; other
gameplay/UI/reward claims need per-body revalidation before use as patch
specifications. No predicates were modified and no runtime online test ran.
Plugin source checks (`controller.cpp`, `framestepper.cpp`, `netplay.h`,
`steam_transport.cpp`) show a narrow custom time hook and separate transport:
direct ENet or custom payloads bridged through SteamNetworkingSockets, **not
retail CMU**.

## Battle Simulation

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 1 | `0x140006437` | `sAction__stepFightingCore` | At round initialization, selects synchronized team-history character assignments online instead of reading local pad input to choose initial active characters. |
| 2 | `0x140006E23` | `sAction__move` | Changes pause gating: online action updates stop only for the online UI pause field, while offline and mission modes also observe the ordinary effective-pause state. |
| 3 | `0x1400072FF` | `sAction__stepFighting` | Replaces locally integrated elapsed fight clocks with the synchronized `sMvc3SyncPad` fight-time cursor. |
| 4 | `0x1400073F8` | `sAction__stepFighting` | Replaces local round-timer decrement with the synchronized netpad/replay remaining-time value. |
| 5 | `0x14000A7F8` | `sAction__stepResult9_10_11` | Activates the online result-restart barrier: updates session state, waits for peer/readiness and license resources, resets lag state, then permits restart. |
| 6 | `0x14002DF59` | `cChrInput__processFrameInput` | Bypasses offline training/replay ownership routing. Online input proceeds to spectator suppression or live `sNetPad` buffered-input retrieval. |
| 7 | `0x14003D29B` | `cChr__onActionChangeReset` | Forces a per-character random cosmetic/action-reset flag to zero online instead of consuming character RNG. |
| 8 | `0x14003F07E` | `cChr__resolveActionTransitionState` | For `mUkemiType == 4`, offline uniformly chooses recovery type `1`, `2`, or `3`, producing action `0x32`, `0x33`, or `0x34` (with input-dependent adjustment for type `1`). Online leaves type `4` unchanged, which takes the no-recovery fallback. |
| 9 | `0x14003F24C` | `cChr__resolveAdvancedGuardTransition` | For `mAdvGType == 2`, offline accepts advancing guard only for rolls `0..49`; rolls `50..99` set `mDmgF & 0x1000` and reject it. Online always accepts type `2` once the other eligibility checks pass. |
| 10 | `0x140040265` | `cChr__resolveDamageReactionTransition` | For `mDownEscType == 4`, offline uniformly selects escape type `1`, `2`, or `3`; online leaves type `4` unchanged, sets the escape-suppression flag `mNotF & 0x400000`, and continues with the ordinary damage-reaction transition. |
| 11 | `0x1400409D1` | `cChr__tryStartThrowTransition` | For `mThrowEscType == 2`, offline accepts throw escape only for rolls `0..49`; rolls `50..99` set `mDmgF & 0x10000` and reject it. Online always accepts type `2` once the throw-escape eligibility checks pass. |
| 12 | `0x140040F3C` | `cChr__tryStartVariableCounterTransition` | For `mVCounterType == 2`, offline accepts variable counter only for rolls `0..49`; rolls `50..99` set `mDmgF & 0x100000` and reject it. Online always accepts type `2` once the remaining meter, teammate, and state checks pass. |
| 13 | `0x1400417BA` | `cChr__resolveDamageReactionAction` | For the type field at `cChr+0x6414 == 4`, offline uniformly chooses type `1`, `2`, or `3`, mapping to action groups `0x66/0x69`, `0x67/0x6A`, or `0x68/0x6B`. Online leaves type `4` unchanged and returns fallback action `0x66`. |
| 14 | `0x140048CB8` | `cChr__populateHitReactionResult` | For `mCHitType == 2` while the defender is not already in the `mAnmF & 0x20000` state, offline rolls `0..99`: `50..99` set contact-result bit `0x4`, selecting the attack's counter-hit reaction at `attackData+0x58` instead of its normal reaction at `+0x54`; `0..49` retain the normal reaction. Online skips the roll and retains the normal reaction. |

## Battle Area Initialization

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 15 | `0x140203864` | `aAction__init` | Together with replay-use state, enables deterministic execution: frame wait, serial unit categories, single-thread collision/hit/effect processing. |
| 16 | `0x1402038EE` | `aAction__init` | Applies the network-battle effect-volume setting. |
| 17 | `0x140203A7B` | `aAction__init` | Forces the match/round setting passed to `FUN_140207BC0` to `180`, instead of using the offline configured value. |
| 18 | `0x140203B8B` | `aAction__init` | Selects the local native-online side and configures the associated network-room/team mapping. |
| 19 | `0x140203E18` | `aAction__init` | Suppresses copying local configuration round flags into both teams online; those values belong to synchronized battle configuration. |
| 20 | `0x140203FB2` | `aAction__init` | Creates the initial online battle banner and marks the current network-room slot as being in battle. |
| 21 | `0x1402043C7` | `aAction__load` | Initializes the native network character-select/readiness object after character archive requests are issued. |
| 22 | `0x140204527` | `aAction__move` | At action step 1, activates the network startup barrier and waits for battle sync, session state, and license resources. |
| 23 | `0x140204675` | `aAction__move` | After entering step 2, creates the online battle UI/network status element. |
| 24 | `0x1402046A1` | `aAction__move` | During active battle phases, continues online license/profile readiness handling. |
| 25 | `0x1402047B1` | `aAction__processBattleCompletionRewards` | Allows completion rewards for offline modes and live online, but immediately excludes spectator mode. |
| 26 | `0x140204B7D` | `aAction__processBattleCompletionRewards` | Gates online battle-count milestone unlocks and banners. |

## Results, Statistics, and Unlocks

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 27 | `0x140204F75` | `aAction__updateMatchResultsAndRewards` | Selects the online cumulative play-time counter and its online achievement thresholds. |
| 28 | `0x140205081` | `aAction__updateMatchResultsAndRewards` | Admits live online result-stat processing but excludes spectator; offline processing is restricted to specific game modes. |
| 29 | `0x1402053D6` | `aNextBattle__setupTeamsAndRules` | Chooses the locally owned online team as the subject of post-battle rewards/statistics instead of always using team 0. |
| 30 | `0x140205518` | `aAction__processBattleCompletionUnlocks` | Selects online play-data accumulation. Live matches update normal online stats; spectator matches take a limited alternate path and return. |
| 31 | `0x140205E98` | `aAction__processBattleCompletionUnlocks` | Enables online-only result banners, online match-count achievements, ranked streak thresholds, and online battle-record updates. |
| 32 | `0x1402069C1` | `aAction__finalizeBattleOutcome` | Excludes spectator mode from win/loss and reward-total finalization while allowing live online. |
| 33 | `0x140206F1C` | `aAction__processOnlineCharacterUnlocks` | Excludes spectator mode before processing character/animation unlock rewards. |
| 34 | `0x14021EB2A` | `cGameAchievement__evaluateTeamAchievements` | Restricts the online achievement cases to actual online battles; a following live-online check excludes spectators. |
| 35 | `0x140223A18` | `cPlayDataAccumulate__accumulateBattleStats` | Begins detection of an eligible live online result for online win/loss/streak counters. Spectator and unsupported room types do not set the online-result flag. |

## Character-Select Startup and Networking

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 36 | `0x140208CAB` | `aChrSelect__init` | Captures native session player-slot IDs online; offline resets the slot-match state. |
| 37 | `0x140208D74` | `aChrSelect__init` | Selects online character-select menu modes, including separate live and spectator/watch setup. |
| 38 | `0x14020978C` | `aChrSelect__update` | Runs one-time online license/profile readiness handling when CSS input is not locked. |
| 39 | `0x1402097F0` | `aChrSelect__update` | Maintains online CSS menu ownership/presentation according to local network-player index. |
| 40 | `0x140209DD5` | `aChrSelectBlank__move` | Disables parallel effect processing before entering the battle area online. |
| 41 | `0x1402144EC` | `aNewChallenger__updateReadyAndSceneTransition` | Selects the native-online readiness/state machine instead of the offline scene path (historically labeled `aChrSelect__func_42B0`). |
| 42 | `0x14024B545` | `sBattleSetting__requestEnter` | Forces pending stage ID `2` for online entry before queuing `aChrSelectBlank`. |
| 43 | `0x14024CBC6` | `sBattleSetting__resolveCharSlotForPlayerSide` | For requested player side 0, detects whether local network ownership requires preserving/remapping the selected color/variant. |
| 44 | `0x14024CBF2` | `sBattleSetting__resolveCharSlotForPlayerSide` | Performs the complementary ownership test for requested player side 1. |
| 45 | `0x140271BF8` | `cNetChrSel__sendChrAssistMsgIfDue_71BD0` | Prevents character/assist protocol messages outside online battles; online additionally requires a live room and send timing conditions. |

## Netpad, Pause, and Runtime UI

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 46 | `0x140280A82` | `uBattleResult__advanceResultFlow` | On online result exit, releases network UI state and clears replay flags before returning to `aNetworkRoom`. |
| 47 | `0x140289CB0` | `sNetPad__processOnlineInput` | Enables consumption of online pause requests after input send/receive and spectator replay processing. |
| 48 | `0x1402B60F6` | `sMvc3Vibration__move` | Stops vibration advancement while the online pause/status UI field is active. |
| 49 | `0x1402BA21C` | `sAction__openVersusPauseMenu` | Blocks creation of the full gameplay-pausing `uBattleUiPause` in all online modes. Spectator uses a separate non-pausing gallery overlay. |
| 50 | `0x1402BEB86` | `sGameUI__move` | Prevents the ordinary local pause-request consumer from running online; `sNetPad` owns that operation there. |
| 51 | `0x1402FB170` | `uBtGameOver__processResultAction` | Activates online result synchronization, room teardown, team-history clearing, and session transition handling. |
| 52 | `0x1402FBA1A` | `uBtGameOver__selectNextState` | Treats online as a result-flow family alongside mode 7, bypassing the ordinary offline-state test. |
| 53 | `0x1402FBB03` | `uBtGameOver__selectNextState` | Selects online result-screen handling for UI modes 6/7 before arcade and other offline branches. |
| 54 | `0x1402FBC7C` | `uBtGameOver__selectNextState` | In the generic result branch, decides whether to wait for spectator streamed-result readiness before advancing. |
| 55 | `0x1402FC817` | `uBtResult__move` | Polls online room/profile readiness and restores the appropriate online result menu mode. |
| 56 | `0x1402FD178` | `uBattleUiResultScreenHandler_maybe` | Uses local online side ownership to determine whether the displayed result counts as a win; live-online check then excludes spectator semantics. |

## Character-Select Slot State Machine

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 57 | `0x1403614E9` | `aChrSelect__playSelectionPromptForSlot` | Restricts prompts to the locally controlled slot online and suppresses prompts completely for spectators. |
| 58 | `0x140362411` | unnamed CSS readiness helper | Gates network-session synchronization/ready handling for this CSS action; offline bypasses the network branch. |
| 59 | `0x140362F5D` | `uMenuChrSel__slotState357_processCharSelectConfirm` | Restricts confirm/cancel processing to the locally owned live-online slot; spectator does not use the offline cancellation path. |
| 60 | `0x1403633A3` | `uMenuChrSel__slotState1_loadTeam` | Selects online team-loader ownership rules, including the live local-side test and spectator fallback. |
| 61 | `0x140363797` | `uMenuChrSel__slotState1_loadTeam` | When cursor control mode is zero, decides whether reserve/control-mode UI may be created locally or must follow online ownership. |
| 62 | `0x1403638EA` | `uMenuChrSel__slotState1_loadTeam` | Separates online-owned team setup from ordinary offline CPU/control-mode setup. |
| 63 | `0x140363C51` | `uMenuChrSel__slotState9_bootNextScene` | Prevents the offline title/Earth-attack scene transition; online remains in synchronized CSS progression. |
| 64 | `0x14036457B` | `aChrSelect__processStageSelect` | Applies online stage-select authority gates: input lock, local-player role, network-library readiness, and synchronized state. |
| 65 | `0x1403646F0` | `aChrSelect__processStageSelect` | After a locally selected stage is committed, marks stage-related network sync fields dirty. |
| 66 | `0x1403648C6` | `aChrSelect__processStageSelect` | Performs the same stage sync-dirty notification on the live-online local-slot-1 path. |
| 67 | `0x14036517F` | `aChrSelect__slotState246_processSelection` | Restricts character-selection narration/confirmation to the locally owned live-online side. |
| 68 | `0x140365237` | `aChrSelect__slotState246_processSelection` | Restricts ready-to-proceed cleanup and reserve-unit destruction to the locally owned online side. |
| 69 | `0x140365AAD` | `aChrSelect__slotState246_processSelection` | Suppresses offline cancel voice playback online. |
| 70 | `0x140365C62` | `aChrSelect__slotState246_processSelection` | On cancel/backtracking, preserves or restores synchronized slot data when online instead of clearing it as local-only state. |
| 71 | `0x140365E30` | `aChrSelect__slotState246_processSelection` | Prevents creation of the offline stage-select prompt/event when no local cursor exists online. |
| 72 | `0x1403663C5` | `aChrSelect__slotState246_processSelection` | Rechecks online mode in the alternate/random-selection path to prevent offline-only cancellation/scene behavior. |
| 73 | `0x1403663E3` | `aChrSelect__slotState246_processSelection` | Preserves synchronized character-slot values during the alternate cancellation path. |
| 74 | `0x1403669AA` | unnamed CSS completion helper | Makes completion depend on online ownership/readiness rather than immediately accepting the local CSS state. |
| 75 | `0x140366AC3` | unnamed CSS completion helper | Selects online synchronized transition/cleanup behavior instead of the offline transition path. |
| 76 | `0x140366F10` | `aChrSelect__enterStageSelection` | Starts online commit processing: locks the cursor, validates net character-select state, rebuilds local slots, and marks live sync data dirty. |
| 77 | `0x140367130` | `aChrSelect__enterStageSelection` | Creates the online loading/status banner for live players but not spectators. |
| 78 | `0x14036715F` | `aChrSelect__enterStageSelection` | Requires broad online plus live-online before constructing local/remote network-player information cards. |
| 79 | `0x140367FF9` | `aChrSelect__postSelectCommitAndEnter_367C40` | Creates the online transition/status UI after CSS objects and stage-select resources are committed. |
| 80 | `0x14036846D` | `uMenuChrSel__tickSlotStateMachine` | Gates each slot-state update on online profile/license readiness; offline dispatches the state immediately. |

## Character-Select Presentation

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 81 | `0x140369117` | `uMenuChrSel__setup` | Suppresses ordinary help-message presentation only for spectator mode; live online still receives local help UI. |
| 82 | `0x140369457` | `uMenuChrSel__setup` | Captures network player-slot IDs online; offline resets slot-match state. |
| 83 | `0x1403696D4` | `uMenuChrSel__setup` | Creates the first control-mode selector only offline or for the locally authoritative live-online host. |
| 84 | `0x1403697C1` | `uMenuChrSel__setup` | Applies online ownership restrictions to creation of the second player's control-mode selector. |
| 85 | `0x1403698E3` | `uMenuChrSel__setup` | Initializes room phase/slot state when CSS is entered from the relevant online room type. |
| 86 | `0x1403699FB` | `uMenuChrSel__setup` | Selects spectator-specific room-watch banners and status handling. |
| 87 | `0x140369DBD` | `uMenuChrSel__updateSelectionPresentation` | Selects online presentation: neutral spectator view or the locally owned player's synchronized CSS state. |
| 88 | `0x14036EAD3` | `uMenuChrSelBgMain__setup` | Hides/disables the non-owned side of the CSS background online; spectator hides both interactive sides. |
| 89 | `0x140370C3B` | `uMenuChrSelCardPlayer__initializePlayerCardDisplay` | Hides the remote player card online and both interactive player cards in spectator mode. |
| 90 | `0x140371F79` | `uMenuChrSelCtrlMode__processControlModeInput` | Suppresses local cancel/back navigation from the control-mode selector online, preventing unilateral CSS rollback. |

## Rendering

| # | Call site | Function | Purpose |
|---:|---|---|---|
| 91 | `0x1404476B5` | `uAntiEdgeBrightnessFilter__draw` | Completely disables this anti-edge/brightness post-processing pass online, avoiding an extra render path during synchronized play/watch. |

## Conclusion

Hooking this predicate to return true for custom rollback would activate
substantially more than synchronized input or timekeeping. It would also enable
native session readiness, character-select ownership and messaging, spectator
paths, persistence/achievement updates, pause restrictions, result teardown,
deterministic single-thread settings, altered combat RNG behavior, and rendering
changes.

This confirms the warning already present in the native-online notes: the
predicate is too broad to use as a general "custom netplay active" switch.
