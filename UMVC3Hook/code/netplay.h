#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Netplay: GGPO-backed netplay interface.
//
// Thin shim layer that translates high-level netplay operations into GGPO
// API calls.  GGPO handles synchronization, rollback, and network I/O.
//
// Netplay_Init creates the GGPO session with the given role (host/client)
// and connection parameters.  The session runs asynchronously;
// Netplay_IsActive returns true once the sync completes.
// ---------------------------------------------------------------------------

// Initialize a GGPO netplay session.
// isHost=true  -> listen on localPort for an incoming connection.
// isHost=false -> connect to remoteIP:remotePort.
void Netplay_Init(bool isHost, const char* remoteIP, unsigned short remotePort, unsigned short localPort);

// Shut down the GGPO session and free resources.
void Netplay_Shutdown();

// Returns true if a GGPO session is active.
bool Netplay_IsActive();

// Returns true if this instance is the netplay host (server).
bool Netplay_IsHost();

// Returns the peer's round-trip time (ping) in milliseconds, or 0 if inactive.
uint32_t Netplay_GetRTT();
