#include "netplay.h"
#include "rollback.h"
#include "controller.h"
#include <cstring>
#include <cstdio>
#include <atomic>
#include <Windows.h>

extern void HookLog(const char* fmt, ...);
extern "C" uint32_t FrameStepper_GetBattleGeneration();

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

static constexpr size_t         MAX_INBOUND_PACKETS = 256;
static InputPacket              g_inboundQueue[MAX_INBOUND_PACKETS] = {};
static size_t                   g_inboundHead = 0;
static size_t                   g_inboundCount = 0;
static CRITICAL_SECTION         g_inboundCS;

// Desync-check packet queue (separate from input packets).


static HANDLE               g_serviceThread  = nullptr;
static uint32_t             g_recvCount = 0;
static uint32_t             g_lastRecv  = 0;
static std::atomic<bool>    g_running { false };
static std::atomic<bool>    g_peerDisconnected { false };
static std::atomic<uint32_t> g_requestedTimeoutMs { 0 };

// Delay + rollback control channel — small packets to keep both peers in sync.
static int32_t              g_outboundDelay    = -1;
static int32_t              g_outboundDelayFrame = -1;
static bool                 g_outboundDelayNew = false;
static int32_t              g_outboundRollback = -1;
static bool                 g_outboundRollbackNew = false;
static int32_t              g_inboundDelay     = -1;
static int32_t              g_inboundDelayFrame = -1;
static bool                 g_inboundDelayNew  = false;
static int32_t              g_inboundRollback  = -1;
static bool                 g_inboundRollbackNew = false;
static CRITICAL_SECTION     g_delayCS;

// Resim-notify channel — informs the remote peer which frames were re-simulated.
static ResimNotifyPacket    g_outboundResimNotify = {};
static bool                 g_outboundResimNotifyNew = false;
static ResimNotifyPacket    g_inboundResimNotify = {};
static bool                 g_inboundResimNotifyNew = false;
static CRITICAL_SECTION     g_resimNotifyCS;

// Ground-truth channel — non-simulating side's exact live state for verification.
static GroundTruthPacket    g_outboundGroundTruth = {};
static bool                 g_outboundGroundTruthNew = false;
static GroundTruthPacket    g_inboundGroundTruth = {};
static bool                 g_inboundGroundTruthNew = false;
static CRITICAL_SECTION     g_groundTruthCS;

static BattleReadyPacket    g_outboundBattleReady = {};
static bool                 g_outboundBattleReadyNew = false;
static BattleReadyPacket    g_inboundBattleReady = {};
static bool                 g_inboundBattleReadyNew = false;
static CRITICAL_SECTION     g_battleReadyCS;

static DesyncPacket             g_outboundDesync = {};
static bool                     g_outboundDesyncNew = false;
static DesyncPacket             g_inboundDesync = {};
static bool                     g_inboundDesyncNew = false;
static CRITICAL_SECTION         g_desyncCS;
static RestartMatchPacket       g_outboundRestart = {};
static bool                     g_outboundRestartNew = false;
static RestartMatchPacket       g_inboundRestart = {};
static bool                     g_inboundRestartNew = false;
static CRITICAL_SECTION         g_restartCS;
static MultiResimPacket         g_outboundMultiResim = {};
static bool                     g_outboundMultiResimNew = false;
static MultiResimPacket         g_inboundMultiResim = {};
static bool                     g_inboundMultiResimNew = false;
static CRITICAL_SECTION         g_multiResimCS;
static DumpNdjsonPacket         g_outboundDumpNDJSON = {};
static bool                     g_outboundDumpNDJSONNew = false;
static DumpNdjsonPacket         g_inboundDumpNDJSON = {};
static bool                     g_inboundDumpNDJSONNew = false;
static CRITICAL_SECTION         g_dumpNDJSONCS;
static AutoTestStartPacket      g_outboundAutoTestStart = {};
static bool                     g_outboundAutoTestStartNew = false;
static AutoTestStartPacket      g_inboundAutoTestStart = {};
static bool                     g_inboundAutoTestStartNew = false;
static CRITICAL_SECTION         g_autoTestStartCS;
// A client can report one checksum for every forced resimulated frame while
// the host's game thread is busy comparing or restarting a case. ENet is
// reliable, but this application queue must not discard the oldest reports.
static constexpr size_t         MAX_AUTOTEST_FRAME_PACKETS = 4096;
static AutoTestFramePacket      g_outboundAutoTestFrames[MAX_AUTOTEST_FRAME_PACKETS] = {};
static size_t                   g_outboundAutoTestFrameHead = 0;
static size_t                   g_outboundAutoTestFrameCount = 0;
static AutoTestFramePacket      g_inboundAutoTestFrames[MAX_AUTOTEST_FRAME_PACKETS] = {};
static size_t                   g_inboundAutoTestFrameHead = 0;
static size_t                   g_inboundAutoTestFrameCount = 0;
static CRITICAL_SECTION         g_autoTestFrameCS;
static bool                     g_outboundAutoTestComplete = false;
static bool                     g_inboundAutoTestComplete = false;
static CRITICAL_SECTION         g_autoTestCompleteCS;

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
        uint32_t timeoutMs = g_requestedTimeoutMs.exchange(0);
        if (timeoutMs != 0 && g_peer)
        {
            enet_peer_timeout(g_peer, 0, timeoutMs, timeoutMs);
            HookLog("Netplay: Peer timeout set to %u ms\n", timeoutMs);
        }

        // --- Periodic stats (every ~3 seconds) ---
        static uint32_t lastStatsTick = GetTickCount();
        uint32_t now = GetTickCount();
        if (now - lastStatsTick >= 5000)
        {
            HookLog("Netplay: received %u packets in last 5s (total %u)\n",
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

        // --- Send delay/rollback control packet (if a new value is pending) ---
        bool hasNewDelay = false;
        int32_t delayVal = -1;
        int32_t delayFrame = -1;
        int32_t rollbackVal = -1;
        {
            ScopedCS lock(g_delayCS);
            if (g_outboundDelayNew || g_outboundRollbackNew)
            {
                delayVal    = g_outboundDelay;
                delayFrame  = g_outboundDelayFrame;
                rollbackVal = g_outboundRollback;
                hasNewDelay = true;
                g_outboundDelayNew   = false;
                g_outboundRollbackNew = false;
            }
        }

        if (hasNewDelay && g_peer)
        {
            DelayPacket dp;
            dp.type           = PACKET_DELAY;
            dp.delay          = delayVal;
            dp.rollbackFrames = rollbackVal;
            dp.effective_frame = delayFrame;
            ENetPacket* packet = enet_packet_create(
                &dp, sizeof(DelayPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        // --- Send outbound resim-notify packet ---
        bool hasNewResimNotify = false;
        ResimNotifyPacket rnPkt = {};
        {
            ScopedCS lock(g_resimNotifyCS);
            if (g_outboundResimNotifyNew)
            {
                rnPkt = g_outboundResimNotify;
                hasNewResimNotify = true;
                g_outboundResimNotifyNew = false;
            }
        }

        if (hasNewResimNotify && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &rnPkt, sizeof(ResimNotifyPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        // --- Send outbound ground-truth packet ---
        bool hasNewGroundTruth = false;
        GroundTruthPacket gtPkt = {};
        {
            ScopedCS lock(g_groundTruthCS);
            if (g_outboundGroundTruthNew)
            {
                gtPkt = g_outboundGroundTruth;
                hasNewGroundTruth = true;
                g_outboundGroundTruthNew = false;
            }
        }

        if (hasNewGroundTruth && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &gtPkt, sizeof(GroundTruthPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        bool hasBattleReady = false;
        BattleReadyPacket readyPkt = {};
        {
            ScopedCS lock(g_battleReadyCS);
            if (g_outboundBattleReadyNew)
            {
                readyPkt = g_outboundBattleReady;
                hasBattleReady = true;
                g_outboundBattleReadyNew = false;
            }
        }
        if (hasBattleReady && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &readyPkt, sizeof(BattleReadyPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        // --- Send desync screen control packet ---
        bool hasDesync = false;
        DesyncPacket desyncPkt = {};
        {
            ScopedCS lock(g_desyncCS);
            if (g_outboundDesyncNew)
            {
                desyncPkt = g_outboundDesync;
                hasDesync = true;
                g_outboundDesyncNew = false;
            }
        }
        if (hasDesync && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &desyncPkt, sizeof(DesyncPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
            enet_host_flush(g_host);
        }

        bool hasRestart = false;
        RestartMatchPacket restartPkt = {};
        {
            ScopedCS lock(g_restartCS);
            if (g_outboundRestartNew)
            {
                restartPkt = g_outboundRestart;
                hasRestart = true;
                g_outboundRestartNew = false;
            }
        }
        if (hasRestart && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &restartPkt, sizeof(RestartMatchPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
            enet_host_flush(g_host);
        }

        bool hasMultiResim = false;
        MultiResimPacket multiResimPkt = {};
        {
            ScopedCS lock(g_multiResimCS);
            if (g_outboundMultiResimNew)
            {
                multiResimPkt = g_outboundMultiResim;
                hasMultiResim = true;
                g_outboundMultiResimNew = false;
            }
        }
        if (hasMultiResim && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &multiResimPkt, sizeof(MultiResimPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
            enet_host_flush(g_host);
        }

        bool hasDumpNDJSON = false;
        DumpNdjsonPacket dumpNDJSONPkt = {};
        {
            ScopedCS lock(g_dumpNDJSONCS);
            if (g_outboundDumpNDJSONNew)
            {
                dumpNDJSONPkt = g_outboundDumpNDJSON;
                hasDumpNDJSON = true;
                g_outboundDumpNDJSONNew = false;
            }
        }
        if (hasDumpNDJSON && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &dumpNDJSONPkt, sizeof(DumpNdjsonPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
            enet_host_flush(g_host);
        }

        bool hasAutoTestStart = false;
        AutoTestStartPacket autoTestStart = {};
        {
            ScopedCS lock(g_autoTestStartCS);
            if (g_outboundAutoTestStartNew)
            {
                autoTestStart = g_outboundAutoTestStart;
                hasAutoTestStart = true;
                g_outboundAutoTestStartNew = false;
            }
        }
        if (hasAutoTestStart && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &autoTestStart, sizeof(autoTestStart), ENET_PACKET_FLAG_RELIABLE);
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
            enet_host_flush(g_host);
        }

        bool hasAutoTestComplete = false;
        {
            ScopedCS lock(g_autoTestCompleteCS);
            hasAutoTestComplete = g_outboundAutoTestComplete;
            g_outboundAutoTestComplete = false;
        }
        if (hasAutoTestComplete && g_peer)
        {
            AutoTestCompletePacket complete = {PACKET_AUTOTEST_COMPLETE};
            ENetPacket* packet = enet_packet_create(
                &complete, sizeof(complete), ENET_PACKET_FLAG_RELIABLE);
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
            enet_host_flush(g_host);
        }

        AutoTestFramePacket autoTestFrame = {};
        bool hasAutoTestFrame = false;
        {
            ScopedCS lock(g_autoTestFrameCS);
            if (g_outboundAutoTestFrameCount != 0)
            {
                autoTestFrame = g_outboundAutoTestFrames[g_outboundAutoTestFrameHead];
                g_outboundAutoTestFrameHead =
                    (g_outboundAutoTestFrameHead + 1) % MAX_AUTOTEST_FRAME_PACKETS;
                --g_outboundAutoTestFrameCount;
                hasAutoTestFrame = true;
            }
        }
        if (hasAutoTestFrame && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &autoTestFrame, sizeof(autoTestFrame), ENET_PACKET_FLAG_RELIABLE);
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
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
                                        if (g_inboundCount == MAX_INBOUND_PACKETS)
                                        {
                                            g_inboundHead = (g_inboundHead + 1) % MAX_INBOUND_PACKETS;
                                            --g_inboundCount;
                                        }
                                        size_t tail = (g_inboundHead + g_inboundCount) % MAX_INBOUND_PACKETS;
                                        g_inboundQueue[tail] = *inPkt;
                                        ++g_inboundCount;
                                    }
                                    g_recvCount++;
                                }
                                break;

                            case PACKET_DELAY:
                                if (event.packet->dataLength >= sizeof(DelayPacket))
                                {
                                    const DelayPacket* dp = (const DelayPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_delayCS);
                                        g_inboundDelay     = dp->delay;
                                        g_inboundDelayFrame = dp->effective_frame;
                                        g_inboundDelayNew  = true;
                                        g_inboundRollback     = dp->rollbackFrames;
                                        g_inboundRollbackNew = true;
                                    }
                                }
                                break;

                            case PACKET_RESIM_NOTIFY:
                                if (event.packet->dataLength >= sizeof(ResimNotifyPacket))
                                {
                                    const ResimNotifyPacket* rnp = (const ResimNotifyPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_resimNotifyCS);
                                        g_inboundResimNotify = *rnp;
                                        g_inboundResimNotifyNew = true;
                                    }
                                }
                                break;

                            case PACKET_GROUND_TRUTH:
                                if (event.packet->dataLength >= sizeof(GroundTruthPacket))
                                {
                                    const GroundTruthPacket* gtp = (const GroundTruthPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_groundTruthCS);
                                        g_inboundGroundTruth = *gtp;
                                        g_inboundGroundTruthNew = true;
                                    }
                                }
                                break;

                            case PACKET_BATTLE_READY:
                                if (event.packet->dataLength >= sizeof(BattleReadyPacket))
                                {
                                    const BattleReadyPacket* brp = (const BattleReadyPacket*)event.packet->data;
                                    ScopedCS lock(g_battleReadyCS);
                                    g_inboundBattleReady = *brp;
                                    g_inboundBattleReadyNew = true;
                                }
                                break;

                            case PACKET_DESYNC:
                                if (event.packet->dataLength >= sizeof(DesyncPacket))
                                {
                                    const DesyncPacket* dp = (const DesyncPacket*)event.packet->data;
                                    if (dp->action == DESYNC_TRIGGER || dp->action == DESYNC_UNPAUSE)
                                    {
                                        ScopedCS lock(g_desyncCS);
                                        g_inboundDesync = *dp;
                                        g_inboundDesyncNew = true;
                                    }
                                }
                                break;

                            case PACKET_RESTART_MATCH:
                                if (event.packet->dataLength >= sizeof(RestartMatchPacket))
                                {
                                    const RestartMatchPacket* rp =
                                        (const RestartMatchPacket*)event.packet->data;
                                    ScopedCS lock(g_restartCS);
                                    g_inboundRestart = *rp;
                                    g_inboundRestartNew = true;
                                }
                                break;

                            case PACKET_MULTI_RESIM:
                                if (event.packet->dataLength >= sizeof(MultiResimPacket))
                                {
                                    const MultiResimPacket* mrp =
                                        (const MultiResimPacket*)event.packet->data;
                                    if (mrp->enabled <= 1)
                                    {
                                        ScopedCS lock(g_multiResimCS);
                                        g_inboundMultiResim = *mrp;
                                        g_inboundMultiResimNew = true;
                                    }
                                }
                                break;

                            case PACKET_DUMP_NDJSON:
                                if (event.packet->dataLength >= sizeof(DumpNdjsonPacket))
                                {
                                    const DumpNdjsonPacket* dnp =
                                        (const DumpNdjsonPacket*)event.packet->data;
                                    ScopedCS lock(g_dumpNDJSONCS);
                                    g_inboundDumpNDJSON = *dnp;
                                    g_inboundDumpNDJSONNew = true;
                                }
                                break;

                            case PACKET_AUTOTEST_START:
                                if (event.packet->dataLength >= sizeof(AutoTestStartPacket))
                                {
                                    const AutoTestStartPacket* asp =
                                        (const AutoTestStartPacket*)event.packet->data;
                                    ScopedCS lock(g_autoTestStartCS);
                                    g_inboundAutoTestStart = *asp;
                                    g_inboundAutoTestStartNew = true;
                                }
                                break;

                            case PACKET_AUTOTEST_FRAME:
                                if (event.packet->dataLength >= sizeof(AutoTestFramePacket))
                                {
                                    const AutoTestFramePacket* afp =
                                        (const AutoTestFramePacket*)event.packet->data;
                                    ScopedCS lock(g_autoTestFrameCS);
                                    if (g_inboundAutoTestFrameCount == MAX_AUTOTEST_FRAME_PACKETS)
                                    {
                                        g_inboundAutoTestFrameHead =
                                            (g_inboundAutoTestFrameHead + 1) % MAX_AUTOTEST_FRAME_PACKETS;
                                        --g_inboundAutoTestFrameCount;
                                    }
                                    size_t tail = (g_inboundAutoTestFrameHead +
                                                   g_inboundAutoTestFrameCount) %
                                                  MAX_AUTOTEST_FRAME_PACKETS;
                                    g_inboundAutoTestFrames[tail] = *afp;
                                    ++g_inboundAutoTestFrameCount;
                                }
                                break;

                            case PACKET_AUTOTEST_COMPLETE:
                                if (event.packet->dataLength >= sizeof(AutoTestCompletePacket))
                                {
                                    ScopedCS lock(g_autoTestCompleteCS);
                                    g_inboundAutoTestComplete = true;
                                }
                                break;
                        }
                    }
                    enet_packet_destroy(event.packet);
                }
                else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                {
                    HookLog("Netplay: Remote peer disconnected\n");
                    g_peerDisconnected = true;
                    // The game thread may not get another tick to consume a
                    // queued remote desync trigger. Save the rolling window
                    // before making the session inactive.
                    Rollback_FlushNetplayLive(-1);
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
    if (g_running || g_serviceThread)
    {
        HookLog("Netplay: Init ignored because service thread is already running\n");
        return;
    }

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
        InitializeCriticalSection(&g_delayCS);
        InitializeCriticalSection(&g_resimNotifyCS);
        InitializeCriticalSection(&g_groundTruthCS);
        InitializeCriticalSection(&g_battleReadyCS);
        InitializeCriticalSection(&g_desyncCS);
        InitializeCriticalSection(&g_restartCS);
        InitializeCriticalSection(&g_multiResimCS);
        InitializeCriticalSection(&g_dumpNDJSONCS);
        InitializeCriticalSection(&g_autoTestStartCS);
        InitializeCriticalSection(&g_autoTestFrameCS);
        InitializeCriticalSection(&g_autoTestCompleteCS);

        g_outboundDelayNew = false;
        g_inboundDelayNew = false;
        g_outboundResimNotifyNew = false;
        g_inboundResimNotifyNew = false;
        g_outboundGroundTruthNew = false;
        g_inboundGroundTruthNew = false;
        g_outboundBattleReadyNew = false;
        g_inboundBattleReadyNew = false;
        g_outboundDesyncNew = false;
        g_inboundDesyncNew = false;
        g_outboundRestartNew = false;
        g_inboundRestartNew = false;
        g_outboundMultiResimNew = false;
        g_inboundMultiResimNew = false;
        g_outboundDumpNDJSONNew = false;
        g_inboundDumpNDJSONNew = false;
        g_outboundAutoTestStartNew = false;
        g_inboundAutoTestStartNew = false;
        g_outboundAutoTestFrameHead = 0;
        g_outboundAutoTestFrameCount = 0;
        g_inboundAutoTestFrameHead = 0;
        g_inboundAutoTestFrameCount = 0;
        g_outboundAutoTestComplete = false;
        g_inboundAutoTestComplete = false;
        std::memset(&g_outboundPacket, 0, sizeof(InputPacket));
        g_csInitialized = true;
    }

    {
        ScopedCS lock(g_inboundCS);
        g_inboundHead = 0;
        g_inboundCount = 0;
    }

    g_recvCount = 0;
    g_lastRecv = 0;
    g_peerDisconnected = false;
    g_requestedTimeoutMs = 0;
    g_running = true;

    // Reset internal player pad ring buffers
    Controller_ResetBuffers();

    g_serviceThread = CreateThread(nullptr, 0, Netplay_ServiceThread, nullptr, 0, nullptr);
    if (g_serviceThread)
    {
        HookLog("Netplay: Initialized, service thread running\n");

        // Controller sends its forced-zero startup history from normal frame
        // boundaries. This primes delay-mode confirmation without a synthetic
        // packet from the pre-battle frame epoch.
    }
    else
    {
        HookLog("Netplay: Failed to create service thread\n");
        g_running = false;
        g_active  = false;
    }
}

void Netplay_SetTimeout(uint32_t timeoutMs)
{
    if (g_active && timeoutMs != 0)
        g_requestedTimeoutMs = timeoutMs;
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
    if (g_inboundCount != 0)
    {
        outPkt = g_inboundQueue[g_inboundHead];
        g_inboundHead = (g_inboundHead + 1) % MAX_INBOUND_PACKETS;
        --g_inboundCount;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Delay + rollback control — sent together to keep both peers in sync.
// ---------------------------------------------------------------------------
void Netplay_SendDelay(int32_t delay, int32_t effectiveFrame)
{
    ScopedCS lock(g_delayCS);
    g_outboundDelay = delay;
    g_outboundDelayFrame = effectiveFrame;
    g_outboundDelayNew = true;
}

void Netplay_SendRollback(int32_t rollbackFrames)
{
    ScopedCS lock(g_delayCS);
    g_outboundRollback = rollbackFrames;
    g_outboundRollbackNew = true;
}

void Netplay_SendResimNotify(const ResimNotifyPacket& pkt)
{
    ScopedCS lock(g_resimNotifyCS);
    g_outboundResimNotify = pkt;
    g_outboundResimNotifyNew = true;
}

bool Netplay_GetInboundResimNotify(ResimNotifyPacket& outNotify)
{
    ScopedCS lock(g_resimNotifyCS);
    if (g_inboundResimNotifyNew)
    {
        outNotify = g_inboundResimNotify;
        g_inboundResimNotifyNew = false;
        return true;
    }
    return false;
}

void Netplay_SendGroundTruth(const GroundTruthPacket& pkt)
{
    ScopedCS lock(g_groundTruthCS);
    g_outboundGroundTruth = pkt;
    g_outboundGroundTruthNew = true;
}

bool Netplay_GetInboundGroundTruth(GroundTruthPacket& outGT)
{
    ScopedCS lock(g_groundTruthCS);
    if (g_inboundGroundTruthNew)
    {
        outGT = g_inboundGroundTruth;
        g_inboundGroundTruthNew = false;
        return true;
    }
    return false;
}

bool Netplay_GetInboundDelay(int32_t& outDelay, int32_t& outEffectiveFrame)
{
    ScopedCS lock(g_delayCS);
    if (g_inboundDelayNew)
    {
        outDelay = g_inboundDelay;
        outEffectiveFrame = g_inboundDelayFrame;
        g_inboundDelayNew = false;
        return true;
    }
    return false;
}

bool Netplay_GetInboundRollback(int32_t& outRollback)
{
    ScopedCS lock(g_delayCS);
    if (g_inboundRollbackNew)
    {
        outRollback = g_inboundRollback;
        g_inboundRollbackNew = false;
        return true;
    }
    return false;
}

void Netplay_SendMultiResim(bool enabled)
{
    ScopedCS lock(g_multiResimCS);
    g_outboundMultiResim.type = PACKET_MULTI_RESIM;
    g_outboundMultiResim.enabled = enabled ? 1u : 0u;
    g_outboundMultiResimNew = true;
}

bool Netplay_GetInboundMultiResim(bool& outEnabled)
{
    ScopedCS lock(g_multiResimCS);
    if (!g_inboundMultiResimNew)
        return false;
    outEnabled = g_inboundMultiResim.enabled != 0;
    g_inboundMultiResimNew = false;
    return true;
}

void Netplay_SendDumpNDJSON(int32_t frame)
{
    ScopedCS lock(g_dumpNDJSONCS);
    g_outboundDumpNDJSON.type = PACKET_DUMP_NDJSON;
    g_outboundDumpNDJSON.frame = frame;
    g_outboundDumpNDJSONNew = true;
}

bool Netplay_GetInboundDumpNDJSON(int32_t& outFrame)
{
    if (!g_active || !g_csInitialized)
        return false;

    ScopedCS lock(g_dumpNDJSONCS);
    if (!g_inboundDumpNDJSONNew)
        return false;
    outFrame = g_inboundDumpNDJSON.frame;
    g_inboundDumpNDJSONNew = false;
    return true;
}

void Netplay_SendAutoTestStart(const AutoTestStartPacket& packet)
{
    ScopedCS lock(g_autoTestStartCS);
    g_outboundAutoTestStart = packet;
    g_outboundAutoTestStart.type = PACKET_AUTOTEST_START;
    g_outboundAutoTestStartNew = true;
}

bool Netplay_GetInboundAutoTestStart(AutoTestStartPacket& outPacket)
{
    if (!g_active || !g_csInitialized)
        return false;
    ScopedCS lock(g_autoTestStartCS);
    if (!g_inboundAutoTestStartNew)
        return false;
    outPacket = g_inboundAutoTestStart;
    g_inboundAutoTestStartNew = false;
    return true;
}

void Netplay_SendAutoTestComplete()
{
    if (!g_active || !g_csInitialized)
        return;
    ScopedCS lock(g_autoTestCompleteCS);
    g_outboundAutoTestComplete = true;
}

bool Netplay_GetInboundAutoTestComplete()
{
    if (!g_active || !g_csInitialized)
        return false;
    ScopedCS lock(g_autoTestCompleteCS);
    bool complete = g_inboundAutoTestComplete;
    g_inboundAutoTestComplete = false;
    return complete;
}

void Netplay_SendAutoTestFrame(const AutoTestFramePacket& packet)
{
    ScopedCS lock(g_autoTestFrameCS);
    if (g_outboundAutoTestFrameCount == MAX_AUTOTEST_FRAME_PACKETS)
    {
        g_outboundAutoTestFrameHead =
            (g_outboundAutoTestFrameHead + 1) % MAX_AUTOTEST_FRAME_PACKETS;
        --g_outboundAutoTestFrameCount;
    }
    size_t tail = (g_outboundAutoTestFrameHead + g_outboundAutoTestFrameCount) %
                  MAX_AUTOTEST_FRAME_PACKETS;
    g_outboundAutoTestFrames[tail] = packet;
    g_outboundAutoTestFrames[tail].type = PACKET_AUTOTEST_FRAME;
    ++g_outboundAutoTestFrameCount;
}

bool Netplay_GetInboundAutoTestFrame(AutoTestFramePacket& outPacket)
{
    if (!g_active || !g_csInitialized)
        return false;
    ScopedCS lock(g_autoTestFrameCS);
    if (g_inboundAutoTestFrameCount == 0)
        return false;
    outPacket = g_inboundAutoTestFrames[g_inboundAutoTestFrameHead];
    g_inboundAutoTestFrameHead =
        (g_inboundAutoTestFrameHead + 1) % MAX_AUTOTEST_FRAME_PACKETS;
    --g_inboundAutoTestFrameCount;
    return true;
}

void Netplay_SendBattleReady(uint32_t generation, uint32_t flags, uint32_t roundSeed)
{
    ScopedCS lock(g_battleReadyCS);
    g_outboundBattleReady.type = PACKET_BATTLE_READY;
    g_outboundBattleReady.generation = generation;
    g_outboundBattleReady.flags = flags;
    g_outboundBattleReady.round_seed = roundSeed;
    g_outboundBattleReadyNew = true;
}

bool Netplay_GetInboundBattleReady(uint32_t& outGeneration, uint32_t& outFlags,
                                   uint32_t& outRoundSeed)
{
    ScopedCS lock(g_battleReadyCS);
    if (!g_inboundBattleReadyNew)
        return false;
    outGeneration = g_inboundBattleReady.generation;
    outFlags = g_inboundBattleReady.flags;
    outRoundSeed = g_inboundBattleReady.round_seed;
    g_inboundBattleReadyNew = false;
    return true;
}

void Netplay_SendDesyncTrigger(int32_t frame, uint32_t localChecksum,
                               uint32_t remoteChecksum)
{
    ScopedCS lock(g_desyncCS);
    g_outboundDesync = {};
    g_outboundDesync.type = PACKET_DESYNC;
    g_outboundDesync.battle_generation = FrameStepper_GetBattleGeneration();
    g_outboundDesync.action = DESYNC_TRIGGER;
    g_outboundDesync.frame = frame;
    g_outboundDesync.localChecksum = localChecksum;
    g_outboundDesync.remoteChecksum = remoteChecksum;
    g_outboundDesyncNew = true;
}

void Netplay_SendDesyncUnpause()
{
    ScopedCS lock(g_desyncCS);
    g_outboundDesync = {};
    g_outboundDesync.type = PACKET_DESYNC;
    g_outboundDesync.battle_generation = FrameStepper_GetBattleGeneration();
    g_outboundDesync.action = DESYNC_UNPAUSE;
    g_outboundDesyncNew = true;
}

bool Netplay_GetInboundDesync(DesyncPacket& outPacket)
{
    if (!g_active || !g_csInitialized)
        return false;

    ScopedCS lock(g_desyncCS);
    if (!g_inboundDesyncNew)
        return false;
    outPacket = g_inboundDesync;
    g_inboundDesyncNew = false;
    return true;
}

void Netplay_SendRestartMatch(int32_t targetFrame, uint32_t roundSeed)
{
    ScopedCS lock(g_restartCS);
    g_outboundRestart.type = PACKET_RESTART_MATCH;
    g_outboundRestart.target_frame = targetFrame;
    g_outboundRestart.round_seed = roundSeed;
    g_outboundRestart.kind = RESTART_FULL_MATCH;
    g_outboundRestart.button_restart = 0;
    g_outboundRestartNew = true;
}

void Netplay_SendTrainingRestart(int32_t targetFrame, uint32_t roundSeed,
                                 int32_t buttonRestart)
{
    ScopedCS lock(g_restartCS);
    g_outboundRestart.type = PACKET_RESTART_MATCH;
    g_outboundRestart.target_frame = targetFrame;
    g_outboundRestart.round_seed = roundSeed;
    g_outboundRestart.kind = RESTART_TRAINING;
    g_outboundRestart.button_restart = buttonRestart;
    g_outboundRestartNew = true;
}

bool Netplay_GetInboundRestartMatch(int32_t& outTargetFrame, uint32_t& outRoundSeed,
                                    uint32_t& outKind, int32_t& outButtonRestart)
{
    if (!g_active || !g_csInitialized)
        return false;

    ScopedCS lock(g_restartCS);
    if (!g_inboundRestartNew)
        return false;
    outTargetFrame = g_inboundRestart.target_frame;
    outRoundSeed = g_inboundRestart.round_seed;
    outKind = g_inboundRestart.kind;
    outButtonRestart = g_inboundRestart.button_restart;
    g_inboundRestartNew = false;
    return true;
}

void Netplay_ClearInputQueues()
{
    {
        ScopedCS lock(g_inboundCS);
        g_inboundHead = 0;
        g_inboundCount = 0;
    }
    {
        ScopedCS lock(g_outboundCS);
        g_outboundNew = false;
        std::memset(&g_outboundPacket, 0, sizeof(g_outboundPacket));
    }
    {
        ScopedCS lock(g_resimNotifyCS);
        g_inboundResimNotifyNew = false;
        g_outboundResimNotifyNew = false;
    }
    {
        ScopedCS lock(g_groundTruthCS);
        g_inboundGroundTruthNew = false;
        g_outboundGroundTruthNew = false;
    }
    {
        ScopedCS lock(g_desyncCS);
        g_inboundDesync = {};
        g_outboundDesync = {};
        g_inboundDesyncNew = false;
        g_outboundDesyncNew = false;
    }
}

// ---------------------------------------------------------------------------
// Netplay_SendInput  — convenience wrapper that sends a single uint32 value
//                      in the first history slot (inputs[0]).
// ---------------------------------------------------------------------------
void Netplay_SendInput(uint32_t rawInput)
{
    InputPacket pkt = {};
    pkt.type        = PACKET_INPUT;
    pkt.battle_generation = FrameStepper_GetBattleGeneration();
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
    return g_inboundCount != 0;
}

uint32_t Netplay_GetRemoteInput()
{
    ScopedCS lock(g_inboundCS);
    if (g_inboundCount != 0)
        return g_inboundQueue[g_inboundHead].inputs[0];
    return 0;
}

bool Netplay_IsActive()
{
    return g_active && g_running && g_peer != nullptr;
}

bool Netplay_TakePeerDisconnected()
{
    return g_peerDisconnected.exchange(false);
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
    int32_t liveFrame = Controller_GetNetFrameCount();
    if (liveFrame >= 0)
        Rollback_FlushNetplayLive(liveFrame);
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

        DeleteCriticalSection(&g_delayCS);
        DeleteCriticalSection(&g_resimNotifyCS);
        DeleteCriticalSection(&g_groundTruthCS);
        DeleteCriticalSection(&g_battleReadyCS);
        DeleteCriticalSection(&g_desyncCS);
        DeleteCriticalSection(&g_restartCS);
        DeleteCriticalSection(&g_multiResimCS);
        DeleteCriticalSection(&g_dumpNDJSONCS);
        DeleteCriticalSection(&g_autoTestStartCS);
        DeleteCriticalSection(&g_autoTestFrameCS);
        DeleteCriticalSection(&g_autoTestCompleteCS);
        g_csInitialized = false;
    }

    HookLog("Netplay: Shutdown complete\n");
}
