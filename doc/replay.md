# Native Replay State

**Status: Native reference with current-plugin boundary. Reviewed 2026-07-23.**
The plugin snapshots only `sReplay+0x54`, using it as the live absolute-frame
source. It does not use native replay input arrays as rollback history.

## Objects

The executable contains two replay-related classes:

- `sReplay`: live in-memory per-team input/state recording and playback.
- `rReplay`: a resource/file wrapper that can copy the in-memory replay payload
  and has `.rpd` load/save virtual functions.

`sReplay` is unquestionably live. Static xref analysis found no ordinary call
to `rReplay__save` and no runtime creation call beyond its own factory/vtable
references. This supports describing the file path as dormant in the analyzed
retail executable, not proving it can never be reached reflectively.

## `sReplay` Lifetime and Core Layout

| Item | Address/value |
|---|---|
| Singleton pointer | `0x140D4D9D8` |
| Constructor | `0x1401CE880` |
| Allocated size | `0x23428` |
| Vtable | `sReplay__vtable` / `0x140AE7DB0` in this build |

The constructor installs the singleton, clears `+0x58` for `0x233C4` bytes,
and invokes native reset logic.

| Offset | Type | Verified role |
|---:|---|---|
| `+0x38` | byte | Selects capture-versus-restore/record-versus-playback behavior in several functions; avoid a universal Boolean name |
| `+0x39` | byte | State flag consulted by `sReplay__move` before replay-seed restoration |
| `+0x3C` | flags | Recording/playback/team flags |
| `+0x40/+0x44` | `int32_t` | Per-team playback/frame cursors |
| `+0x4C` | `uint32_t` | Selected team/input owner in training controls |
| `+0x50` | `uint32_t` | Team selector used by input reads in one recording state |
| `+0x54` | `int32_t` | Global per-frame counter incremented by `sReplay__processPerFrame` |
| `+0x58` | `uint32_t` | Active replay/round seed |
| `+0x5C..` | data | Saved match configuration and character-slot records |
| `+0x184` | `int32_t` | Combined write cursor updated by writing both teams |
| `+0x18C/+0x190` | values | Saved battle flag/timer and stage value |
| `+0x194` | data | Team 0 input record |
| `+0x11AD8` | data | Team 1 input record |
| `+0x2341C/+0x23420` | `uint32_t` | Previous per-team button state for edge detection |
| `+0x23424` | `uint32_t` | New-round/stream seed source |

## Per-Team Input Records

Each team record has size `0x11944`:

```c
struct ReplayTeamInput {
    int32_t frameCount;
    uint32_t input[18000];
};

team[0] = sReplay + 0x194;
team[1] = sReplay + 0x194 + 0x11944;
```

At 60 Hz, 18,000 entries provide five minutes. This is a bounded linear record,
not a circular ring: explicit writes reject indices at or above 18,000, and
implicit writes append while `frameCount < 18000`.

`sReplay__writeInput` at `0x1401CF520` optionally swaps team indices through
flag `0x100`. An explicit frame index updates `frameCount` to at least
`frame+1`; an implicit negative index appends only while the required native
state flags permit recording.

`sReplay__writeBothTeamInputs` at `0x1401CF5D0` writes both records and updates
`+0x184` to `frame+1`.

`sReplay__readInput` at `0x1401CECB0` chooses the requested/swapped team and
returns recorded input if its cursor/index is within `frameCount`. It preserves
selected high bits from a caller-supplied mask via:

```c
return (callerInput & 0x000F8000) | recordedInput;
```

## Flags

The helper predicates establish these masks without requiring every bit to be
given a standalone semantic name:

```c
isPlaying(any)    -> flags & 0x31
isPlaying(team n) -> flags & ((0x10 << n) | 0x01)

isRecording(any)    -> flags & 0xC2
isRecording(team n) -> flags & ((0x40 << n) | 0x02)
```

Flag `0x100` swaps team indices in read/write helpers. Bits `0x04` and `0x08`
participate in state transitions. Older interpretations that assigned
`0x01 = playing` and `0x02 = recording` as mutually exclusive complete states
were too simplistic; they are common bits combined with team-specific masks.

## Per-Frame Processing

`sReplay__processPerFrame` at `0x1401CEFD0` increments `+0x54` every call. It
then processes both teams, tracks button edges at `+0x2341C`, handles native
training record/playback controls, writes input where state permits, and
advances playback cursors.

`sReplay__move` at `0x1401CEED0` handles higher-level transitions. In Training
mode it checks each team record and stops playback after a record exceeds 599
frames. This is not automatic unconditional recording and looping of every last
10 seconds; the per-frame routine exposes explicit training controls and state
predicates.

## Round RNG Initialization

`sReplay__onRoundStart` at `0x1401CFA90` clears team data and reseeds gameplay
RNG domains:

- Global `MtXorShift` state.
- `sEffect` RNG state.
- Every live character's RNG through the character seed dispatcher.

When `+0x38 == 0`, it copies seed `+0x23424` to active seed `+0x58` and uses it.
Otherwise it reuses active seed `+0x58`. Spectator mode copies the streamed seed
to `+0x58` before this branch. The function writes native state value 1 or 2 at
`+0x3C`; those writes should not be explained solely from guessed single-bit
names.

## Match Configuration Capture/Restore

`sReplay__saveRestoreMatchState` at `0x1401CF680` captures when spectator mode
is active or `+0x38 == 0`, and restores otherwise. Captured material includes:

- Battle flag/timer, stage, and area identifiers.
- A recognized stage-object class index.
- Six character-slot configuration records.

This is match setup/configuration state, not a complete gameplay simulation
snapshot. It does not provide rollback restoration of live characters,
projectiles, collision structures, schedulers, and all mutable subsystems.

## `rReplay` File Wrapper

| Address | Function |
|---|---|
| `0x14029C430` | Copy `sReplay+0x58` payload into `rReplay+0x70` |
| `0x14029C460` | Load `.rpd` resource data |
| `0x14029C510` | Save `.rpd` resource data |
| `0x14029C8E0` | Allocate/construct an `rReplay` instance |

The wrapper payload is the `0x233C4`-byte in-memory block beginning at
`sReplay+0x58`. It therefore excludes the live global frame counter at `+0x54`
and the base/state fields before `+0x58`.

## Plugin Boundary

Current rollback registers exactly four bytes at `sReplay+0x54` in snapshot
state. The full native replay payload is intentionally not restored:

- Native playback/recording state has its own ownership and cursors.
- Plugin input history is indexed separately by custom frame number.
- Re-simulation injects historical local/remote input at the character-input
  boundary.
- The debug-menu replay buffer in `defaults.h` is a separate legacy plugin
  feature, not `sReplay`'s 18,000-entry records.

## Key Addresses

| Address | Function |
|---|---|
| `0x1401CE880` | `sReplay` constructor |
| `0x1401CECB0` | Read recorded team input |
| `0x1401CEED0` | High-level replay state update |
| `0x1401CEFD0` | Per-frame update and `+0x54` increment |
| `0x1401CF520` | Write one team's input |
| `0x1401CF5D0` | Write both teams' inputs |
| `0x1401CF680` | Capture/restore match configuration |
| `0x1401CFA90` | Round-start RNG initialization |
| `0x14029C430` | Copy in-memory replay payload to `rReplay` |
