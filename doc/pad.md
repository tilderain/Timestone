# UMVC3 Pad Input System

## Overview

The pad/input system in UMVC3 is managed by a singleton `sPad` struct (created in
`sPad__sPad_ctor` at `0x14050FBA0`).  It supports up to 4 device slots (XInput and
DirectInput), a keyboard path, and routes input to logical players via the
`sMvc3Manager` pad-to-player mapping.

---

## sPad Instance Layout

| Offset   | Size | Name      | Description                                  |
|----------|------|-----------|----------------------------------------------|
| +0x38    | 1    | xinputEn  | Non-zero = XInput processing enabled         |
| +0x40    | 4    | frameTime | Frame timing / delta for vibration calc      |
| +0x48    | 4    | vibRate   | Vibration rate                               |
| +0xB     | 0x5C×4 | maps[]  | 4 button-mapping objects (key remap tables)  |
| +0xC20   | 1    | kbBlock1  | Non-zero blocks keyboard                     |
| +0xC30   | 1    | kbBlock2  | Non-zero blocks keyboard                     |
| +0xBE0   | 1    | vibEnable | Non-zero enables vibration processing        |
| +0xBE2   | 1    | kbMapped  | Keyboard mapped player index                 |
| +0xBE3   | 1    | (flag)    | Poll-related flag                            |
| +0xBF0   | 8×4  | devPtrs[] | Pointers to DirectInput device objects       |

### Pad Slots

4 slots at stride **0x2E0**, each starting at `sPad + slotIdx * 0x2E0`:

| SlotOffset | Name         | Description                                     |
|------------|--------------|-------------------------------------------------|
| +0x60      | Be_flag      | Device active flag (1 = active)                 |
| +0x61      | Rno          | Retry/ready counter                             |
| +0x62      | Pad_no       | Pad number (0-3, matches slot idx)              |
| +0x63      | Kind         | Device kind (0=uninit, 2=DInput, 4=XInput)      |
| +0x64      | (pad)        |                                                 |
| +0x68      | Input_attr   | Flags for detection behavior                    |
| +0x70      | Socket_no    | XInput/DInput device index (0-3)                |
| +0x1BC     | input        | **Current processed game-input bitmask**        |
| +0x1C0     | (stick)      | Stick/analog input (alternate format)           |
| +0x1C4     | (stick2)     | Second stick/analog input format                |
| +0x164-0x190 | raw       | Raw XInput state (On, Old, Trg, Rel, Chg, Rep, sticks) |

---

## Device Polling Chain

Every frame, `padPollAllSlots_maybe` (`0x140514570`) iterates the 4 slots:

```
for each slot:
  if slot[Be_flag] == 0:
    skip (device not present)
  padSlotReadDevice_maybe(slot_cx)      ← 0x140514B90
  if read succeeded:
    padSlotPostReadProcessVibration(...) ← 0x1405146A0
```

### padSlotReadDevice_maybe

Takes a "slot context" pointer = `slot_base + 0x58`.  Branches on `Kind`:

| Kind | Path                                |
|------|-------------------------------------|
| 0    | Uninit — run detection (`FUN_140514950`), return 1 |
| 2    | DirectInput — poll via `devPtrs[]`  |
| 4    | **XInput** — `XInputGetState()` → `xinputToGameMask_maybe()` |

The XInput path (`Kind == 4` or anything not 0 or 2):

1.  Call `XInputGetState(socket_no, &state)`
2.  If success AND `sPad[0x38]` is non-zero:
    - `xinputToGameMask_maybe(sPad, slot_cx, &state)` — converts XInput state to
      game bitmask at `slot[0x164]` (On), `slot[0x168]` (Old), `slot[0x16C]` (Trg), etc.
    - Returns 1
3.  If success but `sPad[0x38]` is 0:
    - **Clears** `slot[0x164-0x190]` (zeroes the input buffer!)
    - Falls through to `padSlotClearOnDisconnect_maybe`
4.  If `XInputGetState` fails: calls `padSlotClearOnDisconnect_maybe`, returns 0

### Critical flags for XInput to work

Two flags MUST be set for the XInput path to actually read input:

1.  **`sPad[0x38]`** = 1 — without this, `xinputToGameMask_maybe` is skipped and the
    input buffer is zeroed instead.
2.  **`slot[Kind]`** = 4 — without this, the device stays in "uninit" (Kind==0) path
    and never reaches the XInput polling code.

---

## Input Routing (GetsCurrentInput_withSOCD)

`GetsCurrentInput_withSOCD` (`0x1402B3700`) is the central per-frame input getter:

```
GetsCurrentInput_withSOCD(sPad, deviceIdx):
  if deviceIdx > 3: return 0

  keyboardPlayer = sMvc3Manager[0x140][0]  // device index that uses keyboard
  if keyboardPlayer == deviceIdx AND sPad[0xC30] == 0:
    return keyboardToGameMask_inline(sPad)  // KEYBOARD input
  else:
    return slot[deviceIdx].input            // PAD input at slot+0x1BC
```

The function returns **either** keyboard **or** pad — never both.  The hook
in `hkprocessFrameInput` ORs the missing source.

### GetsCurrentInput_withSOCD callers

- `cChrInput__processFrameInput` (gameplay per-frame)
- `chrSelectP2PadCheck_maybe` (character select)
- Various menu / pause functions

---

## Keyboard Input

The keyboard is treated as a device at `sMvc3Manager[0x140][0]`.  If this value
matches the requested device index and `sPad[0xC30]` is 0, the function reads
DirectInput keyboard state instead of the physical pad slot.

Key VK-to-bitmask mappings (from `sMvc3Main__keyboardToGameMask_maybe` at
`0x1402B3390` and the inline version at `0x1402B2FA0`):

| VK Code | Game Bit | Button  |
|---------|----------|---------|
| 0x25    | 0x80     | Left    |
| 0x27    | 0x20     | Right   |
| 0x26    | 0x10     | Up      |
| 0x28    | 0x40     | Down    |
| 0x41 (A)| 0x4000   | A       |
| 0x42 (B)| 0x2000   | B       |
| 0x58 (X)| 0x8000   | X       |
| 0x59 (Y)| 0x1000   | Y       |
| 0xA0    | 0x400    | LB      |
| 0xA1    | 0x800    | RB      |
| 0xA2    | 0x100    | LT      |
| 0xA3    | 0x200    | RT      |
| 0xA4    | 0x2      | L3      |
| 0xA5    | 0x4      | R3      |
| 0x09 (Tab) | 0x1   | Back    |
| 0x0D (Enter) | 0x8 | Start   |

The keyboard reader (`sMvc3Main__method_3D00` at `0x1402B3D00`) also checks
`sPad[0xC20]` and `sPad[0xC30]` — if either is non-zero, keyboard input is
blocked (returns false).

### Keyboard-disable flags

Two bytes at `sPad + 0xC20` and `sPad + 0xC30`:

- Set to non-zero during license check when no save data exists.
- **Cleared** by save-data loading (the normal "Press Start" → menu flow).
- In the training boot, we clear them explicitly (`= 0`).

---

## Save-Data Loading (bypassed by training boot)

The normal "Press Start" → title screen → character select flow loads
`savedata/umvc3/savedata.bin`, which performs these pad-related initializations:

1.  Clears `sPad[0xC20]` and `sPad[0xC30]` (keyboard-disable flags)
2.  Sets `sPad[0x38] = 1` (XInput enable)
3.  Sets `sPad[0xBE0] = 1` (vibration enable)
4.  Sets each slot's `Kind` to 4 (XInput) for connected devices
5.  Writes the initial pad-to-player mapping in `sMvc3Manager`

The training boot bypasses the entire menu/save-data flow, so these
initializations must be done manually in `SetupPadToPlayerMapping()`.

---

## Training Boot Requirements

For the gamepad to work in training-boot mode, the following initializations
are required (in `SetupPadToPlayerMapping`):

```c
// 1. Keyboard enable
sPad[0xC20] = 0;
sPad[0xC30] = 0;

// 2. XInput enable
sPad[0x38] = 1;

// 3. Pad slot state (for each connected slot)
slot[0x60] = 1;  // Be_flag = active
slot[0x62] = i;  // Pad_no = slot index
slot[0x63] = 4;  // Kind = XInput

// 4. Manager mapping
manager[0x140][0] = 0;  // slot 0 → P1 (also keyboard)
manager[0x140][1] = 1;  // slot 1 → P2
```

For simultaneous keyboard + gamepad on P1, the input must be OR'd in
`hkprocessFrameInput` because `GetsCurrentInput_withSOCD` returns only one
source.

---

## SOCD Cleaning

Applied in `ApplySOCD` (matches the game's original logic at `0x1402B3700`):

| Combined bits         | Action         |
|-----------------------|----------------|
| IN_STICK_LEFT+IN_RIGHT | Clear IN_RIGHT |
| IN_STICK_RIGHT+IN_LEFT | Clear IN_STICK_RIGHT |
| IN_LEFT+IN_RIGHT       | Clear IN_RIGHT |
| IN_STICK_LEFT+IN_STICK_RIGHT | Clear IN_STICK_RIGHT |
| IN_STICK_DOWN+IN_UP    | Clear IN_STICK_DOWN |
| IN_STICK_UP+IN_DOWN    | Clear IN_DOWN  |
| IN_DOWN+IN_UP          | Clear IN_DOWN  |
| IN_STICK_UP+IN_STICK_DOWN | Clear IN_STICK_DOWN |

Priority: **LEFT wins RIGHT**, **UP wins DOWN** (both D-pad and analog stick).

---

## Netplay Input Flow

### Host (P1 local, P2 remote)

```
Frame:
  hkprocessFrameInput(cChrInput[P1]):
    oprocessFrameInput → GetsCurrentInput_withSOCD(pad, P1_devIdx)
      → returns KEYBOARD input (when manager[0]==P1 && c30==0)
    Our OR: |= padRaw + kb → combined
    g_localPending |= combined       ← accumulated for sending
    delayed = g_oldInputs[delayPos]
    cChrInput[P1].input = delayed    ← delayed combined input

  hkprocessFrameInput(cChrInput[P2]):
    oprocessFrameInput → GetsCurrentInput_withSOCD(pad, P2_devIdx)
      → returns 0 (netplay host: P2 is remote)
    g_localPending |= 0
    g_receivedInput[currentFrame] → cChrInput[P2].input  ← remote's input
```

### Client (P1 remote, P2 local)

```
Frame:
  hkprocessFrameInput(cChrInput[P1]):
    oprocessFrameInput → GetsCurrentInput_withSOCD(pad, P1_devIdx)
      → returns 0 (netplay client: P1 is remote)
    Our OR: teamIdx != 0, skip
    g_localPending |= 0
    g_receivedInput[currentFrame] → cChrInput[P1].input  ← remote's input

  hkprocessFrameInput(cChrInput[P2]):
    oprocessFrameInput → GetsCurrentInput_withSOCD(pad, P2_devIdx)
      → returns pad slot 1 input (mirrored from slot 0 in offline, or slot 0 in netplay)
    Our OR: teamIdx == 1, skip (OR only targets team 0)
    g_localPending |= cChrInput[P2].input
    ...
```

### Controller_OnFrameEnd

At the end of each frame:
```c
memmove(&g_oldInputs[0], &g_oldInputs[1], (MAX_HISTORY - 1) * 4);
g_oldInputs[MAX_HISTORY - 1] = g_localPending;
// Send g_localPending to remote
g_localPending = 0;
```

---

## Key Addresses

| Address       | Function                                 |
|---------------|------------------------------------------|
| 0x140001B00   | `sPad__getInstance`                      |
| 0x14050FBA0   | `sPad__sPad_ctor`                        |
| 0x140514570   | `padPollAllSlots_maybe`                  |
| 0x140514B90   | `padSlotReadDevice_maybe`                |
| 0x1405111B0   | `xinputToGameMask_maybe`                 |
| 0x1402B3700   | `GetsCurrentInput_withSOCD`              |
| 0x1402B3390   | `sMvc3Main__keyboardToGameMask_maybe`    |
| 0x1402B2FA0   | `sMvc3Main__keyboardToGameMask_inline_maybe` |
| 0x1402B3D00   | `sMvc3Main__method_3D00` (keyboard check w/block flags) |
| 0x14002DD60   | `cChrInput__processFrameInput`           |
| 0x14025AD10   | `sMvc3Manager__isPadAssigned`            |
| 0x14025AC40   | `sMvc3Manager__getPlayerCharIdx`         |
| 0x14025B090   | `sMvc3ManagerSetPadToPlayerMapping_maybe` |
| 0x1402B4140   | `sPadCheckSlotDeviceAlive_maybe`         |
| 0x140513A90   | Pad "detect" callback                    |
| 0x140514950   | `FUN_140514950` (pad detection + Kind/Be_flag set) |
| 0x1405106A0   | `padSlotClearOnDisconnect_maybe`         |
| 0x1405146A0   | `padSlotPostReadProcessVibration_maybe`  |
| 0x140E174B8   | `sPad__instance` pointer                 |
| 0x140D510A0   | `sMvc3Manager` pointer                   |
