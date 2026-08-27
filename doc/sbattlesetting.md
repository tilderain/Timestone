# Native `sBattleSetting`

**Status: Native reference. Investigated 2026-07-25.** This document describes
the retail game's `sBattleSetting` singleton. It is not the plugin's custom
netplay configuration.

## Summary

`sBattleSetting` is the game's live match descriptor and match-construction
service. It stores the selected mode, stage, timer, CPU and arcade settings,
six character-slot records, one-play/arcade progression, training and display
toggles, a backup of the six slots, and several runtime tables. Character
select, battle startup, character construction, input routing, training reset,
replay export, and native online setup all consult it.

It is not the full live battle state. Character position, animation, health,
shots, collision, and most frame-by-frame simulation state live in other
subsystems. `sBattleSetting` primarily answers: what match is being played,
which characters and options should be constructed, and which mode-specific
rules should be active?

## Lifetime And Access

The object is allocated by `sMvc3Manager__createBattleSetting @ 0x14024D710`:

```text
allocation size:       0x930 (2352 bytes)
alignment:             0x10
manager field:         sMvc3Manager + 0x40
global singleton:      0x140D50E58
getInstance:           0x140004700
manager getter:        0x1408F1F10
constructor:           0x14024AF90
property registration: 0x14024BDF0
```

`sMvc3Manager` creates it early in its constructor, before `sGameConfig`, UI,
replay, sound, and network services. The object derives from `cSystem`; most
public getters and setters enter its critical section.

The class's `vtable+0x30` method is the shared no-op virtual stub. Calling it
once per frame has no observed effect. In particular, the rollback headless
pipeline's `MgrSubsysCall(FN_GETBATTLESET, 0x30)` does not advance any
`sBattleSetting` state.

## Important Type Distinction

`sGameConfig__getBattleSetting @ 0x1400049F0` is currently misnamed and
mistyped in Ghidra. It returns `sGameConfig + 0x1E30`, whose constructor is
`cSPCData__cSPCData @ 0x140228930`. That embedded object is about `0x49080`
bytes and contains character/rule tables used heavily by combat calculations.
It is not the standalone 0x930-byte `sBattleSetting` singleton.

Consequently, calls of the following form usually refer to the SPC gameplay
database, despite their decompiler types:

```cpp
sGameConfig__getBattleSetting(sGameConfig__getInstance());
```

Calls through `sBattleSetting__getInstance()` or `sMvc3Manager+0x40` refer to
the actual match descriptor documented here. This distinction prevents large
SPC-table routines such as `sBattleSetting__getAttackDamageModifier` from
being incorrectly attributed to the 0x930-byte singleton.

## Confirmed Top-Level Layout

Offsets are from the standalone `sBattleSetting` base. Names come from the
native reflection properties where available.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| `0x000` | `0x38` | `cSystem` base | Vtable, synchronization, system metadata. |
| `0x038` | `0x210` | `cMainChr mMainChr[6]` | Six active match-construction slots, `0x58` bytes each. |
| `0x248` | `0x50` | `cSubChr mSubChr[1]` | One auxiliary/sub-character configuration record. |
| `0x298` | `0xB4` | `cOnePlay mOnePlay` | Score, time, continues, versus history, arcade progression, and commit state. |
| `0x34C` | `4` | `BattleType` | Primary mode discriminator. |
| `0x350` | `4` | `mBattleFlag` | General match-rule flags; defaults to `1`. |
| `0x354` | `4` | unknown locked field | Exported into `rBattleSetting`; exact semantics unknown. |
| `0x358` | `4` | `mStageArea` | Selected stage/area ID. |
| `0x35C` | `4` | `mTimeLimit` | Match time setting; constructor `-1`, normal defaults `180`. |
| `0x360` | `4` | `mRoundMax` | Round count; constructor default `1`. |
| `0x364` | `4` | `mCpuLv` | CPU level, commonly initialized to `0x80`. |
| `0x36C` | `4` | `mArcadeDif` | Arcade difficulty; constructor default `2`. |
| `0x370` | `1` | `Battle Ui Disp` | Show normal battle UI. |
| `0x371` | `1` | `Input Key Disp` | Training input display toggle. |
| `0x372` | `1` | `Finish Picture Save` | Finish-picture capture toggle. |
| `0x373` | `1` | `Replay Use` | Replay-use toggle. |
| `0x374` | `1` | `Damage Disp` | Training damage display toggle. |
| `0x375` | `1` | unknown Boolean | Read by an alternate intro/camera path; semantics unconfirmed. |
| `0x378` | `0x210` | backup `cMainChr[6]` | Backup copy of all six active slot records. |
| `0x588` | `4` | `mBackupStgID` | Backed-up stage ID. |
| `0x58C` | `4` | `mBackupCtrlModeType` | Backed-up control-mode value from `sGameConfig`. |
| `0x590` | `1` | `mBackupMainChrFlag` | Marks the backup as valid and pending restoration. |
| `0x591` | `1` | team 0 AutoJump | Effective automatic-super-jump setting for team 0. |
| `0x592` | `1` | team 1 AutoJump | Effective automatic-super-jump setting for team 1. |
| `0x593` | `1` | `3.0 X-Factor` | Reflected X-Factor option. |
| `0x594` | `1` | unknown training Boolean | Exported through `rBattleSetting`; exact label is garbled. |
| `0x5E0` | `4` | input-history delay/index | Used by the per-character input-history transform. |
| `0x5E4` | `0x2D0` | input-history rings | Six rings of 30 `uint32_t` entries. |
| `0x8B4` | `0x18` | ring cursors | Six `uint32_t` cursors, one per team/member combination. |
| `0x8CC` | `1` | one-play threshold flag | Set when a `cOnePlay` progression value exceeds 5. |
| `0x924` | `8` | team history values | Two `uint32_t` values used by native network/selection code. |

There are additional fields between the listed ranges. Their semantics are not
established strongly enough to name yet.

## Character Slot Records

The active array begins at `+0x38`; slot `i` is:

```cpp
slot = battleSetting + 0x38 + i * 0x58; // i = 0..5
```

The six slots ordinarily represent three members for team 0 and three for team
1, but lookup uses each record's stored team value rather than assuming fixed
array halves. `sBattleSetting__findCharBySlotMatch_locked @ 0x14024CDB0`
scans all six records, selects records whose team matches and whose character
ID is positive, and returns the requested occurrence.

Confirmed reflected slot fields are:

| Slot offset | Reflected name | Notes |
|---:|---|---|
| `0x08` | `mTeam` | Team/side identifier. |
| `0x0C` | `mType` | Character ID; positive values denote active records in normal lookup. |
| `0x10` | unreflected DTI ID | DTI ID resolved from a name such as `uRyu`. |
| `0x14` | `mBody` | Body/configuration value. |
| `0x18` | `mCpu` | Slot CPU/config value. |
| `0x1C` | `mAssist` | Assist/variant-related slot value. |
| `0x20` | `mAbiFlag` | Ability flags. |
| `0x24` | `mAb2Flag` | Secondary ability flags. |
| `0x28` | `mGuardType` | Guard behavior option. |
| `0x2C` | `mUkemiType` | Recovery behavior option. |
| `0x30` | `mDEscType` | Escape behavior option. |
| `0x34` | `mCHitType` | Counter-hit behavior option. |
| `0x38` | `mThrowEscType` | Throw-escape behavior option. |
| `0x3C` | `mAdvGType` | Advancing-guard behavior option. |
| `0x40` | `mVCounterType` | Variable-counter behavior option. |
| `0x44` | `mVoice` | Voice setting. |
| `0x48` | `mRegion` | Region/unlock-related setting. |
| `0x4C` | `mExCpu` | Extended CPU setting. |
| `0x50` | `mLifeRate` | Main-character life scaling/override value. |

Some native getters invoke inherited methods whose current Ghidra names are
wrong. The offset table is reliable; precise semantics beyond the reflected
names still require caller-by-caller confirmation.

The native character-ID setter writes `slot+0x0C` and resolves the matching
character DTI into `slot+0x10`. Directly writing only the ID leaves a stale or
missing DTI and is unsafe for construction.

## Battle Type

`BattleType` at `+0x34C` controls broad mode behavior throughout `sAction`,
input, UI, character select, replay, resource loading, and networking.

| Value | Confirmed behavior |
|---:|---|
| `0` | Offline versus/default battle. |
| `1` | Arcade. |
| `2` | Demo/attract battle. |
| `3` | Live online battle. |
| `4` | Online spectator/watch battle. It is not ranked mode; it consumes streamed replay input. |
| `5` | Training. |
| `6` | Mission. |
| `7` | Distinct non-local mode; exact public label remains unconfirmed. |
| `8` | Offline Heroes & Heralds. |
| `9` | Online Heroes & Heralds. |

Relevant predicates include:

```text
0x140006A60  BattleType == 3 || BattleType == 9  (live online)
0x140006AB0  BattleType == 4                     (spectator/watch)
0x14024D540  BattleType == 2                     (attract)
0x14024D560  BattleType == 6                     (mission)
0x14024D580  BattleType == 5                     (training)
0x140006830  BattleType == 8                     (offline H&H)
0x140006850  BattleType == 9                     (online H&H)
0x140006920  BattleType == 7
```

`sBattleSetting__setBattleType @ 0x14024E050` also updates `sMvc3Guide` for
values 0, 1, 5, and 8, so using the setter is preferable to a raw write.

## Initialization And Defaults

The constructor builds the active six-slot array, one sub-character record,
`cOnePlay`, and the six-slot backup array. It then initializes display flags,
timer/round/CPU/difficulty defaults, lag-simulation storage, and finally calls
`sBattleSetting__applyDefaultMatchConfig @ 0x14024D990`.

The final default setup leaves the object as an offline/default descriptor:

```text
BattleType  = 0
mBattleFlag = 1
mStageArea  = 0
mTimeLimit  = 180
```

It initializes the six slots with character IDs `1, 2, 3, 28, 27, 26`, resets
their extended options, marks the latter three as the other team, and resets
`cOnePlay`. These are bootstrap defaults, not necessarily the final roster of
a match.

Character select later writes the actual slot records, draw modes, assists,
variants, unlock flags, stage, timer, CPU level, and battle type. Arcade and
attract paths can also populate CPU teams from presets or random eligible
characters.

## Backup And Restore

`sBattleSetting__backupBattleConfiguration @ 0x14024B580` copies exactly
`0x210` bytes from active slots `+0x38` to backup slots `+0x378`, stores stage
and control mode, then sets `mBackupMainChrFlag`.

`sBattleSetting__restoreBackupAndInit @ 0x14024DE40` performs the reverse copy
when that flag is set, restores the stage and control mode, and clears the
flag. It then:

1. Resolves each team's three active characters from the six records.
2. Copies character ID, slot field, and draw mode into `sGameUI`.
3. Applies the restored easy/control mode to both `cChrTeam` objects.
4. Performs an extra training-mode clear when the restored control mode is 1.

Retail callers include character-select completion, game-over processing, new
challenger setup, and next-battle victory handling. A backup must therefore be
treated as a complete six-record snapshot, not merely a character-ID array.

## Match Construction

The descriptor is directly involved in constructing battle objects:

- `sBattleSetting__requestEnter @ 0x14024B530` records the pending stage in
  `sGameUI` and queues `aChrSelectBlank`; native online forces stage ID 2 at
  this transition.
- Character-select preload uses all six slot records to request character and
  stage resources and waits for them to become ready.
- `sBattleSetting__spawnConfiguredCharactersAndAudio @ 0x14024B7E0` scans
  active slots, resolves each character DTI, invokes the generic character
  factory with the setting-slot index, applies the assist value, and rebuilds
  pairwise character sound objects. Training reset and round setup call it.
- `uCharacter__setup_base @ 0x1400570D0` reads multiple fields back from the
  character's setting slot, including archive/config values, attack data,
  sound data, DTI ID, one-play data, owner/player data, and another slot flag.

This is why a character cannot safely be replaced by changing only its ID.
The full slot must be coherent before resource loading and object creation.

## Input And Runtime Use

The object is not purely pre-match configuration.

`cChrInput__processFrameInput @ 0x14002DD60` checks its mode predicates to
choose local, training replay, native online, or spectator input behavior.
It also calls `sBattleSetting__pushTeamInputHistory @ 0x14024CFA0`, which
maintains six 30-entry input-history rings at `+0x5E4`. The routine clears the
old cursor entry, advances the slot cursor, stores a masked current input at a
delay-dependent position, and returns selected old bits combined with selected
current bits. This appears to support native lag/input-history behavior.

The two values at `+0x924` are separately read and written by native network or
selection code. Their exact protocol meaning is not yet confirmed.

## `cOnePlay`

The embedded `cOnePlay` object at `+0x298` is `0xB4` bytes. Confirmed reflected
properties include:

```text
+0x08 mScore
+0x0C mTime
+0x10 mContinue
+0x2C mVsHisChr
+0x34 mVsHisStg
+0x38 mArcadeCount
+0x90 mArcadeStg
+0xAC mbAlreadyNetBattle
```

It tracks single-player/arcade progression and history across transitions.
`sBattleSetting__commitOnePlayConfig @ 0x140214A40` updates its tail commit
flag, reflected as `mbAlreadyNetBattle` at `cOnePlay+0xAC`, under the
`sBattleSetting` lock. A separate flag at `sBattleSetting+0x8CC` is updated
when a one-play progression value exceeds five.

## Export Resource

`rBattleSetting` is a separate archive/resource object, not the singleton. Its
constructor creates six `cMainChr` records. `rBattleSetting_vfn15 @
0x1402957A0` snapshots the live singleton's six records and top-level mode,
battle flags, CPU level, difficulty, display/replay settings, area, and training
flags into that resource. `rBattleSetting__clear @ 0x140295B30` serializes it
under the archive object name `"BattleSetting"`.

This provides strong evidence that `sBattleSetting` is the authoritative live
match descriptor and `rBattleSetting` is its serializable transport/archive
form.

## Plugin-Relevant Conclusions

- Obtain the real object through `0x140004700` or `sMvc3Manager+0x40`.
- Prefer locked native setters for battle type, stage, timer, and character
  records. `setBattleType` has side effects beyond storing the integer.
- A valid character record needs at least coherent team, character ID, resolved
  DTI ID, draw mode, assist/variant, and associated flags.
- If backup restoration will run, update the backup records as well as the
  active records, or deliberately call the native backup routine after the
  active records are complete.
- Do not model the object as static configuration during native input handling;
  it contains mutable input-history and small network/runtime fields.
- Do not confuse it with `sGameConfig+0x1E30` (`cSPCData`). The latter owns the
  large per-character combat-rule database.
- The plugin source comments that label BattleType 4 as ranked are incorrect;
  Ghidra confirms it is spectator/watch mode.

## Remaining Unknowns

- The public label and full behavior of BattleType 7.
- The exact meanings of Booleans `+0x375`, `+0x591`, `+0x592`, and `+0x594`.
- Complete names for inherited/non-reflected fields in each `0x58` character
  record.
- The exact protocol role of `+0x924/+0x928`.
- A cleaned, fully typed layout for `cOnePlay`'s tail and all fields after
  `+0x8CC`.
