# Desync Root Cause Analysis — Complete Technical Reference

**Status: Historical capture analysis.** Session-specific observations remain
evidence, but root-cause rankings, source line numbers, snapshot gaps, and fixes
describe an older build. Later investigations are `fable2`, `fable3`, and
`dash_resim_root_cause.md`; current implementation truth is in `current.md` and
the source.

## Supersession Notes

Read the code excerpts below as a record of the July 2-3 investigation, not as
current source. In particular:

- The `mOldHeld` mismatch was a plausible hypothesis from these captures, but
  the later dash investigation identified re-simulation delta-time handling as
  the demonstrated dash root cause. Do not treat the proposed `+0x10` write as
  a currently validated fix merely because the current hook still does not
  write that field.
- Current input scheduling uses absolute-frame scheduled records, centrally
  resolved remote input, and per-frame snapshot metadata. The old
  `g_oldInputs[MAX_HISTORY-delay]` excerpts no longer describe the live path.
- Current `BuildRegions` captures `sEffect+0x3C` as a 16-byte RNG state, captures
  only `0x50` bytes of the `sShot` manager header, reconstructs shots through
  native spawn recipes, and has ownership-aware restore paths for pointer-bearing
  managers and external allocations.
- Character regions retain their captured object addresses in each snapshot.
  Restore does not assign character bodies to whichever object occupies the
  same traversal ordinal. Roster traversal identity can still make diagnostics
  misleading, but it is not by itself evidence that restore swapped bodies.
- `sMain+0x40228` is a member-pointer slot used to call `sId`; it is not the
  `sId` object body. The proposed guessed `0x100`-byte flat snapshot was never
  validated and must not be applied.
- Current re-simulation still calls full native `oMvc3Main_move`; the surrounding
  snapshot, input, rendering, sound, timing, and reconstruction behavior has
  changed substantially from the excerpts below.

## Table of Contents
1. [Compare.py Results](#1-comparepy-results)
2. [cChrInput Field Layout](#2-cchrinput-field-layout)
3. [Ghidra Decompilation: cChrInput__processFrameInput (0x14002DD60)](#3-ghidra-decompilation-cchrinput__processframeinput)
4. [Ghidra: cChrInput_inputMaskPostProcess (0x14002E5A0)](#4-ghidra-cchrinput_inputmaskpostprocess)
5. [Ghidra: cChrInput__getInputDirection (0x1402CE750)](#5-ghidra-cchrinput__getinputdirection)
6. [Ghidra: sMvc3Main__move Execution Order](#6-ghidra-smvc3main__move-execution-order)
7. [uCharacter__move_base Disassembly — Exact Call Order](#7-ucharacter__move_base-disassembly)
8. [hkprocessFrameInput — Full Source (controller.cpp:1423-1600)](#8-hkprocessframeinput)
9. [hkGetsCurrentInput_withSOCD — Full Source (controller.cpp:699-793)](#9-hkgetscurrentinput_withsocd)
10. [BuildRegions — Snapshot Region Enumeration (rollback.cpp:653-836)](#10-buildregions)
11. [Rollback_SaveSnapshot (rollback.cpp:1338-1394)](#11-rollback_savesnapshot)
12. [Rollback_RestoreSnapshot (rollback.cpp:1435-1499)](#12-rollback_restoresnapshot)
13. [Rollback_ReSimulate (rollback.cpp:1907-2099)](#13-rollback_resimulate)
14. [DiagCaptureLiveRef / DiagLogPreOverride / DiagLogPostOverride (diag.cpp)](#14-diag-capture)
15. [BuildRegions — Character Snapshot Details (rollback.cpp:780-826)](#15-character-snapshot-regions)
16. [cChrTeam Snapshot Size Verification](#16-cchrteam-snapshot-verification)
17. [Root Cause Analysis](#17-root-cause-analysis)
18. [The mOldHeld Bug](#18-the-moldheld-bug-detailed-walkthrough)
19. [Verified Snapshot Coverage](#19-verified-snapshot-coverage)
20. [Snapshot Gaps Identified](#20-snapshot-gaps-identified)
21. [Execution Order: sMvc3Main::move → uCharacter__move_base](#21-execution-order)
22. [Resim vs Live Asymmetry: GetsCurrentInput for Remote Player](#22-resim-vs-live-asymmetry)

---

## 1. Compare.py Results

### Session A: nplive_20260702_195747_host/client (735 frames)
- **First divergence**: frame 675 — action 0x0D (host) vs 0x06 (client)
- **Position diverges immediately** (act+pos split)
- **dirRing diverges frame 676+**: host has direction entries (0x100000000), client all zeros
- **Compounds through end** — real desync

### Session B: nplive_20260703_012018_host/client (708 frames)
- **First divergence**: frame 8 — client chars not initialized (act=0x00), host has act=0x82/0x83/0x84
- **Frame 9**: RNG-only divergence on team 1 AND slot swap (mid=0 vs mid=2)
- **Frame 9-11**: host usedRemote=0x001003F0, client usedRemote=0x00000000
- **Frame 220+**: permanent divergence compounded

---

## 2. cChrInput Field Layout

From `initInputBuffer` (0x14002E440) + `processFrameInput` + `inputMaskPostProcess` disassembly:

| Offset | Size | Name | Written By | Description |
|--------|------|------|-----------|-------------|
| +0x00 | 8 | vtable | Game init | cChrInput vtable |
| +0x08 | 8 | cChr* | Game init | Owning uCharacter pointer |
| +0x0C | 4 | _pad0C | — | Padding |
| +0x10 | 4 | mOldHeld | processFrameInput @ 0x14002E3C0 | Previous frame's mHeld |
| +0x14 | 4 | mHeld | processFrameInput @ 0x14002E3D9 | Current frame input (post-remap, post-swap, post-mMask) |
| +0x18 | 8 | frameCounter | anmchr tick | Per-frame incrementing counter |
| +0x1C | 4 | mButtons | inputMaskPostProcess | Raw button state (shifted each frame) |
| +0x20 | 4 | mPressed | inputMaskPostProcess @ +0x20 | Rising edge: mHeld & ~mOldHeld |
| +0x24 | 4 | mReleased | inputMaskPostProcess @ +0x24 | Falling edge: mOldHeld & ~mHeld |
| +0x28 | 128 | mHeldRing[16] | anmchr tick | 16 × u64 ring of held bits |
| +0xA8 | 4 | relRingHead | inputMaskPostProcess @ +0xA8 | Release ring head cursor |
| +0xAC | 128 | mReleasedRing[16] | anmchr tick | 16 × u64 ring of released bits |
| +0x12C | 4 | (cursor?) | — | Ring head for pressed? |
| +0x130 | 128 | mPressedRing[16] | anmchr tick | 16 × u64 ring of pressed bits |
| +0x1B0 | 128 | dirStatus[32] | inputMaskPostProcess | Direction hold-timer status per button |
| +0x230 | 4 | cursor230 | inputMaskPostProcess | Direction hold-timer cursor |
| +0x234 | 1 | flag234 | — | Single-byte flag |
| +0x238 | 4 | cursor238 | inputMaskPostProcess | Direction timer cursor |
| +0x23C | 4 | cursor23C | inputMaskPostProcess | Direction timer cursor |
| +0x240 | 4 | cursor240 | inputMaskPostProcess | Direction timer cursor |
| +0x244 | 4 | cursor244 | inputMaskPostProcess | Direction timer cursor |
| +0x248 | 4 | cursor248 | inputMaskPostProcess | Direction timer cursor |
| +0x24C | 4 | cursor24C | inputMaskPostProcess | Direction timer cursor |
| +0x250 | 4 | cursor250 | inputMaskPostProcess | Direction timer cursor |
| +0x254 | 4 | cursor254 | inputMaskPostProcess | Direction timer cursor |
| +0x258 | 4 | cursor258 | inputMaskPostProcess | Direction timer cursor |
| +0x25C | 4 | cursor25C | inputMaskPostProcess | Direction timer cursor |
| +0x260 | 128 | dirTimer[32] | inputMaskPostProcess | Per-button frame-counter timers |
| +0x2E0 | 128 | dirTimer2[32] | inputMaskPostProcess | Secondary per-button timers |
| +0x360 | 512 | mDirectionHistory[64] | anmchr tick (via FUN_14002B910) | 64 × 8B direction ring buffer |
| +0x560 | 4 | mRemapped | processFrameInput @ 0x14002E2EE | Post-buttonRemap, pre-swapLRIfFaceLeft |
| +0x564 | 4 | mFaceLeftAdj | inputMaskPostProcess @ +0x564 | Facing-adjusted processed input |
| +0x568 | 4 | mMask | processFrameInput @ 0x14002E3C8 | OR-accumulated suppress mask |
| +0x56C | 4 | mProcessed | processFrameInput @ 0x14002E3E2 | Final bitmask used by anmchr |
| +0x570 | 32 | _unk570 | — | Transition buffer region |
| +0x590 | 4 | h590 | anmchr tick | dirRing read head 1 |
| +0x594 | 4 | h594 | anmchr tick | dirRing read head 2 |

---

## 3. Ghidra Decompilation: cChrInput__processFrameInput

```c
/* R2-INPUT chokepoint: per-character input resolver. Called from uCharacter__move_base.
   Reads from sPad singleton (local) OR sReplay (playback/record) OR sNetPad (online).
   Writes to cChrInput+16 (mHeld/filtered), +20 (mButtons/raw), +1376 (mRemapped),
   +1388 (mProcessed). This is the single read-point before game logic. */

void cChrInput__processFrameInput(longlong param_1)
{
  // param_1 = RCX = cChrInput*
  lVar10 = *(longlong *)(param_1 + 8);        // this->cChr = *(this+8)
  if (lVar10 == 0) return;                     // null cChr → bail

  iVar3 = FUN_140021f50(lVar10);               // character state check
  if (((iVar3 < 0) && ((*(uint *)(lVar10 + 0x14f0) & 0x1000) == 0)) &&
     (uVar4 = cChr__getCombinedFlags_29E0(lVar10), (uVar4 >> 0xc & 1) == 0))
    return;

  uVar9 = sAction__getInstance();
  iVar3 = Concurrency_details_SchedulerProxy_GetNumBorrowedCores_void_(uVar9);
  if (iVar3 == 1) return;                      // bail if single core

  // === SAVE mOldHeld; CLEAR mHeld and mProcessed ===
  *(undefined4 *)(param_1 + 0x10) = *(undefined4 *)(param_1 + 0x14);  // mOldHeld = mHeld
  *(undefined4 *)(param_1 + 0x14) = 0;                                  // mHeld = 0
  *(undefined4 *)(param_1 + 0x56c) = 0;                                  // mProcessed = 0

  // ... get character action object, check gameplay active ...

  // === MAIN PATH (at ~0x14002DEB5) ===
  // Get raw input from sPad:
  uVar7 = sPad__getPlayerInput_withSOCDClean(uVar11, uVar7);

  // Button remap:
  uVar4 = cChrInput__buttonRemap(uVar6, uVar7, (byte)(uVar4 >> 0xc) & 1);
  *(uint *)(param_1 + 0x14) = uVar4;            // mHeld = remapped input

  // [[Post-processing at 0x14002E27A — runs whether or not we took sNetPad path]]
  // This is the common convergence point where side-swap + mMask happens:
  //
  //   MOV  EAX,[RSI+0x14]         ; EAX = mHeld (native, before our override)
  //   MOV  [RSI+0x10],EAX         ; mOldHeld = mHeld
  //   MOV  [RSI+0x560],EDX        ; mRemapped = mHeld (pre-swapLR)
  //   CALL cChrInput__swapLRIfFaceLeft
  //   MOV  [RSI+0x14],EAX         ; mHeld = swapLRIfFaceLeft(mRemapped)
  //   ...
  //   MOV  EAX,[RSI+0x14]         ; EAX = mHeld (post-swap)
  //   MOV  [RSI+0x10],EAX         ; mOldHeld = mHeld (again, after swap)
  //   AND  EAX,0x7FFBF0
  //   OR   [RSI+0x568],EAX        ; mMask |= (mHeld & 0x7FFBF0)
  //   MOV  EAX,[RSI+0x568]        ; EAX = mMask
  //   NOT  EAX                     ; ~mMask
  //   AND  [RSI+0x10],EAX         ; mOldHeld &= ~mMask
  //   AND  [RSI+0x14],EAX         ; mHeld &= ~mMask
  //   MOV  EAX,[RSI+0x560]        ; EAX = mRemapped
  //   MOV  [RSI+0x56C],EAX        ; mProcessed = mRemapped
}
```

**Critical**: The post-processing block writes `+0x10` (mOldHeld), `+0x14` (mHeld again after swap), `+0x560` (mRemapped), `+0x568` (mMask), and `+0x56C` (mProcessed). Our `hkprocessFrameInput` calls the native `oprocessFrameInput` FIRST (which runs this), then **overwrites** `+0x14`, `+0x560`, `+0x56C`, and `+0x568` — but NOT `+0x10` (mOldHeld).

---

## 4. Ghidra: cChrInput_inputMaskPostProcess

```c
void cChrInput_inputMaskPostProcess(longlong param_1)
{
  // Called AFTER processFrameInput in uCharacter__move_base.
  // param_1 = cChrInput*

  lVar9 = *(longlong *)(param_1 + 8);  // cChr
  if (lVar9 == 0) return;

  uVar6 = *(uint *)(param_1 + 0x14);   // mHeld (OUR overridden value)
  uVar7 = *(undefined4 *)(param_1 + 0x1c);
  *(uint *)(param_1 + 0x1c) = uVar6;   // mButtons = mHeld (shift)
  *(undefined4 *)(param_1 + 0x18) = uVar7;

  // mMask &= mRemapped  (narrow suppression mask)
  *(uint *)(param_1 + 0x568) = *(uint *)(param_1 + 0x568) & *(uint *)(param_1 + 0x560);

  // === Direction mask handling (collision/auto-repeat suppression) ===
  if ((*(uint *)(lVar9 + 0x14fc) & 0x8000) == 0) {
    // if not flagged: test game flag 0x800000
    if (sAction__testGameFlag(0x800000) != 0)
      goto LAB_14002e622;
  } else {
LAB_14002e622:
    // Clear direction bits: mOldHeld &= ~0xF, mButtons &= ~0xF
    *(uint *)(param_1 + 0x10) &= 0xfffffff0;
    *(uint *)(param_1 + 0x1c) = uVar6 & 0xfffffff0;
  }
  // ... same pattern for other high bits ...

  // === RISING/FALLING EDGE DETECTION ===
  uVar6 = *(uint *)(param_1 + 0x10) ^ *(uint *)(param_1 + 0x1c);  // XOR old vs new
  uVar13 = *(uint *)(param_1 + 0x1c) & uVar6;                      // rising = new & ~old
  uVar15 = 1;
  *(uint *)(param_1 + 0x20) = uVar13;                              // mPressed = rising
  *(uint *)(param_1 + 0x24) = *(uint *)(param_1 + 0x10) & uVar6;  // mReleased = old & ~new
  *(uint *)(param_1 + 0xa8) = uVar13;                              // relRingHead = rising

  // === PER-BUTTON HOLD TIMER RING UPDATE ===
  // For each button bit:
  //   +0x28+mRing[] = released ring cursor advance
  //   +0xAC+mReleasedRing[] = release timer tracking
  //   +0x130+mPressedRing[] = pressed timer tracking
  //   +0x1B0+mDirStatus[] = direction hold-timer states
  //   +0x260+mDirTimer[] = per-button frame counts
  //   +0x2E0+mDirTimer2[] = secondary timers
  //   +0x360+mDirectionHistory[] = per-direction history ring (64 entries × 8B each)
  //
  // The direction history at +0x360 is populated HERE, using the rising/falling
  // edges computed from mOldHeld vs mButtons.  If mOldHeld is wrong (see mOldHeld
  // bug), the edge detection produces wrong results → dirRing diverges.
  do {
    // ... iterate over 32 button slots, update hold-timer rings and dirHistory ...
  } while (i < 32);
}
```

**Critical**: This function reads `mOldHeld (+0x10)` to compute rising/falling edges. If `mOldHeld` was not properly overridden by our hook, the edge detection uses the **native-processed value** instead of our replayed input → wrong `mPressed`/`mReleased`/`dirRing` entries.

---

## 5. Ghidra: cChrInput__getInputDirection

```c
void cChrInput__getInputDirection(longlong param_1, undefined4 param_2)
{
  // Reads dirRing heads at +0x590 and +0x594 to determine current direction.
  // Called by anmchr state machine for dash/walk/etc. detection.

  cVar1 = cChrInput_testFlagAt710(param_1, 0x20);
  if (cVar1 != '\0') {
    cChrInput_setDirectionHead594(param_1, param_2);  // flag 0x20: use param_2 directly
    return;
  }

  cVar1 = cChrInput_testFlagAt710(param_1, 0x40);
  if (cVar1 != '\0') {
    // flag 0x40: lookup from dirRing using head at +0x594
    cChrInput_lookupDir_fromText_with590Fallback(param_1, *(undefined4 *)(param_1 + 0x594));
    return;
  }

  // Default: lookup from dirRing using head at +0x590, fall back to +0x594
  cChrInput_lookupDir_fromText_no590Fallback(param_1, param_2,
                                              *(undefined4 *)(param_1 + 0x590));
}
```

This reads `h590 (+0x590)` and `h594 (+0x594)` — these are beyond the `0x5A0` cChrInput snapshot window (which ends at `cChrInput + 0x5A0 = character+0x2768`), so `cChrInput + 0x590` = `character + 0x21C8 + 0x590` = `character + 0x2758`, which IS within the 0x5A0 window. **Both heads are inside the snapshot.** Good.

---

## 6. Ghidra: sMvc3Main__move Execution Order

```c
/* sMvc3Main::move. Call order:
   sMain::move (QPC) → sMvc3Main::updateWindowFocus
   → sNetPad__stepHelper (input poll + net I/O)
   → subsystem moves... → sAction::subStepHelper (triggers uCharacter__move_base per char)
   → remaining subsystem updates. */

void sMvc3Main__move(longlong param_1)
{
  sMain__move();                                          // 1. Frame timing / QPC
  sMvc3Main__updateWindowFocus(param_1);                  // 2. Window focus
  sNetPad__stepHelper(*(param_1 + 0x401c0));              // 3. Netpad poll (NOT our hook)
  (**(***(param_1 + 0x40260) + 0x30))();                  // 4. Async loader
  // === RENDER BLOCK (skipped during resim) ===
  (**(***(param_1 + 0x401a8) + 0x48))();                  // 5. Draw
  (**(***(param_1 + 0x40198) + 0x48))();                  // 6. Draw
  (**(***(param_1 + 0x401d8) + 0x48))();                  // 7. Draw
  sCollision__sub_BB710(*(param_1 + 0x401e0));            // 8. Collision sound
  sMvc3Manager__getGameConfig(...) vtable+0x48;            // 9. Game config draw
  sRender__setRenderTargetAspect(...);                     // 10. Render setup
  // ... more render setup ...
  (**(***(param_1 + 0x40270) + 0x48))();                  // 11-14. Draw
  (**(***(param_1 + 0x40208) + 0x48))();
  sRender__beginFrame(*(param_1 + 0x40190));               // 15. Present
  // === POST-RENDER GAME LOGIC ===
  (**(***(param_1 + 0x40200) + 0x30))();                  // 16. sMvc3Area update
  sAction__subStepHelper();                                // 17. ACTION SUBSTEP:
                                                           //     → uCharacter__move_base per char
                                                           //     (THIS IS WHERE hkprocessFrameInput fires)
  (**(***(param_1 + 0x40230) + 0x30))();                  // 18. sMvc3Vibration
  (**(***(param_1 + 0x401c0) + 0x30))();                  // 19. sMvc3SyncPad
  (**(***(param_1 + 0x40210) + 0x30))();                  // 20. sMouse
  (**(***(param_1 + 0x40228) + 0x30))();                  // 21. sId  ← UNIQUE ID ALLOCATOR
  (**(***(param_1 + 0x40218) + 0x30))();                  // 22. sKeyboard
  (**(***(param_1 + 0x401c0) + 0x50))();                  // 23. sMvc3SyncPad vfn+0x50
  (**(***(param_1 + 0x401e8) + 0x30))();                  // 24. sMvc3Shader2
  (**(***(param_1 + 0x40240) + 0x30))();                  // 25. sAI
  // Manager subsystems:
  sMvc3Manager__getGameConfig(...) vtable+0x30;            // 26. sGameConfig
  sMvc3Manager__getGameUi(...) vtable+0x30;                // 27. sGameUI
  sMvc3Manager__getShot(...) vtable+0x48;                  // 28. sShot draw (SKIP)
  sMvc3Manager__getGameEffect(...) vtable+0x48;            // 29. sEffect draw (SKIP)
  (**(***(param_1 + 0x401a0) + 0x30))();                  // 30. sCollision__move
  (**(***(param_1 + 0x401e0) + 0x30))();                  // 31. mpCollision 2nd call
  sMvc3Manager__getHitSolver(...) vtable+0x30;             // 32. sHitSolver
  (**(***(param_1 + 0x401a0) + 0x68))();                  // 33. Physics vfn+0x68
  (**(***(param_1 + 0x401a0) + 0x58))();                  // 34. Physics vfn+0x58
  sAI__subStepHelper(*(param_1 + 0x40240));                // 35. AI direct call
  // ... more vtable+0x30 updates ...
  sMvc3Manager__getCharacter(...) vtable+0x30;             // 36. sCharacter update
  sMvc3Manager__getStage(...) vtable+0x30;                 // 37. sStage update
  sMvc3Manager__getShot(...) vtable+0x30;                  // 38. sShot update (logic)
  sMvc3Manager__getGameEffect(...) vtable+0x30;            // 39. sEffect update (logic)
  sMvc3Manager__getHitSolver(...) resetActiveNodes;        // 40. Hit nodes reset
  sMvc3Manager__getReplay(...) vtable+0x30;                // 41. sReplay
  sMvc3Manager__getBattleSet(...) vtable+0x30;             // 42. sBattleSetting
  // ... remaining vtable+0x30 member updates ...
}
```

---

## 7. uCharacter__move_base Disassembly

From raw disassembly at `LAB_14004c0a0`:

```
0x14004C0A6: CALL [RAX+0x108]              ; vtable+0x108 — pre-move dispatch
0x14004C0AF: CALL uCoord__getPos
0x14004C0BE: CALL MtVector3__op_assign
0x14004CC9: CALL FUN_140011140              ; pre-input processing
0x14004CD2: CALL cChrInput__initInputBuffer ; clear +0x10, +0x14, +0x28(128B), +0x560, +0x568
0x14004CD7: CALL cChrInput__processFrameInput  ; ← OUR HOOK (overrides +0x10,+0x14,+0x560,+0x568,+0x56C)
0x14004CDE: CALL FUN_14002E5A0              ; inputMaskPostProcess — populates dirRing, edges, timers
0x14004CE3: CALL FUN_14002B910 @ cChrInput+0x788  ; dirRing writer?
0x14004CEF: CALL FUN_14002B910 @ cChrInput+0x570  ; dirRing writer?
0x14004CF9: CALL FUN_14003A440              ; gate function
    ; if gate returns non-zero:
    0x14004C0EB: CALL FUN_14002CF10
    0x14004C0F7: CALL FUN_14002CF90
    0x14004C0FC: CALL FUN_140039B70
    0x14004C103: CALL FUN_140039FB0
0x14004C10F: TEST [RSI+0x14F0], 0x4400
0x14004C119: CALL vtable+0x140               ; ACTUAL STATE MACHINE DISPATCH
                                              ; (reads mProcessed = our override value)
```

Then back in `uChris__tick`:
```
uCharacter__move_base();                    ; returns from above
if (!(chr+0x1514 & 0x10))
    uChris__stateUpdate_9A70();             ; per-character state update
```

**Key insight**: The state machine (vtable+0x140 and uChris__stateUpdate_9A70) runs **AFTER** our hook. Our overrides ARE visible to it. BUT `inputMaskPostProcess` (FUN_14002E5A0) runs **between** our hook and the state machine, and it reads `mOldHeld (+0x10)` to compute edges.

---

## 8. hkprocessFrameInput

**Historical source excerpt.** Current `hkprocessFrameInput` has different
absolute-frame scheduling and fallback behavior. The missing `+0x10` write is an
observation about this implementation, while its classification as the root
cause is superseded by `dash_resim_root_cause.md`.

```cpp
static void __fastcall hkprocessFrameInput(uint64_t cChrInput)
{
    // 1. Run native translation first
    oprocessFrameInput(cChrInput);  // ← This writes +0x10, +0x14, +0x560, +0x568, +0x56C

    uint64_t character = *(uint64_t*)(cChrInput + 0x8);
    if (!character) return;

    uint32_t teamIdx = fnGetTeamIdx(character);
    int32_t lookupFrame = g_isResimulating ? g_resimulateCurrentFrame : GetGameFrameCount();
    if (lookupFrame < 0) return;
    uint32_t idx = (uint32_t)lookupFrame % RING_SIZE;

    if (Netplay_IsActive()) {
        if (g_isResimulating) {
            // === RESIM PATH — override from snapshot ===
            // Reads snapshot-stored input and writes to ALL four override fields:
            uint32_t resolved = isLocal ? snapLocal : snapRemote;
            *(uint32_t*)(cChrInput + 0x14)  = resolved; // mHeld ✓
            *(uint32_t*)(cChrInput + 0x560) = resolved; // mRemapped ✓
            *(uint32_t*)(cChrInput + 0x56C) = resolved; // mProcessed ✓
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask — clear ✓
            // *** NOTE: +0x10 (mOldHeld) is NOT overridden! ***
            // Native oprocessFrameInput set it to the native sPad value.
        } else {
            // === LIVE PATH ===
            // Host P1 / Client P2 (local):
            g_localPending |= *(uint32_t*)(cChrInput + 0x14);  // accumulate
            uint32_t delayed = g_oldInputs[MAX_HISTORY - g_delayFrames];
            *(uint32_t*)(cChrInput + 0x14)  = delayed;  // mHeld ✓
            *(uint32_t*)(cChrInput + 0x560) = delayed;  // mRemapped ✓
            *(uint32_t*)(cChrInput + 0x56C) = delayed;  // mProcessed ✓
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask ✓
            // *** +0x10 NOT overridden ***

            // Host P2 / Client P1 (remote):
            uint32_t remote = g_usedRemoteInput[idx];
            *(uint32_t*)(cChrInput + 0x14)  = remote;
            *(uint32_t*)(cChrInput + 0x560) = remote;
            *(uint32_t*)(cChrInput + 0x56C) = remote;
            *(uint32_t*)(cChrInput + 0x568) = 0;
            // *** +0x10 NOT overridden ***
        }
    } else {
        // === OFFLINE / F7 TEST PATH ===
        if (g_isResimulating) {
            *(uint32_t*)(cChrInput + 0x14)  = inputVal; // mHeld ✓
            *(uint32_t*)(cChrInput + 0x560) = inputVal; // mRemapped ✓
            *(uint32_t*)(cChrInput + 0x56C) = inputVal; // mProcessed ✓
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask ✓
            // *** +0x10 NOT overridden ***
        } else {
            // Live: just capture
            inputVal = *(uint32_t*)(cChrInput + 0x14);
            g_recordedGameInput[teamIdx & 1][idx] = inputVal;
        }
    }
}
```

**The mOldHeld Bug**: Across all **four** override branches (resim-netplay, live-local, live-remote, resim-offline), `+0x10` (mOldHeld) is NEVER explicitly overridden. The native `oprocessFrameInput` sets it to the **native sPad value** at line 62 of the decompilation. After our override, `mHeld (+0x14)` has the replayed value but `mOldHeld (+0x10)` still has the native value. When `inputMaskPostProcess` runs next, it XORs `mOldHeld` vs `mButtons` to compute rising/falling edges — producing wrong results.

---

## 9. hkGetsCurrentInput_withSOCD

**Historical source excerpt.** Current client-local input reads the physical pad
slot directly and current timeline resolution is not fully represented here.

```cpp
static uint64_t __fastcall hkGetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex)
{
    bool is_active = Netplay_IsActive();

    // --- RAW INPUT DETERMINATION ---
    uint64_t raw;
    if (is_active && !g_isResimulating)
    {
        // === LIVE NETPLAY ===
        if (Netplay_IsHost()) {
            if (playerIndex == 1)
                raw = 0;                    // Host: P2 remote = zero
            else
                raw = oGetsCurrentInput_withSOCD(pad, playerIndex);  // Host: P1 from pad
        } else {
            if (playerIndex == 0)
                raw = 0;                    // Client: P1 remote = zero
            else if (playerIndex == 1) {
                // Client P2: keyboard + gamepad
                uint32_t padInput = (uint32_t)oGetsCurrentInput_withSOCD(pad, 0);
                uint32_t kbInput  = ControllerDecomp::keyboardToGameMask(pad);
                raw = ApplySOCD(padInput | kbInput);
            } else
                raw = oGetsCurrentInput_withSOCD(pad, playerIndex);
        }
    } else {
        // === RESIM OR OFFLINE ===
        // During resim, GetsCurrentInput returns TRUE pad value
        // (but hkprocessFrameInput overrides it before game logic reads it)
        if (playerIndex == 1)
            raw = oGetsCurrentInput_withSOCD(pad, 1);
        else
            raw = oGetsCurrentInput_withSOCD(pad, playerIndex);
    }

    // --- CAPTURE raw input for resim replay ---
    if (!g_isResimulating) {
        int32_t frame = GetGameFrameCount();
        if (frame >= 0 && playerIndex < 2)
            g_rawLocalInput[playerIndex][(uint32_t)frame % RING_SIZE] = (uint32_t)raw;
    }

    // --- RESIM: return recorded historical input ---
    if (g_isResimulating) {
        uint32_t idx = (uint32_t)g_resimulateCurrentFrame % RING_SIZE;
        if (is_active) {
            bool isLocalPlayer = Netplay_IsHost() ? (playerIndex == 0) : (playerIndex == 1);
            if (isLocalPlayer)
                return g_recordedLocalInput[idx];
            else
                return g_usedRemoteInput[idx];
        } else {
            if (playerIndex < 2)
                return g_rawLocalInput[playerIndex][idx];
            else
                return raw;
        }
    }

    return raw;
}
```

**Asymmetry**: During live netplay, GetsCurrentInput returns **0** for the remote player. During resim, it returns `g_usedRemoteInput[idx]` = the corrected remote input. The `processFrameInput` native then uses this for `buttonRemap`, `swapLR`, and **mMask accumulation** — even though `hkprocessFrameInput` ultimately overrides mHeld. The **mMask accumulates extra bits** during resim for the remote player that it didn't during live.

---

## 10. BuildRegions

**Historical source excerpt.** This flat-region inventory is not current. In
particular, current source does not flat-copy projectile bodies or restore the
complete `sMain` body.

```cpp
static int BuildRegions(Region* regions, int maxRegions)
{
    int r = 0;
    auto add = [&](void* ptr, size_t sz) { ... };

    // ---- 1. sReplay: frame counter ----
    add((void*)(sReplay + 0x54), 4);

    // ---- 1a. Global XorShift RNG ----
    add((void*)_addr(0x140d765d8), 16);

    // ---- 2. sShot manager ----
    add((void*)sShot, 0x1E0);

    // ---- 2a. Slot active flags ----
    add((void*)(sGameConfig + 0x4A89C), 10);

    // ---- 2b. uShot projectile objects (heap traversal) ----
    // Walk each sentinel's circular list, DTI-based sizing

    // ---- 3. sHitSolver ----
    add((void*)sHitSolver, 0x34080);

    // ---- 4. aNextBattle (state machine) ----
    add((void*)aNextBattle, 0x20000);

    // ---- 5. sGameUI — SKIPPED (internal ptrs) ----

    // ---- 6. sMain (physics, timing) ----
    add((void*)sMain, 0x40080);

    // ---- 7. sCamera ----
    add((void*)sCamera, 0xD80);

    // ---- 8. sGameEffect — SKIPPED (pool allocator corruption) ----

    // ---- 10. sAction (fighting phase state machine) ----
    add((void*)sAction, 0x300);

    // ---- 11-16. Per-character uCharacter + cChrTeam + cChrInput ----
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (sCharacter) {
        uint64_t team0 = sCharacter + TEAM0_OFFSET;   // 0x40
        uint64_t team1 = sCharacter + TEAM1_OFFSET;   // 0x300
        add((void*)team0, TEAM_SIZE);                  // 0x2C0
        add((void*)team1, TEAM_SIZE);

        for (int ti = 0; ti < 2; ++ti) {
            uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
            uint64_t node = team + TEAM_FIRST_NODE_OFFSET;  // +0x08 = first linked-list node

            while (node && r < maxRegions) {
                uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);  // +0x08 = uCharacter ptr
                if (character) {
                    uint32_t charSize = DecodeDtiSize(GetObjectDti(character));
                    uint32_t finalSize = charSize;
                    if (finalSize == 0 || finalSize > MAX_CHAR_SIZE)
                        finalSize = 0x20000;

                    add((void*)character, finalSize);  // FULL uCharacter via DTI

                    // cChrInput is at character + 0x21C8, size 0x5A0
                    uint64_t cChrInput = character + 0x21C8;
                    add((void*)cChrInput, 0x5A0);       // REDUNDANT but covers dirRing
                }
                node = *(uint64_t*)(node + NODE_NEXT_OFFSET);  // +0x10 = next node
            }
        }
    }

    // ---- 19-22. sUnit effect categories — SKIPPED ----

    return r;
}
```

---

## 11. Rollback_SaveSnapshot

```cpp
void Rollback_SaveSnapshot(int32_t frame)
{
    Region regions[MAX_REGIONS];
    int count = BuildRegions(regions, MAX_REGIONS);
    if (count == 0) return;

    size_t total = 0;
    for (int i = 0; i < count; ++i) total += regions[i].size;

    int slotIdx = frame % ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    // Allocate or grow buffer
    if (slot.capacity < total) {
        delete[] slot.data;
        slot.data = new (std::nothrow) uint8_t[total];
        if (!slot.data) { ... return; }
        slot.capacity = total;
    }

    // Copy data AND save per-region metadata
    size_t offset = 0;
    slot.regionCount = count;
    slot.frame = frame;
    for (int i = 0; i < count; ++i) {
        slot.regions[i].ptr  = regions[i].ptr;   // SAVE address
        slot.regions[i].size = regions[i].size;   // SAVE size
        std::memcpy(slot.data + offset, regions[i].ptr, regions[i].size);
        offset += regions[i].size;
    }
    slot.size = offset;

    // Save input metadata
    slot.localInput = Controller_GetCurrentFrameInput();
    slot.remoteInput = Controller_GetReceivedRemoteInput(frame);
}
```

---

## 12. Rollback_RestoreSnapshot

**Historical source excerpt.** Current restore validates owner slots and object
identity and dispatches dedicated restore/reconstruction policies. The broad
`memcpy` loop below is not an accurate description of current restore safety.

```cpp
void Rollback_RestoreSnapshot(int32_t frame)
{
    int slotIdx = frame % ROLLBACK_WINDOW;
    SnapshotSlot& slot = g_slots[slotIdx];

    if (!slot.data || slot.size == 0 || slot.regionCount == 0 || slot.frame != frame) {
        HookLog("Rollback: no snapshot for frame %d\n", frame);
        return;
    }

    // Restore using SAVED region metadata (addresses + sizes)
    size_t offset = 0;
    for (int i = 0; i < slot.regionCount; ++i) {
        size_t copySize = slot.regions[i].size;
        if (offset + copySize > slot.size)
            copySize = slot.size - offset;
        if (copySize > 0) {
            std::memcpy(slot.regions[i].ptr, slot.data + offset, copySize);
        }
        offset += slot.regions[i].size;
    }

    // Cleanup stale pointers
    Rollback_ClearStalePointers();
}
```

---

## 13. Rollback_ReSimulate

```cpp
void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame)
{
    if (startFrame >= endFrame) return;
    if (g_isResimulating) { g_resimReentranceDepth++; return; }
    g_isResimulating = true;

    // Patch out Sleep/Present/Draw calls
    Rollback_PatchSleep(true);
    Rollback_PatchPresent(true);
    Rollback_PatchSwapchain(true);
    Rollback_PatchDrawCalls(true);

    // Save input history for restoration after re-sim
    Controller_SaveInputHistory();

    // 1. Rewind to start frame
    Rollback_RestoreSnapshot(startFrame);

    // 2. Step forward frame by frame
    for (int32_t f = startFrame; f < endFrame; ++f) {
        g_resimulateCurrentFrame = f;

        if (Netplay_IsActive())
            Controller_ResolveRemoteInput(f);

        Controller_SilentFrameStart();

        // Inject inputs from snapshot
        Controller_InjectLocalInput(f, g_slots[si].localInput);
        if (!Netplay_IsActive())
            Controller_InjectRemoteInput(f, g_slots[si].remoteInput);

        // Capture pre-state
        Rollback_CaptureFrameDiagSnapshot(f, true, false);

        // Execute ONE game tick
        oMvc3Main_move(g_sMvc3Main);           // ← THIS runs the full game tick

        Controller_SilentFrameEnd();

        // Capture post-state
        Rollback_CaptureFrameDiagSnapshot(f, true, true);
    }

    // Restore input history
    Controller_RestoreInputHistory();

    // Resync timing
    Rollback_ResyncMainTiming(sMain);

    g_isResimulating = false;

    // Restore patches
    Rollback_PatchSleep(false);
    Rollback_PatchPresent(false);
    Rollback_PatchSwapchain(false);
    Rollback_PatchDrawCalls(false);
}
```

---

## 14. Diag Capture

```cpp
// CaptureCharSnapshot — copies ALL relevant char state
void CaptureCharSnapshot(uint64_t uChar, uint32_t teamIdx, uint32_t charSlot,
                         CharDiagSnapshot& snap)
{
    uint64_t cci = uChar + 0x21C8;
    std::memcpy(snap.cchr, (void*)cci, CCHRINPUT_SIZE);  // captures +0x21C8 for 0x5A0 bytes
    snap.posX       = *(float*)(uChar + 0x50);
    snap.posY       = *(float*)(uChar + 0x54);
    snap.posZ       = *(float*)(uChar + 0x58);
    snap.actionId   = *(uint32_t*)(uChar + 0x1310);
    snap.flags14F0  = *(uint32_t*)(uChar + 0x14F0);
    snap.flags14F4  = *(uint32_t*)(uChar + 0x14F4);
    snap.flags14F8  = *(uint32_t*)(uChar + 0x14F8);
    snap.flags1504  = *(uint32_t*)(uChar + 0x1504);
    snap.flags1510  = *(uint32_t*)(uChar + 0x1510);
    snap.flags1514  = *(uint32_t*)(uChar + 0x1514);
    snap.flags1524  = *(uint32_t*)(uChar + 0x1524);
    for (int i = 0; i < XORSHIFT_SIZE; i++)
        snap.xorShift[i] = *(uint32_t*)(uChar + 0x2D80 + i * 4);  // per-char RNG
}

// DiagCaptureLiveRef — captures live reference per frame for comparison
void DiagCaptureLiveRef(uint64_t cChrInput, uint32_t teamIdx, int32_t liveFrame)
{
    uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
    uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
    auto& rec = g_liveRef[teamIdx][(uint32_t)liveFrame % 256];
    rec.mHeld       = *(uint32_t*)(cChrInput + 0x14);
    rec.mMask       = *(uint32_t*)(cChrInput + 0x568);
    rec.mProcessed  = *(uint32_t*)(cChrInput + 0x56C);
    rec.frameCnt    = frameCnt;
    rec.h590        = *(uint32_t*)(cChrInput + 0x590);
    rec.h594        = *(uint32_t*)(cChrInput + 0x594);
    rec.heldR0      = *(uint64_t*)(cChrInput + 0x28);
    rec.relHead     = *(uint32_t*)(cChrInput + 0x0A8);
    rec.relR0       = *(uint64_t*)(cChrInput + 0x0AC);
    rec.prsR0       = *(uint64_t*)(cChrInput + 0x130);
    rec.dir0        = *(uint32_t*)(cChrInput + 0x1B0);
    rec.dir4        = *(uint32_t*)(cChrInput + 0x1B4);
    rec.dirRing0    = *(uint64_t*)(cChrInput + 0x360);
    rec.dirRingSlot = *(uint64_t*)(cChrInput + 0x360 + (frameCnt & 0x3F) * 8);
    rec.actionId    = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
    rec.flags14F0   = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
    rec.posX        = uChar ? *(float*)(uChar + 0x50) : 0.0f;
    rec.posY        = uChar ? *(float*)(uChar + 0x54) : 0.0f;
    rec.posZ        = uChar ? *(float*)(uChar + 0x58) : 0.0f;
    for (int i = 0; i < 4; i++)
        rec.rng[i] = uChar ? *(uint32_t*)(uChar + 0x2D80 + i * 4) : 0;
}

// DiagLogPreOverride — logs state BEFORE hkprocessFrameInput overrides
void DiagLogPreOverride(uint64_t cChrInput, uint32_t teamIdx) { ... }

// DiagLogPostOverride — logs state AFTER override AND compares with live
void DiagLogPostOverride(uint64_t cChrInput, uint32_t teamIdx)
{
    // Logs mOld, mHeld, m18, m1C, mMask, mProcessed, cntr, h590, h594,
    // heldR0, relHead, relR0, prsR0, dir[0], dir[4], dirRing[0], dirRing[slot],
    // dirRing[1], dirRing[2], act, fl, pos
    // THEN compares with live reference and logs "ActDiv" on mismatch:
    if (actionId != lr.actionId) {
        HookLog("ActDiv frame=%d team=%d resim_act=0x%08X live_act=0x%08X "
                "mHeld_post=0x%08X mHeld_live=0x%08X fl=0x%08X\n",
                f, teamIdx, actionId, lr.actionId,
                *(uint32_t*)(cChrInput + 0x14), lr.mHeld, flags14F0);
    }
}
```

---

## 15. Character Snapshot Regions

```cpp
// ---- 11-16. Per-character uCharacter + 17-18. cChrTeam ----
uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
if (sCharacter) {
    uint64_t team0 = sCharacter + TEAM0_OFFSET;   // 0x40
    uint64_t team1 = sCharacter + TEAM1_OFFSET;   // 0x300
    add((void*)team0, TEAM_SIZE);                  // 0x2C0 bytes each
    add((void*)team1, TEAM_SIZE);
    // TEAM_SIZE = 0x2C0 covers offsets 0x40..0x300 and 0x300..0x5C0

    for (int ti = 0; ti < 2; ++ti) {
        uint64_t team = sCharacter + (ti == 0 ? 0x40 : 0x300);
        uint64_t node = team + 0x08;  // first linked-list node (cChrNode)

        while (node && r < maxRegions) {
            uint64_t character = *(uint64_t*)(node + 0x08);  // uCharacter*
            if (character) {
                // DTI-based sizing for full uCharacter snapshot
                uint64_t dtiPtr = GetObjectDti(character);
                uint32_t charSize = DecodeDtiSize(dtiPtr);
                uint32_t finalSize = charSize;
                if (finalSize == 0 || finalSize > MAX_CHAR_SIZE)
                    finalSize = 0x20000;

                add((void*)character, finalSize);  // ~0x20000 bytes per character

                // cChrInput at uChar + 0x21C8, size 0x5A0
                uint64_t cChrInput = character + 0x21C8;
                add((void*)cChrInput, 0x5A0);
                // 0x5A0 covers offsets 0x21C8..0x2768
                // cChrInput + 0x360 (dirRing) = uChar + 0x2528 = INSIDE 0x5A0 ✓
                // cChrInput + 0x590 (h590)   = uChar + 0x2758 = INSIDE 0x5A0 ✓
                // cChrInput + 0x594 (h594)   = uChar + 0x275C = INSIDE 0x5A0 ✓
            }
            node = *(uint64_t*)(node + 0x10);  // next node
        }
    }
}
```

---

## 16. cChrTeam Snapshot Verification

`TEAM0_OFFSET = 0x40`, `TEAM1_OFFSET = 0x300`, `TEAM_SIZE = 0x2C0`.

```
Team 0: sCharacter + 0x40 .. +0x40+0x2C0  = +0x40 .. +0x300
Team 1: sCharacter + 0x300 .. +0x300+0x2C0 = +0x300 .. +0x5C0
```

cChrTeam offset map (from doc/rollback.md):
| Offset | Field | In 0x2C0? |
|--------|-------|-----------|
| +0x00 | vtable | ✓ (first 8 bytes) |
| +0x08 | mpFirstNode (cChrNode*) | ✓ |
| +0x10 | mpNext (linked list) | ✓ |
| +0x48 | mpCurrentChr | ✓ |
| +0x4C..0x2C0 | member array, flags, etc. | ✓ |

**cChrTeam +0x48 (mpCurrentChr) IS covered by the 0x2C0 snapshot.** ✓

---

## 17. Root Cause Analysis

**Historical ranking, superseded.** These were hypotheses generated from the two
captures above. They are not a current defect list or a validated ordering of
causes.

### Root Cause #1: mOldHeld (+0x10) Not Overridden by Hook (HIGH)
The native `cChrInput__processFrameInput` writes `mOldHeld (+0x10) = mHeld` at the start of post-processing. Our `hkprocessFrameInput` then overrides `mHeld (+0x14)`, `mRemapped (+0x560)`, `mProcessed (+0x56C)`, and `mMask (+0x568)` — but NOT `mOldHeld (+0x10)`. When `cChrInput_inputMaskPostProcess` runs next, it XORs `mOldHeld (+0x10)` vs `mButtons (+0x1C)` to compute rising/falling edges — producing **wrong edge detection** for the replayed input. This cascades into wrong `dirRing` entries and wrong held/released/pressed ring data.

**Direct impact**: Dash input detection reads from `dirRing` via `getInputDirection`. If the dirRing was populated with wrong edges due to stale mOldHeld, the dash detector may not see a forward press → dash rejected.

### Root Cause #2: cChrTeam Slot Ordering Divergence (HIGH)
In Session B, team 1 has `mid=0` in slot 0 on host but `mid=2` in slot 0 on client. The linked-list order of characters within `cChrTeam` differs between host and client. Causes:
- Character death/tag/assist call-in may reorder the intrusive linked list
- The snapshot captures characters by linked-list traversal order; if the ordering differs at snapshot save time vs restore time, the character states get swapped
- This causes per-character RNG states to be consumed in different order → frame 9 RNG divergence

**Current correction:** snapshots store each region's captured address and do
not remap bodies by later traversal ordinal. Slot-order divergence remains a
diagnostic identity problem and may indicate an earlier gameplay divergence, but
the body-swap mechanism proposed here is not how current restore operates.

### Root Cause #3: sId Allocator Not Snapshotted (MEDIUM)
`sId` at sMvc3Main step #21 (vtable+0x30 on member at offset 0x40228) is NOT in the snapshot regions. sId is the unique ID allocator for projectiles, effects, and other gameplay objects. If a projectile/effect is created during re-sim that consumes a different ID than during live, subsequent allocations diverge. This affects:
- uShot projectile IDs → collision/despawn mismatches
- sGameEffect IDs → visual effect desync
- Any gameplay code that branches on object IDs

**Current correction:** `sMain+0x40228` is the slot containing the `sId*`, not
the allocator body. The target's size and ownership policy were not established
here, so the flat-copy fix proposed below is unsupported.

### Root Cause #4: sEffect RNG (+0x3C) Not Snapshotted (MEDIUM)
The global MtXorShift RNG (0x140d765d8, 16 bytes) IS in the snapshot. Per-character RNG (cChr+0x2D80) IS in the snapshot. But `sEffect` has its own RNG at `sGameEffect + 0x3C` — this is NOT in the snapshot. If any sEffect RNG is consumed during re-sim, the global RNG state diverges from live.

**Current correction:** current `BuildRegions` captures 16 bytes at
`sEffect+0x3C`. Shot reconstruction also preserves and restores that complete
state around native respawn work.

### Root Cause #5: GetsCurrentInput Asymmetry During Resim (MEDIUM)
During live netplay, GetsCurrentInput returns **0** for the remote player. During resim, it returns `g_usedRemoteInput[idx]` (the corrected remote input). The native `processFrameInput` then:
1. Calls `buttonRemap(g_usedRemoteInput)` → writes mHeld with non-zero value
2. Accumulates `mMask |= (mHeld & 0x7FFBF0)` — extra bits not present during live
3. hkprocessFrameInput overrides mHeld with snapshot value but **mMask retains extra bits**

This means after resim ends, the remote player's mMask has accumulated extra suppression bits that it wouldn't have had during normal live play. Future frames may see input incorrectly suppressed.

---

## 18. The mOldHeld Bug — Detailed Walkthrough

### During live play:
```
1. processFrameInput native:
   mOldHeld = mHeld_prev  (native value from previous frame)
   mHeld = GetsCurrentInput  (native sPad value)
   → mMask |= (mHeld & 0x7FFBF0)
   → mHeld &= ~mMask
   → output: mHeld, mOldHeld, mMask all consistent

2. hkprocessFrameInput override:
   mHeld = delayed    (overridden)
   mRemapped = delayed (overridden)
   mMask = 0          (overridden — correctly)
   mOldHeld = ???     (NOT overridden — still has the native value)
   WAIT — actually in live, the native mOldHeld was just set from the native
   mHeld before override, which is close to the delayed value. The discrepancy
   is small in live. BUT during RESIM:
```

### During resim:
```
1. processFrameInput native:
   mOldHeld = mHeld_prev  (from snapshot restore — correct)
   mHeld = GetsCurrentInput  (during resim: returns g_recordedLocalInput
          for local, g_usedRemoteInput for remote — DIFFERENT from live
          where GetsCurrentInput returned 0 for remote!)
   → mMask |= (mHeld & 0x7FFBF0) — EXTRA bits accumulate for remote

2. hkprocessFrameInput override:
   mHeld = snapLocal/snapRemote  (correct snapshot value)
   mMask = 0                     (correct)
   mOldHeld = ???                (NOT overridden — retains the value
                                  written by native, which used the
                                  GetsCurrentInput value—DIFFERENT from live)
```

### After resim ends, when inputMaskPostProcess runs:
```
XOR mOldHeld vs mButtons → edges computed from stale mOldHeld
dirRing written with wrong edges
```

### Fix: In ALL four override branches of hkprocessFrameInput, add:
```cpp
*(uint32_t*)(cChrInput + 0x10) = resolved;  // mOldHeld
```

---

## 19. Verified Snapshot Coverage

**Historical table.** The checks describe the old `BuildRegions` excerpt and do
not represent current coverage or restore policy. A row marked YES here may now
use a narrower ownership-aware restore, while a row marked MISSING may have been
added or reconstructed.

| Region | Address | Size | In Snapshot? |
|--------|---------|------|-------------|
| sReplay frame counter | sReplay+0x54 | 4 | ✓ YES |
| Global MtXorShift RNG | 0x140d765d8 | 16 | ✓ YES |
| sShot manager | [sShot] | 0x1E0 | ✓ YES |
| Slot active flags | sGameConfig+0x4A89C | 10 | ✓ YES |
| uShot projectiles | heap | DTI | ✓ YES |
| sHitSolver | [sHitSolver] | 0x34080 | ✓ YES |
| aNextBattle | [aNextBattle] | 0x20000 | ✓ YES |
| sMain (physics) | [sMain] | 0x40080 | ✓ YES |
| sCamera | [sCamera] | 0xD80 | ✓ YES |
| sAction | sMvc3Mgr+0x48 | 0x300 | ✓ YES |
| cChrTeam[0] | sChar+0x40 | 0x2C0 | ✓ YES |
| cChrTeam[1] | sChar+0x300 | 0x2C0 | ✓ YES |
| uCharacter (DTI) | [character] | DTI~0x20000 | ✓ YES |
| cChrInput (0x21C8..0x2768) | char+0x21C8 | 0x5A0 | ✓ YES (redundant) |
| dirRing (0x360..0x560) | cci+0x360 | 512 | ✓ covered by 0x5A0 |
| h590/h594 | cci+0x590/0x594 | 8 | ✓ covered by 0x5A0 |
| Per-char RNG (0x2D80) | char+0x2D80 | 16 | ✓ covered by DTI save |
| sId allocator | sMain+0x40228 | ? | ✗ MISSING |
| sEffect RNG | sGameEffect+0x3C | 4 | ✗ MISSING |
| sGameUI | [sGameUI] | — | ✗ SKIPPED (ptrs) |
| sGameEffect manager | [sGameEffect] | — | ✗ SKIPPED (ptrs) |
| sUnit effect nodes | heap | — | ✗ SKIPPED (corruption) |

---

## 20. Snapshot Gaps Identified

**Historical proposals.** Gap 2 is resolved in current source. Gap 1's proposed
size is unverified, and Gap 3's body-swap premise does not match address-based
restore. Do not implement these snippets without a new ownership analysis.

### Gap 1: sId Allocator (sMain member at +0x40228)
**Location**: `sMain + 0x40228` — `vtable+0x30` called on this member during sMvc3Main__move step #21.
**Fix**: Add to BuildRegions:
```cpp
{
    uint64_t sId = *(uint64_t*)(sMain + 0x40228);
    add((void*)sId, 0x100);  // or appropriate size
}
```

### Gap 2: sEffect RNG at +0x3C
**Location**: `sGameEffect + 0x3C` — separate from global RNG.
**Fix**: Add to BuildRegions:
```cpp
{
    uint64_t sGameEffect = *(uint64_t*)_addr(PTR_SGAMEEFFECT);
    add((void*)(sGameEffect + 0x3C), 4);
}
```

### Gap 3: cChrTeam Linked-List Order Not Enforced
The snapshot captures characters in linked-list traversal order. If the list ordering differs between save and restore (due to character death/tag), the snapshot bytes get applied to wrong addresses. **The linked-list structure itself is not snapshotted** — only the data each node points to.

---

## 21. Execution Order

### Within sMvc3Main::move (one game tick):
```
1.  sMain__move — QPC, timing, Sleep
2.  updateWindowFocus
3.  sNetPad__stepHelper — input poll (I/O thread)
4.  Async loader (vtable+0x30)
5.  [RENDER BLOCK — ~10 draw calls]
6.  sAction__subStepHelper → uCharacter__move_base per character
    ↓ (within each uCharacter__move_base):
       a. vtable+0x108 — pre-move dispatch
       b. coord update
       c. pre-input processing
       d. cChrInput__initInputBuffer — clears mOldHeld, mHeld, rings
       e. cChrInput__processFrameInput — OUR HOOK
       f. cChrInput_inputMaskPostProcess — dirRing, edges, timers
       g. FUN_14002B910 (×2) — dirRing writer?
       h. gate function
       i. vtable+0x140 — state machine dispatch
       j. (return to uChris__tick) uChris__stateUpdate_9A70
7.  sMvc3Vibration, sMvc3SyncPad, sMouse, sId, sKeyboard
8.  sAI
9.  [manager subsystem updates — sGameConfig, sGameUI, sShot, sEffect]
10. sCollision__move
11. sHitSolver
12. [physics]
13. [sCharacter update, sStage, sShot, sEffect vtable+0x30]
14. Reset hit nodes
15. [remaining: sReplay, sBattleSetting, etc.]
```

### Within Rollback_ReSimulate (re-simulation loop):
```
For each frame f in [startFrame, endFrame):
  1. Controller_ResolveRemoteInput(f) — re-resolve remote input
  2. Controller_SilentFrameStart() — reset accumulator
  3. Controller_InjectLocalInput(f, snap) — prep g_recordedGameInput
  4. Controller_InjectRemoteInput(f, snap) — prep g_usedRemoteInput
  5. Rollback_CaptureFrameDiagSnapshot(f, postOnly=false)
  6. oMvc3Main_move(g_sMvc3Main) — one FULL game tick
  7. Controller_SilentFrameEnd() — advance shift buffer
  8. Rollback_CaptureFrameDiagSnapshot(f, postOnly=true)
```

---

## 22. Resim vs Live Asymmetry

**Historical comparison.** This table describes the source excerpt above. It is
not sufficient evidence that the listed field asymmetry remains a current desync
cause, and its concluding "core" designation is superseded by later captures.

| Aspect | Live | Resim |
|--------|------|-------|
| GetsCurrentInput for remote | **0** (hkGetsCurrentInput line 714/722) | **g_usedRemoteInput[idx]** (line 774/776) |
| Native processFrameInput mHeld | 0 → buttonRemap(0) = 0 → mMask unchanged | g_usedRemoteInput → buttonRemap(non-zero) → **mMask accumulates** |
| hkprocessFrameInput override | remote = g_usedRemoteInput → mHeld overridden | remote = g_usedRemoteInput → mHeld overridden (same final value) |
| **Final mHeld** | g_usedRemoteInput ✓ | g_usedRemoteInput ✓ |
| **Final mMask** | **unchanged** (native saw 0) | **extra bits** from native buttonRemap |
| **Final mOldHeld** | **native sPad value** (NOT overridden) | **native GetsCurrentInput value** (DIFFERENT from live!) |

The `mMask` and `mOldHeld` fields diverge between live and resim even though `mHeld` ends up identical. This is the core asymmetry bug.
