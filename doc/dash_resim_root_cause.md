# DASH Loss During Rollback Resimulation — Root Cause Analysis

## Symptom

Two cases:
1. **Host dashes and client never dashes** — dash sequence lost in netplay pipeline
2. **Client dashes during live prediction, then rollback undoes the dash** — correct live capture, resim produces different game state

## Architecture: Our Netplay vs Game Native

Our mod **does NOT use the game's built-in netplay**. We inject our own rollback netcode. The game's `isGameModeOnline` / `isOnlineBattle` flags are NOT set. This means:

- `sNetPad__readBufferedInputForTeam` — **NEVER CALLED** (gated by `isGameModeOnline` check in native `processFrameInput`)
- `NetplayInputPreprocess` (FUN_14024cfa0) at sBattleSetting+0x5E4 — gated by `sBattleSetting__isNotRecordingAndNetActive` which checks `*(int*)(param_1+0x5E0) != 0` (counter). Counter is 0 in non-online mode → **not called**.
- Native processFrameInput follows the **offline/non-recording path**: GetsCurrentInput → buttonRemap → side-swap → mRemapped/mHeld → mProcessed

Without sNetPad or NetplayInputPreprocess firing, the native processFrameInput is a **pure function of GetsCurrentInput** (which our hook controls).

---

## Ghidra Decompilation: `cChrInput__processFrameInput` (0x14002DD60)

```
/* R2-INPUT chokepoint: per-character input resolver. Called from uCharacter__move_base. Reads from
   sPad singleton (local) OR sReplay (playback/record) OR sNetPad (online). Writes to cChrInput+16
   (mHeld/filtered), +20 (mButtons/raw), +1376 (mRemapped), +1388 (mProcessed). This is the single
   read-point before game logic. */

void cChrInput__processFrameInput(longlong param_1)
{
  longlong *plVar1;
  char cVar2;
  int iVar3;
  uint uVar4;
  int iVar5;
  undefined4 uVar6;
  undefined4 uVar7;
  uint uVar8;
  undefined8 uVar9;
  longlong lVar10;
  undefined8 uVar11;
  undefined8 uVar12;
  qword local_res8;
  qword local_res10;
  qword local_res18;
  qword local_28;
  qword local_20;
  qword local_18;
  
  lVar10 = *(longlong *)(param_1 + 8);    // RCX = param_1 (cChrInput*)
  if (lVar10 == 0) {                       // this->cChr = *(this+8) (uCharacter*)
    return;                                // if null cChr, bail
  }
  iVar3 = FUN_140021f50(lVar10);           // some character state check
  if (((iVar3 < 0) && ((*(uint *)(lVar10 + 0x14f0) & 0x1000) == 0)) &&
     (uVar4 = cChr__getCombinedFlags_29E0(lVar10), (uVar4 >> 0xc & 1) == 0)) { return; }
  uVar9 = sAction__getInstance();
  iVar3 = Concurrency_details_SchedulerProxy_GetNumBorrowedCores_void_(uVar9);
  if (iVar3 == 1) { return; }              // bail if single core/thread?

  // ---- Save old mHeld; clear mHeld and mProcessed ----
  *(undefined4 *)(param_1 + 0x10) = *(undefined4 *)(param_1 + 0x14);  // mOldHeld = mHeld
  *(undefined4 *)(param_1 + 0x14) = 0;                                  // mHeld = 0
  *(undefined4 *)(param_1 + 0x56c) = 0;                                  // mProcessed = 0

  lVar10 = FUN_140004520(*(undefined8 *)(param_1 + 8));                 // get character action obj
  if ((lVar10 != 0) && (cVar2 = MtObject__isAliveState(lVar10), cVar2 == '\0')) { lVar10 = 0; }

  cVar2 = cChrInput__isGameplayActive(param_1);
  if ((cVar2 != '\0') && (lVar10 != 0)) {
    // ---- Dr. Strange / assist state handling ----
    iVar3 = uDrStrangeSh__getter_8270(lVar10);
    iVar5 = uDrStrangeSh__getter_8270(lVar10);
    cVar2 = FUN_14011e910(lVar10);
    if ((cVar2 == '\0') && (iVar5 != 3 && iVar3 != 0)) {
      uVar6 = sAction__getGameFlag(lVar10);
      *(undefined4 *)(param_1 + 0x14) = uVar6;
      uVar6 = cChrInput__swapLRIfFaceLeft(param_1,uVar6);
      *(undefined4 *)(param_1 + 0x560) = uVar6;
      uVar6 = MtObject__getField_11684(*(undefined8 *)(param_1 + 8));
      uVar9 = sReplay__getInstance();
      cVar2 = sReplay__isRecording(uVar9,uVar6);
      if (cVar2 != '\0') {
        uVar6 = MtObject__getField_11684(*(undefined8 *)(param_1 + 8));
        uVar9 = sReplay__getInstance();
        uVar6 = sReplay__readInput(uVar9,uVar6,0,0xffffffff);
        *(undefined4 *)(param_1 + 0x14) = uVar6;
        uVar6 = cChrInput__swapLRIfFaceLeft(param_1,uVar6);
        *(undefined4 *)(param_1 + 0x560) = uVar6;
      }
      *(undefined4 *)(param_1 + 0x56c) = *(undefined4 *)(param_1 + 0x560);
      return;
    }
  }

  // ---- Main processing (starts at ~0x14002DEB5) ----
  plVar1 = *(longlong **)(param_1 + 8);
  uVar4 = MtObject__getField_11684(plVar1);
  uVar9 = sCharacter__getInstance();
  uVar9 = sCharacter__getTeam(uVar9,uVar4 ^ 1);   // get OPPOSING team

  if ((*(uint *)(plVar1 + 0x29e) & 0x1000) != 0) {
    (**(code **)(*plVar1 + 0x328))();                // vtable call (team partner handling?)
    goto LAB_14002e27a;                               // → skip to post-processing
  }

  uVar6 = FUN_140021f50(plVar1);
  uVar4 = cChr__getCombinedFlags_29E0(plVar1);
  if ((uVar4 >> 0xc & 1) != 0) {
    uVar11 = uDrStrangeSh__getter_29D0(*(undefined8 *)(param_1 + 8));
    uVar6 = MtObject__getField_24(uVar11);
  }

  uVar11 = sBattleSetting__getInstance();
  cVar2 = sBattleSetting__isOnlineBattle(uVar11);    // ← ONLINE BATTLE CHECK #1
  if (cVar2 == '\0') {
    uVar11 = sBattleSetting__getInstance();
    cVar2 = sBattleSetting__isTrainingMode(uVar11);   // ← TRAINING MODE CHECK
    if (cVar2 != '\0') {
      uVar11 = sReplay__getInstance();
      cVar2 = sReplay__isPlaying(uVar11,0xffffffff);
      if (cVar2 != '\0') {
        // mission/training replay reading path
        uVar12 = sReplay__getInstance();
        uVar11 = *(undefined8 *)(param_1 + 8);
        iVar3 = cMissionInterpreter__getCommandKind(uVar12);
        iVar5 = MtObject__getField_11684(uVar11);
        if (iVar5 == iVar3) {
          iVar3 = cAnmChrInterp__getSelectedChar(uVar12);
          iVar5 = MtObject__getField_11684(uVar11);
          if (iVar5 != iVar3) {
            uVar6 = Concurrency_details_SchedulerProxy_GetNumExternalThreads_void_(uVar9);
            goto LAB_14002dfe0;
          }
        }
        iVar3 = cMissionInterpreter__getCommandKind(uVar12);
        iVar5 = cAnmChrInterp__getSelectedChar(uVar12);
        if (iVar3 != iVar5) { return; }
        if ((lVar10 != 0) && (iVar3 = uDrStrangeSh__getter_8270(lVar10), iVar3 == 3)) { return; }
        goto LAB_14002dfe0;
      }
    }

    uVar7 = MtObject__getField_11684(*(undefined8 *)(param_1 + 8));
    uVar11 = sReplay__getInstance();
    cVar2 = sReplay__isRecording(uVar11,uVar7);       // ← RECORDING CHECK
    if (cVar2 == '\0') {                                // NOT recording
      uVar11 = sCharacter__getTeamFromCharSlot(*(undefined8 *)(param_1 + 8));
      uVar4 = cChrInput__getFlags_224(uVar11);
      if ((uVar4 >> 0xc & 1) != 0) { return; }
      uVar4 = cChrInput__getFlags_224(uVar9);
      if ((uVar4 >> 0xc & 1) != 0) {
        uVar6 = Concurrency_details_SchedulerProxy_GetNumExternalThreads_void_(uVar9);
      }
    }
  }

LAB_14002dfe0:
  cVar2 = cChrInput__isGameplayActive(param_1);
  if (cVar2 == '\0') { return; }

  // ---- GET CURRENT INPUT ----
  uVar11 = sMvc3Manager__getInstance();
  uVar7 = sMvc3Manager__getPlayerCharIdx(uVar11,uVar6);  // convert teamSlot→playerIdx
  uVar11 = sPad__getInstance();
  uVar7 = sPad__getPlayerInput_withSOCDClean(uVar11,uVar7);  // ← GetsCurrentInput_withSOCD

  if ((lVar10 == 0) || (iVar3 = uDrStrangeSh__getter_8270(lVar10), iVar3 != 3)) {
    // Not Strange or not in 3-state → check if pad assigned
    uVar11 = sMvc3Manager__getInstance();
    cVar2 = sMvc3Manager__isPadAssigned(uVar11,uVar6);
    if (cVar2 == '\0') { uVar7 = 0; }     // no pad → 0 input
  }
  else {
    // Strange in 3-state: may zero input based on team/training flags
    uVar4 = cChrInput__getFlags_224(uVar9);
    uVar11 = sBattleSetting__getInstance();
    cVar2 = sBattleSetting__isTrainingMode(uVar11);
    if (cVar2 != '\0') {
      uVar11 = sReplay__getInstance();
      cVar2 = sReplay__isPlaying(uVar11,0xffffffff);
      if (cVar2 != '\0') goto LAB_14002e12a;
    }
    if ((uVar4 >> 0xc & 1) == 0) { uVar7 = 0; }
  }

LAB_14002e12a:
  // ---- BUTTON REMAP + mHeld ----
  uVar11 = sCharacter__getTeamFromCharSlot(*(undefined8 *)(param_1 + 8));
  uVar4 = MtObject__getField_88(uVar11);               // team face-left flag
  uVar4 = cChrInput__buttonRemap(uVar6,uVar7,(byte)(uVar4 >> 0xc) & 1);
  *(uint *)(param_1 + 0x14) = uVar4;                    // mHeld = remapped input

  uVar8 = cChrInput__getFlags_224(uVar9);              // opponent team flags
  if ((uVar8 >> 0xc & 1) != 0) {
    *(uint *)(param_1 + 0x14) = uVar4 & 0xfff07fff;    // mHeld &= mask (ghost/CPU fix)
  }

  // ---- REPLAY RECORDING CHECK ----
  uVar7 = MtObject__getField_11684(*(undefined8 *)(param_1 + 8));
  uVar9 = sReplay__getInstance();
  cVar2 = sReplay__isRecording(uVar9,uVar7);

  if (cVar2 == '\0') {
    // ========== NOT RECORDING PATH ==========
    uVar9 = sBattleSetting__getInstance();
    cVar2 = sBattleSetting__isGameModeOnlineRanked(uVar9);
    if (cVar2 == '\0') {
      // ----- Not online ranked -----
      uVar9 = sBattleSetting__getInstance();
      cVar2 = isGameModeOnline(uVar9);
      if (((cVar2 != '\0') && (          // ← isGameModeOnline check
            lVar10 = sAction__getGlobalSlot_140D523B0(), lVar10 != 0)) &&
         (cVar2 = sCharacter__getField3344(lVar10), cVar2 != '\0')) {
        // ONLINE MODE: sNetPad path
        uVar6 = sNetPad__readBufferedInputForTeam(lVar10,uVar6);
        *(undefined4 *)(param_1 + 0x14) = uVar6;       // mHeld = sNetPad input
      }
LAB_14002e27a:   // ← Common post-processing convergence point
      // ... (see annotated disassembly below)
    }
    // Online ranked: zero everything
    *(undefined4 *)(param_1 + 0x14) = 0;
    *(undefined4 *)(param_1 + 0x560) = 0;
    *(undefined4 *)(param_1 + 0x56c) = 0;
  }
  else {
    // ========== RECORDING PATH ==========
    // sReplay__readInput → override mHeld, set mRemapped and mProcessed
    ...
  }
  return;
}
```

**IMPORTANT**: The Ghidra decompiler mislabels the fall-through to `LAB_14002e27a` (the common post-processing) as `_fgetchar()` with a "WARNING: Subroutine does not return" — this is a decompilation artifact for tail calls. The actual disassembly shows normal sequential execution.

---

## Annotated Disassembly: Post-Processing at `0x14002E27A` (offline path convergence)

```
; ========== REACHED FROM: ==========
; 1. Early team-partner (vtable) path: goto 0x14002e27a
; 2. NOT recording + NOT online ranked + !(isGameModeOnline && valid)
; 3. NOT recording + isGameModeOnline → sNetPad override → fall-through

0x14002E27A  MOV  RCX,[RSI+0x8]          ; cChr object
0x14002E27E  XOR  BL,BL                   ; BL = 0
0x14002E280  CALL FUN_140004AC0           ; get char index
0x14002E285  TEST EAX,EAX                 ; EAX == 0? (first char?)
0x14002E287  SETZ BPL                     ; BPL = (EAX==0)
0x14002E28B  CALL sMain__getInstance
0x14002E290  XOR  EDX,EDX                 ; EDX = 0
0x14002E292  MOV  RCX,RAX                 ; RCX = sMain
0x14002E295  CALL FUN_140004530           ; sMain__getFrameState
0x14002E29A  TEST RAX,RAX
0x14002E29D  JZ   0x14002E2B6             ; if null → skip
0x14002E29F  LEA  RCX,[RAX+0x1560]        ; sMain field at +0x1560
0x14002E2A6  CALL FUN_140024160           ; read bitfield
0x14002E2AB  MOVZX EBX,BL
0x14002E2AE  BT   EAX,0x10                ; test bit 16 in sMain field
0x14002E2B2  CMOVC EBX,R13D              ; if bit16=1: BL=1 (need side swap)

0x14002E2B6  MOV  RCX,[RSI+0x8]          ; cChr object
0x14002E2BA  ADD  RCX,0x1560             ; cChr + 0x1560
0x14002E2C1  CALL FUN_140024160           ; read bitfield
0x14002E2C6  TEST AL,0x40                 ; test bit 6 in cChr+0x1560
0x14002E2C8  JNZ  0x14002E2CE            ; if set → do side swap
0x14002E2CA  TEST BL,BL                   ; if BL set (from sMain) → do side swap
0x14002E2CC  JZ   0x14002E2E8            ; neither → skip

; ---- Side swap (L↔R, U↔D) based on character orientation ----
0x14002E2CE  MOV  EAX,[RSI+0x14]         ; EAX = mHeld
0x14002E2D1  TEST AL,0x3                  ; L/R bits? (bit 0 = ←, bit 1 = →)
0x14002E2D3  JZ   0x14002E2DB            ; neither set → skip L/R swap
0x14002E2D5  XOR  EAX,0x3                ; swap bits 0↔1 (←↔→)
0x14002E2D8  MOV  [RSI+0x14],EAX         ; mHeld = swapped L/R
0x14002E2DB  MOV  EAX,[RSI+0x14]
0x14002E2DE  TEST AL,0xC                  ; U/D bits? (bit 2 = ↓, bit 3 = ↑)
0x14002E2E0  JZ   0x14002E2E8            ; neither set → skip U/D swap
0x14002E2E2  XOR  EAX,0xC                ; swap bits 2↔3 (↓↔↑)
0x14002E2E5  MOV  [RSI+0x14],EAX         ; mHeld = swapped U/D

; ---- Set mRemapped, then swapLRIfFaceLeft ----
0x14002E2E8  MOV  EDX,[RSI+0x14]         ; EDX = mHeld (after side swap)
0x14002E2EB  MOV  RCX,RSI                ; RCX = this
0x14002E2EE  MOV  [RSI+0x560],EDX         ; mRemapped = mHeld (SAVED PRE-swapLR)
0x14002E2F4  CALL cChrInput__swapLRIfFaceLeft  ; EAX = swapLRIfFaceLeft(this, EDX)
0x14002E2F9  MOV  [RSI+0x14],EAX         ; mHeld = swapLRIfFaceLeft(mRemapped)

; ---- NetplayInputPreprocess check ----
0x14002E2FC  CALL sBattleSetting__getInstance
0x14002E301  MOV  RCX,RAX
0x14002E304  CALL sBattleSetting__isNotRecordingAndNetActive    ; (0x14024d500)
0x14002E309  TEST AL,AL
0x14002E30B  JZ   0x14002E369             ; → skip (our case: counter=0)

; [NetplayInputPreprocess block — NOT TAKEN in our setup]
; Would read mRemapped, push to ring +0x5E4, then modify mRemapped
; ...

; ---- Character facing adjustments ----
0x14002E369  MOV  RAX,[RSI+0x8]          ; cChr object
0x14002E36D  TEST dword [RAX+0x14f0],0x1000  ; certain char flag?
0x14002E377  JNZ  0x14002E39D            ; skip if set
0x14002E379  MOV  RCX,R12                 ; R12 = opposing team's cChrInput
0x14002E37C  CALL cChrInput__getFlags_224
0x14002E381  BT   EAX,0xD                 ; test bit 13
0x14002E385  JNC  0x14002E38B
0x14002E387  OR   dword [RSI+0x14],0x1   ; mHeld |= bit 0 (←) if team flag
0x14002E38B  MOV  RCX,R12
0x14002E38E  CALL cChrInput__getFlags_224
0x14002E393  BT   EAX,0xE                 ; test bit 14
0x14002E397  JNC  0x14002E39D
0x14002E399  OR   dword [RSI+0x14],0x4   ; mHeld |= bit 2 (↓) if team flag

; ---- mMask handling (SUPPRESSED INPUTS) ----
0x14002E39D  CALL sCharacter__getInstance
0x14002E3A2  MOV  RCX,RAX
0x14002E3A5  MOV  RBX,RAX
0x14002E3A8  CALL FUN_140022280           ; check if certain char state
0x14002E3AD  JNZ  0x14002E3CE             ; → skip mMask update
0x14002E3B1  MOV  RCX,RBX
0x14002E3B4  CALL FUN_140006B70           ; check if char is target
0x14002E3B9  JZ   0x14002E3CE             ; → skip mMask update

; mMask = OR of mHeld bits (suppressed input tracking)
0x14002E3BD  MOV  EAX,[RSI+0x14]         ; EAX = mHeld
0x14002E3C0  MOV  [RSI+0x10],EAX         ; mOldHeld = mHeld
0x14002E3C3  AND  EAX,0x7FFBF0           ; mask valid bits only
0x14002E3C8  OR   [RSI+0x568],EAX        ; mMask |= (mHeld & 0x7FFBF0)

; Apply mMask to clear SUPPRESSED bits
0x14002E3CE  MOV  EAX,[RSI+0x568]        ; EAX = mMask
0x14002E3D4  NOT  EAX                     ; EAX = ~mMask
0x14002E3D6  AND  [RSI+0x10],EAX         ; mOldHeld &= ~mMask
0x14002E3D9  AND  [RSI+0x14],EAX         ; mHeld &= ~mMask

; ---- Final: mProcessed = mRemapped ----
0x14002E3DC  MOV  EAX,[RSI+0x560]        ; EAX = mRemapped
0x14002E3E2  MOV  [RSI+0x56C],EAX        ; mProcessed = mRemapped
```

---

## Key Supporting Functions

### `sPad__getPlayerInput_withSOCDClean` (0x1402B3700)

```
/* Returns PadSlot[slotIdx].mCurrentButtons with SOCD cleaning. 
   Called by cChrInput__processFrameInput during local (non-online, non-replay) play. */

ulonglong sPad__getPlayerInput_withSOCDClean(longlong param_1, uint param_2)
{
  // param_1 = sPad instance, param_2 = slot index (0-3)
  if (3 < param_2) return 0;

  // Check if this is P1 (slot 0) AND keyboard input enabled
  uVar2 = sMvc3Manager__getInstance();
  uVar1 = sMvc3Manager__getPlayerCharIdx(uVar2, 0);
  if ((uVar1 == param_2) && (*(char *)(param_1 + 0xc30) == '\0')) {
    return sMvc3Main__keyboardToGameMask_inline_maybe(param_1);  // keyboard path
  }

  // SOCD cleaning: read raw bits, resolve opposing pairs
  uVar1 = *(uint *)((ulonglong)param_2 * 0x2e0 + 0x1bc + param_1);
  // mCurrentButtons = sPad + slotIdx*736 + 444 (= 0x1BC)
  // Known offsets (from sPad_input_system_map.md):
  // +0x0: sPad vtable
  // +0x10: sPad.buttons[0] (P1)
  // Slot stride: 0x2E0 = 736 bytes
  // Per slot: +0x1BC (444) = mCurrentButtons raw bitmask

  // SOCD: up+down → up only
  if ((uVar1 & 0x80020) == 0x80020) { uVar1 &= ~0x20; }   // 0x20 = ↑, 0x8000 = ??
  if ((uVar1 & 0x20080) == 0x20080) { uVar1 &= ~0x20000; } // bit??
  if (((byte)uVar1 & 0xa0) == 0xa0) { uVar1 &= ~0x20; }   // more SOCD
  if ((uVar1 & 0xa0000) == 0xa0000) { uVar1 &= ~0x20000; } 
  if ((uVar1 & 0x40010) == 0x40010) { uVar1 &= ~0x40000; } 
  if ((uVar1 & 0x10040) == 0x10040) { uVar1 &= ~0x40; }    // left+right → left??
  if (((byte)uVar1 & 0x50) == 0x50) { uVar1 &= ~0x40; }    // more SOCD
  if ((uVar1 & 0x50000) == 0x50000) { uVar1 &= ~0x40000; } 

  return uVar1;
}
```

### `cChrInput__swapLRIfFaceLeft` (0x14002E560)

```
/* If cChr[+5368 = 0x14F8] & 0x200000 (face-left flag): swaps forward/back bit
   (bit 0 ↔ bit 1, i.e. ←↔→). Then if both set, clears back bit (bit 1 = →).
   The L/R mirror needed for side-switched play. */

uint cChrInput__swapLRIfFaceLeft(longlong param_1, uint param_2)
{
  uint uVar1 = param_2;
  if ((*(uint *)(*(longlong *)(param_1 + 8) + 0x14f8) & 0x200000) != 0) {
    // Character is facing left: swap bit 0 ↔ bit 1
    uVar1 = param_2 & 0xfffffffc;        // clear both L/R bits
    if ((param_2 & 2) != 0) { uVar1 |= 1; }  // had → → now ←
    if ((param_2 & 1) != 0) { uVar1 |= 2; }  // had ← → now →
  }
  if (((byte)uVar1 & 3) == 3) { uVar1 &= 0xfffffffd; }  // both → neutral → clears back
  return uVar1;
}
```

### `cChrInput__buttonRemap` (0x14002DA30)

```
/* Input: (playerIdx, rawInputBitmask, faceLeft). Reads sGameConfig::getInstance() and
   calls getButtonMappingValue (sub_1402522A0) per button to remap physical buttons to
   logical actions (A1/A2/LP/LK/HP/HK/S/etc). Direction bits pass through unchanged
   (SOCD was already handled by sPad). */

ulonglong cChrInput__buttonRemap(int param_1, uint param_2, undefined1 param_3)
{
  // SOCD + direction bits directly translated to button mapping
  uVar5 = (uint)((param_2 & 0x20020) != 0);     // bit??
  if ((param_2 & 0x80080) != 0) { uVar5 |= 2; }
  if ((param_2 & 0x10010) != 0) { uVar5 |= 4; }
  if ((param_2 & 0x40040) != 0) { uVar5 |= 8; }
  // ... reads sGameConfig button mapping for bits 15,12,13,14,8,9,10,11,1,2,0
  // Remaps physical button indices → game logical actions
  // Directions come through as: bit 0=←, 1=→, 2=↓, 3=↑
  return remapped_value;
}
```

### `cChrInput__initInputBuffer` (0x14002E440)

```
/* Clears input state including mMask and the mHeldRing buffer.
   NOTE: only clears +0x28 for 0x80 bytes (mHeldRing, 16×u64).
   Does NOT initialize mDirectionHistory512 at +0x360! */

void cChrInput__initInputBuffer(longlong param_1)
{
  *(undefined8 *)(param_1 + 0x10) = 0;      // mOldHeld
  *(undefined8 *)(param_1 + 0x18) = 0;      // padding?
  *(undefined8 *)(param_1 + 0x560) = 0;     // mRemapped
  *(undefined8 *)(param_1 + 0x20) = 0;      // mButtons
  *(undefined4 *)(param_1 + 0xa8) = 0;      // ring head cursor for released?
  *(undefined4 *)(param_1 + 300) = 0;       // = 0x12C, ring head cursor?
  *(undefined4 *)(param_1 + 0x230) = 0;     // = 0x230, ring head cursor?
  *(undefined1 *)(param_1 + 0x234) = 0;     // = 0x234
  *(undefined4 *)(param_1 + 0x238) = 0;     // = 0x238
  *(undefined4 *)(param_1 + 0x568) = 0;     // mMask
  memset((void *)(param_1 + 0x28), 0, 0x80); // clears mHeldRing[16] (128 bytes)
  return;
}
```

---

## What We Know: Fields Written by processFrameInput

| Offset | Name | Written? | Value |
|--------|------|----------|-------|
| +0x10 | mOldHeld | YES | copy of mHeld (before clear) |
| +0x14 | mHeld | YES | GetsCurrentInput → buttonRemap → sideSwap → swapLRIfFaceLeft → mMask |
| +0x560 | mRemapped | YES | mHeld BEFORE swapLRIfFaceLeft |
| +0x568 | mMask | YES | OR-accumulated mHeld bits (used to suppress held inputs next frame) |
| +0x56C | mProcessed | YES | = mRemapped (FINAL assignment at 0x14002E3E2) |
| +0x28 | mHeldRing[16] | **NO** | NOT written here |
| +0xAC | mReleasedRing[16] | **NO** | NOT written here |
| +0x130 | mPressedRing[16] | **NO** | NOT written here |
| +0x360 | mDirectionHistory512 | **NO** | NOT written here |

**CRITICAL**: `processFrameInput` does NOT populate any history rings. The rings at +0x28, +0xAC, +0x130, and +0x360 are populated ELSEWHERE — likely in the anmchr (character command interpreter) during character move processing.

---

## The Delta: Live vs Resim (with history rings ruled out)

Since history rings are populated AFTER `hkprocessFrameInput` overrides `mProcessed` to `g_recordedLocalInput`, and that value is identical in both live and resim:

**The history rings are IDENTICAL in both paths. DASH detection sees the same inputs.**

During **live**:

| Step | Source | Sets |
|------|--------|------|
| GetsCurrentInput (native) | physical pad | return value |
| ↓ hkGetsCurrentInput | g_rawLocalInput[idx] = pad value | g_rawLocalInput captured |
| buttonRemap | GetsCurrentInput return | mHeld |
| (post-process) | ... | mRemapped, mProcessed |
| hkprocessFrameInput | g_recordedLocalInput[idx] | mHeld/mRemapped/mProcessed **overridden** |
| anmchr tick | mProcessed (=g_recordedLocalInput) | history rings + DASH detection |

During **resim**:

| Step | Source | Sets |
|------|--------|------|
| GetsCurrentInput (hkGetsCurrentInput) | g_rawLocalInput[idx] (same) | return value |
| buttonRemap | same GetsCurrentInput → same remap | mHeld (same as live) |
| (post-process) | ... | mRemapped, mProcessed (same as live) |
| hkprocessFrameInput | g_recordedLocalInput[idx] (same) | mHeld/mRemapped/mProcessed **overridden (same)** |
| anmchr tick | mProcessed (=g_recordedLocalInput, same) | history rings + DASH detection (SAME) |

---

## Possible Root Causes

### 1. Snapshot Incompleteness — `mMask` (+0x568)

The `mMask` field at +0x568 is an OR-accumulated bitmask that **suppresses** held inputs across frames. Its purpose is to prevent hulk-tap-like behavior: if you hold a button, it gets suppressed after the first frame so the game only sees the initial press edge.

```
mMask |= (mHeld & 0x7FFBF0);   // accumulate pressed bits
mHeld &= ~mMask;                // suppress previously-held inputs
```

If `mMask` is NOT in the snapshot regions:
- During resim, mMask starts from the saved state (which has accumulated bits from the snapshot frame)
- But the resim runs additional frames, accumulating MORE bits
- At the end of resim, the real mMask is replaced by the snapshot-restored value

Wait, actually the problem is different. Let me think more carefully.

During live:
- Frame F0 (snapshot): mMask = X
- Frame F1: mMask = X | pressed
- Frame F2: mMask = X | pressed1 | pressed2
- Frame F3: mMask = X | pressed1 | pressed2 | pressed3

During resim:
- Restore snapshot at F0: mMask = X (from snapshot)
- Frame F1: mMask = X | pressed (same as live)
- Frame F2: mMask = X | pressed1 | pressed2 (same as live)
- Frame F3: mMask = X | pressed1 | pressed2 | pressed3 (same as live)

But after resim, the real mMask continues from wherever the resim left off. If the snapshot is loaded and resim runs, the real mMask gets replaced by the resimmed value — which should be the same.

UNLESS the snapshot does NOT include mMask. In that case:
- Snapshot saves some state
- mMask is NOT saved
- During resim: mMask starts from 0 (or whatever was left from the live run that happened before resim)
- This means during resim, mMask is WRONG
- After resim, mMask has accumulated different bits
- The next frames (after resim) use this corrupted mMask

Hmm, but this would cause issues with all inputs being suppressed or not suppressed, not just DASH.

### 2. Snapshot Incompleteness — Character Action State

The most likely real cause. The snapshot restores character bodies but the character state machine (anmchr) has timers, flags, and counters that affect DASH availability:
- Dash cooldown timer
- Action state (airborne, attacking, blocking, etc.)
- Stagger state
- Superflash state

If these are not restored to exactly the same values they had during the live frame, the DASH detection code in the anmchr interpreter may reject the dash input even though the same input was accepted during live.

### 3. Remote Input Correction During Resim

A rollback is triggered because a late-arriving remote packet differs from the prediction. During resim, `Controller_ResolveRemoteInput(f)` returns the CORRECTED remote input. If this corrected input causes the remote character to:
- Attack earlier → local character enters hitstun → no dash
- Dash in different direction → collision changes → different physics

Then the resim produces a different game state trajectory than the live prediction — the **intended behavior of rollback**. This explains case 2 but NOT case 1.

### 4. `g_recordedLocalInput` vs `g_rawLocalInput` Interaction During Resim

During resim:
- GetsCurrentInput returns `g_rawLocalInput[idx][g_resimulateCurrentFrame % RING_SIZE]`
- hkprocessFrameInput sets mProcessed = `g_recordedLocalInput[g_resimulateCurrentFrame % RING_SIZE]`

These are captured DIFFERENTLY:
- g_rawLocalInput per-player, raw (pre-buttonRemap), undelayed
- g_recordedLocalInput team-accumulated (OR of all chars), post-buttonRemap, delayed by g_delayFrames

The native processFrameInput uses GetsCurrentInput → buttonRemap → mHeld → final mProcessed. During both live and resim, the GetsCurrentInput value is the same (g_rawLocalInput), so the native mProcessed before override is the same.

**However**: the mProcessed AFTER override (g_recordedLocalInput) may differ from the native mProcessed. This is fine as long as both live and resim use the same g_recordedLocalInput. But:

**If the frame indexing into g_recordedLocalInput wraps differently between live and resim** (different modulo / ring head), then the wrong delayed value is read. This would happen if GetGameFrameCount() and g_resimulateCurrentFrame diverge.

---

## Key Questions

### Q1: Is the anmchr animation number important for DASH?

**Yes** — DASH detection is NOT in `processFrameInput`. It happens in the anmchr interpreter (character command processor) which reads `mProcessed` (+0x56C) and checks whether the character's **current animation/action state permits dash**. If the character is in hitstun, attacking, already dashing, recovering from an action, or has internal cooldown timers, the dash input is silently rejected.

The snapshot must restore 100% of this state for the anmchr to produce the same result during resim. If any of the following are missing:

- **Dash cooldown timer** — internal frame counter preventing re-dash
- **Action state flags** — what the character is currently doing (idle, walking, attacking, etc.)
- **Stagger / hitstun state** — prevents dash during recovery
- **Superflash state** — freezes all input processing

...then the resim's anmchr tick sees different character state → dash rejected even with identical mProcessed.

**Relevant snapshot question**: character animation/state lives partly in the `uCharacter` object (at `cChrInput+8`), which IS in the snapshot regions. But the `uCharacter` is ~50KB+, and our snapshot regions may not capture every byte. Any byte at an offset the anmchr checks for dash permission that isn't saved → drift.

### Q2: Is the resim putting in correct inputs for the other player?

**Yes for mProcessed, no for GetsCurrentInput** — there's an asymmetry that corrupts `mMask` (+0x568):

#### Remote player mProcessed: CORRECT in both live and resim

`hkprocessFrameInput` (controller.cpp:745-753) sets remote mProcessed = `g_usedRemoteInput[idx]`:

```cpp
uint32_t remote = g_usedRemoteInput[(uint32_t)lookupFrame % RING_SIZE];
*(uint32_t*)(cChrInput + 0x14)  = remote;   // mHeld
*(uint32_t*)(cChrInput + 0x560) = remote;   // mRemapped
*(uint32_t*)(cChrInput + 0x56C) = remote;   // mProcessed
```

`g_usedRemoteInput` is resolved by `Controller_ResolveRemoteInput` (controller.cpp:293-319):
- **Confirmed frame**: `g_usedRemoteInput[idx] = g_receivedInput[idx]` (actual received remote input)
- **Unconfirmed**: predicts from closest confirmed frame's input

Identical resolution in both live and resim.

#### Remote player GetsCurrentInput: DIFFERENT between live and resim

`hkGetsCurrentInput_withSOCD` (controller.cpp:363-446):

| | Live | Resim |
|---|---|---|
| GetsCurrentInput for remote | `0` (line 375) | `g_usedRemoteInput[idx]` (line 436) |
| Native processFrameInput sees | `0` → mHeld=0, mMask unchanged | `buttonRemap(g_usedRemoteInput)` → mMask accumulates bits |
| hkprocessFrameInput override | `g_usedRemoteInput` (correct) | `g_usedRemoteInput` (correct) |
| **final mProcessed** | `g_usedRemoteInput` ✓ | `g_usedRemoteInput` ✓ |
| **final mMask (+0x568)** | **unchanged** (native saw 0 input) | **extra accumulated bits** from native buttonRemap |

The native processFrameInput for the remote player:
- **Live**: GetsCurrentInput=0 → `mHeld=0` → `mMask |= 0` → mMask clean
- **Resim**: GetsCurrentInput=g_usedRemoteInput → `mHeld=buttonRemap(g_usedRemoteInput)` → `mMask |= (mHeld & 0x7FFBF0)` → **mMask accumulates bits**

Since hkprocessFrameInput does NOT override mMask (+0x568), the accumulated bits persist. This means:

1. During resim, the remote player's native processFrameInput accumulates mMask from g_usedRemoteInput's bits
2. hkprocessFrameInput overrides mHeld with g_usedRemoteInput (undoing the buttonRemap + mMask suppression)
3. **BUT mMask is left with extra accumulated bits**
4. After the resim, the first real-time frame runs processFrameInput on that character again
5. This time, GetsCurrentInput returns 0 (live path) → mHeld=0 → but `mHeld &= ~mMask` cleans nothing
6. **However**: the extra mMask bits remain, and on future frames when GetsCurrentInput returns non-zero (keyboard/P2), some bits get incorrectly suppressed

This wouldn't directly affect the LOCAL player's dash, but it shows the GetsCurrentInput asymmetry is a real bug that should be fixed regardless.

#### Would fixing the GetsCurrentInput for remote player fix the DASH?

The remote player's GetsCurrentInput returning 0 during live is **intentional** — the remote player's physical input shouldn't pollute the netplay stream. The GetsCurrentInput return doesn't matter because hkprocessFrameInput overrides it.

The asymmetry in mMask accumulation probably doesn't cause the LOCAL dash loss. The local player's mMask accumulates correctly (GetsCurrentInput returns the same g_rawLocalInput in both paths). So the DASH loss is likely not from the remote GetsCurrentInput asymmetry, but from either:

1. **Animation/state drift** — the resim produces different game state because the corrected remote input changes the gameplay trajectory, which is expected rollback behavior
2. **Local mMask +0x568 snapshot completeness** — if mMask is NOT fully restored, the suppressed-input tracking desyncs

#### Verdict

The resim **does** put in the correct final mProcessed for the remote player. The GetsCurrentInput asymmetry is a cosmetic issue (accumulating mMask during resim that's then ignored by the override), but not the root cause of DASH loss.

---

## Key Addresses (Ghidra ichneumon_sub:8192)

| Function | Address |
|---|---|
| `cChrInput__processFrameInput` | 0x14002DD60 |
| `cChrInput__swapLRIfFaceLeft` | 0x14002E560 |
| `cChrInput__buttonRemap` | 0x14002DA30 |
| `cChrInput__initInputBuffer` | 0x14002E440 |
| `sPad__getPlayerInput_withSOCDClean` | 0x1402B3700 |
| `sBattleSetting__isNotRecordingAndNetActive` | 0x14024D500 |
| `NetplayInputPreprocess` (FUN_14024cfa0) | 0x14024CFA0 |
| `sNetPad__readBufferedInputForTeam` | 0x140289520 (NOT CALLED by our mod) |

## cChrInput Offsets (from initInputBuffer and decompile)

| Offset | Size | Name | Notes |
|--------|------|------|-------|
| +0x08 | 8 | cChr* | owning uCharacter pointer |
| +0x10 | 4 | mOldHeld | previous frame's mHeld |
| +0x14 | 4 | mHeld | current frame input (post-remap, post-swap, post-mMask) |
| +0x20 | 4 | mButtons | raw button bitmask |
| +0x28 | 128 | mHeldRing[16] | 16 × u64 ring of held bits |
| +0xA8 | 4 | (cursor?) | ring head for released? |
| +0xAC | 128 | mReleasedRing[16] | 16 × u64 ring of released bits |
| +0x12C | 4 | (cursor?) | ring head for pressed? |
| +0x130 | 128 | mPressedRing[16] | 16 × u64 ring of pressed bits |
| +0x1B0..0x22F | 128 | (another ring?) | 128B ring |
| +0x230 | 4 | (cursor?) | |
| +0x234 | 1 | | |
| +0x238 | 4 | | |
| +0x28..0x35F | 56 | gap to +0x360 | |
| +0x360 | 512 | mDirectionHistory512 | 64 × 8B entries (the DASH detector input) |
| +0x560 | 4 | mRemapped | post-buttonRemap, pre-swapLRIfFaceLeft |
| +0x568 | 4 | mMask | OR-accumulated suppress mask |
| +0x56C | 4 | mProcessed | final bitmask used by anmchr |

## Source Locations

- `controller.cpp`: hkGetsCurrentInput_withSOCD (line 363), hkprocessFrameInput (line 701)
- `rollback.cpp`: Rollback_BuildRegions (line 196), Rollback_ReSimulate (line 977)

---

## Fixes Applied & Outcomes

### Fix 1: Remote GetsCurrentInput Returns 0 During Resim

**File**: `UMVC3Hook/code/controller.cpp` in `hkGetsCurrentInput_withSOCD`.

**Status**: Applied. Remote players now return 0 during resim matching live behavior.

### Fix 2: Clear mMask in hkprocessFrameInput

**File**: `UMVC3Hook/code/controller.cpp` in `hkprocessFrameInput`.

**Status**: Applied. `mMask (+0x568)` now cleared to 0 in every override branch.

### Fix 3: Diagnostic Logging During Resim

**Status**: Applied. Pre/post override logging of cChrInput fields including `mDirectionHistory512`.

### Fix 4 (Failed): Correct cChrInput Snapshot Offset

**Change**: `rollback.cpp` — snapshot offset `0x872` → `0x21C8`, size `0x570` → `0x5A0`

**Why it failed**: The main uCharacter DTI-based save (`add(character, finalSize)`) already covers the cChrInput range at +0x21C8. The additional separate save was redundant regardless of offset. The dirHist issue is NOT a snapshot problem.

---

## Root Cause Investigation — Round 2

### Discovery: cChrInput is at uChar+0x21C8, not +0x872

The disassembly of `uCharacter__move_base` at the `processFrameInput` call site:

```
0x14004CCB: LEA RCX, [RSI + 0x21C8]    ; RSI = uCharacter*
0x14004CD2: CALL cChrInput__processFrameInput
```

Both `processFrameInput` AND `initInputBuffer` operate on `uChar + 0x21C8`. The earlier decompiler output showing `param_1 + 0x872` was due to Ghidra's `longlong*` pointer scaling (`0x439 * 8 = 0x21C8`).

### Discovery: processFrameInput runs AFTER the state machine

Call order within `uCharacter__move_base` at `LAB_14004c0a0`:

```
0x14004C0A6: CALL [RAX+0x108]           ; STATE MACHINE DISPATCH (runs FIRST!)
0x14004CAC-0x14004CC6: coord update, etc.
0x14004CCB: LEA RCX,[RSI+0x21C8]
0x14004CD2: CALL cChrInput__processFrameInput  ; (runs AFTER state machine!)
0x14004CD7: LEA RCX,[RSI+0x21C8]
0x14004CDE: CALL FUN_14002E5A0          ; mask/post-process input
0x14004CE3: LEA RCX,[RSI+0x2738]        ; cChrInput+0x570 (just beyond save window)
0x14004CEA: CALL FUN_14002B910          ; ???
0x14004CEF: LEA RCX,[RSI+0x2950]        ; cChrInput+0x788
0x14004CF6: CALL FUN_14002B910          ; ???
```

Input injected in our hook (`hkprocessFrameInput`) affects the NEXT frame's state machine, not the current one.

### Discovery: dirHist is a 64-entry rotating ring buffer

Log evidence from resim sequence (frames 1500-1502):

```
Frame 1500 post: dirHist[0]=0x0000000000000000  ← empty
Frame 1501 pre:  dirHist[0]=0x0000000100000000  ← direction APPEARS
Frame 1501 post: dirHist[0]=0x0000000100000000  ← persists after override
Frame 1502 pre:  dirHist[0]=0x0000000000000000  ← ROTATED OUT!
```

The direction is correctly written to dirHist[0] in frame 1501, but by frame 1502 the ring has advanced and dirHist[0] now contains an old/stale entry. **dirHist[0] is only meaningful when (head % 64) == 0.**

### Who writes dirHist?

Neither `cChrInput__processFrameInput` nor `FUN_14002E5A0` write to dirHist (+0x360). The writer must be in:
1. The vtable+0x108 state machine dispatch chain, OR
2. `FUN_14002B910` called at cChrInput+0x570 and cChrInput+0x788

`cChrInput__getInputDirection` (0x1402CE750) reads from cChrInput+0x590/+0x594 — these are beyond our 0x570 save window but within the main uCharacter DTI save.

### New Logging Added

ResimDiag now logs:
- `cntr` = frame counter at `cChrInput+0x18` (u64)
- `h590` = head/index at `cChrInput+0x590` (u32)
- `h594` = head/index at `cChrInput+0x594` (u32)
- `d[0]` = dirHist slot 0
- `d[slot]` = dirHist at `(frameCnt & 0x3F)` — the logically correct slot for current frame
- `d[1]`, `d[2]` = dirHist slots 1, 2 (to observe rotation)

### Root Cause: `mOldHeld (+0x10)` Not Updated by Our Hook

The native `cChrInput__processFrameInput` post-processing at `0x14002E27A` does:

```asm
0x14002E3BD  MOV  EAX,[RSI+0x14]         ; EAX = mHeld (current input)
0x14002E3C0  MOV  [RSI+0x10],EAX         ; mOldHeld = mHeld
; ... mMask update ...
0x14002E3D6  AND  [RSI+0x10],EAX         ; mOldHeld &= ~mMask
0x14002E3D9  AND  [RSI+0x14],EAX         ; mHeld &= ~mMask
```

Both `+0x10` (mOldHeld) and `+0x14` (mHeld) end up with the same value. Our `hkprocessFrameInput` hook calls the native first (which populates both fields), then overwrites `+0x14`, `+0x560`, `+0x56C`, and zeroes `+0x568` — but **NOT** `+0x10`.

If the character state machine (vtable+0x108, or the per-character `uChris__stateUpdate_9A70` called after `move_base`) reads `mOldHeld (+0x10)` instead of `mHeld (+0x14)` for dash input detection, it gets the **native-processed sPad value** rather than our replayed input.

**Fix**: Write the override value to `cChrInput + 0x10` alongside `+0x14`, `+0x560`, and `+0x56C` at all four override sites in `hkprocessFrameInput`.

### Updated Call Order in move_base (from raw disassembly at LAB_14004c0a0)

```
0x14004C0A6: CALL [RAX+0x108]              ; vtable+0x108 (NOT state machine — attacks/etc.)
0x14004C0AF: CALL uCoord__getPos
0x14004C0BE: CALL MtVector3__op_assign
0x14004CC9: CALL FUN_140011140              ; pre-input processing
0x14004CD2: CALL cChrInput__initInputBuffer
0x14004CD7: CALL cChrInput__processFrameInput  ; OUR HOOK (overrides +0x10, +0x14, +0x560, +0x56C, +0x568)
0x14004CDE: CALL FUN_14002E5A0              ; mask/post-process (sets mMask etc.)
0x14004CE3: CALL FUN_14002B910 @ cChrInput+0x788  ; dirHist writer candidate
0x14004CEF: CALL FUN_14002B910 @ cChrInput+0x570  ; dirHist writer candidate
0x14004CF9: CALL FUN_14003A440              ; gate function
    ; if returns non-zero:
    0x14004C0EB: CALL FUN_14002CF10
    0x14004C0F7: CALL FUN_14002CF90
    0x14004C0FC: CALL FUN_140039B70
    0x14004C103: CALL FUN_140039FB0
    ; endif
0x14004C10F: TEST [RSI+0x14F0], 0x4400
0x14004C119: CALL vtable+0x140               ; ACTUAL STATE MACHINE DISPATCH?
0x14004C123: CALL ...                        ; more code continues...
```

Then back in `uChris__tick`:
```
uCharacter__move_base();                    ; returns from above
if (!(chr+0x1514 & 0x10))
    uChris__stateUpdate_9A70();             ; per-character state update
```

The state machine (`vtable+0x140` and/or `uChris__stateUpdate_9A70`) runs **AFTER** our hook, so our overrides ARE visible to it. The bug is specifically that `mOldHeld (+0x10)` was left with the native-processed value while `mHeld (+0x14)` had our override — if the state machine reads `+0x10`, it sees wrong input.
