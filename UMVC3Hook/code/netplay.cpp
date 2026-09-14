#include "netplay.h"
#include "rollback.h"
#include "rollback_diagnostics.h"
#include "controller.h"
#include "LaunchMenu.h"
#include "steam_lobby.h"
#include "steam_transport.h"
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
static bool                     g_outboundValid  = false;
static uint32_t                 g_outboundLastSendTick = 0;
static constexpr uint32_t       INPUT_RESEND_INTERVAL_MS = 16;
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
static bool                 g_readyTxDiagValid = false;
static uint32_t             g_readyTxDiagGeneration = 0;
static uint32_t             g_readyTxDiagFlags = 0;
static uint32_t             g_readyTxDiagSeed = 0;
static uint32_t             g_readyTxDiagOpeningMember = 0;
static int                   g_readyTxDiagResult = 0;
static int                   g_readyTxDiagPeerState = -1;
static bool                 g_readyRxDiagValid = false;
static uint32_t             g_readyRxDiagGeneration = 0;
static uint32_t             g_readyRxDiagFlags = 0;
static uint32_t             g_readyRxDiagSeed = 0;
static uint32_t             g_readyRxDiagOpeningMember = 0;
static std::atomic<bool>    g_running { false };
static std::atomic<bool>    g_peerDisconnected { false };
static std::atomic<uint32_t> g_requestedTimeoutMs { 0 };
static std::atomic<bool>    g_playerDisconnectRequested { false };
static std::atomic<bool>    g_playerDisconnectSent { false };
static std::atomic<bool>    g_playerDisconnectSendSucceeded { false };
static std::atomic<bool>    g_playerDisconnectNotice { false };

// Delay + rollback control channel — small packets to keep both peers in sync.
static int32_t              g_outboundDelay    = -1;
static int32_t              g_outboundDelayFrame = -1;
static bool                 g_outboundDelayNew = false;
static int32_t              g_outboundRollback = -1;
static int32_t              g_outboundRollbackFrame = -1;
static bool                 g_outboundRollbackNew = false;
static int32_t              g_inboundDelay     = -1;
static int32_t              g_inboundDelayFrame = -1;
static bool                 g_inboundDelayNew  = false;
static int32_t              g_inboundRollback  = -1;
static int32_t              g_inboundRollbackFrame = -1;
static bool                 g_inboundRollbackNew = false;
static CRITICAL_SECTION     g_delayCS;

static BattleReadyPacket    g_outboundBattleReady = {};
static bool                 g_outboundBattleReadyNew = false;
static BattleReadyPacket    g_inboundBattleReady = {};
static bool                 g_inboundBattleReadyNew = false;
static CRITICAL_SECTION     g_battleReadyCS;
static LaunchTrainingPacket g_outboundLaunchTraining = {};
static bool                 g_outboundLaunchTrainingNew = false;
static LaunchTrainingPacket g_inboundLaunchTraining = {};
static bool                 g_inboundLaunchTrainingNew = false;
static CRITICAL_SECTION     g_launchTrainingCS;

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
static SnapDumpPacket           g_outboundSnapDump = {};
static bool                     g_outboundSnapDumpNew = false;
static SnapDumpPacket           g_inboundSnapDump = {};
static bool                     g_inboundSnapDumpNew = false;
static CRITICAL_SECTION         g_snapDumpCS;
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

static constexpr size_t         MAX_EXTENSION_PACKET_SIZE = 256;
static constexpr size_t         MAX_EXTENSION_PACKETS = 32;
struct ExtensionPacket {
    size_t size;
    uint8_t data[MAX_EXTENSION_PACKET_SIZE];
};
static ExtensionPacket          g_outboundExtension[MAX_EXTENSION_PACKETS] = {};
static size_t                   g_outboundExtensionHead = 0;
static size_t                   g_outboundExtensionCount = 0;
static ExtensionPacket          g_inboundExtension[MAX_EXTENSION_PACKETS] = {};
static size_t                   g_inboundExtensionHead = 0;
static size_t                   g_inboundExtensionCount = 0;
static CRITICAL_SECTION         g_extensionCS;

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

static void MarkPeerDisconnected(ENetPeer* peer, bool disconnectNow)
{
    g_peerDisconnected = true;
    g_playerDisconnectNotice = true;
    // The game thread may not get another tick to consume queued diagnostics.
    if (disconnectNow && peer)
        enet_peer_disconnect_now(peer, 0);
    g_running = false;
    g_active = false;
    if (g_peer == peer)
        g_peer = nullptr;
    if (peer)
        peer->data = nullptr;
}

static bool DualTrainingControlPacketsAllowed()
{
    return GetLaunchOption() == LAUNCH_DUAL_TRAINING;
}

// ---------------------------------------------------------------------------
// Service thread: owns ALL ENet calls
// ---------------------------------------------------------------------------
static DWORD WINAPI Netplay_ServiceThread(LPVOID)
{
    HookLog("Netplay: Service thread started\n");

    while (g_running)
    {
        Rollback_FreezeWatchdogPoll();
        uint32_t timeoutMs = g_requestedTimeoutMs.exchange(0);
        if (timeoutMs != 0 && g_peer)
        {
            enet_peer_timeout(g_peer, 0, timeoutMs, timeoutMs);
            HookLog("Netplay: Peer timeout set to %u ms\n", timeoutMs);
        }

        if (g_playerDisconnectRequested.exchange(false))
        {
            bool sent = false;
            if (g_peer)
            {
                const PlayerDisconnectPacket disconnect = {PACKET_PLAYER_DISCONNECT};
                ENetPacket* packet = enet_packet_create(
                    &disconnect, sizeof(disconnect), ENET_PACKET_FLAG_RELIABLE);
                if (enet_peer_send(g_peer, 0, packet) == 0)
                {
                    enet_host_flush(g_host);
                    sent = true;
                }
                else
                {
                    enet_packet_destroy(packet);
                }
            }
            g_playerDisconnectSendSucceeded = sent;
            g_playerDisconnectSent = true;
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
        // Input delivery is unreliable. If both game threads wait for missing
        // input, neither reaches frame end to publish another history packet.
        // Resend the exact latest history independently of simulation progress.
        bool hasNewOutbound = false;
        InputPacket outPkt = {};
        {
            ScopedCS lock(g_outboundCS);
            if (g_outboundValid &&
                (g_outboundNew ||
                 now - g_outboundLastSendTick >= INPUT_RESEND_INTERVAL_MS))
            {
                outPkt = g_outboundPacket;
                hasNewOutbound = true;
                g_outboundNew = false;
                g_outboundLastSendTick = now;
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
        bool hasNewRollback = false;
        int32_t delayVal = -1;
        int32_t delayFrame = -1;
        int32_t rollbackVal = -1;
        int32_t rollbackFrame = -1;
        {
            ScopedCS lock(g_delayCS);
            if (g_outboundDelayNew) {
                delayVal    = g_outboundDelay;
                delayFrame  = g_outboundDelayFrame;
                hasNewDelay = true;
                g_outboundDelayNew = false;
            }
            if (g_outboundRollbackNew) {
                rollbackVal = g_outboundRollback;
                rollbackFrame = g_outboundRollbackFrame;
                hasNewRollback = true;
                g_outboundRollbackNew = false;
            }
        }

        if (hasNewDelay && g_peer)
        {
            DelayPacket dp;
            dp.type           = PACKET_DELAY;
            dp.delay          = delayVal;
            dp.rollbackFrames = -1;
            dp.effective_frame = delayFrame;
            ENetPacket* packet = enet_packet_create(
                &dp, sizeof(DelayPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        if (hasNewRollback && g_peer)
        {
            DelayPacket dp;
            dp.type = PACKET_DELAY;
            dp.delay = -1;
            dp.rollbackFrames = rollbackVal;
            dp.effective_frame = rollbackFrame;
            ENetPacket* packet = enet_packet_create(
                &dp, sizeof(DelayPacket), ENET_PACKET_FLAG_RELIABLE
            );
            if (enet_peer_send(g_peer, 0, packet) != 0) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }

        bool hasLaunchTraining = false;
        LaunchTrainingPacket launchTraining = {};
        {
            ScopedCS lock(g_launchTrainingCS);
            if (g_outboundLaunchTrainingNew)
            {
                launchTraining = g_outboundLaunchTraining;
                hasLaunchTraining = true;
                g_outboundLaunchTrainingNew = false;
            }
        }
        if (hasLaunchTraining && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                &launchTraining, sizeof(launchTraining), ENET_PACKET_FLAG_RELIABLE);
            if (enet_peer_send(g_peer, 0, packet) != 0)
                enet_packet_destroy(packet);
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
            int sendResult = packet ? enet_peer_send(g_peer, 0, packet) : -1;
            const int peerState = (int)g_peer->state;
            if (!g_readyTxDiagValid ||
                g_readyTxDiagGeneration != readyPkt.generation ||
                g_readyTxDiagFlags != readyPkt.flags ||
                g_readyTxDiagSeed != readyPkt.round_seed ||
                g_readyTxDiagOpeningMember != readyPkt.opening_member ||
                g_readyTxDiagResult != sendResult ||
                g_readyTxDiagPeerState != peerState) {
                HookLog("Netplay: BattleReady TX gen=%u flags=0x%X seed=0x%08X "
                        "opening=%u result=%d peerState=%d\n",
                        readyPkt.generation, readyPkt.flags, readyPkt.round_seed,
                        readyPkt.opening_member, sendResult, peerState);
            }
            g_readyTxDiagValid = true;
            g_readyTxDiagGeneration = readyPkt.generation;
            g_readyTxDiagFlags = readyPkt.flags;
            g_readyTxDiagSeed = readyPkt.round_seed;
            g_readyTxDiagOpeningMember = readyPkt.opening_member;
            g_readyTxDiagResult = sendResult;
            g_readyTxDiagPeerState = peerState;
            if (sendResult != 0 && packet) {
                enet_packet_destroy(packet);
            }
            enet_host_flush(g_host);
        }
        else if (hasBattleReady)
        {
            if (!g_readyTxDiagValid || g_readyTxDiagGeneration != readyPkt.generation ||
                g_readyTxDiagFlags != readyPkt.flags || g_readyTxDiagPeerState != -1) {
                HookLog("Netplay: BattleReady TX dropped gen=%u flags=0x%X "
                        "because peer is null\n",
                        readyPkt.generation, readyPkt.flags);
            }
            g_readyTxDiagValid = true;
            g_readyTxDiagGeneration = readyPkt.generation;
            g_readyTxDiagFlags = readyPkt.flags;
            g_readyTxDiagSeed = readyPkt.round_seed;
            g_readyTxDiagOpeningMember = readyPkt.opening_member;
            g_readyTxDiagResult = -1;
            g_readyTxDiagPeerState = -1;
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

        bool hasSnapDump = false;
        SnapDumpPacket snapDumpPacket = {};
        {
            ScopedCS lock(g_snapDumpCS);
            if (g_outboundSnapDumpNew)
            {
                snapDumpPacket = g_outboundSnapDump;
                hasSnapDump = true;
                g_outboundSnapDumpNew = false;
            }
        }
        if (hasSnapDump && g_peer && DualTrainingControlPacketsAllowed())
        {
            ENetPacket* packet = enet_packet_create(
                &snapDumpPacket, sizeof(SnapDumpPacket), ENET_PACKET_FLAG_RELIABLE
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

        ExtensionPacket extension = {};
        bool hasExtension = false;
        {
            ScopedCS lock(g_extensionCS);
            if (g_outboundExtensionCount != 0)
            {
                extension = g_outboundExtension[g_outboundExtensionHead];
                g_outboundExtensionHead =
                    (g_outboundExtensionHead + 1) % MAX_EXTENSION_PACKETS;
                --g_outboundExtensionCount;
                hasExtension = true;
            }
        }
        if (hasExtension && g_peer)
        {
            ENetPacket* packet = enet_packet_create(
                extension.data, extension.size, ENET_PACKET_FLAG_RELIABLE);
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
                        // Count all protocol packets. The old counter only
                        // counted input packets, making a working startup
                        // handshake look like a completely idle connection.
                        g_recvCount++;
                        uint32_t pktType = *(uint32_t*)event.packet->data;
                        switch (pktType)
                        {
                            case PACKET_LAUNCH_TRAINING:
                                if (event.packet->dataLength >= sizeof(LaunchTrainingPacket))
                                {
                                    const LaunchTrainingPacket* packet =
                                        (const LaunchTrainingPacket*)event.packet->data;
                                    if (packet->autoJump[0] <= 1 &&
                                        packet->autoJump[1] <= 1 &&
                                        packet->autoJumpValidMask <= 3 &&
                                        packet->voiceValidMask <= 3)
                                    {
                                        bool validVoice = true;
                                        for (int team = 0; team < 2; ++team) {
                                            for (int member = 0; member < 3; ++member) {
                                                if (packet->voiceLanguage[team][member] > 1)
                                                    validVoice = false;
                                            }
                                        }
                                        if (!validVoice)
                                            break;
                                        ScopedCS lock(g_launchTrainingCS);
                                        g_inboundLaunchTraining = *packet;
                                        g_inboundLaunchTrainingNew = true;
                                    }
                                }
                                break;
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
                                }
                                break;

                            case PACKET_DELAY:
                                if (event.packet->dataLength >= sizeof(DelayPacket))
                                {
                                    const DelayPacket* dp = (const DelayPacket*)event.packet->data;
                                    {
                                        ScopedCS lock(g_delayCS);
                                        if (dp->delay >= 0) {
                                            g_inboundDelay = dp->delay;
                                            g_inboundDelayFrame = dp->effective_frame;
                                            g_inboundDelayNew = true;
                                        }
                                        if (dp->rollbackFrames >= 0) {
                                            g_inboundRollback = dp->rollbackFrames;
                                            g_inboundRollbackFrame = dp->effective_frame;
                                            g_inboundRollbackNew = true;
                                        }
                                    }
                                }
                                break;

                            case PACKET_BATTLE_READY:
                                if (event.packet->dataLength >= sizeof(BattleReadyPacket))
                                {
                                    const BattleReadyPacket* brp = (const BattleReadyPacket*)event.packet->data;
                                    if (!g_readyRxDiagValid ||
                                        g_readyRxDiagGeneration != brp->generation ||
                                        g_readyRxDiagFlags != brp->flags ||
                                        g_readyRxDiagSeed != brp->round_seed ||
                                        g_readyRxDiagOpeningMember != brp->opening_member) {
                                        HookLog("Netplay: BattleReady RX gen=%u flags=0x%X "
                                                "seed=0x%08X opening=%u\n",
                                                brp->generation, brp->flags,
                                                brp->round_seed,
                                                brp->opening_member);
                                    }
                                    g_readyRxDiagValid = true;
                                    g_readyRxDiagGeneration = brp->generation;
                                    g_readyRxDiagFlags = brp->flags;
                                    g_readyRxDiagSeed = brp->round_seed;
                                    g_readyRxDiagOpeningMember = brp->opening_member;
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

                            case PACKET_SNAPDUMP:
                                if (DualTrainingControlPacketsAllowed() &&
                                    event.packet->dataLength >= sizeof(SnapDumpPacket))
                                {
                                    const SnapDumpPacket* packet =
                                        (const SnapDumpPacket*)event.packet->data;
                                    ScopedCS lock(g_snapDumpCS);
                                    g_inboundSnapDump = *packet;
                                    g_inboundSnapDump.timestamp[
                                        sizeof(g_inboundSnapDump.timestamp) - 1] = '\0';
                                    g_inboundSnapDumpNew = true;
                                }
                                break;

                            case PACKET_AUTOTEST_START:
                                if (DualTrainingControlPacketsAllowed() &&
                                    event.packet->dataLength >= sizeof(AutoTestStartPacket))
                                {
                                    const AutoTestStartPacket* asp =
                                        (const AutoTestStartPacket*)event.packet->data;
                                    ScopedCS lock(g_autoTestStartCS);
                                    g_inboundAutoTestStart = *asp;
                                    g_inboundAutoTestStart.move_name[
                                        sizeof(g_inboundAutoTestStart.move_name) - 1] = '\0';
                                    g_inboundAutoTestStartNew = true;
                                }
                                break;

                            case PACKET_AUTOTEST_FRAME:
                                if (DualTrainingControlPacketsAllowed() &&
                                    event.packet->dataLength >= sizeof(AutoTestFramePacket))
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
                                if (DualTrainingControlPacketsAllowed() &&
                                    event.packet->dataLength >= sizeof(AutoTestCompletePacket))
                                {
                                    ScopedCS lock(g_autoTestCompleteCS);
                                    g_inboundAutoTestComplete = true;
                                }
                                break;
                            case PACKET_PLAYER_DISCONNECT:
                                if (event.packet->dataLength >= sizeof(PlayerDisconnectPacket))
                                {
                                    HookLog("Netplay: Remote player sent disconnect notice\n");
                                    MarkPeerDisconnected(event.peer, true);
                                }
                                break;
                            case PACKET_EXTENSION:
                                if (event.packet->dataLength <= MAX_EXTENSION_PACKET_SIZE)
                                {
                                    ScopedCS lock(g_extensionCS);
                                    if (g_inboundExtensionCount == MAX_EXTENSION_PACKETS)
                                    {
                                        g_inboundExtensionHead =
                                            (g_inboundExtensionHead + 1) % MAX_EXTENSION_PACKETS;
                                        --g_inboundExtensionCount;
                                    }
                                    size_t tail = (g_inboundExtensionHead +
                                                   g_inboundExtensionCount) %
                                                  MAX_EXTENSION_PACKETS;
                                    g_inboundExtension[tail].size = event.packet->dataLength;
                                    std::memcpy(g_inboundExtension[tail].data,
                                                event.packet->data,
                                                event.packet->dataLength);
                                    ++g_inboundExtensionCount;
                                }
                                break;
                        }
                    }
                    enet_packet_destroy(event.packet);
                }
                else if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                {
                    HookLog("Netplay: Remote peer disconnected\n");
                    MarkPeerDisconnected(event.peer, false);
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
void Netplay_ConfigureHostLimits(ENetHost* host)
{
    if (!host)
        return;
    host->maximumPacketSize = NET_MAX_PACKET_SIZE;
    host->maximumWaitingData = NET_MAX_WAITING_DATA;
}

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
        InitializeCriticalSection(&g_battleReadyCS);
        InitializeCriticalSection(&g_launchTrainingCS);
        InitializeCriticalSection(&g_desyncCS);
        InitializeCriticalSection(&g_restartCS);
        InitializeCriticalSection(&g_multiResimCS);
        InitializeCriticalSection(&g_snapDumpCS);
        InitializeCriticalSection(&g_autoTestStartCS);
        InitializeCriticalSection(&g_autoTestFrameCS);
        InitializeCriticalSection(&g_autoTestCompleteCS);
        InitializeCriticalSection(&g_extensionCS);

        g_outboundDelayNew = false;
        g_inboundDelayNew = false;
        g_outboundBattleReadyNew = false;
        g_inboundBattleReadyNew = false;
        g_outboundLaunchTrainingNew = false;
        g_inboundLaunchTrainingNew = false;
        g_outboundDesyncNew = false;
        g_inboundDesyncNew = false;
        g_outboundRestartNew = false;
        g_inboundRestartNew = false;
        g_outboundMultiResimNew = false;
        g_inboundMultiResimNew = false;
        g_outboundSnapDumpNew = false;
        g_inboundSnapDumpNew = false;
        g_outboundAutoTestStartNew = false;
        g_inboundAutoTestStartNew = false;
        g_outboundAutoTestFrameHead = 0;
        g_outboundAutoTestFrameCount = 0;
        g_inboundAutoTestFrameHead = 0;
        g_inboundAutoTestFrameCount = 0;
        g_outboundAutoTestComplete = false;
        g_inboundAutoTestComplete = false;
        g_outboundExtensionHead = 0;
        g_outboundExtensionCount = 0;
        g_inboundExtensionHead = 0;
        g_inboundExtensionCount = 0;
        std::memset(&g_outboundPacket, 0, sizeof(InputPacket));
        g_csInitialized = true;
    }

    {
        ScopedCS lock(g_inboundCS);
        g_inboundHead = 0;
        g_inboundCount = 0;
    }

    {
        ScopedCS lock(g_outboundCS);
        g_outboundPacket = {};
        g_outboundNew = false;
        g_outboundValid = false;
        g_outboundLastSendTick = 0;
    }

    g_recvCount = 0;
    g_lastRecv = 0;
    g_readyTxDiagValid = false;
    g_readyRxDiagValid = false;
    g_peerDisconnected = false;
    g_requestedTimeoutMs = 0;
    g_playerDisconnectRequested = false;
    g_playerDisconnectSent = false;
    g_playerDisconnectSendSucceeded = false;
    g_playerDisconnectNotice = false;
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
    g_outboundValid = true;
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

void Netplay_SendRollback(int32_t rollbackFrames, int32_t effectiveFrame)
{
    ScopedCS lock(g_delayCS);
    g_outboundRollback = rollbackFrames;
    g_outboundRollbackFrame = effectiveFrame;
    g_outboundRollbackNew = true;
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

bool Netplay_GetInboundRollback(int32_t& outRollback, int32_t& outEffectiveFrame)
{
    ScopedCS lock(g_delayCS);
    if (g_inboundRollbackNew)
    {
        outRollback = g_inboundRollback;
        outEffectiveFrame = g_inboundRollbackFrame;
        g_inboundRollbackNew = false;
        return true;
    }
    return false;
}

void Netplay_SendLaunchTrainingSettings(const LaunchTrainingPacket& packet)
{
    if (!g_active || !g_csInitialized)
        return;
    ScopedCS lock(g_launchTrainingCS);
    g_outboundLaunchTraining = packet;
    g_outboundLaunchTraining.type = PACKET_LAUNCH_TRAINING;
    g_outboundLaunchTrainingNew = true;
}

bool Netplay_GetInboundLaunchTrainingSettings(LaunchTrainingPacket& outPacket)
{
    if (!g_active || !g_csInitialized)
        return false;
    ScopedCS lock(g_launchTrainingCS);
    if (!g_inboundLaunchTrainingNew)
        return false;
    outPacket = g_inboundLaunchTraining;
    g_inboundLaunchTrainingNew = false;
    return true;
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

void Netplay_SendSnapDump(int32_t frame, const char* timestamp)
{
    if (!DualTrainingControlPacketsAllowed())
        return;

    ScopedCS lock(g_snapDumpCS);
    g_outboundSnapDump = {};
    g_outboundSnapDump.type = PACKET_SNAPDUMP;
    g_outboundSnapDump.frame = frame;
    if (timestamp)
        strncpy(g_outboundSnapDump.timestamp, timestamp,
                sizeof(g_outboundSnapDump.timestamp) - 1);
    g_outboundSnapDumpNew = true;
}

bool Netplay_GetInboundSnapDump(int32_t& outFrame, char* timestamp,
                                size_t timestampSize)
{
    if (!g_active || !g_csInitialized || !timestamp || timestampSize == 0)
        return false;

    ScopedCS lock(g_snapDumpCS);
    if (!DualTrainingControlPacketsAllowed())
    {
        g_inboundSnapDumpNew = false;
        return false;
    }
    if (!g_inboundSnapDumpNew)
        return false;
    outFrame = g_inboundSnapDump.frame;
    strncpy(timestamp, g_inboundSnapDump.timestamp, timestampSize - 1);
    timestamp[timestampSize - 1] = '\0';
    g_inboundSnapDumpNew = false;
    return true;
}

void Netplay_SendAutoTestStart(const AutoTestStartPacket& packet)
{
    if (!DualTrainingControlPacketsAllowed())
        return;
    ScopedCS lock(g_autoTestStartCS);
    g_outboundAutoTestStart = packet;
    g_outboundAutoTestStart.type = PACKET_AUTOTEST_START;
    g_outboundAutoTestStartNew = true;
}

bool Netplay_GetInboundAutoTestStart(AutoTestStartPacket& outPacket)
{
    if (!DualTrainingControlPacketsAllowed() || !g_active || !g_csInitialized)
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
    if (!DualTrainingControlPacketsAllowed() || !g_active || !g_csInitialized)
        return;
    ScopedCS lock(g_autoTestCompleteCS);
    g_outboundAutoTestComplete = true;
}

bool Netplay_GetInboundAutoTestComplete()
{
    if (!DualTrainingControlPacketsAllowed() || !g_active || !g_csInitialized)
        return false;
    ScopedCS lock(g_autoTestCompleteCS);
    bool complete = g_inboundAutoTestComplete;
    g_inboundAutoTestComplete = false;
    return complete;
}

void Netplay_SendAutoTestFrame(const AutoTestFramePacket& packet)
{
    if (!DualTrainingControlPacketsAllowed())
        return;
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
    if (!DualTrainingControlPacketsAllowed() || !g_active || !g_csInitialized)
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

void Netplay_SendBattleReady(uint32_t generation, uint32_t flags, uint32_t roundSeed,
                             uint32_t openingMember)
{
    ScopedCS lock(g_battleReadyCS);
    g_outboundBattleReady.type = PACKET_BATTLE_READY;
    g_outboundBattleReady.generation = generation;
    g_outboundBattleReady.flags = flags;
    g_outboundBattleReady.round_seed = roundSeed;
    g_outboundBattleReady.opening_member = openingMember;
    g_outboundBattleReadyNew = true;
}

bool Netplay_GetInboundBattleReady(uint32_t& outGeneration, uint32_t& outFlags,
                                   uint32_t& outRoundSeed, uint32_t& outOpeningMember)
{
    ScopedCS lock(g_battleReadyCS);
    if (!g_inboundBattleReadyNew)
        return false;
    outGeneration = g_inboundBattleReady.generation;
    outFlags = g_inboundBattleReady.flags;
    outRoundSeed = g_inboundBattleReady.round_seed;
    outOpeningMember = g_inboundBattleReady.opening_member;
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
    g_outboundRestart.battle_generation = FrameStepper_GetBattleGeneration();
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
    g_outboundRestart.battle_generation = FrameStepper_GetBattleGeneration();
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
    if (g_inboundRestart.battle_generation != FrameStepper_GetBattleGeneration()) {
        HookLog("Netplay: discarded stale restart generation=%u current=%u target=%d\n",
                g_inboundRestart.battle_generation,
                FrameStepper_GetBattleGeneration(),
                g_inboundRestart.target_frame);
        g_inboundRestartNew = false;
        return false;
    }
    outTargetFrame = g_inboundRestart.target_frame;
    outRoundSeed = g_inboundRestart.round_seed;
    outKind = g_inboundRestart.kind;
    outButtonRestart = g_inboundRestart.button_restart;
    g_inboundRestartNew = false;
    return true;
}

bool Netplay_HasInboundRestartMatch()
{
    if (!g_active || !g_csInitialized)
        return false;

    ScopedCS lock(g_restartCS);
    return g_inboundRestartNew &&
           g_inboundRestart.battle_generation == FrameStepper_GetBattleGeneration();
}

bool Netplay_SendExtensionPacket(const void* data, size_t size)
{
    if (!g_active || !g_csInitialized || !data || size < sizeof(uint32_t) ||
        size > MAX_EXTENSION_PACKET_SIZE ||
        *(const uint32_t*)data != PACKET_EXTENSION)
        return false;

    ScopedCS lock(g_extensionCS);
    if (g_outboundExtensionCount == MAX_EXTENSION_PACKETS)
        return false;
    size_t tail = (g_outboundExtensionHead + g_outboundExtensionCount) %
                  MAX_EXTENSION_PACKETS;
    g_outboundExtension[tail].size = size;
    std::memcpy(g_outboundExtension[tail].data, data, size);
    ++g_outboundExtensionCount;
    return true;
}

bool Netplay_GetExtensionPacket(void* data, size_t capacity, size_t& outSize)
{
    outSize = 0;
    if (!g_active || !g_csInitialized || !data)
        return false;

    ScopedCS lock(g_extensionCS);
    if (g_inboundExtensionCount == 0)
        return false;
    const ExtensionPacket& packet = g_inboundExtension[g_inboundExtensionHead];
    if (capacity < packet.size)
        return false;
    outSize = packet.size;
    std::memcpy(data, packet.data, packet.size);
    g_inboundExtensionHead = (g_inboundExtensionHead + 1) % MAX_EXTENSION_PACKETS;
    --g_inboundExtensionCount;
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
        g_outboundValid = false;
        g_outboundLastSendTick = 0;
        std::memset(&g_outboundPacket, 0, sizeof(g_outboundPacket));
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

    Netplay_SendPacket(pkt);
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
    return g_running.load();
}

bool Netplay_TakePeerDisconnected()
{
    return g_peerDisconnected.exchange(false);
}

bool Netplay_SendPlayerDisconnect(uint32_t timeoutMs)
{
    if (!Netplay_IsActive())
        return false;

    g_playerDisconnectSent = false;
    g_playerDisconnectSendSucceeded = false;
    g_playerDisconnectRequested = true;

    const DWORD start = GetTickCount();
    while (!g_playerDisconnectSent && GetTickCount() - start < timeoutMs)
        Sleep(1);
    return g_playerDisconnectSent && g_playerDisconnectSendSucceeded;
}

bool Netplay_TakePlayerDisconnectNotice()
{
    return g_playerDisconnectNotice.exchange(false);
}

bool Netplay_IsHost()
{
    return g_host && g_host->address.port != 0;
}

uint32_t Netplay_GetRTT()
{
    if (SteamTransport_IsActive())
        return SteamTransport_GetRTT();
    if (g_peer)
        return g_peer->roundTripTime;
    return 0;
}

void Netplay_Shutdown()
{
    HookLog("Netplay: Shutting down...\n");
    if (Netplay_IsActive())
        Netplay_SendPlayerDisconnect(250);
    g_running = false;

    if (g_serviceThread)
    {
        WaitForSingleObject(g_serviceThread, 3000);
        CloseHandle(g_serviceThread);
        g_serviceThread = nullptr;
    }

    SteamLobby_Shutdown(false);

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
        DeleteCriticalSection(&g_battleReadyCS);
        DeleteCriticalSection(&g_launchTrainingCS);
        DeleteCriticalSection(&g_desyncCS);
        DeleteCriticalSection(&g_restartCS);
        DeleteCriticalSection(&g_multiResimCS);
        DeleteCriticalSection(&g_snapDumpCS);
        DeleteCriticalSection(&g_autoTestStartCS);
        DeleteCriticalSection(&g_autoTestFrameCS);
        DeleteCriticalSection(&g_autoTestCompleteCS);
        DeleteCriticalSection(&g_extensionCS);
        g_csInitialized = false;
    }

    HookLog("Netplay: Shutdown complete\n");
}
