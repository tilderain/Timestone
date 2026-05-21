#include "netplay.h"
#include <cstring>
#include <cstdio>
#include <atomic>
#include <Windows.h>

// HookLog from dllmain.cpp
extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ENetHost*            g_host   = nullptr;
static ENetPeer*            g_peer   = nullptr;
static bool                 g_active = false;

// Thread-safe input exchange (lock-free atomics)
static std::atomic<uint32_t> g_outboundInput { 0 };   // game -> network thread
static std::atomic<uint32_t> g_inboundInput  { 0 };   // network -> game thread
static std::atomic<bool>     g_running { false };

static HANDLE g_serviceThread = nullptr;
static uint32_t g_seq = 0;           // only accessed from service thread
static uint32_t g_recvCount = 0;     // only accessed from service thread
static uint32_t g_lastRecv  = 0;     // snapshot for stats

// ---------------------------------------------------------------------------
// Service thread: owns ALL ENet calls
// ---------------------------------------------------------------------------
static DWORD WINAPI Netplay_ServiceThread(LPVOID)
{
    HookLog("Netplay: Service thread started\n");

    while (g_running)
    {
        // --- Periodic stats (every ~3 seconds) ---
        static uint32_t lastStatsTick = GetTickCount();
        uint32_t now = GetTickCount();
        if (now - lastStatsTick >= 3000)
        {
            HookLog("Netplay: received %u packets in last 3s (total %u)\n",
                    g_recvCount - g_lastRecv, g_recvCount);
            g_lastRecv = g_recvCount;
            lastStatsTick = now;
        }

        // --- Send outbound input (if the game thread queued any) ---
        uint32_t outInput = g_outboundInput.exchange(0);
        if (outInput != 0 && g_peer)
        {
            InputPacket pkt;
            pkt.sequence  = g_seq++;
            pkt.inputBits = outInput;

            ENetPacket* packet = enet_packet_create(
                &pkt, sizeof(pkt), ENET_PACKET_FLAG_UNSEQUENCED
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        // --- Service the host (keep-alive + receive) ---
        // Use a 1 ms timeout so the loop keeps the connection alive
        // without busy-waiting.
        if (g_host)
        {
            ENetEvent event;
            while (enet_host_service(g_host, &event, 1) > 0)
            {
                if (event.type == ENET_EVENT_TYPE_RECEIVE)
                {
                    if (event.packet->dataLength >= sizeof(InputPacket))
                    {
                        const InputPacket* inPkt = (const InputPacket*)event.packet->data;
                        g_inboundInput.store(inPkt->inputBits);
                        g_recvCount++;
                    }
                    enet_packet_destroy(event.packet);
                }
                else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                {
                    HookLog("Netplay: Remote peer disconnected\n");
                    g_running = false;
                    g_active  = false;
                    g_peer    = nullptr;
                    event.peer->data = nullptr;
                }
                else if (event.type == ENET_EVENT_TYPE_CONNECT)
                {
                    HookLog("Netplay: Late CONNECT event\n");
                    event.peer->data = (void*)"connected";
                }
            }
        }
    }

    HookLog("Netplay: Service thread exiting\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Netplay_Init(ENetHost* host, ENetPeer* peer)
{
    g_host   = host;
    g_peer   = peer;
    g_seq    = 0;
    g_active = (host != nullptr && peer != nullptr);

    if (!g_active)
    {
        HookLog("Netplay: Init called but host/peer not ready — disabled\n");
        return;
    }

    g_inboundInput.store(0);
    g_outboundInput.store(0);
    g_seq = 0;
    g_recvCount = 0;
    g_lastRecv = 0;
    g_running = true;

    // Start the background service thread
    g_serviceThread = CreateThread(nullptr, 0, Netplay_ServiceThread, nullptr, 0, nullptr);
    if (g_serviceThread)
    {
        HookLog("Netplay: Initialized (host=%p, peer=%p), service thread running\n",
                (void*)host, (void*)peer);
    }
    else
    {
        HookLog("Netplay: Failed to create service thread!\n");
        g_running = false;
        g_active  = false;
    }
}

void Netplay_SendInput(uint32_t localInput)
{
    // Write the current frame's input into the outbound atom.
    // The service thread picks it up and sends it.
    g_outboundInput.store(localInput);
}

uint32_t Netplay_GetRemoteInput()
{
    return g_inboundInput.load();
}

bool Netplay_IsActive()
{
    return g_active && g_running && g_peer != nullptr;
}

void Netplay_Shutdown()
{
    HookLog("Netplay: Shutting down...\n");

    g_running = false;

    // Wait for the service thread to exit
    if (g_serviceThread)
    {
        WaitForSingleObject(g_serviceThread, 3000);
        CloseHandle(g_serviceThread);
        g_serviceThread = nullptr;
    }

    // Now it's safe to destroy ENet resources
    if (g_peer)
    {
        enet_peer_disconnect(g_peer, 0);
        g_peer = nullptr;
    }

    if (g_host)
    {
        if (g_host->connectedPeers > 0)
        {
            ENetEvent ev;
            enet_host_service(g_host, &ev, 1000);
        }
        enet_host_destroy(g_host);
        g_host = nullptr;
    }

    g_active = false;
    HookLog("Netplay: Shutdown complete\n");
}
