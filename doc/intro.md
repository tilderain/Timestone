# Intro Skip Mechanism

**Status: Native reference with current-plugin notes. Reviewed 2026-10-26.**
The plugin hooks `sAction__stepIntro` at `0x140007D90`. Native intro behavior is
described separately from the custom netplay override below.

## Native Dispatcher

`sAction__stepIntro` selects one of two intro handlers and requests action step
3 when that handler reports completion:

```c
void sAction__stepIntro(sAction *action)
{
    bool finished;

    if (sAction__testGameFlag(action, 0x10000)) {
        finished = sAction__advanceBattleIntroAndHandleSkip(action);
    } else if (g__isMissionMode(sBattleSetting__getInstance())) {
        finished = true;
    } else {
        finished = processNormalIntro_8440(action);
    }

    if (finished) {
        sAction__setNextStep(action, 3);
    }
}
```

The tail call at `0x140007DDB` targets `0x14000AE00`. That function writes
`sAction::mStepOrder` under the game-system critical section. It does not store
a three-second countdown timer; the argument value `3` is the next action step.

### Mission-mode bypass

`g__isMissionMode` at `0x14024D560` returns `gameMode == 6`. Native Mission mode
bypasses both intro handlers and requests step 3 immediately. Earlier versions
of this note incorrectly called mode 6 Training mode.

### Game flags

The dispatcher tests mask `0x10000` through the function at `0x14000B840`.
Other intro paths set flags through `sAction__orGameFlag` at `0x14000ADD0`.
These are action-state flags, not direct button fields.

## Normal Intro Processor (`0x140008440`)

Ghidra currently names this function `sAction__checkRoundTimeout`, but its body
is the normal character-intro processor. The timeout name is misleading.

The function advances `sAction::mSubStep` using `sAction::mStepTime`:

| Substep | Threshold | Verified behavior |
|---:|---:|---|
| 0 | Immediate | Starts team intro actions and intro audio. |
| 1 | `230.0f` | Advances team actions, performs mode-dependent UI/audio setup, and starts BGM when configured. |
| 2 | `280.0f` | Advances another team action. |
| 3 | `460.0f` | Plays an intro voice/sound reference. |
| 4 | `505.0f` | Clears the team intro action. |

An optional cut-in camera object also receives a time derived from the intro
timeline. Without that object, natural completion occurs after `600.0f`; with
it, completion follows the cut-in animation end. Natural completion sets game
flag `0x8000` and returns true.

### Local input path

The function reads player 1 through `sPad__getRawStickInput_noSOCD`. It may OR
in player 2's raw input if `sAction__allowsSecondPlayerSkipInput` (historically
`sAction__queryGameModeUnlockState_6980`) permits it
and player 2 has an assigned pad. Mode 2 explicitly clears the combined local
input. A nonzero `combinedInput & 0xF000` dismisses the prompt and completes the
intro.

The exact controller actions represented by all four high bits are not proven
here. The verified fact is only that the native intro tests mask `0xF000`.
Earlier versions incorrectly described this reader as applying SOCD cleaning
and supplied an unsupported full controller-bit mapping.

### Native online paths

For a live online battle (`BattleType` 3 or 9), after the initial intro guard,
the function replaces the local decision with two bytes from the synchronized
netpad buffers. If either byte has a high-nibble bit (`(p1 | p2) & 0xF0`), it
produces action input bit `0x1000` and completes the intro.

For online spectator mode (`BattleType` 4), it produces the same `0x1000` bit
after the replay's team-0 frame count exceeds 10 and the intro guard permits
skipping. This is a replay-driven path, not ranked-mode behavior.

## Alternate Handler (`0x140007DF0`)

`sAction__advanceBattleIntroAndHandleSkip` (historically
`uActionSideCamera__func_7DF0`) runs when action flag `0x10000` is already set.
It has two animation branches selected by a battle-setting byte currently
exposed as `sBattleSetting__getByte_at_885`. The function updates character
intro actions, reads local raw pad input with the same `0xF000` mask, and may:

- Set `0x8000` on natural completion after `780.0f`.
- Set `0x8000000` and perform the full skip transition when input is accepted.
- Reset camera/character intro state and dismiss the intro prompt.

The setting byte's semantic name is not established, so it should not be
described as a generic "side-camera" Boolean without further evidence.

## Current Plugin Override

`controller.cpp` installs `hkStepIntro` at `0x140007D90`. While custom netplay
is active, the hook waits for `FrameStepper_IsBattleSynchronized()` and then
calls `sAction__setNextStep(action, 3)` directly:

```cpp
if (Netplay_IsActive() && FrameStepper_IsBattleSynchronized()) {
    setNextStep(action, 3); // native function at 0x14000AE00
    return;
}
```

Before the battle-ready barrier, it calls the original intro function **and
clears any nonnegative queued step back to -1** through `setNextStep`. This
prevents held native skip input from escaping phase 2 before mutual release.
The pseudocode above shows only the post-release fast path, not the whole hook.
The shared readiness gate is established when phase 2 has been entered and no next
phase is queued; `mSubStep` may still be zero if the first intro tick was
gated by a native pause. The override therefore does not skip merely because
netplay became active, nor does it depend on intro button input.

## Key Addresses

| Address | Verified role |
|---|---|
| `0x140007D90` | `sAction__stepIntro`, intro dispatcher and plugin hook target |
| `0x140007DF0` | Alternate flagged intro/skip handler |
| `0x140008440` | Normal intro processor; current Ghidra timeout name is misleading |
| `0x140006980` | Mode-dependent predicate controlling whether local player 2 is read |
| `0x14000ADD0` | OR a mask into the action game-flags field |
| `0x14000AE00` | Set `sAction::mStepOrder` under the system critical section |
| `0x14000B570` | Select and initialize mode/intro sound state |
| `0x14000B840` | Test an action game-flag mask |
| `0x14024D560` | `g__isMissionMode`, true when `gameMode == 6` |

## Review scope and evidence

The dispatcher body at `0x140007D90` was checked on 2026-10-26 in live Ghidra
8193 (`umvc3.exe.unpacked.exe`, preferred base `0x140000000`).
It confirms the flagged handler's current name, Mission bypass, and step-3
request. `controller.cpp::hkStepIntro` and `framestepper.cpp::HookedMove`
confirm the queued-step cancellation and separate opening/intro barriers.
Detailed normal/alternate timeline offsets above retain historical static
evidence and were not exhaustively rechecked. This was a read-only review,
not a runtime intro/skip test.
