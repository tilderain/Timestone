# Online rewards and Steam achievements

Written 2026-11-18. Ghidra port 8192, `umvc3.exe.unpacked.exe`. Static
analysis plus a build; **nothing here has been run in a match or against
Steam**. Code: `UMVC3Hook/code/online_rewards.cpp`.

## Reward commit

`aAction__commitBattleRewards` (`0x1402052F0`, was
`aNextBattle__setupTeamsAndRules`) pays a finished battle's rewards once.

- Called from `aAction::move` when `sAction+0x171` is set
  (`sAction__setRewardCommitRequest`, from `sAction__executeMatchPhase` and
  `uBtResult`), and from area teardown.
- Needs `sAction` step 7..11 (or training / mission) and `sAction+0x172`
  clear; it sets `+0x172` when done. `aAction::init` clears it.
- Team rewarded: team 0 offline; `sNetworkRoom::mMyNetTeam` (`+0xBE8`) when
  live online.
- Calls, in order: `aAction__processBattleCompletionUnlocks` (play data,
  achievement requests), `aAction__updateMatchResultsAndRewards` (play time),
  `aAction__awardPlayPoints`, `aAction__processArcadeClearUnlocks`,
  `aAction__processBattleCompletionRewards` (titles),
  `cGameAchievement__evaluateTeamAchievements`.

## Player Points

`aAction__awardPlayPoints` (`0x1402069B0`, was
`aAction__finalizeBattleOutcome`) writes the breakdown with
`sLicense__setPPResult_locked(type, total, score, bonus, win, reject)` and adds
the total with `sLicense__addPlayPoint` (cap 99,999,999).

Online matches are selected by the `sGameUI` css state (`+0x5B74`), set on the
way into character select:

| Css state | Set by | PP type | Total |
|---:|---|---:|---|
| 8 | `handleNetworkRoomParam` (room battle start) | 1 | 448 + `floor(mBattleTime / 60) * 60`, win or lose |
| 6 | `handleNetworkMatchNext`, room type 0 or 1 | 2 | 280, or 580 with a win |
| 7 | `handleNetworkMatchNext`, room type 2 | 3 | 280, or 580 with a win |

`mBattleTime` is `cPlayData+0xC` of the rewarded team; a win is
`cPlayData::mBattleFlag & 4`. Room type is `sNetworkRoom+0x38`. Types 0 and 1
are the ranked ones (they alone reach rank points and ranked win counts), 4 is
Heroes & Heralds; 2 as player match and 3 as the 8-player lobby are inferred
from which handler serves them, not from a label.

Offline, by battle type: versus (0) is type 6,
`floor(mBattleTime / 60) * 150` capped at 300; arcade type 5; training type 7;
mission type 0. Spectators (battle type 4) return before any of it.

A plugin match is battle type 0. Left alone it pays the versus amount to
whoever is on team 0 and records nothing as online.

## What the plugin does

`Hook_CommitBattleRewards`, netplay players only (spectators return, as
native ones earn nothing). For the length of the native call it sets:

| Field | Value |
|---|---|
| `sBattleSetting+0x34C` battle type | 3 (live online) |
| `sNetworkRoom+0x38` room type | 3 (lobby) |
| `sNetworkRoom+0xBE8` my net team | 0 for the ENet host, 1 for the client |
| `sGameUI+0x5B74` css state | 8 (lobby PP) |

and restores all four afterwards. Every native online branch then runs as in a
lobby match: online play data and play time, 448 PP plus the minute bonus,
online titles, and the online achievement requests below. Room type 3 keeps
the ranked code out (`aAction__updateOnlineBattleRecords` returns, the ranked
win counters are skipped).

To pay 280/580 instead, set the css state to 7 and the room type to 2.

**Double payment.** A rollback rewinds `sAction`, `+0x172` included, to before
the commit, and the replayed frames ask for it again. The hook refuses a second
commit within 5 s of one that paid (`kCommitReplayGuardMs`).

Not checked: which match phase sets `+0x171`; that the reward step is reached
in every way a plugin match can end; the default room type when no native room
was ever entered; how the native result screen draws PP type 1 in a plugin
match.

## Achievements

47 achievements, ids 0..46, Steam API names `ACHIEVEMENT_00`..`ACHIEVEMENT_46`
(table `0x140D222E0`). The id-to-index table at `0x140B0DDA0` is the identity.

Request and delivery:

1. `sMvc3Guide__requestAchievement(guide, id, 600)` (`0x1402833D0`) marks
   `guide+0x378[id]` and starts a 600-frame countdown at `+0x374`.
2. `sMvc3Guide__flushAchievementRequests` (`0x140284FD0`, from
   `sMvc3Guide::move`) hands one pending id to
   `nNet__sGuide__awardAchievement` (`0x140458F80`; id in `r8d`) when the
   countdown ends.
3. That queues request `0x404` on the `MtNetAchievement` request controller.
   `MtNetAchievement__moveAward` calls `ISteamUserStats::SetAchievement`;
   `MtNetMediator::move` then calls `StoreStats`, and is also the only place
   `RequestCurrentStats` is issued.
4. The work in step 3 runs from `MtNetCore::move`, reached each frame only through
   `sNetwork::move` under `nNet::sComponent::move`.

The plugin removes the `nNet::sComponent::move` call
(`native_network_disable.md`). So with the plugin active **no achievement
reached Steam, offline ones included**: the first request stayed queued and the
rest stayed pending.

`Hook_GuideAwardAchievement` replaces step 3: it requests the user's stats
once, then `GetAchievement` / `SetAchievement` / `StoreStats` through the flat
exports of the game's `steam_api64.dll` (`STEAMUSERSTATS_INTERFACE_VERSION011`).
A false return leaves the request pending and the game asks again 10 s later.
Not installed for Launch Normally.

### Requests that need an online match

| Id | Condition (native) |
|---:|---|
| `0x19`, `0x1A` | online play time of 5 h / 30 h (`sLicense` field `0x248`, seconds) |
| `0x1C`, `0x1D`, `0x1E` | 100 / 200 / 300 online battles |
| `0x20` | online win without the flag at `mBattleFlag & 0x400` |
| `0x21` | online win with no blocks recorded (`cPlayData` guard total 0) |
| `0x24`, `0x25`, `0x26` | 10 / 50 / 100 ranked wins |
| `0x27`, `0x28`, `0x29` | ranked win streak of 3 / 5 / 10 |

The ranked ones (`0x24`..`0x29`) need room type 0 or 1 and stay out of reach
in plugin matches. The rest of the list (combos, hypers, X-Factor, stages,
play-data totals) counts online and offline play data together; a plugin match
now adds to the online side.

The executable also carries the names `ACHIEVEMENTSID_GREATEST_HERO` ..
`ACHIEVEMENTSID_TRAVEL_HERO` (`0x140B0DE78`); which name is which id was not
matched up.

## Ghidra names changed

`aNextBattle__setupTeamsAndRules` -> `aAction__commitBattleRewards`,
`aAction__finalizeBattleOutcome` -> `aAction__awardPlayPoints`,
`aAction__processOnlineCharacterUnlocks` -> `aAction__processArcadeClearUnlocks`
(it is arcade-only), `sLicense__addClampedField_74` -> `sLicense__addPlayPoint`,
`sLicense__writeWonLostMatchTotals_4774_locked` -> `sLicense__setPPResult_locked`,
`sBattleSetting__isField844Clear_locked` -> `sBattleSetting__isVersusBattle_locked`,
`sGameUI__readField23412_locked` -> `sGameUI__getCssState_locked`,
`sMvc3Guide__markUnlockNoticeAndStorePayload` -> `sMvc3Guide__requestAchievement`,
`sAction__setByte171` -> `sAction__setRewardCommitRequest`,
`sAction__getByte_172` / `setByte_172` -> `sAction__getRewardsCommitted` /
`setRewardsCommitted`. `ingameonlinechecks.md` still uses the old names.
