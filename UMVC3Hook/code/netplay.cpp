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


static HANDLE               g_serviceThread  = nullptr;
static uint32_t             g_recvCount = 0;
static uint32_t             g_lastRecv  = 0;
static std::atomic<bool>    g_running { false };

// Delay-change handshake — queue-based control channels (REQ and ACK).
static std::vector<DelayChangeRequest> g_inboundDelayReqQueue;
static CRITICAL_SECTION               g_delayReqCS;
static std::vector<DelayChangeRequest> g_inboundDelayAckQueue;
static CRITICAL_SECTION               g_delayAckCS;

// Outbound REQ — signalled by the game thread, consumed by the service thread.
static bool    g_outboundReqNew = false;
static int32_t g_outboundReqDelay = -1;
static int32_t g_outboundReqRollback = -1;

// Outbound ACK — signalled by the game thread (receiver of REQ), consumed
// by the service thread.
static bool    g_outboundAckNew = false;
static int32_t g_outboundAckDelay = -1;
static int32_t g_outboundAckRollback = -1;
static int32_t g_outboundAckActivation = -1;

// Rollback-only (single-value, no queue needed — loss-tolerant).
static int32_t              g_outboundRollback  = -1;
static bool                 g_outboundRollbackNew = false;
static int32_t              g_inboundRollback   = -1;
static bool                 g_inboundRollbackNew = false;
static CRITICAL_SECTION     g_rollbackCS;

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

        // --- Send delay-change REQUEST (if a new value is pending) ---
        {
            bool hasNewReq = false;
            int32_t reqDelay = -1;
            int32_t reqRollback = -1;
            {
                ScopedCS lock(g_delayReqCS);
                if (g_outboundReqNew)
                {
                    reqDelay       = g_outboundReqDelay;
                    reqRollback    = g_outboundReqRollback;
                    hasNewReq = true;
                    g_outboundReqNew = false;
                }
            }

            if (hasNewReq && g_peer)
            {
                DelayReqPacket dp;
                dp.type           = PACKET_DELAY_REQ;
                dp.delay          = reqDelay;
                dp.rollbackFrames = reqRollback;
                ENetPacket* packet = enet_packet_create(
                    &dp, sizeof(DelayReqPacket), ENET_PACKET_FLAG_RELIABLE
                );
                if (enet_peer_send(g_peer, 0, packet) != 0)
                    enet_packet_destroy(packet);
                enet_host_flush(g_host);
            }
        }

        // --- Send delay-change ACKNOWLEDGMENT (if a new value is pending) ---
        {
            bool hasNewAck = false;
            int32_t ackDelay = -1;
            int32_t ackRollback = -1;
            int32_t ackActivation = -1;
            {
                ScopedCS lock(g_delayAckCS);
                if (g_outboundAckNew)
                {
                    ackDelay       = g_outboundAckDelay;
                    ackRollback    = g_outboundAckRollback;
                    ackActivation  = g_outboundAckActivation;
                    hasNewAck = true;
                    g_outboundAckNew = false;
                }
            }

            if (hasNewAck && g_peer)
            {
                DelayAckPacket dp;
                dp.type            = PACKET_DELAY_ACK;
                dp.delay           = ackDelay;
                dp.rollbackFrames  = ackRollback;
                dp.activationFrame = ackActivation;
                ENetPacket* packet = enet_packet_create(
                    &dp, sizeof(DelayAckPacket), ENET_PACKET_FLAG_RELIABLE
                );
                if (enet_peer_send(g_peer, 0, packet) != 0)
                    enet_packet_destroy(packet);
                enet_host_flush(g_host);
            }
        }

        // --- Send rollback-only control packet (applied immediately) ---
        {
            ScopedCS lock(g_rollbackCS);
            if (g_outboundRollbackNew && g_peer)
            {
                RollbackPacket rp;
                rp.type            = PACKET_ROLLBACK;
                rp.rollbackFrames  = g_outboundRollback;
                ENetPacket* packet = enet_packet_create(
                    &rp, sizeof(RollbackPacket), ENET_PACKET_FLAG_RELIABLE
                );
                if (enet_peer_send(g_peer, 0, packet) != 0)
                    enet_packet_destroy(packet);
                enet_host_flush(g_host);
                g_outboundRollbackNew = false;
            }
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

                            case PACKET_DELAY_REQ:
                                if (event.packet->dataLength >= sizeof(DelayReqPacket))
                                {
                                    const DelayReqPacket* dp = (const DelayReqPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_delayReqCS);
                                        DelayChangeRequest req;
                                        req.delay           = dp->delay;
                                        req.rollbackFrames  = dp->rollbackFrames;
                                        req.activationFrame = -1;
                                        g_inboundDelayReqQueue.push_back(req);
                                    }
                                }
                                break;

                            case PACKET_DELAY_ACK:
                                if (event.packet->dataLength >= sizeof(DelayAckPacket))
                                {
                                    const DelayAckPacket* dp = (const DelayAckPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_delayAckCS);
                                        DelayChangeRequest ack;
                                        ack.delay           = dp->delay;
                                        ack.rollbackFrames  = dp->rollbackFrames;
                                        ack.activationFrame = dp->activationFrame;
                                        g_inboundDelayAckQueue.push_back(ack);
                                    }
                                }
                                break;

                            case PACKET_ROLLBACK:
                                if (event.packet->dataLength >= sizeof(RollbackPacket))
                                {
                                    const RollbackPacket* rp = (const RollbackPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_rollbackCS);
                                        g_inboundRollback     = rp->rollbackFrames;
                                        g_inboundRollbackNew  = true;
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
        InitializeCriticalSection(&g_inboundCS);
        InitializeCriticalSection(&g_delayReqCS);
        InitializeCriticalSection(&g_delayAckCS);
        InitializeCriticalSection(&g_rollbackCS);

        g_outboundReqNew = false;
        g_outboundAckNew = false;
        g_outboundRollbackNew = false;
        g_inboundRollbackNew = false;
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
// Delay-change handshake (REQ/ACK) — queue-based, thread-safe.
// ---------------------------------------------------------------------------
void Netplay_SendDelayReq(int32_t delay, int32_t rollbackFrames)
{
    ScopedCS lock(g_delayReqCS);
    g_outboundReqDelay    = delay;
    g_outboundReqRollback = rollbackFrames;
    g_outboundReqNew      = true;
}

void Netplay_SendDelayAck(int32_t delay, int32_t rollbackFrames, int32_t activationFrame)
{
    ScopedCS lock(g_delayAckCS);
    g_outboundAckDelay       = delay;
    g_outboundAckRollback    = rollbackFrames;
    g_outboundAckActivation  = activationFrame;
    g_outboundAckNew         = true;
}

bool Netplay_GetInboundDelayReq(DelayChangeRequest& outReq)
{
    ScopedCS lock(g_delayReqCS);
    if (!g_inboundDelayReqQueue.empty())
    {
        outReq = g_inboundDelayReqQueue.front();
        g_inboundDelayReqQueue.erase(g_inboundDelayReqQueue.begin());
        return true;
    }
    return false;
}

bool Netplay_GetInboundDelayAck(DelayChangeRequest& outAck)
{
    ScopedCS lock(g_delayAckCS);
    if (!g_inboundDelayAckQueue.empty())
    {
        outAck = g_inboundDelayAckQueue.front();
        g_inboundDelayAckQueue.erase(g_inboundDelayAckQueue.begin());
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Rollback-only control — single-value, applied immediately on receipt.
// ---------------------------------------------------------------------------
void Netplay_SendRollback(int32_t rollbackFrames)
{
    ScopedCS lock(g_rollbackCS);
    g_outboundRollback    = rollbackFrames;
    g_outboundRollbackNew = true;
}

bool Netplay_GetInboundRollback(int32_t& outRollback)
{
    ScopedCS lock(g_rollbackCS);
    if (g_inboundRollbackNew)
    {
        outRollback          = g_inboundRollback;
        g_inboundRollbackNew = false;
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
        DeleteCriticalSection(&g_inboundCS);
        DeleteCriticalSection(&g_delayReqCS);
        DeleteCriticalSection(&g_delayAckCS);
        DeleteCriticalSection(&g_rollbackCS);
        g_csInitialized = false;
    }

    HookLog("Netplay: Shutdown complete\n");
}
