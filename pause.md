# UMVC3 Pause System Analysis

## Overview

UMVC3 has a multi-layered pause system that freezes game logic at several
independent levels when the pause UI is onscreen.  Understanding these layers
is necessary to keep the simulation ticking during rollback netplay.

---

## Layer 1 — sMain pause flag (`sMain_isPaused_maybe`)

**Address:** `0x140006b50`
**Function:** `sMain_isPaused_maybe` (named via Ghidra)

```c
undefined1 sMain_isPaused_maybe(longlong param_1)
{
  if ((*(int *)(param_1 + 0x78) == 0) && (*(char *)(param_1 + 0x7c) != '\0')) {
    return 1;  // paused
  }
  return 0;    // not paused
}
```

- `sMain + 0x78` = pause counter (counts down each frame)
- `sMain + 0x7c` = pause flag (set to 1 when pause is requested)
- `sMain + 0x7d` = computed pause state (1 when counter reached 0 and flag still set)

**Who reads it:**
- `sAction__move` (`0x140006d20`): early-returns if this returns nonzero
- `sCharacter__move` (`0x1400597f0`): early-returns if this returns nonzero

**How it's set:**
Every frame `sMain__move` (`0x140521df0`) manages the counter at +0x78.
When +0x7c is non-zero, the counter decrements.  When it hits zero AND +0x7c
is still set, +0x7d is written to 1.

**Attempted patches:**
1. Clear +0x7c to 0 in sMain__move (`0x140521e0e`, `C6 41 7C 00`):
   Did not work — still hangs.
2. NOP sMain_isPaused_maybe to always return 0 (`0x140006b50`, `32 C0 C3`):
   Did not work — still hangs.

---

## Layer 2 — Stage scheduler pause

**Scheduler-pause check function:** `uStageObject__isSchedulerPaused_maybe` (`0x1401f0940`)

```c
byte uStageObject__isSchedulerPaused_maybe(longlong param_1)
{
  return *(byte *)(param_1 + 0x1474) & 1;
}
```

- `stage_object + 0x1474` bit 0 = scheduler pause flag

**Who reads it:** `sStage__move` (`0x1401f0aa0`):

```c
void sStage__move(longlong param_1)
{
  // ... cleanup dead scheduler ...

  uVar1 = *(uint *)(param_1 + 0x40);
  if (((uVar1 & 4) == 0) && (*(longlong *)(param_1 + 0x2050) != 0)) {
    cVar2 = uStageObject__isSchedulerPaused_maybe(param_1);
    if (cVar2 != '\0') {
      *(uint *)(param_1 + 0x40) = uVar1 | 4;   // set "skip" bit
    }
  }
  // ^^^ scheduler at +0x2050 is paused when bit 2 at +0x40 is set

  if (*(longlong *)(param_1 + 0x2058) != 0) {
    // ... clean up secondary scheduler ...
  }
  FUN_1401f03d0(param_1);   // shot processing — always runs
}
```

- `stage + 0x40` bit 2 = "stage scheduler skip" flag
- When set, the stage's main scheduler FSM commands don't execute
- `FUN_1401f03d0` (shot/projectile processing) always runs regardless

**Attempted patches:**
1. NOP the `or edi, 0x04` at `0x1401f0af7` (`83 CF 04` -> `90 90 90`):
   Did not work — still hangs.

---

## Layer 3 — OBJ_MovePause (object-level movement pause)

**FSM handler:** `uStageObject__setObjMovePauseFlag_maybe` (`0x1401ec900`)

```c
void uStageObject__setObjMovePauseFlag_maybe(longlong param_1, byte param_2)
{
  *(uint *)(param_1 + 0x1478) = *(uint *)(param_1 + 0x1478) & 0xffffdfff;
  *(uint *)(param_1 + 0x1478) = *(uint *)(param_1 + 0x1478) | (param_2 & 1) << 0xd;
}
```

- Sets/clears bit 0x2000 at `stage_object + 0x1478` — gates individual object
  movement within the stage (animations, environment motion).

**Registered as FSM command:** `OBJ_MovePause` string at `0x140afbbb8`.
Referenced from `FUN_1401f1c00` (`0x1401f1c00`) which is the stage FSM
command registration function.

---

## Layer 4 — FSM scheduler-pause commands

Registration at `FUN_1401f1c00` (`0x1401f1c00`) registers these FSM commands
via `MtMemoryAllocator__allocTail`:

| FSM Command String           | Handler/Data Address |
|------------------------------|----------------------|
| `OBJ_MovePause`              | `FUN_1401ec900`      |
| `STG_MainSchedulerPause`     | `LAB_1401f1abc`      |
| `STG_EnvSchedulerPause`      |                      |
| `STG_DefLimSchedulerPause`   |                      |

These are dispatched by the stage's scheduler FSM.  Each pause command causes
the corresponding scheduler layer to stop processing until an unpause command
is received.

---

## Layer 5 — `cChrPause` (character-level pause)

**Class:** `cChrPause` (DTI at `0x140a8e450`)

**Properties:** `mMode` and `mbDisable`

**Constructor:** `cChrPause__createProperty` (`0x140031080`)

The character pause manager sits at offset `0x1560` from the character object.
`uGalactus__stateCheck_41B0` (`0x1400241b0`) checks 6 entries (team members)
starting at `char + 0x1570` and returns a bitmask of active pause states.

The `sCharacter__move` path checks character state flags at `char + 0x14f4`
(bit 0x80000) and other offsets in `moveAfter` to skip per-character updates.

---

## Patch attempts and results

| Attempt | Address | What | Result |
|---------|---------|------|--------|
| Patch `sMain_isPaused_maybe` | `0x140006b50` | Always return 0 | Game still hangs |
| Patch `sMain__move` +0x7c | `0x140521e0e` | Clear pause flag byte | Game still hangs |
| NOP stage scheduler `or edi,0x04` | `0x1401f0af7` | Prevent scheduler skip-bit set | Game still hangs |
| Patch `uStageObject__isSchedulerPaused_maybe` | `0x1401f0940` | Always return 0 | Game still hangs |
| Patch `uStageObject__setObjMovePauseFlag_maybe` | `0x1401ec900` | RET (disabled) | Untested alone |

None of these individual patches worked — the game hangs on a black screen
when attempting to enter battle, even with a single patch applied.

---

## Possible root causes

1. **Timing-dependent check**: The pause flag may be checked during loading
   before we get a chance to clear it.  If the pause flag is supposed to be
   set during loading and the game checks it synchronously in the same
   function call, the game logic freezes before reaching our patch site.

2. **Different code path**: The hang may not be caused by these pause
   mechanisms at all — there may be a separate code path that waits for
   resources or signals that depends on the game's normal pause/loading
   state machine.

3. **The pause flag reinitializes each frame**: Something might be setting
   +0x7c back to non-zero after our patch clears it, in a function that
   runs after sMain__move but before sAction__move.

4. **A deeper freeze mechanism**: OBJ_MovePause sets a flag, and there
   may be a check somewhere that actively sleeps/spins based on this flag
   rather than just skipping FSM commands.

---

## Scheduler Layer Summary

```
sMain (game-level)
 ├── +0x78 pause counter (counts down)
 ├── +0x7c pause flag (master enable)
 └── +0x7d computed pause state
     ├── sAction__move — early return when paused
     └── sCharacter__move — early return when paused

stage_object +0x40 bit 2 — stage scheduler skip flag
 └── set by sStage__move when isSchedulerPaused_maybe returns true

stage_object +0x1474 bit 0 — scheduler pause source flag
 └── read by uStageObject__isSchedulerPaused_maybe

stage_object +0x1478 bit 0x2000 — OBJ_MovePause flag
 └── set by uStageObject__setObjMovePauseFlag_maybe (OBJ_MovePause FSM command)

cChrPause at character +0x1560
 └── mMode, mbDisable — per-character pause state

---

## Netplay Pause Solution — `uBattleUiPauseNetGallery` redirect

### Background

The real `uBattleUiPause` menu (created in offline mode when Start is pressed)
freezes game logic via the `sMain_isPaused_maybe` chain.  For rollback netplay
we need the simulation to keep running underneath any pause UI.

### Online-mode pause behavior

In the original game, online matches handle Start differently than offline:

**`uBattleUiCreatePauseMenu_maybe` (`0x1402ba150`)** has two checks:

1. `isGameModeOnlineRanked()` — if true, creates `uBattleUiPauseNetGallery`
   (a lightweight quit-match confirmation overlay).
2. `sBattleSetting__isOnlineBattle()` — if true, skips creating the real
   `uBattleUiPause` menu entirely.

Result: in online ranked mode, pressing Start shows a quit dialog overlay while
the game continues running underneath.  In online player matches, pressing Start
does nothing at all.

### `uBattleUiPauseNetGallery`

**Class:** `uBattleUiPauseNetGallery` (DTI registered at `0x140a1c700`)

**Key functions:**

| Function | Address |
|----------|---------|
| Constructor / `FUN_1402e3eb0` | `0x1402e3eb0` |
| `operator_new` | `0x1402e3f20` |
| vfn5 (per-frame update) | `0x1402e4320` |
| vfn29 (event handler) | `0x1402e40b0` |

**What it does:**
- A quit-match confirmation overlay shown when Start is pressed during an online
  ranked match (or when spectating a ranked match)
- It overlays a yes/no dialog on top of the running game
- Confirming yes disconnects from the network session and returns to the lobby
- The game simulation continues running underneath at full speed — no freeze

**Important internal check:**
Its vfn5 (`0x1402e4320`) calls `isGameModeOnlineRanked()` — if it returns false,
it calls the parent class's vfn78 instead of `uMenuSubBase_vfn5`, which means
the overlay doesn't render.  This check must also be patched for the overlay
to appear in offline/netplay mode.

### Patches applied (dllmain.cpp `OnInitializeHook`)

Three permanent byte patches redirect the pause to the NetGallery in all modes:

| # | Address | Original | Patched | Effect |
|---|---------|----------|---------|--------|
| 1 | `0x1402BA17E` | `0F 84 90 00 00 00` (JZ +0x90) | `90 90 90 90 90 90` (NOP × 6) | Always fall through to NetGallery creation code |
| 2 | `0x1402BA223` | `75 5C` (JNZ +0x5C) | `EB 5C` (JMP +0x5C) | Always skip real pause menu creation |
| 3 | `0x1402E433B` | `75 0C` (JNZ +0x0C) | `EB 0C` (JMP +0x0C) | NetGallery vfn5 always takes render path |

**Safety:** Online ranked mode is unaffected — all three changes result in the
same control flow as the original (the jumps go the same way when the checks
return true).  Only offline mode behavior changes: pressing Start now shows the
NetGallery quit dialog instead of the real pause menu.

