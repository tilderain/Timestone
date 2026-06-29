#pragma once
#include <cstdint>
#include <enet/enet.h>

// ---------------------------------------------------------------------------
// Netplay: rollback netcode input transport over ENet.
//
// Architecture:
//   Raw physical input is captured at the pad level by
//   hkGetsCurrentInput_withSOCD.  For the local player, raw input is
//   accumulated into a delay shift buffer (g_oldInputs[]) and the delayed
//   value is returned to the native pipeline — letting button remapping,
//   face-left flipping, SOCD cleaning, and all history rings populate
//   naturally.  For the remote player, the centrally-resolved network input
//   (g_usedRemoteInput) is returned instead of the physical pad value.
//
//   Each frame's accumulated local input is sent as a sliding-history window
//   (NET_HISTORY_SIZE entries) over the wire, indexed by the sender's
//   absolute frame number.
//
//   On receipt, every history slot is written into g_receivedInput[] at
//   the sender's absolute frame index (% RING_SIZE), binding both games
//   to the same timeline and preventing drift.
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
//   A checksum packet is piggybacked on every frame's InputPacket: it
//   carries an additive checksum of all character position/state.  If
//   the remote side's checksum for a frame diverges from the local
//   checksum beyond a threshold, desync is declared and the game pauses
//   with an on-screen warning.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Packet type identifiers (first uint32_t of every ENet payload).
// ---------------------------------------------------------------------------
enum PacketType : uint32_t {
    PACKET_INPUT = 0,  // InputPacket
    PACKET_DELAY = 1,  // DelayPacket
    PACKET_RESIM_NOTIFY = 2,  // ResimNotifyPacket
};

#pragma pack(push, 1)
constexpr size_t NET_HISTORY_SIZE = 16;

struct InputPacket {
    uint32_t type;               // PACKET_INPUT
    uint32_t start_frame;        // inputs[0] corresponds to this frame
    uint32_t checksum_frame;     // game frame this checksum is for (past frame)
    uint32_t checksum;           // checksum of game state at checksum_frame
    uint32_t inputs[NET_HISTORY_SIZE];
    int32_t  frame_advantage;    // sender's local frame advantage (negative = sender is ahead)
    uint32_t flags;              // 0 = seed packet, non-zero = real packet
};

struct DelayPacket {
    uint32_t type;               // PACKET_DELAY
    int32_t  delay;              // delay frame count
    int32_t  rollbackFrames;     // rollback frame count (both sides stay in sync)
};

static constexpr int MAX_RESIM_FRAMES = 16;

struct ResimFrameResult {
    int32_t  frame;          // absolute frame number
    uint32_t preActionId;    // entry actionId from live reference
    float    prePosX;        // entry X position from live reference
    float    prePosY;        // entry Y position from live reference
    uint32_t postActionId;   // character actionId after resim tick (remote side)
    float    postPosX;       // character X position after resim tick
    float    postPosY;       // character Y position after resim tick
    uint32_t usedInput;      // input used for this frame during resim
    uint32_t recvInput;      // input received from remote for this frame
};

struct ResimNotifyPacket {
    uint32_t type;               // PACKET_RESIM_NOTIFY
    int32_t  resimStartFrame;    // first frame of re-simulation
    int32_t  resimEndFrame;      // end frame (exclusive)
    uint32_t correctedTeam;      // which team (0 or 1) had its input corrected
    uint32_t frameCount;         // number of valid entries in frames[]
    ResimFrameResult frames[MAX_RESIM_FRAMES];
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

// Send a delay-control value to the remote peer.
void Netplay_SendDelay(int32_t delay);

// Send a rollback-frame control value to the remote peer.
void Netplay_SendRollback(int32_t rollbackFrames);

// Send a resim-notify packet to the remote peer — signals which frames were
// re-simulated due to a late-arriving input correction.  The caller should
// fill ringIdx fields (correctedTeam, frameCount, frames[]) before sending.
void Netplay_SendResimNotify(const ResimNotifyPacket& pkt);

// Retrieve the last resim-notify received from the remote peer.  Returns true
// if a new notification was available (consumed by this call).
bool Netplay_GetInboundResimNotify(ResimNotifyPacket& outNotify);

// Retrieve the last delay value received from the remote peer.  Returns true
// if a new value was available (it is consumed by this call).
bool Netplay_GetInboundDelay(int32_t& outDelay);

// Retrieve the last rollback value received from the remote peer.  Returns true
// if a new value was available (it is consumed by this call).
bool Netplay_GetInboundRollback(int32_t& outRollback);

// --- Desync-check: checksum is piggybacked on InputPacket (Slippi-style) ---
