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
`sAction__stepFighting` runs at `0x140007080`, which calls
`uActionSideCamera__classFunc` at `0x140006370` for the inner update:

```
# Inner fighting step pipeline (uActionSideCamera__classFunc):
1. Clear flags (+0xC4, +0x114, +0xF4)
2. Reset round timer from config  (+0xF8)
3. Clear frame sync counters      (+0xFC, +0x100, +0x104)
4. Reset cActionTime              (sAction +0x38)
5. sShot: clear stale shots       (sShot__createProperty_B80)
6. sShot: process special types   (uShotShieldOuroboros)
7. sGameEffect: clear stale       (sGameEffect__createProperty_6B0)
8. sGameEffect: purge             (sGameEffect__purgeStaleEffects)
9. sCharacter: tick each fighter  → vtable+0x2f0 per character
10. Process tag swaps             (character_tag_swap_orchestrator)
11. Update positions              (cChrNode__updatePosition_maybe)
12. Team-level functions          (FUN_140021510 per team)
13. Training reset timer check
14. FUN_14005a3e0                 (character post-update)
15. Camera update
```

**CRITICAL ORDER:** Shots and effects are cleared BEFORE character ticks.
Characters create new projectiles/effects during their tick, which are then
valid for the rest of the frame. Tag swaps happen AFTER character ticks
(characters set their swap requests during the tick, then swaps execute).

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

**Training mode replay loop** (from `sReplay__move_trainingLoop` at `0x1401CEF28`):
- Frame counter at `+0x194` per team slot
- Team replay slots are `0x23288` bytes apart
- Plays back recorded inputs during training dummy recording
- Uses `sReplay__updatePerTeam()` for each team

**Total sReplay size**: `~0x233C4` bytes (from constructor memset).
This includes the replay data buffer for storing entire match input logs.

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

**Layout** (from `sCharacter__getTeam` at `0x140001B60`):
```
sCharacter + 0x40 + teamIndex * 0x2C0  →  cChrTeam (0x2C0 bytes each)
```

- sCharacter + 0x40       = Team 0 (cChrTeam, 0x2C0 bytes)
- sCharacter + 0x300      = Team 1 (cChrTeam, 0x2C0 bytes)
- Total sCharacter size ≈ 0x5C0 + header = ~2-4 KB (manager) + 6 character nodes

**Per-frame character dispatch** (from `FUN_14005a210` at `0x14005A210`):
```c
for (teamIdx = 0; teamIdx < 2; teamIdx++) {
    team = sCharacter__getTeam(param_1, teamIdx);
    team_state_reset(team, 0);
    for (node = cChrTeam__getNextNode(team); node != 0; node = cChrNode__getNext(node)) {
        chr = cChrNode__getChr(node);
        chr->vtable->tick(chr);           // vtable call at +0x2F0
    }
}
```

This is the per-frame character FSM update. Each character's vtable+0x2F0
dispatches to its character-specific tick function (e.g. `uFirebrand__tick`
at `0x1400BE8F0`), which handles:
- State checks (action conditions, hit state, airborne, etc.)
- Action transitions (block, hitstun, attack startup/active/recovery)
- Weapon/effect updates
- Animation progress

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

**cChrTeam layout** (from `cChrTeam__createProperty` at `0x140020710`,
`cChrTeam__cChrTeam_ctor` at `0x14001FEE0`):
| Offset | Size | Field                 | Init/Notes                |
|--------|------|-----------------------|---------------------------|
| +0x00  | 8    | vtable                |                           |
| +0x08  | 0x40 | mNode[3]              | 3 cChrNode entries (0x14 bytes each + padding) |
| +0x48  | 8    | **mpCurrentChr**       | currently active uCharacter ptr |
| +0x50  | 4    | mDeltaTime            | team time delta           |
| +0x54  | 4    | mDeltaTimeOrder       | init -1.0f                |
| +0x58  | 4    | **mFlag**             | team flags (uint32)       |
| +0x5C  | 4    | **mAlive**            | alive character count     |
| +0x60  | 4    | **mAliveMember**      | alive member bitmask      |
| +0x68  | 4    | **mLifeTotal**        | total team health (f32)   |
| +0x6C  | 4    | **mLifeMaxTotal**     | max team health (f32)     |
| +0x70  | 4    | mLifeRate             | health rate multiplier    |
| +0x74  | 4    | mLifeRate2            | health rate 2             |
| +0x78  | 4    | **mGauge**            | hyper meter (0-50000 f32) |
| +0x7C  | 4    | mGaugeDefault         | default gauge (init 10000)|
| +0x80  | 4    | mGaugeAdd             | gauge to add              |
| +0x84  | 4    | mHitLast              | last hit damage           |
| +0x8C  | 4    | mHitTotal             | total hit damage          |
| +0x90  | 4    | **mHitCount**         | hit counter               |
| +0x94  | 4    | mHitCountFrame        | hits this frame           |
| +0x98  | 4    | mDmgTotal             | total damage dealt        |
| +0xA8  | 4    | **mChangeEnable**     | tag change enabled        |
| +0xAC  | 4    | mChangeEnable2        | tag change enabled 2      |
| +0xB0  | 4    | **mAssist**           | assist type              |
| +0xB4  | 4    | **mAssistUse**        | assist remaining          |
| +0xB8  | 4    | mHVRCount             | HVR count                 |
| +0xBC  | 4    | mDHCCount             | DHC count                 |
| +0xC0  | 4    | **mXFactorTime**      | X-Factor remaining time   |
| +0xC4  | 4    | mXFactorSetTime       | X-Factor set time         |
| +0xC8  | 4    | mXFactorSetTimeLv1    | X-Factor time Lv1         |
| +0xCC  | 4    | mXFactorSetTimeLv2    | X-Factor time Lv2         |
| +0xD0  | 4    | mXFactorSetTimeLv3    | X-Factor time Lv3         |
| +0xD4  | 4    | mXFactorResAddTime    | X-Factor additional time  |
| +0xD8  | 4    | **mXFactorLv**        | X-Factor level (1-3)      |
| +0xDC  | 4    | **mXFactorLvNow**     | current X-Factor level    |
| +0xE0  | 4    | mInputSwitch          | input switch flags        |
| +0xE4  | 4    | mCPlrId               | current player ID         |
| +0xE8  | 4    | **mCAnmNo**           | current action number     |
| +0xEC  | 4    | mCAnmNoReq            | requested action          |
| +0xF0  | 4    | mCAnmNoHVR            | HVR action number         |
| +0xF4  | 4    | mCAnmTimePass         | animation time passed     |
| +0xF8  | 4    | **mCAnmF**            | current animation flags   |
| +0xFC  | 4    | **mCPrgF**            | current program flags     |
| +0x100 | 4    | **mCEtcF**            | current etc flags         |
| +0x104 | 4    | **mCDmgF**            | current damage flags      |
| +0x110 | 12   | **mCPos** (Vector3)   | current character position|
| +0x120 | 12   | mCAngle (Vector3)     | current character angle   |
| +0x130 | ~190 | mPlayData             | replay/playback data      |

**cChrTeam must be snapshotted** because team-level state (health, meter,
X-Factor, assist counts, active character tracking) is essential for
deterministic simulation. These values change every frame during combat.

**uCharacter verified layout** (from `cChr__createProperty` at `0x1400511D0`):

| Offset   | Size | Field               | Notes                      |
|----------|------|---------------------|----------------------------|
| +0x00    | ~64  | vtable + MtObject   | position in MtObject base  |
| +0x38    | 4    | cActionTime         | speed scale/hitstop        |
| +0x439   | ~64  | cChrStatus          | health, meter, flags       |
| +0x1300  | ~80  | **Animation state** | mDeltaTimeCoef, mAnmNo, mAnmTime, mAnmRate |
| +0x1450  | ~32  | mCnsShareList       | constraint data            |
| +0x1470  | 4    | mCnsJointNum        | constraint joint count     |
| +0x1480  | 4    | HipRate             | hip rate                   |
| +0x14f0  | 4    | **mChrF**           | character flags            |
| +0x14f4  | 4    | **mAnmF**           | animation flags            |
| +0x14f8  | 4    | **mPrgF**           | program flags              |
| +0x14fc  | 4    | **mNotF**           | note flags                 |
| +0x1500  | 4    | mNo2F               | note 2 flags               |
| +0x1504  | 4    | mDmgF               | damage flags               |
| +0x1508  | 4    | mEtcF               | etc flags                  |
| +0x150c  | 4    | mEt2F               | etc 2 flags                |
| +0x1510  | 4    | mUsrF               | user flags                 |
| +0x1514  | 4    | mDbgF               | debug flags                |
| +0x1560  | 4    | **mStateCheck**     | state check value (read by FUN_14005a4f0, combined with flag masks) |
| +0x1878  | 4    | char/variant idx    | character/variant index    |
| +0x212c  | 1    | active byte         | active state               |
| +0x29e   | 2    | **mAssistFlags**    | assist entry/DHC condition flags |
| +0x29f   | 1    | mActionFlags1      | action state sub-flags     |
| +0x2a0   | 1    | mActionFlags2      | more action flags          |
| +0x2a1   | 1    | mActionFlags3      | char-specific state flag   |
| +0x2ac   | 4    | **mStateCtx**      | state context / state check parameter |
| +0x2d94  | 4    | frame counter       | incremented per frame      |
| +0x2d9c  | 4    | **mId**             | character ID               |
| +0x2da0  | 4    | mPlrId             | player ID                  |
| +0x2da4  | 4    | mTeamId            | team ID                    |
| +0x2da8  | 4    | mSettingId         | setting/slot               |
| +0x2dac  | 4    | mDefMemberId       | default member             |
| +0x2db0  | 4    | mMemberId          | current member             |
| +0x2db8  | 4    | mBody              | body type                  |
| +0x2dbc  | 4    | mAssistType        | assist type                |
| +0x2dc0  | 4    | mLanguage          | language setting            |
| +0x3560  | 8    | mpResCamera        | resource camera ptr        |
| +0x3ed0  | 4    | mFaceParts         | face part                  |
| +0x3f40  | 8    | **weaver ptr**     | weaver factory pointer     |
| +0x3f48  | 4    | **mAnmNoReqAlive** | requested anim (alive)     |
| +0x3f4c  | 4    | **mAnmNoReqStd**   | requested anim (standard)  |
| +0x3f50  | 4    | **mAnmNoReqCmd**   | requested anim (command)   |
| +0x3f5c  | 4    | mDeltaRateAnm      | animation rate delta       |
| +0x3f60  | 4    | mMotMode           | animation mode             |
| +0x3f64  | 4    | mNextMotBlend      | next motion blend          |
| +0x3f68  | 4    | mThrowAnmNo        | throw animation            |
| +0x3f6c  | 4    | mTdownAnmNo        | knockdown animation        |
| +0x3f78  | 4    | mThrowAnmNoEPLast  | throw EP last              |
| +0x3f7c  | 4    | mTdownAnmNoEPLast  | knockdown EP last          |
| +0x3f80  | 4    | mEPCnt             | extra part count           |
| +0x40c0  | 12   | **mMove** (Vec3)   | movement vector            |
| +0x40e0  | 4    | **mFSpd**          | forward speed              |
| +0x40e4  | 4    | **mFAcl**          | forward acceleration       |
| +0x40e8  | 4    | **mYSpd**          | Y speed                    |
| +0x414c  | 4    | action/combo       | action/combo value         |
| +0x4200  | 8    | mpHitboxArray      | hitbox array ptr           |
| +0x4210  | 0xB00| **16 hits × 0xB0** | hitbox/hurtbox array       |
| +0x4d10  | 0x100 | **16 × Vec3**     | position data block        |
| +0x6398  | 4    | mLastDComboRate    | last damage combo rate     |
| +0x63b4  | 4    | **mDmgSta**        | damage status              |
| +0x63b8  | 4    | mDmgPowLv          | damage power level         |
| +0x63bc  | 4    | mDmgPoint          | damage point               |
| +0x63c0  | 4    | mDmgMaterial       | damage material            |
| +0x63c4  | 4    | mDmgDir            | damage direction           |
| +0x63c8  | 4    | **mAirTime**       | air time in frames         |
| +0x63cc  | 4    | mAirTimeOfs        | air time offset            |
| +0x63d0  | 4    | mAirDist           | air distance               |
| +0x63d4  | 4    | mAirSpeed          | air speed                  |
| +0x63d8  | 4    | mAirFallAcl        | air fall acceleration      |
| +0x63dc  | 4    | **mDmgHitCount**   | damage hit count           |
| +0x63e0  | 4    | mDmgAirHitCount    | air hit count              |
| +0x63e4  | 4    | **mDmgHeap**       | damage heap/value          |
| +0x63ec  | 4    | mDmgTime           | damage time (hitstun)      |
| +0x63f0  | 4    | mDmgAirTime        | air damage time            |
| +0x63f4  | 4    | **mComboDmgTotal** | total combo damage         |
| +0x63f8  | 4    | mStunCharge        | stun charge                |
| +0x63fc  | 4    | mSameTdownNum      | same knockdown count       |
| +0x68fd  | 1    | mExtraFlag         | per-character extra flag (Firebrand's fire state) |
| +0x6908  | 8    | mpExtraWeapon      | per-character weapon/effect pointer |
| ...      | ...  | ...                | character-specific fields extend beyond base offset |

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
| DTI registers  | `0x1409E5CA0`             |
| DTI class      | `0x14005A840`             |
| DTI size       | **8 bytes** (= sizeof(MtObject), no own data) |
| Priority       | **SKIP**                  |

**Role:** Command input buffer. Stores parsed motion command history
for special move detection (QCF, DP, charge, etc.).

**DTI registration** at `0x1409E5CA0`:
```c
cChrCommand__MyDTI__MyDTI(&cChrCommand__DTI, "cChrCommand", MtObject__getDTI(), 8, 0, 0);
```

The **size is 8** — just the vtable pointer. This means cChrCommand
has no local data storage beyond MtObject. The actual command history
is processed through other means (cChrInput data in uCharacter, or
the command processor runs inline in the character FSM).

**Conclusion:** cChrCommand does NOT need its own snapshot. The
motion command state is derived from cChrInput (already captured)
and the character's current action/state (already captured).

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
| +0x1308| 4    | mDeltaTime         | time delta               |
| +0x1310| 4    | **mAnmNo**          | current animation number |
| +0x1314| 4    | mAnmNoOld          | previous animation       |
| +0x1318| 4    | mAnmNoReq          | **requested animation**  |
| +0x131c| 4    | mAnmTimeReq        | requested animation time |
| +0x1320| 4    | mAnmNoTakeon       | taken-over animation     |
| +0x1328| 4    | **mAnmTime**        | current animation time (subframe, f32) |
| +0x132c| 4    | mAnmTimePass       | animation time elapsed   |
| +0x1330| 4    | **mAnmRate**        | animation playback speed |
| +0x1334| 4    | mAnmWait           | animation wait flag      |
| +0x1338| 4    | mAnmWaitTime       | wait remaining           |
| +0x133c| 4    | mAnmSerial         | animation serial number  |
| +0x1340| 4    | mWaitMotType       | wait motion type         |

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

**Role:** Manages all active projectiles. Shots are stored in a per-team
linked list. The list heads are at a fixed global address (relative to the
sShot singleton offset +0x450).

**Shot list structure:**
- List heads at `0x140d47f90 + team * 40` (0x140d47f90 = sShot + 0x450)
- Team 0 head at `0x140d47f90`
- Team 1 head at `0x140d47fa8`
- Each is a cChrNode-like linked list (40 bytes per team head struct)
- Traverse via `node + 0x10` (next pointer), like cChrNode

**Per-shot linked list node** (from `uShot__getListNode` at `0x1401D5E00`):
- List node is at `uShot + 0x1450` (returned by `LEA RAX, [RCX + 0x1450]`)

**Shot list iteration:**
```
head = *(qword*)(0x140d47f90 + teamIdx * 40)  // first shot node
while head:
    uShot = (byte*)head - 0x1450               // back-calculate uShot*
    // update uShot
    head = *(qword*)(head + 0x10)              // next node in list
```

**Known sShot fields:**
| Offset | Size | Field             |
|--------|------|-------------------|
| +0x38  | 0x400 | frame order array (256 × uint32) |
| +0x438 | 4    | frame order index |
| +0x43c | 4    | mKillOrder        |
| +0x440 | 4    | mMaxSpreadHierarchy |
| +0x444 | 4    | mDeltaTimeCoef    |
| +0x448 | 8    | spin lock (?lock) |
| +0x450 | 40   | Team 0 shot list head |
| +0x478 | 40   | Team 1 shot list head |

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
| +0x1450| 8+32  | linked list node       | cChrNode-like: +0x10=next, +0x08=prev |
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

**Online frame sync** (in `sAction__stepFighting`):
The function already has a branch for online mode:
- **Offline**: Frame sync counters (+0xFC, +0x100, +0x104) are accumulated
  from `deltaTime * cActionTime__getSpeedScale()` using sMain timing.
- **Online**: Frame sync values are read from `sMvc3SyncPad` via the
  `network_get_frame_sync()` mechanism (the original GGPO-sync layer).

The existing netplay bypasses this by delivering inputs directly through
the netplay module without using the sync pad. For proper rollback, you
will need to feed these counters yourself based on your frame timing.

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

**Layout** (from `sGameUi__createProperty` at `0x1402BA290`):
| Offset | Size | Field                     | Notes            |
|--------|------|---------------------------|------------------|
| +0x5790| 4    | mMenuPhase                | menu phase       |
| +0x5798| 4    | mReturnPhase              | return phase     |
| +0x579c| 4    | mMenuResponse (f32)       | menu response    |
| +0x57a0| 4    | mMenuInputResponse (f32)  | input response   |
| +0x57a4| 4    | mMenuOkResponse (f32)     | OK response      |
| +0x57a8| 4    | mMenuMoveCam (f32)        | camera movement  |
| +0x57b0| 4    | mMenuCursorPos            | cursor position  |
| +0x5ab0| 8    | pause UI pointer          | checked for null |
| +0x5ab8| 1    | **mPauseMenuPause**       | effective pause  |
| +0x5ab9| 1    | **mPauseMenuReq**         | pause request    |
| +0x5aba| 1    | mPauseMenuState           | pause state      |
| +0x5abb| 1    | **mPauseChange**          | pause changed    |
| +0x5c3c| 4    | mChrSelectDecideFrame     | char select frame|
| +0x5c48| 4    | mSearchCancelResponse (f32)| search cancel   |
| +0x5c9c| 4    | mPopupNetworkBattleWait   | popup handle     |
| +0x5ea0| 4    | mTSCheckPhase             | TS check phase   |
| +0x5ea8| 4    | mTSCheck (bool)           | TS check flag    |

**Why it must be snapshotted:** Contains `mPauseMenuPause` (+0x5ab8) which
determines whether game logic advances. If pause state desyncs between
peers, the simulation will diverge. The menu/misc fields don't change
during gameplay.

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
GSound (sound manager), and player character indices. The `getAction`
method returns the current sAction singleton used for phase dispatch.

**Layout:**
```
sMvc3Manager → getAction → sAction  (delegates step to current phase)
sMvc3Manager → getGameConfig → sGameConfig
```

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
| ↓ cChrTeam(×2)  | in sCharacter  | +0x40/+0x300  | 0x2C0 each    |
| sShot           | 0x1400047D0    | 0x140d47f40   | ~4 KB + bullets|
| sHitSolver      | 0x14004AFF0    | 0x140d44060   | ~200 KB       |
| sAction         | via sMvc3Mgr   | —             | ~16 KB        |
| aNextBattle     | 0x140001A90    | 0x140d47e68   | ~128 KB       |
| sGameUI         | 0x140001AC0    | 0x140d53a18   | ~64 KB        |
| sMain           | 0x140003150    | 0x140e177e8   | ~256 KB       |

**Per-character MUST snapshot** (×6 characters):
- **uCharacter bulk** (entire object) — ~0x8000 each (use DTI size)
- cChrStatus (health, meter) — ~64 bytes each at uCharacter +0x439
- cAtkCtrl (combo, hitstop, target info) — ~0x950 bytes each
- cChrInput (input state) — ~0x570 bytes each
- cChrAbility (ability flags) — ~0xD0 bytes each
- uCharacter flags (+0x14f0..+0x1514) — ~40 bytes each
- uCharacter assist flags (+0x29e..+0x2ac) — ~16 bytes each
- uCharacter animation state (+0x1300..+0x1400) — ~256 bytes each
- uCharacter damage/combo state (+0x6398..+0x63FC) — ~104 bytes each
- cActionTime per-character (hitstop/blockstun) — ~0x80 bytes each

### SHOULD snapshot:

| Singleton       | getInstance    | Ptr storage   |
|-----------------|----------------|----------------|
| sGameEffect     | 0x140004740    | 0x140d46470   |
| sMvc3Manager    | 0x140001AF0    | 0x140d510a0   |
| sBattleSetting  | 0x140004700    | 0x140d4f718   |
| sPad            | 0x140001B00    | 0x140e174b8   |

### SKIP (never changes during a match, or visual-only):

**sStage analysis** (from `sStage__createProperty` at `0x1401F0520`,
constructor at `0x1401F0110`):
| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| +0x00  | 8    | vtable | |
| +0x38  | 4    | (cSystem base) | |
| +0x3C  | 4    | mDeltaTimeCoef (f32) | init 1.0f, cosmetic timing adjust |
| +0x40  | 4    | mStateFlags (uint32) | bit 2 = scheduler pause |
| +0x2048| 8    | mpBackupData | stage ref set at init, static during match |
| +0x2050| 8    | mpStageObj1 | alive-tracked ptr, null during combat |
| +0x2058| 8    | mpStageObj2 | alive-tracked ptr, null during combat |
| +0x2060| ~0x10| MtArray | stage object children (visual) |

sStage has no gameplay-essential mutable state. Destructible background
objects (e.g., Training stage walls) are managed entirely by the stage's
own objects, not by sStage itself. **SKIP** for rollback.

**sMvc3SyncPad analysis** (getInstance at `0x140004770`, ptr `0x140d523b0`):
Contains the `cCommunicationTool` with message types for the original GGPO
sync layer (MsgInputActData, MsgCrcData, MsgInputData, MsgInputCmd, etc.).
The game's existing netplay bypasses this layer entirely. **SKIP** since
you will provide your own sync mechanism through rollback netcode.

**Gameplay RNG check:**
No gameplay-affecting random number generator was found in the Ghidra
analysis. All identified RNG functions are non-gameplay:
- `sChrSelect__randomPickFromReplaySeed` — character select only
- `RandomCharacterPoolFillForArcadeMode` — arcade mode team gen
- `RandomColorIDGenerator` — cosmetic color selection
- `sLicense__correctionRandomGet` — license/achievement system

During combat, the game is fully deterministic given the same inputs.
No RNG state needs snapshotting.

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

## Character-Specific Persistent State

Some characters create persistent objects that must also be rolled back:

| Character | Object | How it's tracked |
|-----------|--------|-------------------|
| **Doom** | Hidden Missiles | Created as uShot objects via the weaver at uCharacter +0x3f40; exist in sShot's per-team shot list |
| **Dr. Strange** | Eye of Agamotto / Daggers | Created via weaver; some are uShot, others are uGameEffect entries |
| **Phoenix** | Dark Phoenix transformation | Flag in uCharacter +0x14f8 (mPrgF) bit 0x400000; separate character state machine |
| **Dormammu** | Dark Hole / Purification | uShot projectiles + persistent status effect on the opponent |
| **Chris** | Weapons (handgun, shotgun, etc.) | Per-character weapon slot managed by weaver at +0x3f40 |
| **Rocket Raccoon** | Spitfire / traps | uShot projectiles with custom timers |
| **Arthur** | Armor pieces | Checked/set via flags at uCharacter +0x14f4, +0x14f8 |
| **Haggar** | Pipes | Weapon object managed via weaver |
| **Taskmaster** | Weapon mimic | State tracked via character-specific fields in uCharacter +0x.. |
| **Amaterasu** | Weapon types | Flag-based in uCharacter +0x14f4..+0x150c |
| **Zero** | Ryuenjin / Sougenmu state | Flag-based + custom projectile behavior |
| **Strider** | Orbs (Oroboros) | Created as uShot projectiles from weaver |
| **Ghost Rider** | Chains | Persistent hitbox state managed by weaver |
| **Nemesis** | Tentacles | Created as ongoing hitbox arrays |
| **Frank West** | Level-up | Combo-based level counter (potentially in cAtkCtrl mTotalCombo or character-specific field) |

**Weaver at uCharacter +0x3F40:**
The weaver is a **unit group factory** that creates character-specific runtime
objects (weapons, projectiles, persistent effects). Each character has a
`getUnitGroupOverride()` vtable function that returns a unit group ID:
- Akuma = 0x24, Felicia = 0x22, Doom = 0x31, etc.
- These IDs map to unit groups that define what objects a character can spawn.
- The weaver itself does NOT store mutable state — it delegates to the
  resource system and creates objects that appear in sShot/sGameEffect lists.

**Practical note:** Most character-specific state is stored either in:
1. uCharacter flag fields (+0x14f0 to +0x1514) — already captured with per-character snapshot
2. uCharacter unit group/weaver (+0x3f40) — creates new uShot objects that appear in sShot's lists
3. Custom fields in the uCharacter-derived class's own memory region (beyond the base uCharacter size)

When taking coarse snapshots of the entire uCharacter object, ALL of this is captured automatically.
Only targeted snapshots need to worry about tracking which specific fields each character uses.

---

## 18. Training Mode State

| Field          | Value                     |
|----------------|---------------------------|
| Access         | sGameUI + sAction + sPad  |
| Priority       | **SHOULD**                |

Training mode includes:
- **Dummy recording/playback** — stored in sReplay replay slot data (+0x58 onwards)
- **Training reset flash state** — sAction +0x10C
- **Training menu settings** — stored in sGameUI (dummy action, guard mode, etc.)

**Important:** Training mode does NOT have a "save frame state and restore"
mechanism. The `sAction__stepTrainingReset` function (at `0x140009DF0`)
works by:
1. Purging stale units, shots, and effects
2. Reinitializing the stage
3. Restarting the match from battle settings

There is no pre-existing byte-level save/restore for training mode. For
rollback, this means you must implement your own state save/restore system
(the snapshot system itself is the save state).

For rollback, training mode settings don't change during simulation, but
training reset state and save-state positions should be included to prevent
desyncs if training reset is triggered during rollback.

---

## 19. The Frame Update Pipeline (Summary)

From `sMvc3Main__move` at `0x1402594A0`, the per-frame update order is:

```
1. sMain__move()                    → fps/timing, pause state
2. sMvc3Main__updateManagers_maybe()→ resource streaming
3. mpNetPad (+0x401c0).vfn+0x30    → sync pad (network input sync)
4. mpPlatformUtil (+0x40260).vfn+0x30
5. mpResource (+0x401a8).vfn+0x48  → resource processing
6. mpTool (+0x40198).vfn+0x48      → tool/system dispatch
7. mpEffect (+0x401d8).vfn+0x48    → sGameEffect update
8. mpCollision (+0x401e0)           → collision detection
9. sGameConfig update               → game config dispatch
10. sRender + sPrimitive setup      → rendering prep
11. sMvc3Manager__getAction         → sAction move dispatch
12.   └→ stepFighting (step 5)      → round timer, frame sync, training
```

The actual character update (`uCharacter`, `uShot`, `sHitSolver`) happens inside
the sAction stepFighting dispatch, which calls:
- `sCharacter__iterateAllFighters(vfn+0x2f0)` — per-character state machine update
- sShot linked-list iteration — per-shot update
- sHitSolver — collision resolution

---

## 20. cChrFlag — Character Flag Instances

| Field          | Value                     |
|----------------|---------------------------|
| Factory        | `0x1401ED5B0`             |
| DTI            | `0x140d4f338`             |
| Priority       | **SHOULD**                |

**Role:** Per-character flag instance management. Simple DTI-based object
with no extra properties beyond vtable. Not snapshot-critical since
per-character flags are already captured in uCharacter's `+0x14f0..+0x1514`.

---

## 21. Estimated uCharacter Object Size

| Measurement | Value |
|-------------|-------|
| Lowest offset used | +0x00 (vtable) |
| Confirmed offsets up to | +0x6908 (mpExtraWeapon) |
| Flag field region | +0x14f0..+0x1514 (40 bytes of flags) |
| Assist/state region | +0x29e..+0x2ac (~16 bytes) |
| Animation state region | +0x1300..+0x1348 (~80 bytes) |
| Hitbox array region | +0x4210..+0x4D10 (0xB00 bytes, 16 × 0xB0) |
| Position data block | +0x4D10..+0x4E10 (0x100 bytes, 16 × 0x10) |
| Damage/combo state | +0x6398..+0x63FC (~104 bytes) |
| Extra fields region | +0x68FD..+0x6908 (char-specific extensions) |
| **Estimated minimum** | **~0x7000 bytes (~28 KB) per character** |
| **Estimated conservative** | **~0x8000 bytes (~32 KB) per character** |
| **All 6 characters** | **~192 KB at 32 KB each** |

The actual uCharacter size varies per character (determined by DTI at
runtime). Character-specific fields for complex characters (Dante with
his weapon switching, Dormammu with his summoned objects) extend beyond
the base uCharacter size. A conservative snapshot should save 0x8000
bytes per uCharacter to ensure all character-specific state is captured.

For maximum safety, read the actual DTI size at runtime:
```
uint32_t size = *(uint32_t*)(uCharacter->dti_ptr + 0x34);
```
This gives the exact allocated size for each character's uCharacter instance.

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
