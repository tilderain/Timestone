# Native Pause and Plugin Frame Stop

**Status: Native reference with current-plugin override. Reviewed 2026-07-23.**
Retail pause UI and the plugin's frame-step/desync stop are separate systems.
The plugin does not enable native online pause synchronization.

## Native Battle-Type Behavior

The relevant predicates are:

| Address | Predicate | Condition |
|---|---|---|
| `0x140006A60` | `sBattleSetting__isLiveOnlineBattle` | `BattleType == 3 || BattleType == 9` |
| `0x140006AB0` | `sBattleSetting__isOnlineSpectatorBattle` | `BattleType == 4` |
| `0x140006AF0` | `g__isOnlineBattle` | Live online or spectator |

`sAction__openVersusPauseMenu` at `0x1402BA150` creates pause-related objects by
these predicates:

| Battle type | Object created |
|---|---|
| Offline, non-attract | Full `uBattleUiPause` |
| Live online, type 3 or 9 | No full pause object |
| Spectator, type 4 | `uBattleUiPauseNetGallery`, but no full pause object |

The spectator object is a network-gallery/watch overlay. Its creation does not
establish that native live online supports synchronized simulation pause.

## Full Pause Creation

The verified native structure of `sAction__openVersusPauseMenu` is:

```c
if (sBattleSetting__isOnlineSpectatorBattle(settings)) {
    destroyOldGalleryIfNeeded();
    gallery = new uBattleUiPauseNetGallery();
    sUnit__addUnit(sUnit, 0x19, gallery);
}

if (!g__isOnlineBattle(settings) &&
    !sBattleSetting__isGameModeAttract(settings) &&
    fullPause == NULL) {
    fullPause = new uBattleUiPause(0, 0);
    sUnit__addUnit(sUnit, 0x19, fullPause);
}
```

Thus native live online skips full pause-object construction. Patching only
this branch can create the object, but does not add a network protocol for menu
state, cursor state, simulation stopping, or teardown.

## `sMain` Pause Gate

`sMain__isPausedThisFrame` at `0x140006B50` returns true exactly when:

```c
sMain->mSkip == 0 && sMain->mPause != false
```

The relevant offsets are:

| Offset | Role |
|---:|---|
| `sMain+0x78` | Skip/countdown value |
| `sMain+0x7C` | Pause request Boolean |
| `sMain+0x7D` | Pause-edge/result byte written by `sMain__move` |

`sMain__move` at `0x140521DF0` decrements `+0x78` while pause is set and writes
the `+0x7D` result. `sAction__move` at `0x140006D20` and `sCharacter__move` at
`0x1400597F0` both consult `sMain__isPausedThisFrame` and return before normal
updates when it is true.

This is one verified native simulation gate. It should not be interpreted as a
complete inventory of every UI, scheduler, effect, stage, audio, and character
pause mechanism.

## Stage and Character Pause State

The executable also contains stage scheduler commands and reflected character
pause state, including:

- `OBJ_MovePause` registration and its stage-object flag writer at
  `0x1401EC900`.
- Stage scheduler-pause command registration around `0x1401F1C00`.
- Reflected `cChrPause` objects with `mMode`/`mbDisable` properties around
  `0x140031080`.

These establish additional local pause controls. Static presence alone does not
prove that all of them are activated by the full pause menu, nor that one patch
to one layer safely converts retail online play into synchronized pause.

## Current Plugin Behavior

The plugin's `framestepper.cpp` stops execution by intercepting the game move
boundary. This is independent of native `uBattleUiPause` construction:

- Manual frame-step pause is available only in Training mode.
- A checksum desync can auto-pause in any mode by setting the plugin's
  `isPaused` state.
- While plugin-paused, the hooked move is skipped unless one-frame advance is
  requested; a captured D3D frame is presented as the background.
- Custom netplay sends `PACKET_DESYNC` actions for desync trigger and desync
  unpause. Remote actions are applied on the game thread.
- Pressing `P` dismisses a desync alert locally and sends `DESYNC_UNPAUSE` when
  netplay is active.

This protocol synchronizes the plugin's desync-screen stop/dismiss action. It
does not synchronize arbitrary native pause-menu opening, navigation, or quit
operations.

## Safety Boundary

Creating `uBattleUiPause` in native online mode is not sufficient for safe
rollback pause. Any full-menu experiment must account for:

- Both peers stopping at the same custom simulation frame.
- Input packets continuing to be serviced while simulation is stopped.
- Menu ownership and cursor decisions.
- Resume at an agreed frame.
- Quit/restart scene and session teardown.

The current implementation only has a narrow synchronized desync-alert action;
it does not claim these general native-menu guarantees.

## Key Addresses

| Address | Verified role |
|---|---|
| `0x140006A60` | Live-online predicate |
| `0x140006AB0` | Spectator predicate |
| `0x140006AF0` | Broad online predicate |
| `0x140006B50` | Test `sMain` pause/skip state |
| `0x140006D20` | `sAction__move`, includes pause gate |
| `0x1400597F0` | `sCharacter__move`, includes pause gate |
| `0x1401EC900` | Stage object move-pause flag writer |
| `0x1402BA150` | Construct battle pause/gallery objects by mode |
| `0x140521DF0` | Update `sMain` pause and timing state |
