# Replay System Analysis (umvc3.exe unpacked)

Investigated via Ghidra MCP.  All addresses are VA (base = 0x140000000).

## Overview

The game has two replay-related classes:

- **`sReplay`** — in-memory input recording/playback state machine (sizeof ~0x23428).
- **`rReplay`** — file I/O for `.rpd` replay files (inherits from `cPlayDataAccumulate`).

The **in-memory recording system (`sReplay`)** is live and used by the game (training mode
auto-records & loops the last 10 seconds).  The **file save/load path (`rReplay`)** is
**dead code** — nothing in the retail binary ever creates an `rReplay` object or calls
`save`/`load`.  There is no UI for saving or loading replays.

---

## `sReplay` — In-Memory Recording/Playback

### Singleton

```
g_psReplay  = *(uint64_t*)0x140d4d9d8    (written by sReplay__sReplay_ctor)
sReplay__getInstance() returns g_psReplay
```

Created by `sMvc3Manager__createReplay()` (at 0x1401cf3a0) → calls `op_new(0x23428)` + ctor.

### Constructor  (`sReplay__sReplay_ctor` @ 0x1401ce880)

```
offset  size    field
------  ----    -----
+0x00     8     vtable ptr  → 0x140ae7db0
+0x38     1     isOnlineRanked flag (cVar1)
+0x48     1     unused byte
+0x4c     8     null qword
+0x54     4     frame counter  (INC dword [RCX+0x54] at 0x1401cefdd)
+0x58  0x233c4  match state + input buffers (zeroed in ctor)
```

### State Flags (`sReplay+0x3c`, uint32)

```
Flag    Meaning
----    -------
0x01    Playing (any team)
0x02    Recording (any team)
0x04    (used in move logic)
0x08    (used in move logic)
0x10    Playing team 0
0x20    Playing team 1
0x40    Recording team 0
0x80    Recording team 1
0x100   Swap team indices (XOR team with 1)
```

Read/written by:
- `sReplay__checkFlags(param_1, mask)` → `return *(param_1+0x3c) & mask;`
- `sReplay__setFlags(param_1, val)` → `*(param_1+0x3c) = val;`
- `sReplay__addFlags(param_1, mask)` → `*(param_1+0x3c) |= mask;`
- `sReplay__clearFlags(param_1, mask)` → `*(param_1+0x3c) &= ~mask;`

### Input Buffers

Two per-team ring buffers at `sReplay + 0x194`:

```
Team 0:  sReplay + 0x194
Team 1:  sReplay + 0x194 + 0x11944  (= +0x11ad8)
Each:    0x11944 bytes = 4 (frameCount) + 18000 * 4 (inputs)
```

Layout per team:
```
+0x0000  int32 frameCount      (max 18000 = 5 min @ 60fps)
+0x0004  int32 inputs[18000]   (4 bytes per frame = raw game mask)
```

- **`sReplay__writeInput(param_1, team, input, frameIdx)`** @ 0x1401cf520
- **`sReplay__readInput(param_1, team, flags, frameIdx)`** @ 0x1401cecb0
- **`sReplay__writeBothTeamInputs(param_1, inputP1, inputP2, frameIdx)`** @ 0x1401cf5d0

### Key Functions

| Address | Function | Notes |
|---------|----------|-------|
| 0x1401ce000 | `sReplay__checkReplayValid` | Dead code, never called |
| 0x1401ce040 | `sReplay__processReplayFile` | Dead code, never called |
| 0x1401ce120 | `sReplay__handleReplayLoad` | Called via vtable from `j_sReplay__handleReplayLoad` |
| 0x1401ce1a0 | `sReplay__processRecordPlayback` | Core record/playback frame logic |
| 0x1401ce340 | `sReplay__advanceFrame` | Just calls `cUnit__getDeltaTime()` |
| 0x1401ce380 | `sReplay__processEndOfRound` | Handles round end, audio effects |
| 0x1401ce6b0 | `sReplay__resetFrameCounters` | Reset per-team frame counters |
| 0x1401ce6e0 | `sReplay__initReplaySlot` | Dead (?), calls getDeltaTime |
| 0x1401ce880 | `sReplay__sReplay_ctor` | Constructor |
| 0x1401ce970 | `sReplay__copyCharSlotData` | Copies 20 dwords of char slot data |
| 0x1401ce9e0 | `sReplay__copyFromBuffer` | Copy match state into another sReplay |
| 0x1401ceaa0 | `sReplay__setStatePlay` | Clears flags 0xC2 |
| 0x1401ceab0 | `sReplay__updatePerTeam` | Takes max of both team frame counts, clears play flags |
| 0x1401ceb10 | `sReplay__getFrameIndex` | Returns frame index (via allocator call — may be mislabeled) |
| 0x1401ceb20 | `sReplay__getTeamDataPtr` | Returns `*(param_1 + 0x7048 + team*8)` |
| 0x1401ceba0 | `sReplay__copyBuffer` | Memcpy of 0x233c4 bytes from +0x58 |
| 0x1401cecb0 | `sReplay__readInput` | Read input for team at frame |
| 0x1401ced80 | `sReplay__getFrameCursor` | Returns `*(param_1 + 0x40 + team*4)` |
| 0x1401ced90 | `sReplay__getTeam0FrameCount` | Returns `*(param_1 + 0x194)` |
| 0x1401ceda0 | `sReplay__getStageValue` | Returns stage value |
| 0x1401cedd0 | `sReplay__getStoredInput` | Returns `*(param_1 + 0x56c)` |
| 0x1401cee10 | `sReplay__isDataEmpty` | True if both teams have 0 frames |
| 0x1401cee40 | `sReplay__isRecording` | Checks flags 0x42/0x82 per team |
| 0x1401cee80 | `sReplay__isPlaying` | Checks flags 0x31/0x21 per team |
| 0x1401ceed0 | `sReplay__move` | State machine update (vtable dispatch) |
| 0x1401cef28 | `sReplay__move_trainingLoop` | Training: playback if >600 frames recorded |
| 0x1401cefd0 | `sReplay__processPerFrame` | Increments frame counter at +0x54 |
| 0x1401cf520 | `sReplay__writeInput` | Write input at frame index |
| 0x1401cf5d0 | `sReplay__writeBothTeamInputs` | Write both team inputs simultaneously |
| 0x1401cf680 | `sReplay__saveRestoreMatchState` | Save/restore stage, timer, area, chars |
| 0x1401cfa90 | `sReplay__onRoundStart` | Seeds RNG, clears team data |
| 0x1401cfcc0 | `sReplay__reseedFromReplaySeed` | Re-seed RNG from saved seed, clear flags |
| 0x1401cfd60 | `sReplay__final` | Destructor cleanup (dead code, never called) |

### State Machine — `sReplay__move` @ 0x1401ceed0

```
move():
    if NOT online_ranked:
        if flags & 0x0C != 0x0C:
            goto skip_play
        if replaySlotHasData AND NOT isDataEmpty:
            reseedFromReplaySeed()
            goto skip_play
    else (online ranked):
        if NOT isRecording(-1) AND flags & 0x0C != 0x0C:
            goto skip_play

    setStatePlay()   // clear flags 0xC2

skip_play:
    if training_mode:
        for each team:
            if frameCount > 599:  // 10 seconds
                if isPlaying(0) OR isPlaying(1):
                    updatePerTeam()  // transition to playback
```

In training mode: records 600 frames (10s), then auto-plays back.  Loops indefinitely.

### Training Loop — `sReplay__move_trainingLoop` @ 0x1401cef28

Same as `move()` but with training-specific transition: 600 frames threshold, auto-playback.

### Round Start — `sReplay__onRoundStart` @ 0x1401cfa90

```
onRoundStart():
    if onlineRanked:
        seed = *(param_1 + 0x23424)    // online seed
        *(param_1 + 0x58) = seed
    if NOT recording:
        // RECORD mode:
        clearTeamData()
        seed = *(param_1 + 0x23424)
        *(param_1 + 0x58) = seed
        seedGlobalXorShift(seed)
        seedNextBattleRng(seed)
        for each team, each character:
            dispatcherSeed(seed, character)
        setFlags(0x01)   // set "playing" flag?  No — set to 0x01 which is Playing
    else:
        // PLAYBACK mode:
        clearTeamData()
        seed = *(param_1 + 0x58)    // use saved seed
        seedGlobalXorShift(seed)
        seedNextBattleRng(seed)
        for each team, each character:
            dispatcherSeed(seed, character)
        setFlags(0x02)   // set Recording flag
```

Wait — this looks inverted.  When `isOnlineRanked` is FALSE:
- If `*(param_1+0x38) == 0` → recording mode? Seeds from `+0x23424`, sets flag 0x01 (Playing)
- If `*(param_1+0x38) != 0` → playback mode? Uses saved seed from `+0x58`, sets flag 0x02 (Recording)

This is confusing — the "recording" path sets flag 0x01 (Playing) and vice versa.  The flag
semantics may be inverted or the variable at `+0x38` might mean "isReplay."

### Re-seed — `sReplay__reseedFromReplaySeed` @ 0x1401cfcc0

```
reseedFromReplaySeed():
    clearTeamData()
    seed = *(param_1 + 0x58)
    seedGlobalXorShift(seed)
    seedNextBattleRng(seed)
    for each team, each character:
        dispatcherSeed(seed, character)
    clearFlags(0x0C)
```

### Match State Save/Restore — `sReplay__saveRestoreMatchState` @ 0x1401cf680

Stores/restores at +0x18c (timer), +0x190 (stage), +0x5c (area), +0x180, +0x60, +0x64, +0x68.
Uses `sBattleSetting` + `sArea` API to read/write values.

---

## `rReplay` — File I/O (DEAD CODE)

### File Extension

`.rpd` at 0x140b10d70 ("rpd\0").

### Vtable (0x140b10d90 for real instance, 0x140b10d30 for base)

```
offset  function
------  --------
+0x00   dtor
+0x08   (?)
+0x10   rReplay__load     @ 0x14029c460
+0x18   rReplay__save     @ 0x14029c510
+0x20   (?)
+0x28   (?)
+0x30   (?)
+0x38   rReplay__copyFromSReplay  @ 0x14029c430
```

### Save Format — `rReplay__save` @ 0x14029c510

```
Offset  Size  Content
------  ----  -------
+0x00   4     Magic: 0x445052  ("RPD" little-endian)
+0x04   4     Version: 0xFF
+0x08   0x233c4  raw copy of sReplay + 0x70
```

Total file size: 0x233cc bytes (~144 KB).

### Load — `rReplay__load` @ 0x14029c460

Checks magic == 0x445052 and version == 0xFF, then reads 0x233c4 bytes into
`rReplay + 0x0e` (the offset within rReplay where the sReplay copy starts).

### Constructor — `rReplay__newInstance` @ 0x14029c8e0

Allocates 0x88 bytes, then calls `cPlayDataAccumulate_13`.

### `rReplay__copyFromSReplay` @ 0x14029c430

Calls `sReplay__getInstance()` then `sReplay__copyBuffer(sReplay, rReplay + 0x70)`.

### Call Graph

```
rReplay__newInstance (0x14029c8e0)
    └─ DATA ref only in .rdata — **NO CALLERS**
```

**The entire `rReplay` save/load path is unreachable.**  No code in the binary ever
creates an `rReplay` object, calls `save()`, or calls `load()`.  Zero call-site XREFs.

---

## Observations

1. **In-memory recording works** — training mode records 10s of input and loops playback.
   The `sReplay` instance is alive, `processPerFrame` increments the frame counter each
   frame, `writeInput`/`readInput` are called from `cChrInput__processFrameInput`.

2. **No save path exists** — the `rReplay` class that serializes to `.rpd` files is compiled
   into the binary but never instantiated.  There's no menu option, no end-of-match trigger,
   nothing that calls `rReplay__newInstance` or `rReplay__save`.  The file I/O code is
   dead — possibly left over from development, or intended for a feature that was cut.

3. **Online/ranked seeds** — The replay system reads a seed from `sReplay+0x23424` in ranked
   mode.  This is the RNG seed that gets synchronized over the network, ensuring deterministic
   replay playback.  The seed is stored at `+0x58` for the duration of the match.

4. **The Vita version reportedly has replays** in online ranked mode.  The PC port's `rReplay`
   save/load code may be vestigial from a multiplatform codebase — the `.rpd` writer exists
   but was never wired up in the PC retail build.

## What Would Need to Happen to Enable Replay Saving

To save a replay, one would need to:

1. Call `rReplay__newInstance()` — create an rReplay object (dead function, no callers)
2. Call `rReplay__copyFromSReplay(rReplay)` — copy the live sReplay match state into it
3. Call `rReplay__save(rReplay, filePath)` — write the `.rpd` file to disk

The buffer that gets saved is `sReplay + 0x70` (0x233c4 bytes), which includes:
- RNG seed at +0x58 (relative to sReplay, offset -0x18 from the copied region)
- Match settings (stage, timer, area)
- Both team input buffers (up to 18000 frames each)

The stored seed at sReplay+0x58 is set at round start and is the deterministic RNG
seed that controls all random events in the match.  The inputs are raw per-frame game
button masks.
