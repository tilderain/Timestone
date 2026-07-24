# Input Hook Comparison: Pad-Level vs Character-Level

**Status: Historical design comparison.** The decision is implemented: current
netplay uses the character-level `cChrInput__processFrameInput` hook after
native translation, plus frame-boundary history management. The shown source
line numbers and pseudocode predate the current absolute-frame queues, scheduled
delay transitions, startup gates, and resimulation injection. The pad prototype
captures two 550-byte slices, not the complete four-slot `sPad` object; claims
below describing complete pad-state capture are design-era overstatements.

## Overview

Two existing input hook approaches exist in the codebase. Both were written for
rollback netcode but at different layers of abstraction. This document compares
them for the purpose of designing the final rollback input delivery system.

---

## Approach A: Pad-Level Hook (FUN_1402b41b0)

**Address:** `0x1402B41B0`
**Ghidra name:** `SomethingRelatingToInputHandling`
**Used in:** `defaults.h` lines 315-347

### What it does

Operates on the **global sPad instance** (param_1). The original function:

1. Copies byte `+0xC30` → `+0xC31` (shift a flag)
2. Checks sLicense → sMain pause state
3. If unpaused and timer expired, calls `padPollAllSlots_maybe(param_1)`
4. Has a re-entry timer at `+0xC24` to prevent polling too frequently

The `defaults.h` hook captures `ReplayBufferSize` (550) bytes starting at
`inputRef` (the sPad object), recording or replaying them each frame:

```cpp
if (recording) {
    memcpy(&replayBuffer[recordReplayIndex], (uint8_t*)inputRef, ReplayBufferSize);
    memcpy(&replayBuffer2[recordReplayIndex], (uint8_t*)inputRef + 0x2c0, ReplayBufferSize);
}
```

This saves the **entire pad state** — all 4 slots × 0x2E0 bytes plus headers.

### Pros

- **Single hook point** — one function call per frame for all players
- **Complete capture** — every pad slot's raw input is preserved
- **Works below the game's routing** — no interference with cChrInput logic
- **Simple conceptually** — save the pad, restore the pad

### Cons

- **Massive data per frame** — 550+ bytes per save, times 2 for the two
  buffer regions = 1100 bytes per frame per direction (record + replay)
- **No character routing** — you still need to re-implement the mapping from
  pad slots to team/player indices
- **Bypasses replay system** — training mode dummy recording/playback reads
  from sReplay, not the raw pad. Hook has to implement its own replay logic
- **Physics coupling** — the defaults.h code shows physics state being
  separately captured, indicating this level doesn't capture everything
- **No online mode awareness** — the original function just polls devices;
  the game's online input path (sMvc3SyncPad) is entirely separate
- **Coarse granularity** — saves lots of data that never changes (configuration,
  device state, deadzone calibration)

---

## Approach B: Character-Level Hook (cChrInput__processFrameInput)

**Address:** `0x14002DD60`
**Ghidra name:** `cChrInput__processFrameInput`
**Used in:** `controller.cpp` lines 403-480 (`hkprocessFrameInput`)

### What it does

Operates on each **cChrInput instance** (param_1 = a per-character input
processor). The full original function:

1. Gets owner character from `param_1 + 0x08`
2. Checks character aliveness + input-disabled flags
3. Checks if battle step is valid (not intro/result)
4. Shifts input history: `mPrev = mNow`, zeroes `mNow` and `mTemp`
5. **Branch: Replay recording** — reads input from sReplay recording buffer
   (training mode recording), overrides pad input
6. **Branch: Online (sMvc3SyncPad)** — reads input from network sync buffer
   via `network_sync_buffer_read()`
7. **Branch: Training replay playback** — reads input from sReplay playback
8. **Default: Offline live** — calls `GetsCurrentInput_withSOCD(pad, playerIdx)`
   to read the physical device
9. Applies button remapping
10. Writes final result to `+0x14` (mNow), `+0x560` (mOrg), `+0x56c` (mTemp)

### Current hook behavior (hkprocessFrameInput, controller.cpp):

```cpp
// Run native translation first
oprocessFrameInput(cChrInput);

// Host P1 (local): accumulate, then overwrite with delayed value
g_localPending |= *(uint32_t*)(cChrInput + 0x14);
uint32_t delayed = g_oldInputs[MAX_HISTORY - g_delayFrames];
*(uint32_t*)(cChrInput + 0x14) = delayed;
*(uint32_t*)(cChrInput + 0x560) = delayed;
*(uint32_t*)(cChrInput + 0x56C) = delayed;

// Host P2 (remote): inject received input
uint32_t remote = g_receivedInput[(uint32_t)g_netFrameCount % RING_SIZE];
*(uint32_t*)(cChrInput + 0x14) = remote;
*(uint32_t*)(cChrInput + 0x560) = remote;
*(uint32_t*)(cChrInput + 0x56C) = remote;
```

### Pros

- **After input routing** — the game has already mapped pad slot → player →
  character. You just override the final per-character value
- **Handles replay mode** — training dummy recording and playback are
  processed by the original function before the hook runs
- **Already has an online branch** — the original code checks `isGameModeOnline`
  and reads from sMvc3SyncPad. The hook replaces this with frame-indexed input
- **Small data** — only 3 uint32 fields to overwrite (+0x14, +0x560, +0x56C)
  = 12 bytes per character per frame
- **Deterministic** — each character's input is independent; no shared state
- **Works with existing netplay** — the current delay-based netcode already
  uses this hook successfully

### Cons

- **Called 6 times per frame** — one per character (3 per team × 2 teams).
  Must re-determine team index each call via `uAmaterasu__getter_4AC0`
- **Must know cChrInput layout** — requires hardcoded offsets (+0x14, +0x560,
  +0x56C). If the game updates, these could change
- **After native processing** — the original function runs first, so you're
  undoing work the game just did. Slight inefficiency
- **Team routing complexity** — the hook must distinguish host/client and
  P1/P2, mirroring some of the game's own routing logic
- **Doesn't handle mTrg/mPop** — only overwrites mNow/mOrg/mTemp. The derived
  trigger/release edges (+0x20 mTrg, +0x24 mPop) were computed by the original
  call from mPrev/mNow — which are already overwritten by the hook, so they
  reflect the overwritten value's edges. This works because the overwrite
  happens in the original call, then the hook overwrites again. But the edge
  detection is based on whatever was in +0x10 (mPrev) before this frame.

---

## Side-by-Side Comparison

| Aspect | FUN_1402b41b0 (Pad) | cChrInput__processFrameInput |
|--------|---------------------|------------------------------|
| **Hook frequency** | 1× per frame | 6× per frame |
| **Data per frame** | 550+ bytes | 12 bytes per char × 6 = 72 bytes |
| **Granularity** | Entire pad struct | Per-character input uint32 |
| **Replay-aware** | No (must implement own) | Yes (native training replay) |
| **Online-aware** | No | Yes (sMvc3SyncPad branch) |
| **Physics coupling** | Separate physics snapshot needed | None (input only) |
| **Team routing** | Must re-implement | Already done by game |
| **Current state** | Prototype / incomplete in defaults.h | Working in controller.cpp |

---

## The Separation in defaults.h

The `defaults.h` file separates physics from input at two levels:

### 1. Physics Tracking via Trampoline Hooks

Three physics functions are intercepted to capture their parameters:

```cpp
// Physics update (main) — FUN_140521df0
FUN_140521df0(param_1)     → stores physics.param_1

// Physics Y axis — FUN_1400199c0  
FUN_1400199c0(param_1..5)  → stores physicsY.param_1..5

// Physics X axis — FUN_1405ab9a0
FUN_1405ab9a0(param_1..9)  → stores physicsX.param_1..9
```

These are separate from the pad input hook (`FUN_1402b41b0`). The commented-out
simulation loop in `playercode()` (lines 390-445) shows the intended flow:

```
1. SaveState()                    — capture all fighter + team + physics state
2. For each runahead frame:
   a. LoadState()                 — restore to snapshot
   b. Tick all 6 fighters         — re-simulate character FSM
   c. Call physics update         — FUN_140521df0 (physics.param_1)
   d. Call physics Y              — FUN_1400199c0 (physicsY params)
   e. SaveState()                 — capture new state
```

### 2. SaveLoadState — What Gets Snapshotted

```cpp
void SaveLoadState(bool save) {
    for (size_t i = 0; i < 6; i++) {
        SaveLoadPtr(fighter + 0x00, save, backupSize);     // 1000 bytes
        SaveLoadPtr(fighter + 0x538, save, backupSize);    // 1000 bytes
        SaveLoadPtr(fighter + 0x18, save, backupSize);     // 1000 bytes
        SaveLoadPtr(fighter + 0x6d8, save, backupSize);    // 1000 bytes
        SaveLoadPtr(fighter + 0x858, save, backupSize);    // 1000 bytes
        SaveLoadPtr(fighter + 0x9d8, save, backupSize);    // 1000 bytes
        SaveLoadPtr(fighter + 0xb58, save, backupSize);    // 1000 bytes
    }
    SaveLoadPtr(mysterytable, save, 9200);     // sCharacter? (9.2 KB)
    SaveLoadPtr(block2, save, 2272);            // aNextBattle? (2.2 KB)
    SaveLoadPtr(team1, save, teamBackupSize);   // 0x960 = 2.4 KB
    SaveLoadPtr(team2, save, teamBackupSize);   // 0x960 = 2.4 KB
    if (physics.inited)
        SaveLoadPtr(physics.param_1, save, 11424);  // 11.4 KB physics state
}
```

Total per snapshot: ~6 × 7000 + 9200 + 2272 + 2 × 0x960 + 11424 ≈ **~57 KB**

The 7 × 1000-byte slices per fighter suggest a coarse memcpy approach over
specific regions of each character object, not a single contiguous dump.
The selected offsets (0x00, 0x18, 0x538, 0x6d8, 0x858, 0x9d8, 0xb58) were
chosen to skip large static regions (model data, animation resources) while
capturing the mutable fields between them.

### Why Separate Physics?

The physics engine (`FUN_140521df0` = physics update, `FUN_1400199c0` = Y-axis,
`FUN_1405ab9a0` = X-axis) operates on a **separate physics world** that is
NOT part of the character objects. The physics simulation runs independently
of the character FSM tick:

- `FUN_140521df0` — main physics step (integrates forces, updates positions)
- `FUN_1400199c0` — Y-axis physics (gravity, jump arcs, landing detection)
- `FUN_1405ab9a0` — X-axis physics (ground movement, friction, knockback)

The commented-out simulation loop shows that simply re-ticking the character
FSM without also re-running the physics step produced incorrect results:
```
//Physics doesn't work during runahead; This should in theory fix it. But doesn't
```

This was a design-stage observation. Current rollback is not only an input hook:
`BuildRegions` captures character, timing, collision, hit-solver, manager, and
external allocation state, and restore applies subsystem-specific policies.

---

## Input Flow: End-to-End

```
Physical Device (XInput)
        ↓
sPad::padPollAllSlots           ← FUN_1402b41b0 territory
        ↓
sPad slot 0..3 raw data
        ↓
GetsCurrentInput_withSOCD(pad, playerIndex)
        ↓
cChrInput__processFrameInput    ← Hook point B (hkprocessFrameInput)
        ↓
  → GetsCurrentInput_withSOCD (routed per-character)
  → sReplay (training mode)
  → sMvc3SyncPad (original online)
        ↓
cChrInput.+0x14 (mNow)          ← Final per-character input value
        ↓
Character FSM reads mNow/mTrg/mPop during vtable+0x2F0 tick
```

---

## Recommendations for Rollback

### Primary: Stick with cChrInput__processFrameInput (current approach)

The existing hook at `0x14002DD60` is the right layer because:

1. **Already works** — the delay-based netplay uses it and functions correctly
2. **Replay-compatible** — training mode dummy recording/playback is handled
   by the original function below the hook
3. **Small data** — 3 uint32 writes per character = 72 bytes/frame total
4. **No routing needed** — the game maps pad→player→character before this hook

### What Was Missing In This Historical Design

The input hook alone did not address physics rollback:

```cpp
// In hkprocessFrameInput — input ONLY
*(uint32_t*)(cChrInput + 0x14)  = value;   // mNow
*(uint32_t*)(cChrInput + 0x560) = value;   // mOrg
*(uint32_t*)(cChrInput + 0x56C) = value;   // mTemp
```

For full rollback you additionally need:
- **Physics state snapshot** (~11 KB from `FUN_140521df0`'s param_1)
- **Character state snapshot** (~28 KB per character × 6)
- **Team state** (cChrTeam × 2, 0x2C0 each)
- **Battle state** (aNextBattle, ~2.2 KB minimum)

### Could Use Both

The pad-level hook (`FUN_1402b41b0`) is useful as a **supplementary recording
mechanism** for debug/verification — capture the raw input alongside the
processed input to verify that the game's routing logic is deterministic.
But for actual input delivery, the character-level hook is superior.
