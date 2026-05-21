#pragma once
#include <cstdint>
#include <enet/enet.h>

// ---------------------------------------------------------------------------
// Netplay: frame-synced input transport over ENet.
//
// A dedicated background thread handles ALL ENet I/O (send, receive,
// keep-alive). The game thread communicates via lock-free atomics:
//
//   Game thread (HookedMove):  writes local P1 input -> reads remote input
//   Service thread (1ms loop): reads outbound atom -> enet_peer_send
//                              enet_host_service -> writes inbound atom
//
// This guarantees ENet is always serviced (no timeout disconnects) and
// avoids thread-safety issues with ENet internals.
// ---------------------------------------------------------------------------

// Input packet sent every frame over the wire
#pragma pack(push, 1)
struct InputPacket {
    uint32_t sequence;     // monotonic frame counter (for debugging)
    uint32_t inputBits;    // input bitmask (IN_UP, IN_DOWN, etc.)
};
#pragma pack(pop)

// Hand over the ENet host/peer from the launch menu and start the service
// thread. Must be called once from the game thread after the game is loaded.
void Netplay_Init(ENetHost* host, ENetPeer* peer);

// Called once per game frame from HookedMove (game thread).
// Writes local P1 input into the outbound atom for the service thread to send.
void Netplay_SendInput(uint32_t localInput);

// Returns the most recently received remote input bitmask (from the atom
// updated by the service thread). Call after Netplay_SendInput each frame.
uint32_t Netplay_GetRemoteInput();

// Returns true if at least one remote input packet has been received.
// Use this guard before applying Netplay_GetRemoteInput() to distinguish
// "no data yet" from "remote player is idle (input = 0)".
bool Netplay_HasRemoteInput();

// Returns true if netplay is active and the peer is still connected.
bool Netplay_IsActive();

// Returns true if this instance is the netplay host (server), false if client.
bool Netplay_IsHost();

// Returns the peer's mean round-trip time (ping) in milliseconds, or 0 if inactive.
uint32_t Netplay_GetRTT();

// Shut down the service thread, disconnect peer, destroy host.
void Netplay_Shutdown();
