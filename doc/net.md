# Native Netpad Architecture

**Status: Native reference only. Reviewed 2026-07-23.** Retail UMVC3 uses a
delayed synchronized-input timeline. UMVC3Rollback does not extend this native
transport; it uses ENet, its own absolute-frame input history, prediction,
snapshot restore, and re-simulation. See [`current.md`](current.md).

## Scope

The native gameplay object is referred to here as `sMvc3SyncPad`/`sNetPad`,
matching recovered function roles. Older names such as
`cCommunicationTool` are not required to understand the verified layout.

Native code establishes:

- Two 120-slot input rings at `+0xD18` and `+0xD20`.
- Separate read cursors at `+0xD34` and `+0xD38`.
- A live synchronized frame cursor at `+0xD30`.
- Initial insertion of configured neutral delay frames.
- Stalling/indicator behavior for unavailable fighting input.
- A 90-frame fencepost containing frame number and aggregate team life.
- No native simulation-state snapshot, restore, or re-simulation path.

## Mode Predicates

| Address | Predicate | Verified condition |
|---|---|---|
| `0x140006A60` | `sBattleSetting__isLiveOnlineBattle` | `BattleType == 3 || BattleType == 9` |
| `0x140006AB0` | `sBattleSetting__isOnlineSpectatorBattle` | `BattleType == 4` |
| `0x140006AF0` | `g__isOnlineBattle` | Either predicate above |

Spectator mode consumes replay-backed data and must not be conflated with the
two-player live netpad timeline.

## Verified Layout

| Offset | Type | Verified role |
|---:|---|---|
| `+0xC50` | `uint32_t` | Cached aggregate team life used by the fencepost |
| `+0xC58` | pointer | Character-select/network message interface used by fencepost send |
| `+0xC60` | `uint32_t` | Game-facing buffered input from ring `+0xD18` |
| `+0xC8C` | `uint32_t` | Game-facing buffered input from ring `+0xD20` |
| `+0xD10` | byte | Local readiness component |
| `+0xD11` | byte | Remote/input-buffer readiness component |
| `+0xD14` | `uint32_t` | Active slot index used when selecting configured delay |
| `+0xD18` | pointer | First 120-entry synchronized input ring |
| `+0xD20` | pointer | Second 120-entry synchronized input ring |
| `+0xD28` | pointer | Netpad/session status object |
| `+0xD30` | `uint32_t` | Live consumed-frame cursor and fight-time source |
| `+0xD34` | `uint32_t` | First ring read cursor |
| `+0xD38` | `uint32_t` | Second ring read cursor |
| `+0xD3C` | `uint32_t` | First buffered-input consumer count |
| `+0xD40` | `uint32_t` | Second buffered-input consumer count |
| `+0xD44` | `uint32_t` | Local ring write/transmit cursor |
| `+0xD48` | `uint32_t` | Peer/dispatch comparison cursor |
| `+0xD50` | `uint32_t` | Counter incremented on the non-connected/advance path |
| `+0xD58` | `uint32_t` | Current local input value written to a ring slot |
| `+0xD60` | `uint32_t` | First ring buffered-depth metric |
| `+0xD64` | `uint32_t` | Second ring buffered-depth metric |
| `+0xD78` | `float` | Dynamic delay metric |
| `+0xD7C` | `float` | Delay-adjustment cooldown |
| `+0xD80` | byte | Delay-adjustment state flag |
| `+0xD84` | `float` | Network wait/timeout counter |
| `+0xD8C` | byte | Enables dynamic-delay adjustment |
| `+0xD90/+0xD94` | `float` | Upper/lower delay bounds |
| `+0xD98/+0xD9C` | `float` | Delay increase/decrease increments |
| `+0xDA4` | `uint32_t` | Base initial delay |
| `+0xDA8` | array | Per-slot initial-delay additions |
| `+0xDBC` | `uint32_t` | Buffered-depth limit used before writing/resending |
| `+0xDC8` | `float` | Send-gating timing accumulator |
| `+0xDCC` | `float` | Send-gating timing threshold |
| `+0xDD0` | `float` | Resend-gating timing accumulator |
| `+0xDD4` | `float` | Resend-gating timing threshold |
| `+0xDE0/+0xDE4` | `uint32_t` | Resend comparison metrics |
| `+0xDEC/+0xDF0/+0xDF8` | `uint32_t` | Send/delay comparison metrics |
| `+0xE0C` | byte | Enables in-match wait-indicator behavior |
| `+0xE10` | flags | Records neutral pre-fight ring substitutions |
| `+0xE14/+0xE18` | pair | Local fencepost frame and aggregate life |
| `+0xE1C/+0xE20` | pair | Remote fencepost frame and aggregate life |
| `+0xE34` | `uint32_t` | Fight-time baseline cursor |
| `+0xE40` | `uint32_t` | Startup accumulator cleared with delay state |

Several fields retain generic descriptions because their exact network metric
names are not established. In particular, timing values derived from `sMain`
must not be labeled character-position drift.

## Ring Format

Each pointer at `+0xD18`/`+0xD20` refers to a ring with 120 entries:

```c
struct NativeInputRing {
    uint32_t unknown00;
    uint32_t writeCursor; // +0x04
    uint32_t unknown08;
    NativeInputSlot slots[120]; // +0x0C
};

struct NativeInputSlot {
    uint32_t sequence;
    uint16_t buttons;
    uint16_t padding;
};
```

`sNetPad__writeRingSlot` at `0x140288D30` selects
`slots[sequence % 120]`, writes the sequence and low 16 input bits, optionally
adds bit `0x8000` from an input flag, and advances `ring+0x04` to
`sequence + 1`.

`sNetPad__initRingSentinel` at `0x140288E20` writes `0x0000CDCD` to the slot
sequence and `0xCDCD` to its button field. Consumers test this marker to detect
an unavailable slot.

## Startup Delay

`sNetPad__initializeInputDelay` at `0x14028B4D0` computes:

```c
delay = netpad->baseDelay_DA4 + netpad->slotDelay_DA8[netpad->slot_D14];
```

In a live online battle, it writes that many zero-button slots to the first
ring, advances `+0xD44`, dispatches pending inbound data, clears the fencepost
and synchronization region at `+0xE14..+0xE33`, and clears `+0xE40`. This is
input delay, not rollback prediction.

## Per-Frame Consumption

`sNetPad__tickInputSender` at `0x14028BC10` has two important paths:

- While its status predicate permits sending, it accumulates `sMain`-derived
  timing at `+0xDC8/+0xDD0`, conditionally writes the current input and cached
  life aggregate to the first ring, advances `+0xD44`, and dispatches inbound
  packets.
- On the alternate advancement path, it handles resend gating, initializes the
  current ring slots to `0xCDCD`, advances both ring read cursors and `+0xD30`,
  updates buffered-depth metrics, processes the desync fencepost, and exposes
  both consumed button values at `+0xC60/+0xC8C`.

The predicate's decompiler label is not reliable enough to describe these as
simply "connection alive" and "connection dead" branches.

`sNetPad__readBufferedInputForTeam` at `0x140289520` chooses a P1/P2 buffered
reader based on local slot ownership and requested team. It increments `+0xD3C`
or `+0xD40`. It is not a generic host-versus-client network buffer read.

## Missing Input

`sNetPad__consumeRemoteInputFrame` at `0x14028BE70` examines the next entries in
both rings. Once both room-ready bytes are set:

- Before action step 3, selected missing slots that are already within the
  ring's written range may be replaced with neutral input and recorded in
  `+0xE10`.
- During relevant match steps, unresolved `0xCDCD` slots activate the network
  wait indicator when enabled.
- The alternate status path accumulates a 180-frame timeout and only dismisses
  the indicator once required slots/session conditions permit progress.

The authoritative fighting cursor therefore does not freely advance from local
wall-clock updates while synchronized input is unavailable.

## Resend and Dynamic Delay

`sNetPad__resendIfTimeout` at `0x14028B1D0` may write another current-input slot
when ring depth is below `+0xDBC` and resend metrics permit it, then dispatch
inbound data. It does not restore old game state or re-simulate a frame and is
not a rollback surrogate.

`sMvc3SyncPad__adjustDynamicDelay` at `0x14028C090` adjusts `+0xD78` when enabled:

- If first-ring buffered depth is below 3 and metric `+0xDF0` exceeds 2, it
  increases delay by `+0xD98` up to `+0xD90`.
- Otherwise it decreases delay by `+0xD9C` down to `+0xD94`.
- It sets the cooldown at `+0xD7C` to `30.0f`.

## Desync Fencepost

Every 90 live-room frames, `sNetPad__sendDesyncFencepost_90f` at `0x14028A730`
sends a message containing:

```text
live cursor (+0xD30)
aggregate team life (+0xC50)
```

`sNetPad__checkDesyncFencepost` at `0x140288DB0` compares local and remote frame
and life values. Matching pairs clear the fencepost storage. This is not a CRC
of complete simulation state and cannot detect every deterministic divergence.

## Transport Boundary

The executable imports Steam interfaces including `SteamNetworking005` and has
Capcom room/message classes, but this note does not assign every gameplay
message to a particular transport path without tracing its sender into the
Steam API. Matchmaking/session evidence and synchronized-netpad behavior are
separate claims.

## Rollback Contrast

The current plugin follows the independent-transport design once proposed in
older versions of this note:

- ENet carries custom absolute-frame input history and checksums.
- Missing remote input is predicted by the plugin.
- Late corrections request restore and deterministic re-simulation.
- Native rings and adaptive delay are reference behavior, not the active
  rollback authority.

## Key Addresses

| Address | Verified function |
|---|---|
| `0x140288D30` | Write native input-ring slot |
| `0x140288DB0` | Compare frame/life desync fencepost |
| `0x140288E20` | Initialize ring slot with `0xCDCD` marker |
| `0x140289520` | Read buffered input for a requested team |
| `0x14028A730` | Send 90-frame frame/life fencepost |
| `0x14028B1D0` | Resend current input when gating permits |
| `0x14028B4D0` | Initialize native input delay |
| `0x14028BC10` | Tick sender and consumed-input timeline |
| `0x14028BE70` | Handle unavailable next synchronized input |
| `0x14028C090` | Adjust dynamic delay metric |
