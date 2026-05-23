# Netcode Architecture

## Overview

This game is **delay-based**, not rollback. There are no `saveState`, `rollback`, `rewind`, or `resimulate`
functions anywhere in the binary. The netcode revolves around an object called `sMvc3SyncPad::cCommunicationTool`
which manages input sync, prediction, and delay logic.

## Network Stack

```
sMvc3Main  (application root)
  └─ sMvc3Manager::registerNetworkScheduler  (registers the sync callback)
       └─ sMvc3SyncPad::cCommunicationTool  (core netcode object)
            ├─ Ring buffer A (local input history)  [param_1 + 0xd18]
            ├─ Ring buffer B (remote input history) [param_1 + 0xd20]
            ├─ network_sync_buffer_read()          [0x140289520]
            ├─ sMvc3Manager::processNetworkSync()  [0x14025c860]
            └─ sNetworkRoom                        [0x14028c700]
                 └─ cNetRoomCmu  (Capcom Matchmaking Unit)
```

## cCommunicationTool Object Layout

Key offsets on `this` (the cCommunicationTool instance, size ~0xe50+):

| Offset | Size | Description |
|--------|------|-------------|
| +0xc48 | 1    | Current input state (local pad) |
| +0xc58 | 8    | Character select interface pointer |
| +0xd10 | 1    | Flag: player 3 (spectator) mode |
| +0xd11 | 1    | Flag: local input disabled? |
| +0xd13 | 1    | Flag: auto-input mode |
| +0xd14 | 4    | Auto-input frame counter / data |
| +0xd18 | 8    | **Ring buffer A** (originally received from remote / authoritative) |
| +0xd20 | 8    | **Ring buffer B** (locally predicted inputs sent to remote) |
| +0xd28 | 8    | Network status object pointer |
| +0xd30 | 4    | Total frame counter |
| +0xd34 | 4    | Read cursor for ring buffer A |
| +0xd38 | 4    | Read cursor for ring buffer B |
| +0xd3c | 4    | Dropped packet counter |
| +0xd40 | 4    | Received packet counter |
| +0xd44 | 4    | Write cursor for transmitted inputs |
| +0xd48 | 4    | Acknowledged frame by remote |
| +0xd4c | 4    | Total inputs stored counter |
| +0xd50 | 4    | Frame skip / desync counter |
| +0xd54 | 4    | Input prediction lookahead |
| +0xd58 | 4    | Current input data being processed |
| +0xd60 | 4    | Local frames - remote frames distance |
| +0xd68 | 1    | Flag: replay recording mode |
| +0xd6c | 4    | Replay frame counter |
| +0xd78 | 4    | Delay value (frames of delay, float) |
| +0xd7c | 4    | Delay change cooldown timer (float) |
| +0xd80 | 1    | Flag: delay increasing |
| +0xd8c | 1    | Flag: dynamic delay adjustment enabled |
| +0xd90 | 4    | Max delay threshold (float) |
| +0xd94 | 4    | Min delay threshold (float) |
| +0xd98 | 4    | Delay increase rate (float) |
| +0xd9c | 4    | Delay decrease rate (float) |
| +0xdbc | 4    | Max prediction frames allowed (uint) |
| +0xdc8 | 4    | Accumulated position drift (float) |
| +0xdcc | 4    | Drift threshold for resync (float) |
| +0xdd0 | 4    | Total accumulated drift |
| +0xdd4 | 4    | Drive resync threshold (float) |
| +0xdd8 | 4    | Local player position at last sync |
| +0xde0 | 4    | Frame delay metric 1 |
| +0xde4 | 4    | Frame delay metric 2 |
| +0xdec | 4    | Remote quality indicator |
| +0xdf0 | 4    | Local quality indicator |
| +0xdf4 | 4    | Network RTT / ping (float) |
| +0xdf8 | 4    | Current delay mode |
| +0xdfc | 4    | Low ping threshold for delay reduction |
| +0xe00 | 4    | High ping threshold for delay increase |
| +0xe04 | 4    | Delay value applied at high ping |
| +0xe08 | 4    | Delay value applied at medium ping |
| +0xe14 | 8    | CRC / checksum pair 1 (for desync detection) |
| +0xe1c | 8    | CRC / checksum pair 2 (for desync detection) |
| +0xe40 | 4    | Client-side position accumulator (player 3 drift fix) |

## Ring Buffer Design

Each ring buffer (`0xd18` and `0xd20`) points to a structure:

```
struct SyncBuffer {
    int write_head;           // +0x00: total frames written
    int max_frames;           // +0x04: capacity (always 0x78 = 120)
    FrameEntry entries[120];  // +0x0c: circular buffer
};

struct FrameEntry {   // 8 bytes each
    uint frame;       // +0x00: frame number (also stores 0xCDCD when cleared)
    ushort input;     // +0x04: 16-bit input data (bitfield)
    // +0x06: padding
};
```

Entry lookup: `base + 0xc + (frame % 120) * 8` for the frame number, and
`base + 0x10 + (frame % 120) * 8` for the 2-byte input value.

Input bits (from `pad.md` and decompilation):
- 10 bindings per player (LP, MP, HP, LK, MK, HK, S1, S2, Assist, Up, Down, Left, Right, etc.)
- 0x10 = 16 bits per entry seems sufficient

### Buffer Operations

**Write** (`FUN_140288d30` at `0x140288d30`):
```
void syncBuffer_write(buffer, inputData, frame, flags) {
    slot = frame % 120;
    buffer.entries[slot].frame = frame;
    buffer.entries[slot].input = inputData | (flags & 0x8000);
    buffer.write_head = frame + 1;
}
```

**Clear** (`FUN_140288e20` at `0x140288e20`):
```
void syncBuffer_clearEntry(buffer, frame) {
    slot = frame % 120;
    buffer.entries[slot].frame = 0xCDCD;  // tombstone
    buffer.entries[slot].input = 0xCDCD;
}
```

**Read** (`FUN_140289490`/`FUN_140289450`):
- Checks if read_cursor < buffer.write_head
- Returns the 16-bit input value from the entry at read_cursor
- Handles negative values (cast to unsigned)

## Network Sync Flow

### sMvc3Manager::processNetworkSync (`0x14025c860`)

Called each frame. This is the main sync decision function.

Takes: `(manager, playerIndex, syncType, param4)`
Returns: `bool` (true = inputs match / frame is synced)

Sync types (switch cases):
- **0**: Read `param_1 + 0x54` (standard frame input check)
- **1**: Read `param_1 + 0x48` (action/assist data)
- **2**: Read `param_1 + 0x4c` (secondary input data)
- **3**: Read `param_1 + 0x58` (alternate input)
- **4**: Read `param_1 + 0x5c` (extra data)
- **7**: Read `param_1 + 0x9c` (bulk data)

The manager stores input slots as an array of 4 entries (for 4 possible players),
each 0x70 bytes:

```
struct PlayerInputSlot {     // 0x70 bytes
    char active;              // +0x00
    // ... (3 bytes padding)
    uint input_frame;         // +0x04: which frame we're checking
    uint expected_frame;      // +0x08: which frame we expect
    // +0x0c to +0x44: various input buffers
    uint normal_input;        // +0x48: normal button input
    uint default_input;       // +0x4c: default/fallback input
    uint frame_input;         // +0x54: frame-specific input
    uint alt_input;           // +0x58: alternate input
    uint extra_input;         // +0x5c: extra/bulk input
    // ...
    uint bulk_data;           // +0x9c: bulk input data
};
```

### network_sync_buffer_read (`0x140289520`)

```
void network_sync_buffer_read(cCommunicationTool* tool) {
    if (isHost()) {
        // Host reads from ring buffer A (remote inputs received)
        readFrame(tool + 0xd18);  // FUN_140289490
        tool.recv_counter++;
    } else {
        // Client reads from ring buffer B (local predictions)
        readFrame(tool + 0xd20);  // FUN_140289450
        tool.drop_counter++;  // counts frames read from prediction buffer
    }
}
```

### FUN_14028bc10 (`0x14028bc10`) - Main Sync/Transmit Function

This is the core function that decides whether to send input data or handle
prediction/desync. It runs each frame.

**When the network connection is alive:**
1. Accumulates position drift (`this + 0xdd0`)
2. If the sync condition is met (frames available, delay threshold exceeded, etc.):
   - Writes current input into the transmit buffer (ring buffer B for client, A for host)
   - Increments the transmit counter `+0xd44`
   - Resets drift accumulator
   - Sends the input data to the remote peer
3. Calls `FUN_14028a810` to send CRC data if not all frames are transmitted

**When the network connection is dead/dropped:**
1. Increments `+0xd50` (desync/drop counter)
2. Calls `FUN_14028b1d0` for re-transmission
3. Clears both ring buffer entries for current frame (marks them 0xCDCD)
4. Increments read cursors for both buffers
5. Increments total frame counter
6. Resets the received/predicted frame counters

### FUN_14028b1d0 (`0x14028b1d0`) - Retransmission / Rollback Surrogate

This function decides what to do when frames are missing. It checks if the
frame gap is within the prediction window (`+0xdbc`). If yes, it checks:

- If the frame budget has been exceeded (various counters)
- If so, it resets drift and does nothing (essentially holds)
- Otherwise, it re-transmits the input data by calling `FUN_140288d30` again

This is the closest thing to a rollback mechanism — it can re-insert inputs
into the buffer for re-simulation, but there's no state save/restore.

### FUN_14028c090 (`0x14028c090`) - Dynamic Delay Adjustment

```
void adjustDynamicDelay(cCommunicationTool* tool) {
    if (!tool.delayAdjustEnabled) return;
    
    tool.delayIncreasing = 0;
    if (tool.delayChangeCooldown > 0.0)
        tool.delayChangeCooldown -= 1.0;
    
    if (ringBufferAGap < 3 && tool.qualityLocal > 2) {
        // Network is good: increase delay (smooth out jitter)
        newDelay = tool.currentDelay + tool.delayIncreaseRate;
        clamp to tool.maxDelayThreshold;
    } else {
        // Network is bad: decrease delay
        newDelay = tool.currentDelay - tool.delayDecreaseRate;
        clamp to tool.minDelayThreshold;
    }
    
    tool.currentDelay = newDelay;
    tool.delayChangeCooldown = 30.0;  // ~0.5 second cooldown
}
```

This implements **adaptive delay**: it measures network quality and adjusts
the delay buffer size dynamically. When the ring buffer gap is small (< 3 frames)
and local quality is good, it increases delay. Otherwise it reduces it.

## Message Types

All messages are serialized through a property/message system (MtObject/DTI)
with virtual function dispatch (`vfn8` at `0x14028a080` dispatches by message size).

| Message Type | Size | Description |
|---|---|---|
| MsgInputData | small | 1 frame of button input data |
| MsgInputActData | small+ | 1 frame input + action data (2 entries in buffer) |
| MsgInputActData2 | larger | 2 frames of input bundled |
| MsgInputActData3 | larger | 3 frames of input bundled |
| MsgInputActData4 | larger | 4 frames bundled |
| MsgInputActData5 | larger | 5 frames bundled |
| MsgInputActData6 | larger | 6 frames bundled |
| MsgInputActData7 | larger | 7 frames bundled |
| MsgInputActData8 | larger | 8 frames bundled |
| MsgInputActData9 | larger | 9 frames bundled |
| MsgInputActDataRep | special | Repair / retransmission request |
| MsgInputCmd | ? | Command data |
| MsgCrcData | ? | CRC checksum data (desync detection) |
| MsgGalleryData | ? | Gallery/spectator data |

The `vfn8` handler (`0x14028a080`) dispatches incoming messages by comparing
the message size against known DTI sizes. Each message type has a dedicated
handler function that writes the data into the appropriate ring buffer.

## Steam Integration

The game initializes Steam via `cChrTeam__processTeamLogic` (`0x140212e30`):

```
SteamClient017         -> ISteamClient
SteamUser019           -> ISteamUser
SteamFriends015        -> ISteamFriends
SteamUtils008          -> ISteamUtils
SteamMatchMaking009    -> ISteamMatchmaking      (lobbies)
SteamMatchMakingServers002 -> ISteamMatchmakingServers
SteamNetworking005     -> ISteamNetworking       (P2P transport)
STEAMUSERSTATS_INTERFACE_VERSION011 -> ISteamUserStats
STEAMAPPS_INTERFACE_VERSION008 -> ISteamApps
STEAMREMOTESTORAGE_INTERFACE_VERSION013 -> ISteamRemoteStorage
STEAMSCREENSHOTS_INTERFACE_VERSION002 -> ISteamScreenshots
STEAMHTTP_INTERFACE_VERSION002 -> ISteamHTTP
STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001 -> ISteamUnifiedMessages
SteamController003     -> ISteamController
STEAMUGC_INTERFACE_VERSION008 -> ISteamUGC
STEAMAPPLIST_INTERFACE_VERSION001 -> ISteamAppList
STEAMMUSIC_INTERFACE_VERSION001 -> ISteamMusic
STEAMMUSICREMOTE_INTERFACE_VERSION001 -> ISteamMusicRemote
STEAMHTMLSURFACE_INTERFACE_VERSION_003 -> ISteamHTMLSurface
STEAMINVENTORY_INTERFACE_V001 -> ISteamInventory
STEAMVIDEO_INTERFACE_V001 -> ISteamVideo
```

## Matchmaking / Lobby System

**Network topology**: Steam Lobbies + custom Capcom matchmaking server.

### cNetRoomCmu (Capcom Matchmaking Unit)

Created during `sNetworkRoom::sNetworkRoom` at `0x14028c700`.

- Game identifier string: `"MARVEL_VS_CAPCOM_3"` (used to identify the game to the CMU)
- The room communicates with a Capcom matchmaking server (CMU = Capcom Matchmaking Unit)
- Rooms are identified by IDs, contain player info, battle records, match state
- `cNetRoomCmu::MsgRoomVer` — version negotiation
- `cNetRoomCmu::MsgRoomInfo` — room metadata
- `cNetRoomCmu::MsgPlayerInfo` / `MsgPlayerInfoAll` — player info exchange
- `cNetRoomCmu::MsgMatchReadyInfo` / `MsgMatchReadyOk` — ready state
- `cNetRoomCmu::MsgBattleResult` — match results
- `cNetRoomCmu::MsgRoomID` — room identification

### sNetworkRoom

Higher-level room abstraction. Handles:
- Player slots (array of `NetworkUniqueId`)
- Ready states
- Match configuration
- UI state for lobby display

The string `+connect_lobby` exists at `0x140b083e0` (used via `FUN_140255a70`),
suggesting lobby connectivity is tied to the Steam lobby system.

### Summary

The game uses a **hybrid approach**:
1. **Capcom Matchmaking Server** (CMU) — handles room creation, player matching,
   battle records, and coordination
2. **Steam P2P** (SteamNetworking005) — handles the actual gameplay data transport
3. **Steam Lobbies** (SteamMatchMaking009) — likely used for session discovery
   and invite functionality

## Prediction / Delay System

### How Input Delay Works

1. The local player's inputs are queued into a ring buffer
2. These inputs are sent to the remote player over Steam P2P
3. The local player also receives the remote player's inputs
4. Both sides delay processing by `N` frames (the delay value at `+0xd78`)
5. The delay is **dynamically adjusted** based on RTT (`+0xdf4`) and buffer health

### Prediction

When remote input hasn't arrived yet (within the prediction window `+0xdbc`):
- The game **re-uses the last known input** (it doesn't actually predict — it holds)
- The retransmission system (`FUN_14028b1d0`) tries to resend/recover
- No actual forward prediction of inputs occurs

### Desync Detection

- Two CRC/checksum pairs are stored at `+0xe14` and `+0xe1c`
- `FUN_140288db0` (`0x140288db0`) compares them:
  - If CRCs match: resets all four to zero
  - If CRCs mismatch: returns `true` (desync detected)
  - This is used by the gallery/spectator system

## Recommendations for Rollback Implementation

### Option A: Piggyback off existing infrastructure (moderate complexity)

**Pro:**
- Ring buffer already exists (120-frame capacity, well-structured)
- Network transport layer (Steam P2P) already works
- Message system can be extended
- Input format is known (16-bit bitfield)
- Frame timing / counter is already integrated

**Con:**
- No state save/restore at all — you'd need to add this for every game object
- The existing delay logic is deeply baked into the frame pipeline
- Dynamic delay adjustment fights against rollback (it expects variable latency)
- The prediction buffer (`+0xd20`) is used asymmetrically between host/client
- The Capcom CMU server dependency adds complexity for a custom solution
- You'd need to hook every frame update to implement save states

### Option B: Own ring buffer + IP direct connect (more work but cleaner)

**Pro:**
- No fighting with existing delay logic
- Full control over input prediction and state management
- No Capcom server dependency
- Can implement proper GGPO-style rollback
- Steam P2P can be bypassed or wrapped freely

**Con:**
- Must implement own networking from scratch
- Must hook game's frame update loop for save states
- Must implement own serialization of game state
- Must work around game's existing pad input system
- Must handle Steam overlay / invites yourself

### Verdict

**Option A is risky** — the existing delay system is not designed for rollback.
The "prediction" is really just hold-last-input, and there's no state
save/restore infrastructure. You'd be fighting the architecture.

**Option B is recommended** — hook the game at the input level (replace the
pad read) and at the frame level (save/restore state). Run your own rollback
loop with your own ring buffer. The game's existing netcode is fine for single-player
and replays but not worth adapting for rollback — the delay-based assumption
is too deeply embedded.
