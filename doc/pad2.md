# sPad vs sMvc3Pad

Two separate singleton classes in the game, not the same object.

## sPad — Low-Level Device Handler

- **Singleton pointer**: `sPad__instance` at `0x140E174B8`
- **Constructor**: `sPad__sPad_ctor` at `0x14050FBA0`
- **Vtable**: `&PTR_sPad__dtor_140ba5e60`
- **Getter**: `sPad__getInstance()` at `0x140001B00` — returns the pointer

Extends `cSystem`. Manages raw hardware polling (XInput, DirectInput), keyboard,
vibration, and per-slot raw state.

### Structure

```
sPad (base):
  +0x00  vtable
  +0x38  xinputEn (byte, 1 = enabled)
  +0x40  frameTime
  +0x48  vibRate
  +0x58  Map[0]    }  4 Map objects at stride 0x5C
  +0xB4  Map[1]    }  (button remap tables, via FUN_14050fad0)
  +0x110 Map[2]    }
  +0x16C Map[3]    }
  +0xBE0 vibEnable
  +0xBE2 kbMapped (keyboard→player index)
  +0xBE3 poll flag
  +0xBF0 devPtrs[4] (DirectInput device pointers)
  +0xC20 kbBlock1 (keyboard disable flag)
  +0xC30 kbBlock2 (keyboard disable flag)
```

4 Pad slots at stride **0x2E0**, each starting at `slotIdx * 0x2E0`:

| Offset | Name       | Description                |
|--------|------------|----------------------------|
| +0x00  | detect     | Detection callback         |
| +0x08  | Be_flag    | Device active flag         |
| +0x09  | Rno        | Retry/ready counter        |
| +0x0A  | Pad_no     | Slot index (0-3)           |
| +0x0B  | Kind       | Device kind (0=uninit, 2=DInput, 4=XInput) |
| +0x0C  | Ability    | Capability flags           |
| +0x10  | Input_attr | Detection behavior flags   |
| +0x18  | Socket_no  | Device socket index        |
| +0x1C  | Press_free |                            |
| +0x1D  | Trigger_free |                          |
| +0x1E  | Analog_free |                           |
| +0x20  | Analog_cross_free |                     |
| +0x164 | On         | Raw XInput On bits         |
| +0x168 | Old        | Previous frame raw bits    |
| +0x16C | Trg        | Trigger bits               |
| +0x170 | Rel        | Release bits               |
| +0x174 | Chg        | Change/XOR bits            |
| +0x178 | Rep        | Auto-repeat bits           |
| +0x17C | Rx         | Right stick X              |
| +0x180 | Ry         | Right stick Y              |
| +0x184 | Lx         | Left stick X               |
| +0x188 | Ly         | Left stick Y               |
| +0x18C | Rz/RT      | Right trigger              |
| +0x18D | Lz/LT      | Left trigger               |
| +0x190 | mTriggerVibLow |                       |
| +0x191 | mTriggerVibHigh |                      |
| +0x192 | mVibStartValue |                       |
| +0x194 | mVibEndValue   |                       |
| +0x196 | mVibTime       |                       |
| +0x198 | JoyPadMapping  | Button→game mask map   |

### Key functions

| Address     | Function                             |
|-------------|--------------------------------------|
| 0x14050FBA0 | `sPad__sPad_ctor`                    |
| 0x140001B00 | `sPad__getInstance`                  |
| 0x140514570 | `padPollAllSlots_maybe`             |
| 0x140514B90 | `padSlotReadDevice_maybe`           |
| 0x1405111B0 | `xinputToGameMask_maybe`            |
| 0x1402B3700 | `GetsCurrentInput_withSOCD`         |
| 0x1405146A0 | `padSlotPostReadProcessVibration_maybe` |
| 0x140514F20 | `inputAutoRepeatTimerProcess_maybe` |
| 0x1402B9E80 | `sPad__remapButtonMask_maybe`       |
| 0x1405142A0 | `sPad__initSlotInner`               |
| 0x140514950 | Pad detection + Kind/Be_flag set    |
| 0x140514EA0 | `sPad__stopVib`                     |
| 0x1405106A0 | `padSlotClearOnDisconnect_maybe`    |
| 0x1402B4140 | `sPadCheckSlotDeviceAlive_maybe`    |

Sub-objects:
- `sPad__Pad` (vtable, DTI, createProperty, vfn6)
- `sPad__Map` (vtable, DTI, createProperty, vfn6, dtor)

---

## sMvc3Pad — Game-Level Pad Configuration Manager

- **Singleton pointer**: `QWORD_140e1b3e0` (written in ctor, cleared in dtor)
- **Constructor**: `sMvc3Pad__sMvc3Pad` at `0x14075b620`
- **Vtable**: `&PTR_sMvc3Pad__dtor_140bbc450`
- **DTI**: `sMvc3Pad__DTI` (returned by `sMvc3Pad__getDTI()` at `0x140289440`)

Also extends `cSystem` (separate instance from sPad). Manages game-specific
pad configuration — button decisions, repeat rates, XInput toggle.

### Registered Properties (from `sMvc3Pad__createProperty_0` at `0x140513720`)

| Property             | Offset | Type      | Description                |
|----------------------|--------|-----------|----------------------------|
| mXInput              | +0xBE0 | 0x30003   | XInput enable/disable      |
| mDecideButton        | +0xBD8 | 6         | Confirm/A button mapping   |
| mCancelButton        | +0xBDC | 6         | Cancel/B button mapping    |
| mSysRepeadStDefault  | +0x4C  | 6         | Auto-repeat start delay    |
| mSysRepeadNxDefault  | +0x50  | 6         | Auto-repeat interval       |
| mPad                 | +0x58  | 0x1200001 | Array of 4 Pad objects     |

Additional fields (from decompiled functions):
- +0x38: state
- +0x3C: counter/lock
- +0x40: state/substate
- +0x60: config (from +0x1360 << 3)
- +0x78: computed size
- +0x88: allocator handle
- +0x90: allocator handle
- +0x98: state
- +0x1360: some count
- +0x1364: flag
- +0x1366: flag
- +0x1367: 0x40-byte buffer
- +0x1430: buffer handle
- +0x1438: buffer handle
- +0x1440: counter
- +0x1448: string buffer

### Key functions

| Address     | Function                        |
|-------------|----------------------------------|
| 0x14075b620 | `sMvc3Pad__sMvc3Pad` (ctor)     |
| 0x140289440 | `sMvc3Pad__getDTI`              |
| 0x1402b2f80 | `sMvc3Pad__createProperty`      |
| 0x140513720 | `sMvc3Pad__createProperty_0`    |
| 0x1402b2e40 | `sMvc3Pad__operator_new`        |
| 0x14075b7e0 | `sMvc3Pad__dtor`                |
| 0x14075b730 | `sMvc3Pad__dtor_B730`           |
| 0x140a18f20 | `sMvc3Pad__MyDTI__register`     |
| 0x14075ba20 | Config init / state machine     |
| 0x14075bb80 | State machine loop              |
| 0x14075be20 | Save/load pad config            |
| 0x14075c1e0 | (used by state machine)         |
| 0x14075bd30 | (used by state machine)         |

---

## Key Differences

| Aspect              | sPad                              | sMvc3Pad                              |
|---------------------|-----------------------------------|---------------------------------------|
| **Role**            | Hardware device handler           | Game config manager                   |
| **Singleton**       | `sPad__instance` (0x140E174B8)    | `QWORD_140e1b3e0`                     |
| **Vtable**          | `PTR_sPad__dtor_140ba5e60`       | `PTR_sMvc3Pad__dtor_140bbc450`       |
| **DTI**             | `sPad__getDTI`                   | `sMvc3Pad__getDTI`                    |
| **cSystem base**    | Yes                               | Yes (separate instance)               |
| **Polls hardware**  | Yes (XInput, DInput)              | No                                    |
| **Vibration**       | Yes (per-slot low/high motor)     | No                                    |
| **Button mapping**  | Raw remap (`sPad__Map`)           | High-level (mDecideButton, mCancelButton) |
| **Auto-repeat**     | Timer process (`+0x164-0x178`)    | Configuration (mSysRepeadSt/NxDefault) |
| **Save/load**       | No                                | Yes (FUN_14075be20 reads/writes config string) |
| **Slot stride**     | 0x2E0                             | 0x58 (mPad array, different layout)   |

## How they relate

They are **not** the same object and do not wrap each other. They are three
separate singletons that coordinate through `sMvc3Manager`:

```
sMvc3Manager (0x140D510A0)
  +0x140: pad-to-player mapping int32_t[4]

sPad (0x140E174B8)                    sMvc3Pad (QWORD_140e1b3e0)
  polls XInput/DInput                   configures:
  → raw bitmask per slot                  - XInput toggle
  → GetsCurrentInput_withSOCD             - decide/cancel buttons
    checks:                               - repeat rates
      sMvc3Manager+0x140[0]               - pad sub-settings
      sPad+0xC30
      sPad+slot*0x2E0+0x1BC
```

- **sPad** owns the actual device polling and raw input state
- **sMvc3Manager** routes pad slots to player indices
- **sMvc3Pad** configures game-level pad preferences (which button is
  "decide", what the repeat delays are, whether XInput is on)
