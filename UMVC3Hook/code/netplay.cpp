// netplay.cpp
// Thin shim layer between the game's netplay API and the GGPO backend.
//
// GGPO handles all synchronization, rollback, and network I/O internally.
// This module translates the simple Netplay_* interface into GGPO calls
// and manages session lifecycle.
#include "netplay.h"
#include "ggpo_netplay.h"
#include "controller.h"
#include <cstring>

extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void Netplay_Init(bool isHost, const char* remoteIP, unsigned short remotePort, unsigned short localPort)
{
    Controller_ResetBuffers();
    if (GGPO_StartSession(isHost, remoteIP, remotePort, localPort))
    {
        HookLog("Netplay: GGPO session started (isHost=%d, remote=%s:%d)\n",
                isHost ? 1 : 0, remoteIP, remotePort);
        GGPO_WaitForRunning();
    }
    else
    {
        HookLog("Netplay: FAILED to start GGPO session\n");
    }
}

void Netplay_Shutdown()
{
    GGPO_CloseSession();
}

bool Netplay_IsActive()
{
    return GGPO_IsActive();
}

bool Netplay_IsHost()
{
    return GGPO_IsHost();
}

uint32_t Netplay_GetRTT()
{
    return GGPO_GetRTT();
}
