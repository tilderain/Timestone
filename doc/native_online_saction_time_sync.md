# Native Online `sAction` Time Synchronization

**Status: Native reference. Reviewed 2026-07-23.** This explains retail online
timekeeping. The plugin does not activate the complete retail online session;
it hooks `sAction__stepFighting` and maintains its own synchronized frame
timeline. Native behavior is design evidence, not the active implementation.

## Scope

This note documents how native UMVC3 synchronizes fight time in `sAction` during a live online battle. It distinguishes live-player online modes from spectator/watch mode because the two paths use different authoritative frame cursors.

The relevant `BattleType` values are:

| Value | Mode |
|---|---|
| 3 | Standard live online battle |
| 4 | Online spectator/watch battle |
| 9 | Live online Heroes & Heralds |

The mode predicates are:

| Address | Name | Meaning |
|---|---|---|
| `0x140006A60` | `sBattleSetting__isLiveOnlineBattle` | `BattleType == 3 || BattleType == 9` |
| `0x140006AB0` | `sBattleSetting__isOnlineSpectatorBattle` | `BattleType == 4` |
| `0x140006AF0` | `g__isOnlineBattle` | Live online or spectator/watch |

## Summary

Native online play does not advance `sAction` fight clocks by integrating each machine's local `sMain` delta. Instead, `sAction` derives elapsed fight time from an authoritative synchronized frame cursor.

For live online play:

```cpp
elapsedSeconds = float(uint32_t(syncPad->liveFrameCursor - syncPad->fightTimeBaseline) / 60);
remainingSeconds = max(180.0f - elapsedSeconds, 0.0f);
```

The relevant `sMvc3SyncPad` fields are:

| Offset | Meaning |
|---|---|
| `+0xD30` | Live synchronized input/simulation cursor |
| `+0xE34` | Cursor captured when `sAction` enters fighting phase 5 |

Spectator mode substitutes the streamed `sReplay` frame cursor for `+0xD30`.
That path uses signed integer subtraction/division and then clamps a negative
result to zero. The live path converts the 32-bit cursor difference to an
unsigned value before division; under normal monotonic cursor operation this is
the same whole-second delta, but it is not literally a signed `max(delta, 0)`.
The replay-backed path must not be mistaken for normal live online behavior.

## Startup Ordering

### 0. Synchronize match construction and readiness

Native live online performs substantial synchronization before the phase-2 input command. The
character-select protocol contains explicit messages for character type, assist, stage, battle
data, battle info, selection completion, and readiness. Relevant message classes include:

| Message | Size | Purpose established by surrounding call paths |
|---|---:|---|
| `cNetChrSel::MsgStg` | protocol object | Selected stage |
| `cNetChrSel::MsgBattleData` | `0x48` | Match-construction data |
| `cNetChrSel::MsgBattleInfo` | `0x30` | Match/session information |
| `cNetChrSel::MsgFinish` | protocol object | Character-select completion |
| `cNetRoomCmu::MsgMatchReadyInfo` | `0x18` | Match readiness |
| `cNetRoomCmu::MsgMatchReadyOk` | `0x18` | Readiness acknowledgement |

`aChrSelect__preloadBattleArchives_209500 @ 0x140209500` constructs the character and stage
resources from the synchronized `sBattleSetting` slot records. It does not proceed until the team
subsystems and stage archives report ready. `aChrSelect__tickReadyWaitAndInput @ 0x1402138A0`
tracks both players' ready bytes and waits for the network message queue to drain. The transition
path also checks that the selected characters and their license resources are loaded.

There is a second gameplay-ready gate in the netpad object:

```cpp
bool sNetPad__isRoomReady(syncPad) {
    return syncPad->localReady && syncPad->remoteReady;
    // +0xD10 && +0xD11
}
```

`sNetworkRoom__setReady @ 0x14000B380` controls `+0xD10`. `sMvc3SyncPad__move @ 0x140289820`
sets `+0xD11` only after the peer-side input buffer has been initialized. The result is consumed by
`sNetPad__consumeRemoteInputFrame @ 0x14028BE70`; synchronized input consumption and normal
missing-slot handling are not enabled until both flags are set. Match-transition code also polls
network/session and resource-ready state before advancing.

This is not a state-copy barrier. It guarantees that both peers have received the same match
configuration and have locally constructed the required gameplay objects before the shared input
timeline is consumed.

### Deterministic round RNG initialization

When match phase 3/4 setup calls `sReplay__onRoundStart @ 0x1401CFA90`, the function reseeds all
gameplay RNG domains from the round seed stored at `sReplay+0x23424`:

```text
sReplay+0x23424
  -> global MtXorShift state
  -> sEffect+0x3C MtXorShift state
  -> every live uCharacter+0x2D80 MtXorShift state
```

The same seed is copied to `sReplay+0x58` as the active replay/round seed. Spectator mode explicitly
receives this value as part of its streamed replay state. The live-online character-select protocol
contains battle-data and battle-info payloads large enough to carry deterministic match metadata,
but this analysis has not yet identified the exact live wire field that populates
`sReplay+0x23424`. Therefore the reseeding behavior is confirmed, while the exact live sender and
payload offset remain unconfirmed.

The network session's private RNG at `nNetwork::Session+0x2030` is not this gameplay seed. It is
seeded from local time and a network identifier in `nNetwork__Session__startAndSeed @ 0x140798170`,
which makes it suitable for protocol nonces but not deterministic gameplay.

### 1. Initialize the delayed input timeline

`sAction__executeMatchPhase @ 0x140004B80` performs live-online initialization when entering phase 2. It calls:

```text
sNetPad__processPendingNetworkMessages
sNetPad__sendInitialBattleSyncCommand
```

`sNetPad__sendInitialBattleSyncCommand @ 0x14028BA00` sends `MsgInputCmd(0)` if the local peer owns slot 1 and the room battle is live.

The receiver dispatches the command through:

```text
sNetPad__handleInputCommand @ 0x140288D10
  -> command == 0
  -> sNetPad__initializeInputDelay @ 0x14028B4D0
```

For a live online battle, `sNetPad__initializeInputDelay`:

- Computes the configured delay from `+0xDA4` and the active slot's value at `+0xDA8`.
- Inserts that many empty delayed input slots into the ring at `+0xD18`.
- Advances the transmitted-input write cursor at `+0xD44`.
- Dispatches pending inbound data.
- Clears synchronization/fencepost state at `+0xE14..+0xE33`.
- Clears the startup accumulator at `+0xE40`.

This establishes the delayed input timeline before the actual fight clock begins.

### 2. Capture fight time zero

When `sAction__executeMatchPhase` enters phase 5, it calls:

```text
sMvc3SyncPad__captureFightTimeBaseline @ 0x14028B970
```

For live online play, this performs:

```cpp
syncPad->fightTimeBaseline = syncPad->liveFrameCursor;
// +0xE34 = +0xD30
```

For spectator/watch mode, it stores `sReplay__getFrameCursor()` instead.

Capturing the baseline at phase 5 excludes setup, intro, countdown, and preceding phase-transition frames from elapsed fight time. It also means native fight time zero is not simply netpad frame zero.

## `sAction__stepFighting`

The timing branch is in `sAction__stepFighting @ 0x140007080`.

### Offline behavior

Offline modes integrate local timing:

```cpp
float delta = sMainDelta * actionSpeedScale / sMainTimeScale;

mRTimePass += delta;
mTimePass += delta;
mRTimePass2 += delta;
mRTimeLimit -= delta;
```

The exact decompiler names for the two `sMain` accessors remain poor. They are timing values, not character positions.

### Online behavior

For any mode accepted by `g__isOnlineBattle`, the three elapsed clocks are overwritten from synchronized time instead of incremented:

```cpp
float elapsed = sMvc3SyncPad__getFightElapsedSeconds(syncPad);

mRTimePass = elapsed;
mTimePass = elapsed;
mRTimePass2 = elapsed;
mRTimeLimit = sMvc3SyncPad__getRemainingRoundSeconds(syncPad);
```

The helper implementations are:

| Address | Current name | Behavior |
|---|---|---|
| `0x1402895C0` | `sMvc3SyncPad__getFightElapsedSeconds` | Returns cursor delta divided by 60, clamped to zero |
| `0x140289590` | `sMvc3SyncPad__getRemainingRoundSeconds` | Returns `max(180.0f - elapsed, 0.0f)` |

The live-online assembly at `0x140289611` reads `syncPad+0xD30`, subtracts
`syncPad+0xE34`, zero-extends the 32-bit difference, divides it by 60, and
converts it to `float`. The helper's final negative clamp affects the signed
spectator calculation but cannot make this unsigned live result negative.

The integer division is significant: these `sAction` fields represent whole elapsed/remaining seconds derived from the synchronized frame timeline, not a continuously accumulated local fractional clock.

## Cursor Advancement And Stalling

The live cursor at `+0xD30` advances in `sNetPad__tickInputSender @ 0x14028BC10` together with the two ring read cursors:

```cpp
++syncPad->ringAReadCursor; // +0xD34
++syncPad->ringBReadCursor; // +0xD38
++syncPad->liveFrameCursor; // +0xD30
```

The same path then reads both synchronized ring slots into the game-facing pad state at `+0xC60` and `+0xC8C`.

Before this consumption path, `sNetPad__consumeRemoteInputFrame @ 0x14028BE70` examines the next ring slots. Unfilled slots retain marker `0xCDCD`. Depending on action phase and netpad flags, unavailable slots cause the network wait/indicator path or are filled with neutral input only in explicitly allowed pre-fight states.

Consequently, normal fighting cannot independently advance the authoritative cursor while one peer lacks the corresponding synchronized input frame. A local render/update call alone is not sufficient to advance online fight time.

## Collision-State Synchronization Finding

No native match-start path found in this analysis transmits, restores, or compares authoritative
push-box state. In particular, no startup message was found to contain:

- `uCnsGroup` solver accumulators or ordering state.
- Collision-body cached positions at `+0xB0..+0xB8`.
- Collision-body saved positions at `+0x160..+0x16C`.
- Contact flags at `+0x1B4`.
- Character world positions or a complete simulation snapshot.

The periodic native desync fencepost is also too narrow to enforce collision determinism.
`sNetPad__sendDesyncFencepost_90f @ 0x14028A730` sends only the synchronized frame cursor and a
team-life aggregate. `sNetPad__checkDesyncFencepost @ 0x140288DB0` compares only those values.

The functions named `sync`, such as `sCollision__sync @ 0x1405BA240`, are local MT Framework
scene/object transform synchronization routines. They do not send network data and should not be
interpreted as online state synchronization.

Native live online therefore appears to obtain deterministic push calculation indirectly:

```text
same host-authoritative match selection/settings
+ all character and stage resources loaded
+ deterministic round RNG reseed
+ two-sided gameplay-ready gate
+ identical delayed input frame consumption
+ fight time derived from the synchronized cursor
= independently constructed but expected-identical collision simulation
```

This design has no mechanism to repair a position-only push divergence after it occurs. If a peer
enters playable simulation with different persistent collision state, or executes an extra
pre-fight solver tick, native input synchronization and the life-only fencepost will not correct or
necessarily detect it.

## Why Synchronization Is Necessary

The visible round clock is only one consumer. `sAction` elapsed time participates in phase and gameplay decisions, and synchronized input frames drive the rest of deterministic simulation. If each peer integrated local `sMain` delta independently, small differences in host scheduling, frame pacing, focus state, or network waits could make one peer reach a time threshold first.

Potentially affected systems include:

- Round timeout and transition into timeout/KO handling.
- Fight-start and post-fight thresholds.
- Character action and animation timing.
- Hitbox, hurtbox, throw, cancel, and invulnerability windows.
- Projectile, assist, and effect lifetimes.
- Scheduled state changes and delayed events.
- Collision and constraint state that persists across simulation frames.

Equal input packets are insufficient if they are consumed against different simulation frames. Native delay netcode therefore couples three properties:

```text
same delayed input timeline
+ same consumed input cursor
+ fight time derived from that cursor
= same input and time for each simulated frame
```

When input is unavailable, time must stall with simulation. Advancing local action time while waiting would allow time-triggered state changes without advancing the shared input timeline.

## Implications For UMVC3Rollback

`Controller_ResetFrameEpoch()` resets the mod's custom frame numbering, but it is not equivalent to native fight-time initialization. Native behavior includes both:

- Delayed input-ring initialization during phase 2.
- Capturing the authoritative cursor as fight time zero during phase 5.

If the mod leaves the game's live-online mode disabled, `sAction__stepFighting` follows the offline local-delta path. Pinning `sMain` timing values reduces variability but does not reproduce the native invariant that fight time is a function of the shared input/simulation cursor and cannot advance during an input stall.

A native-equivalent replacement should ensure:

1. The custom synchronized frame cursor is established before playable simulation.
2. Fight time zero is captured at the actual transition into fighting, not merely when an intro-ready packet is exchanged.
3. `sAction` elapsed and remaining time are derived from the custom synchronized cursor.
4. No gameplay simulation or persistent collision-solver time advances while waiting for the next shared input frame.
5. Rollback/resimulation restores or recomputes `sAction` time from the simulated net frame rather than local wall-clock state.

This is relevant even in pure delay mode. A peer that advances one local action/solver tick while the other waits can enter the first character-push contact with a different constraint accumulator or cached collision-body state.

## Mod Implementation

`controller.cpp` now hooks `sAction__stepFighting @ 0x140007080` while custom netplay is active. The hook captures a custom fight baseline on the first synchronized fighting update and computes time from the frame being simulated:

```cpp
frame = g_isResimulating
    ? g_resimulateCurrentFrame
    : GetGameFrameCount();

elapsedSeconds = max((frame - fightFrameBase) / 60, 0);
remainingSeconds = max(180.0f - elapsedSeconds, 0.0f);
```

It writes `sAction+0xF8..+0x104` before calling the original function so timeout and action thresholds observe synchronized time. It writes the same values again afterward to remove the local-delta increment performed by the game's offline timing branch.

The custom fight baseline is reset by `Controller_ResetFrameEpoch()` at the battle-ready barrier. During rollback, using `g_resimulateCurrentFrame` makes each re-simulated tick receive the time associated with that historical net frame rather than the current live `sReplay` cursor.

This is intentionally narrower than changing the game's `BattleType` or hooking `g__isOnlineBattle`: enabling those native predicates would also activate native session, netpad, ownership, UI, and teardown paths that the custom ENet implementation does not initialize.

### Deterministic engine execution

`rollback.cpp` forces `sMain`'s desired and active job-thread counts to one while custom netplay is
active:

```text
sMain+0xA8 = 1  // desired job-thread count
sMain+0xA4 = 1  // active job-thread count
```

Ghidra confirms that `sMain__setJobThreadNum @ 0x140522450` writes `+0xA8`, and
`sMain__move @ 0x140521DF0` copies `+0xA8` to `+0xA4` each tick. The job executor uses an inline
single-thread path when the active count is one; otherwise it signals workers and waits for their
completion.

The override is applied before `sMain__move`, after it defensively, and in rollback resimulation.
It starts as soon as custom netplay is active rather than only after the intro barrier. This ensures
that pre-fight collision/constraint caches and fighting push resolution are constructed under the
same execution model on both peers. It specifically targets the observed first divergence: an
equal-and-opposite X correction beginning at the first character-push contact while input, action,
flags, and RNG still agree.

This is a broad deterministic fallback, not proof that worker ordering was the sole cause.

### Native online disables gameplay parallelism

Further analysis found the native mechanism that makes differing local worker counts irrelevant to
gameplay determinism. `aNextBattle__initBattleRound @ 0x140203810` enters a determinism setup branch
when either replay-use state is active or `g__isOnlineBattle` is true. The branch does the following:

```cpp
sMain__setFrameWait(sMain, true);

for (int category = 0; category < 64; ++category) {
    sUnit__setMoveLineParallel(sUnit, category, false);
}

auto moveMatrix = sCollision__getMoveMatrixSubsystem(sCollision);
sCollisionMoveMatrix__setParallelFlags(moveMatrix, false); // +0x3C..+0x3E
sHitSolver__setMultiThread(sHitSolver, false);
sBattleRuntime__setParallelFlag281(battleRuntime, false);
```

The `sUnit` setter is `aNextBattle__createProperty_7B10 @ 0x140207B10`. Its third argument is passed
in `R8B`; native setup clears it for every move line. `sUnit__moveCategory @ 0x14051B4C0` checks this
bit. When clear, it invokes every unit's `move` callback directly in linked-list order. When set, it
enqueues one job per unit and calls `sMain__executeJob`, allowing callbacks in that category to run
concurrently and in scheduler-dependent completion order.

The additional native switches are:

| Address | Recovered behavior |
|---|---|
| `0x1402033D0` | Returns the `sCollision+0x7368` move-matrix subsystem |
| `0x1405B7BF0` | Clears parallel bytes at subsystem `+0x3C..+0x3E` |
| `0x140207660` | Writes `sHitSolver::mMultiThread` |
| `0x140207C30` | Writes another battle-runtime parallel byte at `+0x281` |

This explains how native delay netcode avoids push-position divergence without synchronizing
`sMain+0xA4/+0xA8`: it does not require equal engine worker counts. It serializes gameplay-sensitive
unit, collision, and hit-solver work at battle initialization while leaving the worker pool available
for unrelated engine work.

Custom ENet play keeps a non-online `BattleType`, so native battle initialization does not enter this
branch automatically. Current plugin code compensates in
`Rollback_ApplyNetplayDeterminism`: it enables frame wait, clears all 64 `sUnit` move-line parallel
bits, disables the collision move-matrix parallel flags, disables `sHitSolver` multithreading, and
clears the battle-runtime parallel byte. The function runs after netplay/rollback initialization and
again when a synchronized restart creates a new battle epoch.

The plugin still forces the entire regular `sMain` executor to one thread as an additional fallback.
It does not currently restore either the battle-local settings or the global worker count on netplay
teardown. Therefore the native-equivalent settings are implemented, while narrowing or removing the
global fallback remains contingent on two-peer delay and rollback validation.

### Correction: generic constraint collision is not proven fighter push collision

`uCnsGroup__stepSolver @ 0x14071C170` and
`uCollisionNode_processCharacterPush @ 0x140725B70` belong to the generic `uCnsTinyChain`
secondary-motion constraint system. Reflection exposes fields such as `mGravity`, `mDamping`,
`mSpringCoef`, chain-node radius, and collision iteration counts. The function name describes a
generic selected-collision pass; static analysis alone does not establish that its bodies are MVC3
fighter gameplay pushboxes.

There is a potentially non-reentrant lazy guard in
`uCnsTinyChain__cChainGroup__adjust @ 0x140719860`:

```cpp
if (group->solvedThisMove == 0) { // +0xA9
    uCnsGroup__stepSolver(group); // callee writes +0xA9 = 1 at entry
}
```

The check and set are not atomic. However, it becomes an actual race only if separate concurrent unit
jobs can reach the same chain group. The owning `uCnsTinyChain` normally calls its group adjustments
inside one unit callback, so runtime pointer/thread provenance is required before attributing the
observed fighter X divergence to this guard. Native online's blanket serialization would avoid such a
race if shared ownership does occur, but it also avoids many other order-sensitive gameplay paths.

The highest-value runtime confirmation is therefore to log the native settings and solver provenance,
not merely solver entry counts:

- All 64 `sUnit` move-line parallel bits at battle start.
- `sHitSolver::mMultiThread` and collision move-matrix `+0x3C..+0x3E`.
- Every job callback that writes either fighter's `uCharacter+0x50` on the first divergent frame.
- For `uCnsGroup__stepSolver`, OS thread ID, group pointer, owner `uCnsTinyChain`, and whether any
  overlapping call uses the same group or body pointer.

An A/B test should retain the normal worker count and apply the native four-part serialization setup.
If that fixes the two-peer push split, it confirms the missing native determinism branch as the cause
of the global-thread-count symptom. Individual switches can then be re-enabled one at a time to locate
the smallest responsible subsystem.

## Corrections To Existing Notes

Some labels in `doc/net.md` predate this analysis and should not be relied on literally:

- `sNetPad+0xDC8` and `+0xDD0` are timing/send accumulators derived from `sMain` timing accessors, not player-position drift.
- The accessor currently named `cUnit__getPositionX` at `0x140004580` is misnamed and reads a timing value from `sMain+0x80`.
- `sNetPad+0xD30` is not merely an unrelated total-frame statistic; it is the live-online cursor used to derive `sAction` fight time.
- `sNetPad+0xE34` is the phase-5 fight-time baseline.

These corrections should be incorporated if the main netcode layout table is revised.

## Ghidra Changes Made During Analysis

The following functions were renamed and commented:

- `0x1402895C0`: `sMvc3SyncPad__getFightElapsedSeconds`
- `0x140289590`: `sMvc3SyncPad__getRemainingRoundSeconds`
- `0x14028B970`: `sMvc3SyncPad__captureFightTimeBaseline`
- `0x140287030`: `sNetPad__MsgInputCmd_ctor`
- `0x1402874C0`: `cNetMessagePayload__storeCommandDword`
- `0x140203770`: `sGameConfig__getLockedField_D4C`
- `0x1402032A0`: `sLicense__getByte_4A5`
- `0x1402850A0`: `sLicense__tickResultWaitTimer`
- `0x140283900`: `sLicense__resetResultWaitTimer`
- `0x140271350`: `sGameConfig__tickCountdownNetChrSelMessage`
- `0x140274790`: `sAgentSession__setOrRandomizeField_144`
- `0x140262490`: `cNetChrSel__MsgBattleData__releaseInstance`
- `0x1402624B0`: `cNetChrSel__MsgBattleInfo__releaseInstance`
- `0x14028B9F0`: `sNetPadStatus__setDesyncFlag`
- `0x14023D260`: `sBattleSetting__pickRandomEligibleCharacter`

Comments were also added to:

- `sAction__executeMatchPhase @ 0x140004B80`
- `sAction__stepFighting @ 0x140007080`
