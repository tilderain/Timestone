# Skip Intro Button Mechanism

## Overview

The character intro sequence (where characters banter before "Ready? Fight!") has multiple skip mechanisms. This document traces the full flow through the action state machine.

## State Machine Entry: `sAction__stepIntro` (0x140007d90)

This is the main intro processing function, called each frame during the intro cutscene.

```
sAction__stepIntro(param_1):
  1. Check flag at param_1+0xc4 for bit 0x10000  <- "skip requested" flag
  2. If flag NOT set:
     a. If game mode == 6 (Training) -> jump directly to 3-second countdown
     b. Otherwise -> process cutscene normally via uActionSideCameraProcCutscene_maybe
  3. If flag IS set:
     -> call uActionSideCamera__func_7DF0 (skip transition handler)
  4. If either path returned non-zero (cutscene done or skipped):
     -> set 3-second countdown via sAction__setCountdownTimer(param_1, 3)
```

### Training Mode Skip

`aNextBattle__createProperty_D560` (0x14024d560) simply returns `gameMode == 6`. In training mode the intro is completely skipped — the function jumps straight to the countdown.

### The state flag field

Both `sGameUI__checkInputFlag` and `sAction__transitionToStep` operate on the same field:
- **Read:** `sGameUI__checkInputFlag(param_1, mask)` → `*(uint*)(param_1 + 0xc4) & mask`
- **Write:** `sAction__transitionToStep(param_1, bits)` → ORs bits into `*(uint*)(param_1 + 0xc4)`

Despite the name `checkInputFlag`, this is a general-purpose state/step flags field, not literally a button input check.

## Normal Cutscene Processing: `uActionSideCameraProcCutscene_maybe` (0x140008440)

Steps through the character intro animation in 5 phases tracked by `*(int*)(param_1 + 0xe0)`:

| State | Timer Threshold | Action |
|-------|----------------|--------|
| 0 | (immediate) | Init animations (0x82/0x83/0x84/0x88), play intro BGM |
| 1 | 0x43660000 (~230) | Transition animations to combat stances, play sound |
| 2 | 0x438c8000 (~280) | Transition animations |
| 3 | 0x43e60000 (~460) | Play outro sound |
| 4 | 0x43fc8000 (~504) | Exit cutscene, transition to step 0x8000 |

### Button Check (end of function)

After processing animation state, it checks for player input to skip:

```
rawP1 = GetsStickInputAndTriesToSOCDCleanIt(pad, p1CharIdx)
if (shouldCheckP2) {
    rawP2 = GetsStickInputAndTriesToSOCDCleanIt(pad, p2CharIdx)
    combined = rawP1 | rawP2
}
```

The function `FUN_140006980` determines whether P2's input is checked — it returns 1 for game modes 0 and 4(?), and 0 for others.

**Online mode special path:**
- If online: checks Galactus remote button state → sets skip flag to 0x1000 if any high nibble bits set
- If ranked: after 10+ replay frames → sets skip flag to 0x1000
- **Training mode**: skip flag forced to 0

**Skip condition: `if (combined & 0xf000) != 0`**

On skip: calls `FUN_1402bb750` (destroys the UI "Press button" prompt element) and returns 1.

If no skip: waits for cutscene timer to exceed ~600 (0x44160000), then transitions to step 0x8000 naturally.

## Skip Transition Handler: `uActionSideCamera__func_7DF0` (0x140007df0)

Called when the 0x10000 flag is already set. Handles the skip transition animation.

Two major branches based on `uActionSideCamera__getter_67C0` (a boolean at `battleSettings + 0x375`):
- **Non-side-camera** (standard intro): plays skip animation frames for both teams, then near the end checks actual pad input
- **Side-camera** (special intro framing): simpler animation path, timer-driven

### Button check (bottom of function)

Same pattern as the normal cutscene function:

```
combinedPad = 0
combinedPad |= GetsStickInputAndTriesToSOCDCleanIt(pad, p1CharIdx)
if (FUN_140006980()) combinedPad |= GetsStickInputAndTriesToSOCDCleanIt(pad, p2CharIdx)

if (combinedPad & 0xf000) {
    // Non-side-camera mode:
    if (state > 0 && (state != 1 || timer >= 100) && (state != 2 || timer <= 0x44250000)) {
        sAction__transitionToStep(param_1, 0x8000000)  // Full skip
    }
}
// Also transitions when timer > 0x443e0000 (~780)
```

## The Button Bitmask: 0xf000

In `uActionSideCameraProcCutscene_maybe`, the skip condition is `combined & 0xf000 != 0`. This checks ANY of the 4 high bits in the 16-bit input word.

For the online path, the skip flag is specifically 0x1000 (bit 12).

### Typical MvC3 controller bit layout (from pad structure at offset playerIdx*0x2E0+0x1C4):

```
Bit positions (approximate MvC3 mapping):
  0x0001  - L (Light/Punch1)
  0x0002  - M (Medium/Punch2)
  0x0004  - H (Heavy/Kick1)
  0x0008  - S (Special/Assist)
  0x0010  - Up
  0x0020  - Right
  0x0040  - Down
  0x0080  - Left
  0x0100  - Start
  0x0200  - Back/Select
  0x1000  - (High button 1 - likely A for confirm / map-specific)
  0x2000  - (High button 2)
  0x4000  - (High button 3)
  0x8000  - (High button 4)
```

The 0xf000 mask covers the high nibble — these are the right-side face buttons or shoulder buttons depending on mapping. **Pressing any combination of these buttons during the intro will skip it.**

### SOCD Cleaning

The `GetsStickInputAndTriesToSOCDCleanIt__0` function (0x1402b3950) reads raw pad state from offset `playerIdx*0x2E0 + 0x1C4` and applies Simultaneous Opposite Cardinal Direction (SOCD) cleaning:

| Conflict | Resolution |
|----------|-----------|
| 0x80020 (specific down+up) | Remove the bit 0x20 |
| 0x20080 (specific) | Remove bit 0x20000 |
| 0x20 + 0x80 (left+right) | Remove 0x20 (right) |
| 0x10 + 0x40 (up+down) | Remove 0x10 (up) |
| 0x40010 (specific) | Remove 0x40000 |
| 0x10040 (specific) | Remove 0x40 |
| 0x50000 (specific) | Remove 0x20000 |

## FUN_14000b570 (0x14000b570) — Skip Sound & Animation Setup

Called during the skip transition, this function:
1. Checks game mode
2. Based on `sGameUI__checkInputFlag(param_1, 0x10000)` and game config:
   - Sets the appropriate skip animation ID (0x46, 0x52, 0x51, 0x70)
   - Plays the corresponding sound effect
3. Stores the MVC3 setting reference for the skip animation

## Post-Intro Countdown: `sAction__stepCountdown` (0x140009670)

After the intro is skipped or completes:
1. `sAction__setCountdownTimer(param_1, 3)` → configures 3-second "Ready? Fight!" phase
2. The `stepCountdown` handler:
   - Resolves BGM and plays "FIGHT!" sound
   - At timer threshold 0x78 (120 frames = 2 seconds at 60fps), transitions to `sAction__stepFighting`

## Key Addresses

| Function | Address | Purpose |
|----------|---------|---------|
| `sAction__stepIntro` | 0x140007d90 | Main intro state handler |
| `uActionSideCameraProcCutscene_maybe` | 0x140008440 | Normal cutscene animation + button check |
| `uActionSideCamera__func_7DF0` | 0x140007df0 | Skip transition handler |
| `sGameUI__checkInputFlag` | 0x14000b840 | Reads step flag at param_1+0xC4 |
| `sAction__transitionToStep` | 0x14000add0 | Sets step flag at param_1+0xC4 |
| `sAction__setCountdownTimer` | 0x14000aee0 | Triggers countdown phase transition |
| `GetsStickInputAndTriesToSOCDCleanIt__0` | 0x1402b3950 | Reads pad input with SOCD cleaning |
| `FUN_140006980` | 0x140006980 | Determines if P2 input should be checked |
| `FUN_14000b570` | 0x14000b570 | Skip sound/animation setup |
| `aNextBattle__createProperty_D560` | 0x14024d560 | Returns true if game mode == 6 (Training) |

## Netplay Skip Fix (Final: Hook `sAction__stepIntro`)

The current fix hooks `sAction__stepIntro` (0x140007D90) to auto-skip the intro entirely during netplay.

### Why the `GetsStickInputAndSOCD` hook didn't work

The initial fix hooked `GetsStickInputAndTriesToSOCDCleanIt__0` (0x1402B3950) to return frame-delayed synced input from the ring buffers. Two problems:

1. **`g_oldInputs` is not populated during the intro.** The ring buffer is fed by `hkprocessFrameInput`, which runs during gameplay input processing. During the intro cutscene, `processFrameInput` is never called, so `g_oldInputs` stays all zeros → skip never triggers.

2. **Fundamental delay mismatch.** Even with live input reads, the delay-based system means one side's button press at frame N isn't visible to the other side until frame N + g_delayFrames. This causes the combined `P1 | P2` decision to diverge → desync.

### How the original game handles it

The original game's netplay (SyncPad singleton) bypasses pad input entirely during online intros:

```c
// In uActionSideCameraProcCutscene_maybe (simplified):
if (isGameModeOnline()) {
    // IGNORE local pad input entirely
    // Read both players' states from SyncPad network ring buffers
    b1 = readSyncPadRingBuffer(syncPad, 0);  // P1 network state
    b2 = readSyncPadRingBuffer(syncPad, 1);  // P2 network state
    combined = (b1 | b2) & 0xf0;
    if (combined != 0) uVar5 = 0x1000;  // skip bit
}
```

The SyncPad singleton maintains its own network-synchronized ring buffers (at syncPad+0xD18/+0xD20), which both sides read identically. Additionally, `FUN_140006980` returns 0 for online modes → P2's pad input is never checked (only SyncPad state matters).

### The Fix: Auto-skip in netplay mode

Since we can't use the SyncPad singleton in our rollback netplay, the simplest correct fix is to auto-skip the intro when netplay is active:

```cpp
static void __fastcall hkStepIntro(uint64_t param_1)
{
    if (Netplay_IsActive()) {
        // Same as training mode skip: jump directly to countdown
        auto setCountdown = (void(__fastcall*)(uint64_t, int))_addr(0x14000AE00);
        setCountdown(param_1, 3);
        return;
    }
    oStepIntro(param_1);
}
```

This calls `sAction__setCountdownTimer` (at 0x14000AE00), which:
1. Stores 3 at `param_1 + 0xDC` (3-second countdown)
2. Returns → the frame dispatcher sees `0xDC >= 0` on the next frame
3. The dispatcher calls `executeMatchPhase()`, which transitions the step from 2 (intro) to 3 (countdown)
4. Both sides transition on the same frame → no desync

### Training Mode Reference

The original game already has this exact auto-skip path for training mode (`gameMode == 6`):

```asm
; sAction__stepIntro at 0x140007D90:
...
140007db9: CALL aNextBattle__createProperty_D560  ; returns true if gameMode == 6
140007dbe: TEST AL, AL
140007dc0: JNZ countdown_addr                    ; skip to countdown if training
...
countdown_addr:
140007dce: MOV EDX, 0x3
140007dd3: MOV RCX, RBX
140007ddb: JMP sAction__setCountdownTimer        ; tail call
```

Our hook replicates this exact path for netplay mode.
