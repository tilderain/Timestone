# Rollback Netcode: Gameplay State Analysis

## Overview

This document catalogs all game systems (singletons) that contain mutable gameplay
state and must be snapshotted/restored for deterministic rollback netcode in UMvC3.

Each section covers one singleton:
  1. Pointer storage address (read via `getInstance`)
  2. Role in the game
  3. What state it holds
  4. Whether it MUST be snapshotted

---

## Legend

| Priority | Meaning                              |
|----------|--------------------------------------|
| MUST     | Core gameplay state, rollback breaks without it |
| SHOULD   | Affects visible gameplay state, desync risk |
| COSMETIC | Visual/audio only, can skip          |
| SKIP     | Never changes during a match         |

---

## Per-Frame Update Order

Understanding the update order is critical for knowing what state depends on what.
From `sMvc3Main__move` (at `0x1402594A0`):

```
sMain__move()                                   → fps/timing
sMvc3Main__updateManagers_maybe()               → managers dispatch
mpNetPad[+0x401c0].vfn+0x30                     → sync pad processing
mpPlatformUtil[+0x40260].vfn+0x30               → platform utilities
mpResource[+0x401a8].vfn+0x48                   → resource processing
mpTool[+0x40198].vfn+0x48                       → tool processing
mpEffect[+0x401d8].vfn+0x48                     → effect processing
mpCollision[+0x401e0] → FUN_1405BB710            → collision
sMvc3Manager → getGameConfig → vfn+0x48         → game config
sRender → setRenderTargetAspect + sPrimitive     → render setup
mpPrimitive[+0x40270].vfn+0x48                  → primitive
mpGpuParticle[+0x40208].vfn+0x48                → gpu particle
mpRender[+0x40190] → FUN_140537080              → render
mpArea[+0x40200].vfn+0x30                       → area
sMvc3Manager__getAction → delegates to sAction  → ACTION STEP
```

The action step dispatches to the current phase. During fighting (step 5),
`sAction__stepFighting` runs at `0x140007080`.

---

## 1. sReplay — Frame Counter & Replay Data

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x1400047B0`             |
| Ptr storage    | `0x140d4d9d8`             |
| Struct size    | ~0x2341C bytes (~144 KB)  |
| Priority       | **MUST**                  |

**Role:** Master frame counter; replay recording buffer.

**Layout** (from constructor at `0x1401CE880`):
| Offset | Size    | Field                        |
|--------|---------|------------------------------|
| +0x00  | 8       | vtable ptr                   |
| +0x08  | ~0x30   | cSystem base                 |
| +0x38  | 1       | byte flag                    |
| +0x4C  | 8       | qword pointer                |
| +0x54  | 4       | **frame counter** (uint32)   |
| +0x58  | 0x233C4 | replay data buffer (zeroed)  |

**Why it must be snapshotted:** The frame counter at `+0x54` is the master
frame clock. All gameplay logic uses it. Without it, rollback has no timeline.

---

## 2. sCharacter — Character Manager

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x140001AA0`             |
| Ptr storage    | `0x140d44a70`             |
| Priority       | **MUST**                  |

**Role:** Owns all character instances.

**Character iteration pattern** (from `sCharacter__move` at `0x1400597F0`):
```
team (0-1) × slot (0-2)  →  up to 6 characters
```

**Character traversal** (from `sCharacter__setFlagOnAllFighters_*` at `0x140059A50`):
```
sCharacter → getTeam(teamIdx) → cChrTeam
cChrTeam → cChrTeam__getNextNode → first cChrNode (linked list head)
cChrNode → cChrNode__getNext(lVar2) → next node (singly linked via +0x10)
cChrNode → cChrNode__getChr(lVar2) → uCharacter pointer at cChrNode + 0x08
```

**cChrNode layout** (from `cChrNode__createProperty` at `0x140030D70`):
| Offset | Size | Field        |
|--------|------|--------------|
| +0x00  | 8    | vtable       |
| +0x08  | 8    | mpChr (uCharacter*) |
| +0x10  | 8    | mpNext (next cChrNode*) |
| +0x18  | 8    | mpPrev        |
| +0x20  | 8    | mpFatherNext  |
| +0x28  | 8    | mpFatherPrev  |
| +0x30  | 8    | mpParent      |
| +0x38  | 8    | mpChild       |

**uCharacter known fields** (verified via Ghidra):
| Offset   | Size | Field (guess)                |
|----------|------|------------------------------|
| +0x08    | ~64  | MtObject base (position, etc)|
| +0x38    | 4    | cActionTime (speed scale)    |
| +0x439   | ~64  | **cChrStatus** (health, etc) |
| +0x1450  | ~32  | mCnsShareList (constraints)  |
| +0x1470  | 4    | mCnsJointNum                |
| +0x1480  | 4    | HipRate                     |
| +0x14f4  | 4    | flags bitfield              |
| +0x14f8  | 4    | flags bitfield              |
| +0x14fc  | 4    | flags bitfield              |
| +0x150c  | 4    | flags bitfield              |
| +0x1878  | 4    | character/variant index      |
| +0x212c  | 1    | active state byte           |
| +0x2d94  | 4    | frame counter (incremented each frame in moveAfter) |
| +0x414c  | 4    | action/combo value          |
| +0x4200  | 16    | some array/struct           |
| +0x4210  | 0xB00 | array of 16 structs × 0xB0  |
| +0x4d10  | 16*N  | position data block         |
| +0x63e4  | 8     | ptr/state                   |

**cChrStatus layout** (from constructor at `0x1400311E0`,
`cChrStatus__getLife` at `0x14054A250`, `cChrStatus__getLifeMax` at `0x1406476F0`):
| Offset | Size | Field                  | Default   |
|--------|------|------------------------|-----------|
| +0x00  | 8    | vtable (cChrStatus)    |           |
| +0x08  | 8    | unknown                | 0         |
| +0x10  | 8    | unknown                | 0         |
| +0x1c  | 4    | **max life** (int)     | 10000     |
| +0x20  | 4    | **current life** (f32) | 10000.0f  |
| +0x28  | 4    | starting life (f32)    | 10000.0f  |
| +0x?   | 4    | red health             |           |
| +0x?   | 4    | hyper meter[0-3]       |           |

---

## 3. cActionTime — Hitstop / Time Scaling System

| Field          | Value                     |
|----------------|---------------------------|
| Access         | Embedded in sAction (+0x38), aNextBattle (+0x38), and per-character |
| Priority       | **MUST**                  |

**Role:** Controls hitstop (hit pause), blockstun, and time scaling.
When a hit connects, cActionTime freezes gameplay for a duration.
This is CRITICAL for rollback — if hitstop state is wrong, the frame
timing of everything after the hit will desync.

**Layout** (from `cActionTime__createProperty` at `0x140012060`):
| Offset | Size | Field                      | Notes                    |
|--------|------|----------------------------|--------------------------|
| +0x00  | 8    | vtable                     |                          |
| +0x08  | 4    | mHitSlowTime               | hitstop duration         |
| +0x0c  | 4    | mHitSlowDelay              | hitstop delay before start |
| +0x10  | 4    | mHitSlowOrder              | hitstop priority         |
| +0x14  | 4    | **mHitSlow**               | current hitstop remaining |
| +0x18  | 1    | mbHitSlowStart             | bool: has hitstop started |
| +0x20  | 8    | mpHitSlowAtkChr            | attacker character ptr   |
| +0x28  | 8    | mpHitSlowDmgChr            | defender character ptr   |
| +0x38  | 8    | mpPauseChr                 | pause character ptr      |
| +0x40  | 4    | mPauseMode                 | pause mode               |
| +0x44  | 4    | mFinishSlowType            | finish slow type         |
| +0x48  | 4    | mFinishSlowTime            | finish slow remaining    |
| +0x4c  | 4    | mBlockingHitSlowTime       | blockstun duration       |
| +0x50  | 4    | mBlockingHitSlowDelay      | blockstun delay          |
| +0x54  | 4    | mBlockingHitSlowOrder      | blockstun priority       |
| +0x58  | 4    | **mBlockingHitSlow**        | current blockstun remaining |
| +0x5c  | 1    | mbBlockingHitSlowStart     | bool: blockstun started  |
| +0x60  | 4    | mBlockingHitSlowTimeInitW  | blockstun init W (light) |
| +0x64  | 4    | mBlockingHitSlowTimeInitM  | blockstun init M (medium)|
| +0x68  | 4    | mBlockingHitSlowTimeInitS  | blockstun init S (heavy) |
| +0x6c  | 4    | mBlockingHitSlowTimeInitEx | blockstun init EX        |
| +0x70  | 4    | mBlockingHitSlowTimeInitDmgW | blockstun dmg W        |
| +0x74  | 4    | mBlockingHitSlowTimeInitDmgM | blockstun dmg M        |
| ...    | ...  | ...                        | continues with more blockstun init values |

The per-character cActionTime at aNextBattle +0x38 controls global hitstop
during combat. Each character also has its own cActionTime.

**Why snapshot:** The mHitSlow and mBlockingHitSlow fields count down
each frame. If restored incorrectly, a character could skip or repeat
hitstop frames, altering damage, pushback, and follow-up timing.

**Speed scale mechanism** (from `cActionTime__getSpeedScale` at `0x1400128E0`):
The function reads `mFinishSlowTime` (`+0x48`) and returns:
- **1.0f** — if mFinishSlowTime is near -1.0f (no hitstop, normal speed)
- **0.0f** — if mFinishSlowTime is below a threshold (frozen during hitstop)
- **Lookup value** — based on mFinishSlowType (`+0x44`) for slow-motion effects

In `sAction__stepFighting`, the result is applied as:
```
fVar10 = cActionTime__getSpeedScale(param_1 + 0x38);  // sAction's cActionTime
fVar11 = deltaTime * fVar10;                           // scale delta time
```
This scaled delta time drives the frame sync counters (`+0xFC`, `+0x100`,
`+0x104`) and the round timer (`+0xF8`). When the scale is 0, nothing
advances — effectively freezing gameplay for hitstop duration.

---

## 4. cChrInput — Per-Character Input Processor

| Field          | Value                     |
|----------------|---------------------------|
| Access         | Embedded in/pointed-to by uCharacter |
| Priority       | **MUST**                  |

**Role:** Per-character input processing. Reads raw pad input and converts
to logical button state (directional input, triggers, releases, repeats).
This is the interface the character FSM reads.

**Layout** (from `cChrInput__createProperty` at `0x14002D810`
and `cChrInput__processFrameInput` at `0x14002DD60`):
| Offset | Size | Field   | Notes                              |
|--------|------|---------|-------------------------------------|
| +0x00  | 8    | vtable  |                                     |
| +0x08  | 8    | mpOwner | back-pointer to uCharacter          |
| +0x10  | 4    | mPrev   | previous frame input (prev +0x14)   |
| +0x14  | 4    | mNow    | **current frame raw input**         |
| +0x1c  | 4    | mDat    | current directional/button data     |
| +0x20  | 4    | mTrg    | **trigger** (buttons just pressed)  |
| +0x24  | 4    | mPop    | **pop** (buttons just released)     |
| +0xa8  | 4    | mRep    | repeat state                       |
| +0x12C | 4    | mPre    | previous directional input          |
| +0x230 | 4    | mSame   | same-held frame counter             |
| +0x238 | 4    | mBeat   | beat/rhythm counter                 |
| +0x560 | 4    | mOrg    | original raw (before button mapping)|
| +0x564 | 4    | mAct    | active input                        |
| +0x568 | 4    | mPause  | pause input flag                    |
| +0x56c | 4    | mTemp   | temporary (zeroed each frame)       |

**Processing per frame:**
```
mPrev = mNow                          (shift buffer)
mNow  = raw input                     (from pad or replay)
mDat  = directional/buttons           (decoded from mNow)
mTrg  = mDat & ~mPre                  (rising edge: just pressed)
mPop  = ~mDat & mPre                  (falling edge: just released)
mPre  = mDat                          (save for next frame)
```

**Why snapshot:** mTrg (triggers) and mPop (releases) are derived from
the previous frame's input. If cChrInput isn't restored on rollback,
the character will see incorrect button press/release edges and may
execute the wrong moves or miss inputs entirely.

---

## 5. cChrCommand — Command Buffer

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x140153180` (jmp to 0x1401CE000) |
| Priority       | **MUST**                  |

**Role:** Command input buffer. Stores parsed motion command history
for special move detection (QCF, DP, charge, etc.). Gets populated
from cChrInput data. Likely per-character.

---

## 6. cAtkCtrl — Attack Controller

| Field          | Value                     |
|----------------|---------------------------|
| Access         | Embedded in cChrNode/uCharacter? |
| Priority       | **MUST**                  |

**Role:** Per-character attack state controller. Manages current attack
data, combo counting, hit confirm, and target info.

**Layout** (from `cAtkCtrl__createProperty` at `0x14000C810`):
| Offset | Size   | Field          | Notes                  |
|--------|--------|----------------|------------------------|
| +0x00  | 8      | vtable         |                        |
| +0x10  | 8      | mpUnit         | owner character        |
| +0x20  | 8      | mpResource     | resource reference     |
| +0x28  | 4      | mAtkInfoId     | current attack ID      |
| +0x2c  | 4      | mFlag          | attack state flags     |
| +0x30  | 4      | mTotalCombo    | **combo counter**      |
| +0x34  | 4      | mDelayTime     | hitstop frames         |
| +0x38  | 4      | mLifeTime      | attack lifetime        |
| +0x3c  | 4      | mCountTime     | counter time           |
| +0x40  | 4      | mEndTime       | end time               |
| +0x48  | 4      | mAnmNo         | animation number       |
| +0x50  | 0x300  | mTInfo         | **target info array** (target hit state, damage scaling) |
| +0x350 | 0x300  | mTInfoShot     | shot target info       |
| +0x650 | 0x300  | mTInfoEft      | effect target info     |

**Why it must be snapshotted:** The mTotalCombo (combo counter), mDelayTime
(hitstop), and all mTInfo entries (per-target hit confirm state + damage
scaling, each 0x300 bytes) are critical gameplay state that changes every
frame during combat.

---

## 7. cChrAbility — Character Ability State

| Field          | Value                     |
|----------------|---------------------------|
| Priority       | **MUST**                  |

**Role:** Per-character ability/technique state. Stores character-specific
flags like "can air dash", "has used double jump", etc.

**Layout** (from `cChrAbility__createProperty` at `0x1400240A0`):
| Offset | Size   | Field  | Notes                     |
|--------|--------|--------|---------------------------|
| +0x00  | 8      | vtable |                           |
| +0x10  | 0xC0   | mPrim  | array of 6 × 0x20 ability primitives |

---

## 8. uActorGameModel — Animation State (per-character)

| Field          | Value                     |
|----------------|---------------------------|
| Access         | Embedded in uCharacter (inherited) |
| Priority       | **MUST**                  |

**Role:** Character animation state. Controls which animation is playing,
the playback time, playback rate, and animation transitions.

**Layout** (from `uActorGameModel__createProperty` at `0x1402007D0`):
| Offset | Size | Field              | Notes                    |
|--------|------|--------------------|--------------------------|
| +0x1300| 4    | mDeltaTimeCoef     | time coefficient         |
| +0x1304| 4    | mDeltaTimeLocal    | local time step          |
| +0x1310| 4    | **mAnmNo**          | current animation number |
| +0x1314| 4    | mAnmNoOld          | previous animation       |
| +0x1318| 4    | mAnmNoReq          | requested animation      |
| +0x131c| 4    | mAnmTimeReq        | requested animation time |
| +0x1320| 4    | mAnmNoTakeon       | taken-over animation     |
| +0x1328| 4    | **mAnmTime**        | current animation time (subframe) |
| +0x132c| 4    | mAnmTimePass       | animation time elapsed   |
| +0x1330| 4    | **mAnmRate**        | animation playback speed |
| +0x1334| 4    | mAnmWait           | animation wait flag      |
| +0x1338| 4    | mAnmWaitTime       | wait remaining           |
| +0x1348| ?    | mActorGameModelAnm | animation data system    |

**Why snapshot:** If animation state is wrong after rollback, characters
will display wrong animations, hitbox/hurtbox positions will be wrong,
and attack timing (active frames, recovery) will be incorrect.

---

## 9. sShot — Projectile Manager

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x1400047D0`             |
| Ptr storage    | `0x140d47f40`             |
| Priority       | **MUST**                  |

**Role:** Manages all active projectiles. Shot list stored as an `MtArray`
at `sShot + 0x43c` (`mShotList`). Each entry is a pointer to a `uShot`
derived object.

**Known sShot fields:**
| Offset | Size | Field             |
|--------|------|-------------------|
| +0x43c | 4    | mKillOrder        |
| +0x440 | 4    | mMaxSpreadHierarchy |
| +0x444 | 4    | mDeltaTimeCoef    |
| +0x448 | ~32  | mShotList (MtArray of uShot*) |

**uShot per-frame state** (from `uShot__move` at `0x1401CCEB0`):
| Offset | Size | Field                  | Notes                 |
|--------|------|------------------------|-----------------------|
| +0x3eb | 4    | lifetime min (float)   |                       |
| +0x3ec | 4    | lifetime cur (float)   |                       |
| +0x3fc | 4    | flags bitfield         | 0x830033bf = common active flags |
| +0x40f | 4    | state float            | -2 = special state    |
| +0x418 | 12   | position (MtVector3)   |                       |
| +0x41a | 4    | speed multiplier       |                       |
| +0x1fe4| 4    | flags                  | 0x08, 0x80, 0x4000, 0x4000000 |
| +0x1fe8| 4    | flags                  | 0x80 = no-speed-change |
| +0x1fec| 4    | flags                  | 0x1000000             |
| +0x1ffc| 4    | status result          | 0x80000 = killed      |
| +0x2000| 8    | pointer                |                       |
| +0x212c| 1    | active flag byte       | 0x01 = active, 0x00 = inactive |

---

## 10. sHitSolver — Hit Detection

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x14004AFF0`             |
| Ptr storage    | `0x140d44060`             |
| Priority       | **MUST**                  |

**Role:** Hitbox/hurtbox collision solving. Called each frame.

**Layout** (from `sHitSolver__move_0` at `0x1400119C0`):
| Offset | Size    | Field                        |
|--------|---------|------------------------------|
| +0x38  | varies  | collision pair MtArray       |
| +0x30068| 1      | collision processing flag    |
| +0x30070| 0x4008 | collision result buffer      |

**Why it must be snapshotted:** The collision pair array contains active
hitbox/hurtbox contacts. If not restored, hit detection can double-hit
or miss entirely after a rollback.

---

## 11. sGameEffect — Gameplay Effects

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x140004740`             |
| Ptr storage    | `0x140d46470`             |
| Priority       | **SHOULD**                |

**Role:** Manages gameplay effects. Some effects have gameplay impact
(hitstop, screen-freeze, certain projectiles disguised as effects).

**Layout** (from `sGameEffect__createProperty_impl` at `0x140130960`):
| Offset | Size    | Field                        |
|--------|---------|------------------------------|
| +0x38  | 4       | mDeltaTimeCoef               |
| +0x40  | ~0x50   | some array                   |
| +0x90  | 0x90    | 6 effect slots × 0x18 bytes |
| +0x120 | varies  | MtArray of effect pointers   |
| +0x1940| varies  | mHitMarkOrderPtr             |
| +0x1d48| 4       | purge counter flags          |

---

## 12. sAction — Game Phase State Machine

| Field          | Value                     |
|----------------|---------------------------|
| Access         | via `sMvc3Manager__getAction` |
| Priority       | **MUST**                  |

**Role:** Game phase state machine. The per-frame fight step runs in
`sAction__stepFighting` at `0x140007080`.

**Key offsets** (from stepFighting decompilation):
| Offset | Size | Field                 | Notes                     |
|--------|------|-----------------------|---------------------------|
| +0x38  | ~32  | cActionTime           | time scale, frame progress |
| +0xCC  | 4    | countdown initial     | round timer starting value |
| +0xD8  | 4    | step/phase            | 2=intro,3=countdown,5=fighting,6=postRound |
| +0xF8  | 4    | **round timer** (float) | displayed countdown       |
| +0xFC  | 4    | frames P1 (float)     | frame sync counter P1     |
| +0x100 | 4    | frames P2 (float)     | frame sync counter P2     |
| +0x104 | 4    | frames general (float)| frame sync counter gen    |
| +0x10C | 4    | training reset timer  |                          |
| +0x110 | 4    | overtime timer (float)|                          |
| +0x114 | 4    | unknown               | set to 0xFFFF             |
| +0x118 | 4    | countdown active flag |                          |
| +0x11C | 4    | demo/training flag    |                          |
| +0x168 | 8    | ptr with alive check  |                          |
| +0x230 | 8    | ptr with alive check  |                          |

**Step transitions** happen via `sAction__transitionToStep(param_1, newStep)`.

---

## 13. aNextBattle — Battle State (Amaterasu)

| Field          | Value                     |
|----------------|---------------------------|
| Access         | `uAmaterasu__getter_1A90` at `0x140001A90` |
| Ptr storage    | `0x140d47e68`             |
| Priority       | **MUST**                  |

**Role:** The battle singleton. Contains the battle state machine,
game flags, and round tracking. Accessed as `uAmaterasu__getter_1A90()`.

This object is very large (appears to be the entire battle logic engine).
Accessed in stepFighting for game flags, training reset, round loss checks.

---

## 14. sGameUI — HUD / Pause State

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x140001AC0`             |
| Ptr storage    | `0x140d53a18`             |
| Priority       | **MUST**                  |

**Role:** Pause state, HUD display data, training mode UI.

**Why it must be snapshotted:** Contains `effectivePause` state which
determines whether game logic advances. If pause state desyncs between
peers, the simulation will diverge.

---

## 15. sMain — Frame Timing & Pause

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x140003150`             |
| Ptr storage    | `0x140e177e8`             |
| Priority       | **MUST**                  |

**Role:** Frame timing, pause, frame skip.

**Known offsets:**
| Offset | Size | Field                    |
|--------|------|--------------------------|
| +0x78  | 4    | frame skip counter       |
| +0x7c  | 1    | pause flag (byte)        |
| +0x7d  | 1    | pause trigger flag       |
| +0xa4  | 4    | previous frametime       |
| +0xa8  | 4    | current frametime        |
| +0x40028 | 8  | double timing value      |
| +0x40070 | 8  | 1.0/perfCounter          |
| +0x40078 | 8  | perf counter current     |

---

## 16. sMvc3Manager — Game State Manager

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x140001AF0`             |
| Ptr storage    | `0x140d510a0`             |
| Priority       | **SHOULD**                |

**Role:** Central game state coordinator. Holds references to sAction,
GSound (sound manager), and player character indices.

**State to snapshot:**
- Player character index mapping
- Sound state (if gameplay-relevant sounds affect state)

---

## 17. sPad — Input Controller

| Field          | Value                     |
|----------------|---------------------------|
| getInstance    | `0x140001B00`             |
| Ptr storage    | `0x140e174b8`             |
| Priority       | **SHOULD** (if not using own input layer) |

**Role:** Raw input from devices. 4 pad slots × 0x2E0 bytes.

If you're delivering input yourself via the rollback netplay layer,
you don't need to snapshot this — the input state is deterministic
from the saved input frames.

---

## Quick Reference: All Singleton Pointer Addresses

### MUST snapshot (gameplay-critical):

| Singleton       | getInstance    | Ptr storage   | Size (approx) |
|-----------------|----------------|----------------|---------------|
| sReplay         | 0x1400047B0    | 0x140d4d9d8   | ~144 KB       |
| sCharacter      | 0x140001AA0    | 0x140d44a70   | ~48 KB        |
| sShot           | 0x1400047D0    | 0x140d47f40   | ~4 KB + bullets|
| sHitSolver      | 0x14004AFF0    | 0x140d44060   | ~200 KB       |
| sAction         | via sMvc3Mgr   | —             | ~16 KB        |
| aNextBattle     | 0x140001A90    | 0x140d47e68   | ~128 KB       |
| sGameUI         | 0x140001AC0    | 0x140d53a18   | ~64 KB        |
| sMain           | 0x140003150    | 0x140e177e8   | ~256 KB       |

**Per-character MUST snapshot** (×6 characters):
- cChrStatus (health, meter) — ~64 bytes each
- cAtkCtrl (combo, hitstop, target info) — ~0x950 bytes each
- cChrInput (input state) — ~0x570 bytes each  
- cChrAbility (ability flags) — ~0xD0 bytes each
- uCharacter flags (+0x14f4..+0x150c) — ~32 bytes each
- uCharacter animation state (+0x1300..+0x1400) — ~256 bytes each
- cActionTime per-character (hitstop/blockstun) — ~0x80 bytes each

### SHOULD snapshot:

| Singleton       | getInstance    | Ptr storage   |
|-----------------|----------------|----------------|
| sGameEffect     | 0x140004740    | 0x140d46470   |
| sMvc3Manager    | 0x140001AF0    | 0x140d510a0   |
| sBattleSetting  | 0x140004700    | 0x140d4f718   |
| sPad            | 0x140001B00    | 0x140e174b8   |

### SKIP (never changes during a match, or visual-only):

| Singleton       | getInstance    | Ptr storage   |
|-----------------|----------------|----------------|
| sGameConfig     | 0x140001AB0    | 0x140d50f88   |
| sId             | 0x140001AD0    | 0x140d51218   |
| sLicense        | 0x140001AE0    | 0x140d522d8   |
| sResource       | 0x140001B10    | 0x140e175a8   |
| sApp            | 0x140003140    | 0x140e17758   |
| sUnit           | 0x140003160    | 0x140e17698   |
| sAgentSession   | 0x1400046E0    | 0x140d52158   |
| sArea           | 0x1400046F0    | 0x140e17a80   |
| sCamera         | 0x140004710    | 0x140e17930   |
| sEarthAtk       | 0x140004720    | 0x140d52220   |
| sLicense (2)    | 0x140004750    | 0x140d52268   |
| sMission        | 0x140004760    | 0x140d471f8   |
| sMvc3SyncPad    | 0x140004770    | 0x140d523b0   |
| sNetworkLibrary | 0x140004780    | 0x140d6c748   |
| sRender         | 0x1400047A0    | 0x140e179a8   |
| sMessage        | 0x1400047C0    | 0x140d6c6c8   |
| sStage          | 0x1400047E0    | 0x140d4f718   |
| sMvc3Vibration  | 0x1400047F0    | 0x140e17868   |
| sMvc3Setting    | 0x140037060    | 0x140d537c8   |
| sCollision      | 0x14004AFE0    | 0x140e18250   |
| sSound          | 0x140056030    | 0x140e18520   |
| sAI             | 0x1401CFF50    | 0x140e18ee8   |
| sPrimitive      | 0x1401E0CA0    | 0x140e193e8   |
| sMvc3Ranking    | 0x14020B9E0    | 0x140d52360   |
| sNetwork        | 0x140251F00    | 0x140e198b0   |
| sGpuParticle    | 0x1402585C0    | —             |
| sShadow         | 0x140258790    | 0x140e1a8c0   |
| sSyncPoint      | 0x1402587E0    | 0x140e1b468   |
| sMouse          | 0x14025C5A0    | 0x140e1a220   |
| sMvc3Scene      | 0x140452680    | 0x140e1bbc0   |
| sArea (2)       | 0x140545340    | 0x140e17a88   |

---

## Practical Rollback Snapshot Strategy

### Approach A: Coarse (save entire singleton regions)

Before each frame update, memcpy these regions:

1. **sReplay** — entire struct (~144 KB at `*0x140d4d9d8`)
2. **sCharacter** — entire struct (contains 6 character nodes + teams)
3. **sShot** — entire struct + all uShot objects in mShotList
4. **sHitSolver** — entire struct (contains collision pairs + 0x4008 buffer)
5. **aNextBattle** — entire battle object
6. **sGameUI** — HUD/pause state
7. **sMain** — timing/pause
8. **sGameEffect** — effect list (optional)

### Approach B: Targeted (save only what changes)

**Per character (×6):**
- cChrStatus (health, meter) — ~64 bytes  at uCharacter +0x439
- uCharacter flags (+0x14f4..+0x150c) — ~32 bytes
- uCharacter animation state (+0x1300..+0x1400) — ~256 bytes
- uCharacter position/velocity (MtObject base) — ~64 bytes
- uCharacter frame counter (+0x2d94) — 4 bytes
- cActionTime (hitstop/blockstun at uCharacter +0x?) — ~0x80 bytes
- cAtkCtrl (combo, hitstop, target info) — ~0x950 bytes
- cChrInput input buffer — ~0x570 bytes
- cChrAbility — ~0xD0 bytes
- uCharacter body (everything from +0x08 to +0x4D10) — ~20 KB

**Per projectile:**
- uShot header + state — ~0x2000 bytes each

**Per-frame globals:**
- sReplay frame counter (+0x54) — 4 bytes
- sAction round timer (+0xF8) — 4 bytes
- sAction step (+0xD8) — 4 bytes
- sAction frame syncs (+0xFC, +0x100, +0x104) — 12 bytes
- sMain pause state (+0x7c) — 1 byte
- aNextBattle cActionTime (+0x38 onwards) — ~0x80 bytes

### Size estimation (Approach A):

| Singleton    | Size estimate |
|--------------|---------------|
| sReplay      | ~144 KB       |
| sCharacter   | ~48 KB        |
| sShot        | ~4 KB + bullets |
| sHitSolver   | ~200 KB       |
| aNextBattle  | ~128 KB       |
| sGameUI      | ~64 KB        |
| sMain        | ~256 KB       |
| sGameEffect  | ~16 KB        |
| **Total**    | **~700-900 KB per frame** |

### Size estimation (Approach B, targeted):

| Component            | Size estimate |
|----------------------|---------------|
| 6 × uCharacter       | ~120 KB (20 KB each) |
| Active projectiles   | ~16 KB        |
| sReplay frame cnt    | 4 bytes       |
| sAction state        | ~256 bytes    |
| sMain pause          | 1 byte        |
| **Total**            | **~140 KB per frame** |

### Memory pool sizing:

For a 10-frame rollback window: ~1.4 MB (targeted) to ~9 MB (coarse).
At 60fps this is trivially small; even at 16 frames it's ~2.2 MB.

### Determinism verification:

The game already has CRC checking (`MsgCrcData` in `sMvc3SyncPad`).
After each frame, hash the saved state and exchange checksums with
the remote peer. A mismatch means the simulation has diverged.

### Practical implementation roadmap:

1. **Hook the frame boundary** — find where to save before and restore after
2. **Implement memcpy snapshot** — coarse save of all MUST regions
3. **Frame counter** — sync sReplay + 0x54 as the master timeline
4. **Input delivery** — replace sPad reads with rollback input buffers
5. **Restore** — on late input arrival, restore snapshot and re-simulate
6. **CRC check** — hash and compare with peer each frame
7. **Optimize** — shrink snapshot size, use copy-on-write if needed
