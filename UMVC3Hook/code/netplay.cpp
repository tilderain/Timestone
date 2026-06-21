#include "netplay.h"
#include <cstring>
#include <cstdio>
#include <atomic>
#include <Windows.h>
#include <vector>

extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static ENetHost*            g_host   = nullptr;
static ENetPeer*            g_peer   = nullptr;
static bool                 g_active = false;

// Thread-safe queues protected by Win32 critical sections
// (MinGW-w64 win32 threading model does not support std::mutex)
static InputPacket              g_outboundPacket = {};
static bool                     g_outboundNew    = false;
static CRITICAL_SECTION         g_outboundCS;

static std::vector<InputPacket> g_inboundQueue;
static CRITICAL_SECTION         g_inboundCS;

// Desync-check packet queue (separate from input packets).
static std::vector<DesyncCheckPacket> g_inboundDesyncQueue;
static CRITICAL_SECTION               g_inboundDesyncCS;

static HANDLE               g_serviceThread  = nullptr;
static uint32_t             g_recvCount = 0;
static uint32_t             g_lastRecv  = 0;
static std::atomic<bool>    g_running { false };

// Delay control channel — small packets to keep both peers' delay in sync.
static int32_t              g_outboundDelay    = -1;
static bool                 g_outboundDelayNew = false;
static int32_t              g_inboundDelay     = -1;
static bool                 g_inboundDelayNew  = false;
static CRITICAL_SECTION     g_delayCS;

// Desync-check outbound packet (separate from input to avoid conflicts).
static DesyncCheckPacket   g_outboundDesyncPkt   = {};
static bool                g_outboundDesyncNew   = false;
static CRITICAL_SECTION    g_outboundDesyncCS;

// One-time critical section initialization flag
static bool                 g_csInitialized  = false;

// Reset hook defined in controller.cpp
extern "C" void Controller_ResetBuffers();

// ---------------------------------------------------------------------------
// Scoped critical section helper (lightweight replacement for std::lock_guard)
// ---------------------------------------------------------------------------
class ScopedCS {
    CRITICAL_SECTION* m_cs;
public:
    ScopedCS(CRITICAL_SECTION& cs) : m_cs(&cs) { EnterCriticalSection(m_cs); }
    ~ScopedCS() { LeaveCriticalSection(m_cs); }
};

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

        // --- Send outbound packet ---
        bool hasNewOutbound = false;
        InputPacket outPkt = {};
        {
            ScopedCS lock(g_outboundCS);
            if (g_outboundNew)
            {
                outPkt = g_outboundPacket;
                hasNewOutbound = true;
                g_outboundNew = false;
            }
        }

        if (hasNewOutbound && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &outPkt, sizeof(InputPacket), ENET_PACKET_FLAG_UNSEQUENCED
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        // --- Send delay control packet (if a new value is pending) ---
        bool hasNewDelay = false;
        int32_t delayVal = -1;
        {
            ScopedCS lock(g_delayCS);
            if (g_outboundDelayNew)
            {
                delayVal = g_outboundDelay;
                hasNewDelay = true;
                g_outboundDelayNew = false;
            }
        }

        if (hasNewDelay && g_peer)
        {
            DelayPacket dp;
            dp.type  = PACKET_DELAY;
            dp.delay = delayVal;
            ENetPacket* packet = enet_packet_create(
                &dp, sizeof(DelayPacket), ENET_PACKET_FLAG_UNSEQUENCED
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        // --- Send desync-check packet ---
        bool hasNewDesync = false;
        DesyncCheckPacket outDesyncPkt = {};
        {
            ScopedCS lock(g_outboundDesyncCS);
            if (g_outboundDesyncNew)
            {
                outDesyncPkt = g_outboundDesyncPkt;
                hasNewDesync = true;
                g_outboundDesyncNew = false;
            }
        }

        if (hasNewDesync && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &outDesyncPkt, sizeof(DesyncCheckPacket), ENET_PACKET_FLAG_UNSEQUENCED
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        // --- Service the host ---
        if (g_host)
        {
            ENetEvent event;
            while (enet_host_service(g_host, &event, 1) > 0)
            {
                if (event.type == ENET_EVENT_TYPE_RECEIVE)
                {
                    if (event.packet->dataLength >= 4)
                    {
                        uint32_t pktType = *(uint32_t*)event.packet->data;
                        switch (pktType)
                        {
                            case PACKET_INPUT:
                                if (event.packet->dataLength >= sizeof(InputPacket))
                                {
                                    const InputPacket* inPkt = (const InputPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_inboundCS);
                                        g_inboundQueue.push_back(*inPkt);
                                    }
                                    g_recvCount++;
                                }
                                break;

                            case PACKET_DESYNC_CHECK:
                                if (event.packet->dataLength >= sizeof(DesyncCheckPacket))
                                {
                                    const DesyncCheckPacket* dcp = (const DesyncCheckPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_inboundDesyncCS);
                                        g_inboundDesyncQueue.push_back(*dcp);
                                    }
                                }
                                break;

                            case PACKET_DELAY:
                                if (event.packet->dataLength >= sizeof(DelayPacket))
                                {
                                    const DelayPacket* dp = (const DelayPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_delayCS);
                                        g_inboundDelay = dp->delay;
                                        g_inboundDelayNew = true;
                                    }
                                }
                                break;
                        }
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
    g_active = (host != nullptr && peer != nullptr);

    if (!g_active)
    {
        HookLog("Netplay: Init called but host/peer not ready — disabled\n");
        return;
    }

    // Initialize critical sections (once)
    if (!g_csInitialized)
    {
        InitializeCriticalSection(&g_outboundCS);
        InitializeCriticalSection(&g_outboundDesyncCS);
        InitializeCriticalSection(&g_inboundCS);
        InitializeCriticalSection(&g_inboundDesyncCS);
        InitializeCriticalSection(&g_delayCS);
        g_csInitialized = true;
    }

    {
        ScopedCS lock1(g_outboundCS);
        ScopedCS lock2(g_outboundDesyncCS);
        ScopedCS lock3(g_inboundCS);
        ScopedCS lock4(g_inboundDesyncCS);
        ScopedCS lock5(g_delayCS);
        g_outboundNew = false;
        g_outboundDesyncNew = false;
        g_inboundQueue.clear();
        g_inboundDesyncQueue.clear();
        g_outboundDelayNew = false;
        g_inboundDelayNew = false;
        std::memset(&g_outboundPacket, 0, sizeof(InputPacket));
    }

    g_recvCount = 0;
    g_lastRecv = 0;
    g_running = true;

    // Reset internal player pad ring buffers
    Controller_ResetBuffers();

    g_serviceThread = CreateThread(nullptr, 0, Netplay_ServiceThread, nullptr, 0, nullptr);
    if (g_serviceThread)
    {
        HookLog("Netplay: Initialized, service thread running\n");

        // Send a seed (zeroed) packet to pre-load the peer's receive buffer.
        // Without this, both sides deadlock on frame 0 — each waits for the
        // other's data in Controller_OnFrameStart but neither reaches
        // Controller_OnFrameEnd to send its own first packet.
        InputPacket seed = {};
        Netplay_SendPacket(seed);
    }
    else
    {
        HookLog("Netplay: Failed to create service thread\n");
        g_running = false;
        g_active  = false;
    }
}

void Netplay_SendPacket(const InputPacket& pkt)
{
    ScopedCS lock(g_outboundCS);
    g_outboundPacket = pkt;
    g_outboundNew = true;
}

bool Netplay_GetInboundPacket(InputPacket& outPkt)
{
    ScopedCS lock(g_inboundCS);
    if (!g_inboundQueue.empty())
    {
        outPkt = g_inboundQueue.front();
        g_inboundQueue.erase(g_inboundQueue.begin());
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Delay control — out-of-band value sent to keep both peers' delay in sync.
// ---------------------------------------------------------------------------
void Netplay_SendDelay(int32_t delay)
{
    ScopedCS lock(g_delayCS);
    g_outboundDelay = delay;
    g_outboundDelayNew = true;
}

bool Netplay_GetInboundDelay(int32_t& outDelay)
{
    ScopedCS lock(g_delayCS);
    if (g_inboundDelayNew)
    {
        outDelay = g_inboundDelay;
        g_inboundDelayNew = false;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Netplay_SendInput  — convenience wrapper that sends a single uint32 value
//                      in the first history slot (inputs[0]).
// ---------------------------------------------------------------------------
void Netplay_SendInput(uint32_t rawInput)
{
    InputPacket pkt = {};
    pkt.type        = PACKET_INPUT;
    pkt.start_frame = 0;
    pkt.inputs[0] = rawInput;
    // inputs[1..15] remain zero — unused for simple delay-based netplay.

    ScopedCS lock(g_outboundCS);
    g_outboundPacket = pkt;
    g_outboundNew = true;
}

void Netplay_SendDesyncCheck(uint32_t frame, uint32_t checksum)
{
    DesyncCheckPacket pkt;
    pkt.type     = PACKET_DESYNC_CHECK;
    pkt.frame    = frame;
    pkt.checksum = checksum;

    ScopedCS lock(g_outboundDesyncCS);
    g_outboundDesyncPkt = pkt;
    g_outboundDesyncNew = true;
}

bool Netplay_GetInboundDesyncCheck(uint32_t& outFrame, uint32_t& outChecksum)
{
    ScopedCS lock(g_inboundDesyncCS);
    if (!g_inboundDesyncQueue.empty())
    {
        DesyncCheckPacket pkt = g_inboundDesyncQueue.front();
        g_inboundDesyncQueue.erase(g_inboundDesyncQueue.begin());
        outFrame    = pkt.frame;
        outChecksum = pkt.checksum;
        return true;
    }
    return false;
}

bool Netplay_HasRemoteInput()
{
    ScopedCS lock(g_inboundCS);
    return !g_inboundQueue.empty();
}

uint32_t Netplay_GetRemoteInput()
{
    ScopedCS lock(g_inboundCS);
    if (!g_inboundQueue.empty())
        return g_inboundQueue.front().inputs[0];
    return 0;
}

bool Netplay_IsActive()
{
    return g_active && g_running && g_peer != nullptr;
}

bool Netplay_IsHost()
{
    return g_host && g_host->address.port != 0;
}

uint32_t Netplay_GetRTT()
{
    if (g_peer)
        return g_peer->roundTripTime;
    return 0;
}

void Netplay_Shutdown()
{
    HookLog("Netplay: Shutting down...\n");
    g_running = false;

    if (g_serviceThread)
    {
        WaitForSingleObject(g_serviceThread, 3000);
        CloseHandle(g_serviceThread);
        g_serviceThread = nullptr;
    }

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

    if (g_csInitialized)
    {
        DeleteCriticalSection(&g_outboundCS);
        DeleteCriticalSection(&g_outboundDesyncCS);
        DeleteCriticalSection(&g_inboundCS);
        DeleteCriticalSection(&g_inboundDesyncCS);
        DeleteCriticalSection(&g_delayCS);
        g_csInitialized = false;
    }

    HookLog("Netplay: Shutdown complete\n");
}
