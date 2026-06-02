#pragma once
#include <cstdint>
#include <ggponet.h>

// ---------------------------------------------------------------------------
// GGPO Netplay Layer
//
// Wraps the GGPO SDK session lifecycle, player management, and input
// synchronisation.  Global state is exposed directly so controller.cpp
// and framestepper.cpp can access the synchronised input array and session
// flags without going through an abstraction layer.
//
// Session flow:
//   1. GGPO_StartSession()  — create session, add local + remote players
//   2. GGPO_WaitForRunning() — mark session ready (callbacks begin)
//   3. Each frame:
//        GGPO_AddLocalInput()  — submit physical input
//        GGPO_SyncInputs()     — retrieve sync'd inputs for both players
//        Game logic processes g_ggpoSyncInputs[]
//        GGPO_AdvanceFrame()   — notify GGPO frame is done
//   4. GGPO_CloseSession()   — cleanup
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Global state — exposed for direct access by controller/framestepper
// ---------------------------------------------------------------------------
extern GGPOSession* g_ggpoSession;
extern GGPOPlayerHandle g_ggpoLocalHandle;
extern GGPOPlayerHandle g_ggpoRemoteHandle;
extern bool g_ggpoActive;
extern bool g_ggpoRunning;
extern bool g_ggpoIsHost;

// Synchronised input array: [0] = P1 input, [1] = P2 input (uint32 each).
extern uint32_t g_ggpoSyncInputs[2];

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------
bool GGPO_StartSession(bool isHost, const char* remoteIP, unsigned short remotePort, unsigned short localPort);
void GGPO_CloseSession();
bool GGPO_IsActive();
bool GGPO_IsHost();

// ---------------------------------------------------------------------------
// Input synchronisation
// ---------------------------------------------------------------------------
GGPOErrorCode GGPO_AddLocalInput(uint32_t input);
GGPOErrorCode GGPO_SyncInputs();
GGPOErrorCode GGPO_AdvanceFrame();
GGPOErrorCode GGPO_Idle(int timeout);

// ---------------------------------------------------------------------------
// Network stats
// ---------------------------------------------------------------------------
uint32_t GGPO_GetRTT();
GGPOErrorCode GGPO_GetNetworkStats(GGPONetworkStats* stats);

// ---------------------------------------------------------------------------
// Session ready
// ---------------------------------------------------------------------------
void GGPO_WaitForRunning();
