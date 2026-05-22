# UMVC3 Pause System — Complete Analysis

## Overview

UMVC3 has a multi-layered pause system that freezes game logic at several
independent levels when the pause UI is onscreen. Understanding these layers
is necessary to keep the simulation ticking during rollback netplay.

**Key finding:** You cannot pause during online matches in UMVC3's native
netcode. Pressing Start in an online match either shows a non-pausing gallery
overlay (ranked) or does nothing at all (player match). The game logic and
network sync continue running underneath.

---

## Two Distinct Pause UI Objects

UMVC3 has two separate pause-related UI objects, only one of which actually
freezes the game:

### `uBattleUiPause` — The Real Pause Menu (offline only)

- Size: **0x14a0** bytes
- Created only when `sBattleSetting__isOnlineBattle()` returns **false**
- Provides: resume, quit, options, etc.
- **Actually freezes game logic** via the `sMain` pause flag

### `uBattleUiPauseNetGallery` — Gallery Overlay (online ranked only)

- Size: **0xc0** bytes
- Created only when `isGameModeOnlineRanked()` returns **true**
- Provides: a way to quit the match, possibly gallery/photo viewing
- **Does NOT freeze game logic** — the game continues underneath

### Where Pause Is Created

`uBattleUiCreatePauseMenu_maybe` (`0x1402BA150`) is the central decision point:

```c
void uBattleUiCreatePauseMenu_maybe(longlong param_1)
{
    *(undefined2 *)(param_1 + 0x5ab8) = 0;
    *(undefined1 *)(param_1 + 0x5aba) = 0;

    // --- ONLINE RANKED path: create NetGallery overlay ---
    uVar3 = sBattleSetting__getInstance();
    cVar2 = isGameModeOnlineRanked(uVar3);
    if (cVar2 != '\0') {
        if ((*(longlong *)(param_1 + 0x5ab0) != 0) && ...)
            *(undefined8 *)(param_1 + 0x5ab0) = 0;
        lVar4 = uBattleUiPauseNetGallery__operator_new(0xc0, 0x10);
        if (lVar4 != 0)
            uVar3 = FUN_1402e3eb0(lVar4);
        *(undefined8 *)(param_1 + 0x5ab0) = uVar3;
        sUnit__addUnit(sUnit__getInstance(), 0x19, uVar3);
    }

    // --- OFFLINE path: create real pause menu ---
    uVar5 = sBattleSetting__getInstance();
    cVar2 = sBattleSetting__isOnlineBattle(uVar5);
    if (cVar2 == '\0') {  // <-- only offline
        lVar4 = uBattleUiPause__operator_new(0x14a0, 0x10);
        if (lVar4 != 0)
            uVar3 = uBattleUiPauseConstructor_maybe(lVar4, 0, 0);
        *(undefined8 *)(param_1 + 0x5aa8) = uVar3;
        sUnit__addUnit(sUnit__getInstance(), 0x19, uVar3);
    }
}
```

### Decision Table

| Mode                | `isOnlineRanked` | `isOnlineBattle` | What's created             | Game pauses? |
|---------------------|:----------------:|:----------------:|----------------------------|:------------:|
| Offline (any)       | false            | false            | `uBattleUiPause`           | **Yes**      |
| Online player match | false            | **true**         | **Nothing**                | No           |
| Online ranked       | **true**         | true             | `uBattleUiPauseNetGallery` | No           |

---

## Online Pause Input Handling

`uBattleUiReadInputsAndCheckPause_maybe` (`0x1402E019D`) is called each frame:

```c
// P1 start button press
if ((uVar4 & 8) != 0 && (uVar6 & 1) == 0) {  // start edge
    *(int*)(unaff_RSI + 0x1458) = 0;           // active player = P1
    bVar1 = true;
}

// P2 start button press
if ((uVar5 & 8) != 0 && (uVar7 & 1) == 0) {  // start edge
    cVar2 = sBattleSetting_isOfflineVersus_maybe();  // <-- must be OFFLINE versus
    if (cVar2 != '\0') {
        cVar2 = sMvc3Manager__isPadAssigned(1);       // <-- must have pad assigned
        if (cVar2 != '\0') {
            *(int*)(unaff_RSI + 0x1458) = 1;           // active player = P2
            bVar1 = true;
        }
    }
}
```

P1 can always press Start to trigger the pause check. P2's Start is **gated**
behind `sBattleSetting_isOfflineVersus_maybe()` — in online mode this returns
false, so P2's Start is silently ignored. There is no mechanism to sync pause
state between the two peers.

---

## The `uBattleUiPauseNetGallery` (Online Ranked "Pause")

Its vfn29 event handler is essentially a **quit-only dialog** with a 60-frame
(1 second) exit countdown:

```c
void uBattleUiPauseNetGallery_vfn29(longlong param_1)
{
    lVar8 = param_1 + 0xb0;
    lVar7 = sGSound__queryEvent(sGameUI__getInstance(), lVar8);

    if (*(int*)(param_1 + 0xb8) == 0) {  // not yet quitting
        if (lVar7 != 0) {
            cVar2 = uUiElement_int_short_int_short_int_bool_long_____(lVar7);
            if (cVar2 != '\0') return;  // dialog active, wait

            iVar6 = aTitle__getter_C7C0(lVar7);
            if (iVar6 == 0) {           // player confirmed quit
                FUN_140272ea0(sAgentSession__getInstance());  // disconnect
                FUN_1402e42e0(sMvc3Scene__getUI(), 0, 1);     // go to menu
                *(int*)(param_1 + 0xb8) = 1;                   // quitting state
            }
        }
        if (/* license ok && start edge */) {
            sGSound__postEvent(...);  // play sound, dismiss
        }
    }
    else {
        if (*(float*)(param_1 + 0xb4) < 60.0f)  // wait 60 frames
            cUnit__getDeltaTime();
        sArea__getInstance()->vfn50(&aNetworkRoom__DTI);
        FUN_1402e4110();
    }
}
```

It does not pause, does not offer resume, and does not sync with the remote player.

---

## Pause Freeze Mechanism — The 5 Layers

When offline pause is active, game logic is frozen through five independent layers.

### Layer 1 — sMain pause flag (`sMain_isPaused_maybe`)

**Address:** `0x140006b50`

```c
undefined1 sMain_isPaused_maybe(longlong param_1)
{
  if ((*(int *)(param_1 + 0x78) == 0) && (*(char *)(param_1 + 0x7c) != '\0')) {
    return 1;  // paused
  }
  return 0;    // not paused
}
```

| Offset   | Type | Description                                      |
|----------|------|--------------------------------------------------|
| `+0x78`  | int  | Pause counter — counts down each frame            |
| `+0x7c`  | byte | Pause enable flag (1 = pause requested)           |
| `+0x7d`  | byte | Computed result: 1 when counter=0 && enable=1     |

**Who reads it:**
- `sAction__move` (`0x140006d20`): early-returns if nonzero
- `sCharacter__move` (`0x1400597f0`): early-returns if nonzero

**How it's set:** Every frame `sMain__move` (`0x140521df0`) manages the
counter at +0x78. When +0x7c is non-zero the counter decrements. When it hits
zero AND +0x7c is still set, +0x7d is written to 1.

In online mode, none of the code paths set these flags — the `uBattleUiPause`
constructor is what would set them, and it's never called for online matches.

---

### Layer 2 — Stage scheduler pause

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
  uVar1 = *(uint *)(param_1 + 0x40);
  if (((uVar1 & 4) == 0) && (*(longlong *)(param_1 + 0x2050) != 0)) {
    cVar2 = uStageObject__isSchedulerPaused_maybe(param_1);
    if (cVar2 != '\0') {
      *(uint *)(param_1 + 0x40) = uVar1 | 4;   // set "skip" bit
    }
  }
  // scheduler at +0x2050 is paused when bit 2 at +0x40 is set

  if (*(longlong *)(param_1 + 0x2058) != 0) {
    // ... clean up secondary scheduler ...
  }
  FUN_1401f03d0(param_1);   // shot processing — always runs
}
```

- `stage + 0x40` bit 2 = "stage scheduler skip" flag
- When set, the stage's main scheduler FSM commands don't execute
- `FUN_1401f03d0` (shot/projectile processing) always runs regardless

---

### Layer 3 — OBJ_MovePause (object-level movement pause)

**FSM handler:** `uStageObject__setObjMovePauseFlag_maybe` (`0x1401ec900`)

```c
void uStageObject__setObjMovePauseFlag_maybe(longlong param_1, byte param_2)
{
  *(uint *)(param_1 + 0x1478) = *(uint *)(param_1 + 0x1478) & 0xffffdfff;
  *(uint *)(param_1 + 0x1478) = *(uint *)(param_1 + 0x1478) | (param_2 & 1) << 0xd;
}
```

Sets/clears bit 0x2000 at `stage_object + 0x1478` — gates individual object
movement within the stage (animations, environment motion).

**Registered as FSM command:** `OBJ_MovePause` string at `0x140afbbb8`.
Referenced from `FUN_1401f1c00` which is the stage FSM command registration
function.

---

### Layer 4 — FSM scheduler-pause commands

Registration at `FUN_1401f1c00` (`0x1401f1c00`) registers these FSM commands
via `MtMemoryAllocator__allocTail`:

| FSM Command String         | Handler/Data Address |
|----------------------------|----------------------|
| `OBJ_MovePause`            | `FUN_1401ec900`      |
| `STG_MainSchedulerPause`   | `LAB_1401f1abc`      |
| `STG_EnvSchedulerPause`    |                      |
| `STG_DefLimSchedulerPause` |                      |

These are dispatched by the stage's scheduler FSM. Each pause command causes
the corresponding scheduler layer to stop processing until an unpause command
is received.

---

### Layer 5 — `cChrPause` (character-level pause)

**Class:** `cChrPause` (DTI at `0x140a8e450`)

**Properties:** `mMode` and `mbDisable`

**Constructor:** `cChrPause__createProperty` (`0x140031080`)

The character pause manager sits at offset `0x1560` from the character object.
`uGalactus__stateCheck_41B0` (`0x1400241b0`) checks 6 entries (team members)
starting at `char + 0x1570` and returns a bitmask of active pause states.

The `sCharacter__move` path checks character state flags at `char + 0x14f4`
(bit 0x80000) and other offsets in `moveAfter` to skip per-character updates.

---

## Patch Attempts and Results

All individual patches failed — the game hangs on a black screen when
attempting to enter battle even with a single patch applied.

| Attempt | Address | What | Result |
|---------|---------|------|--------|
| Patch `sMain_isPaused_maybe` | `0x140006b50` | Always return 0 | Game still hangs |
| Patch `sMain__move` +0x7c | `0x140521e0e` (`C6 41 7C 00`) | Clear pause flag byte | Game still hangs |
| NOP stage scheduler `or edi,0x04` | `0x1401f0af7` (`83 CF 04` → `90 90 90`) | Prevent scheduler skip-bit set | Game still hangs |
| Patch `uStageObject__isSchedulerPaused_maybe` | `0x1401f0940` | Always return 0 | Game still hangs |
| NOP `sMain_isPaused_maybe` to return 0 | `0x140006b50` (`32 C0 C3`) | Always return 0 | Game still hangs |
| Patch `uStageObject__setObjMovePauseFlag_maybe` | `0x1401ec900` | RET (disabled) | Untested alone |

---

## Possible Root Causes

**Timing-dependent check.** The pause flag may be checked during loading before
the patch has a chance to clear it. If the pause flag is supposed to be set
during loading and the game checks it synchronously in the same function call,
the game logic freezes before reaching the patch site.

**Different code path.** The hang may not be caused by these pause mechanisms
at all — there may be a separate code path that waits for resources or signals
that depends on the game's normal pause/loading state machine.

**The pause flag reinitializes each frame.** Something might be setting +0x7c
back to non-zero after the patch clears it, in a function that runs after
`sMain__move` but before `sAction__move`.

**A deeper freeze mechanism.** OBJ_MovePause sets a flag, and there may be a
check somewhere that actively sleeps/spins based on this flag rather than just
skipping FSM commands.

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
```

---

## Full Capability Matrix

| Capability                   | Offline | Online (Ranked) | Online (Player) |
|------------------------------|:-------:|:---------------:|:---------------:|
| Game freezes                 | Yes     | No              | No              |
| Pause menu (resume/quit/opt) | Yes     | No              | No              |
| Gallery overlay              | No      | Yes             | No              |
| Quit match                   | Yes     | Yes             | No*             |
| P2 can trigger pause         | Yes     | No              | No              |
| Netcode syncs pause state    | N/A     | No              | No              |

*In player matches, since no UI element is created at all, there is no way to
quit via the pause button either. The match must be quit via other means (e.g.
ALT+F4, task manager, or the Steam overlay).*