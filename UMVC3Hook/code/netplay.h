#pragma once
#include <cstdint>
#include <enet/enet.h>

// ---------------------------------------------------------------------------
// Netplay: rollback netcode input transport over ENet.
//
// Architecture:
//   Local physical input is accumulated per-character in hkprocessFrameInput.
//   Each frame's raw accumulated input is written into a delay ring buffer at
//   (currentFrame + delayFrames), and sent as a sliding-history window
//   (NET_HISTORY_SIZE entries) over the wire.
//
//   On receipt, every history slot is written into the OTHER player's delay
//   buffer at a relative offset from the receiver's current frame, providing
//   resilience against packet loss.  Both sides read at currentFrame during
//   hkprocessFrameInput — symmetric input delay for both players.
//
//   If remote input for the current frame hasn't arrived, we predict using
//   the last known value.  The rollback engine (rollback.cpp) detects
//   late-arriving corrections via g_usedRemoteInput vs g_receivedInput
//   comparison and automatically restores snapshots + re-simulates with
//   render disabled.
//
//   A dedicated background thread owns ALL ENet I/O. The game thread
//   communicates via Win32 critical sections for packet exchange.
//
//   A desync-check packet is also sent every frame: it carries a simple
//   XOR checksum of all character position/state.  If the remote side's
//   checksum for a frame doesn't match the local checksum, desync is
//   declared and the game pauses with an on-screen warning.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Packet type identifiers (first uint32_t of every ENet payload).
// ---------------------------------------------------------------------------
enum PacketType : uint32_t {
    PACKET_INPUT      = 0,  // InputPacket
    PACKET_DELAY_REQ  = 2,  // DelayReqPacket — handshake request
    PACKET_DELAY_ACK  = 3,  // DelayAckPacket — handshake acknowledge
    PACKET_ROLLBACK   = 4,  // RollbackPacket — rollback-only sync
};

#pragma pack(push, 1)
constexpr size_t NET_HISTORY_SIZE = 16;

struct InputPacket {
    uint32_t type;               // PACKET_INPUT
    uint32_t start_frame;        // inputs[0] corresponds to this frame
    uint32_t checksum_frame;     // game frame this checksum is for (past frame)
    uint32_t checksum;           // checksum of game state at checksum_frame
    uint32_t inputs[NET_HISTORY_SIZE];
    uint64_t timestamp_us;       // sender's QPC in microseconds at send time
    uint64_t echo_timestamp_us;  // timestamp_us from remote's most recent packet (0 = none)
};

// ---------------------------------------------------------------------------
// Delay change handshake — negotiated transaction (see framestepper.cpp)
//
// Protocol:
//   1. Initiator sends  MSG_DELAY_REQ(delay, rollbackFrames)
//   2. Receiver computes  T = currentFrame + MARGIN, sends
//      MSG_DELAY_ACK(delay, rollbackFrames, T) AND schedules locally for T.
//   3. Initiator receives MSG_DELAY_ACK, cancels speculation, schedules for T.
// ---------------------------------------------------------------------------
struct DelayReqPacket {
    uint32_t type;           // PACKET_DELAY_REQ
    int32_t  delay;
    int32_t  rollbackFrames;
};

struct DelayAckPacket {
    uint32_t type;            // PACKET_DELAY_ACK
    int32_t  delay;
    int32_t  rollbackFrames;
    int32_t  activationFrame; // T — both sides commit at this frame
};

struct RollbackPacket {
    uint32_t type;            // PACKET_ROLLBACK
    int32_t  rollbackFrames;
};

// Queue-based control channel entry (matches Ack layout; Req omits activationFrame).
struct DelayChangeRequest {
    int32_t delay;
    int32_t rollbackFrames;
    int32_t activationFrame;   // -1 when unused (Req)
};
#pragma pack(pop)

// Hand over the ENet host/peer and start the service thread.
void Netplay_Init(ENetHost* host, ENetPeer* peer);

// Send a single raw uint32 input value over the wire (wraps into InputPacket).
void Netplay_SendInput(uint32_t rawInput);

// Queue a full history packet for the service thread to send.
void Netplay_SendPacket(const InputPacket& pkt);

// Retrieve the most recently received inbound packet. Returns true if data
// was available (the packet is consumed / cleared by this call).
bool Netplay_GetInboundPacket(InputPacket& outPkt);

// Returns true if netplay is active and the peer is still connected.
bool Netplay_IsActive();

// Returns true if this instance is the netplay host (server), false if client.
bool Netplay_IsHost();

// Returns the peer's mean round-trip time (ping) in milliseconds, or 0 if inactive.
uint32_t Netplay_GetRTT();

// Compatibility queries (return first slot of latest inbound packet)
bool     Netplay_HasRemoteInput();
uint32_t Netplay_GetRemoteInput();

// Shut down the service thread, disconnect peer, destroy host.
void Netplay_Shutdown();

// Send a delay-change request (handshake REQ).  Does NOT schedule locally.
void Netplay_SendDelayReq(int32_t delay, int32_t rollbackFrames);

// Send a delay-change acknowledgment (handshake ACK).  Sent by the receiver
// after computing the safe commit frame T.
void Netplay_SendDelayAck(int32_t delay, int32_t rollbackFrames, int32_t activationFrame);

// Retrieve an inbound delay REQ (thread-safe queue).  Returns true if consumed.
bool Netplay_GetInboundDelayReq(DelayChangeRequest& outReq);

// Retrieve an inbound delay ACK (thread-safe queue).  Returns true if consumed.
bool Netplay_GetInboundDelayAck(DelayChangeRequest& outAck);

// Send a rollback-only control value to the remote peer (applied immediately).
void Netplay_SendRollback(int32_t rollbackFrames);

// Retrieve inbound rollback-only value.  Returns true if a new value consumed.
bool Netplay_GetInboundRollback(int32_t& outRollback);

// --- Desync-check: checksum is piggybacked on InputPacket (Slippi-style) ---
