#include "netplay.h"
#include "rollback.h"
#include "rollback_diagnostics.h"
#include "controller.h"
#include "LaunchMenu.h"
#include "steam_lobby.h"
#include "steam_transport.h"
#include <cstring>
#include <atomic>
#include <Windows.h>

extern void HookLog(const char* fmt, ...);
extern "C" uint32_t FrameStepper_GetBattleGeneration();
extern "C" void Controller_ResetBuffers();

// Receivers own their state. ENet payloads are dispatched synchronously, never
// stored in transport queues. The only background work is the freeze watchdog.
static ENetHost* g_host = nullptr;
static ENetPeer* g_peer = nullptr;
static std::atomic<bool> g_running { false };
static std::atomic<bool> g_isHost { false };
static std::atomic<bool> g_peerDisconnected { false };
static std::atomic<bool> g_playerDisconnectNotice { false };
static std::atomic<DWORD> g_gameThread { 0 };
static bool g_pumping = false;
static uint32_t g_recvCount = 0;
static uint32_t g_lastRecv = 0;
static uint32_t g_recvInputs = 0;
static uint32_t g_lastRecvInputs = 0;
static uint32_t g_lastStatsTick = 0;
static bool g_readyTxDiagValid = false;
static BattleReadyPacket g_readyTxDiag = {};
static int g_readyTxDiagResult = 0;
static int g_readyTxDiagPeerState = -1;
static bool g_readyRxDiagValid = false;
static BattleReadyPacket g_readyRxDiag = {};
static constexpr size_t MAX_EXTENSION_PACKET_SIZE = 256;

// Render/UI callbacks can send or request shutdown. Serialize ENet's mutable
// host/peer state with them, but NEVER hold this lock while invoking receivers.
static SRWLOCK g_enetLock = SRWLOCK_INIT;
class EnetLock {
public:
    EnetLock() { AcquireSRWLockExclusive(&g_enetLock); }
    ~EnetLock() { ReleaseSRWLockExclusive(&g_enetLock); }
    EnetLock(const EnetLock&) = delete;
    EnetLock& operator=(const EnetLock&) = delete;
};

static HANDLE g_watchdogThread = nullptr;
static std::atomic<bool> g_watchdogRunning { false };
static DWORD WINAPI FreezeWatchdogThread(LPVOID)
{
    while (g_watchdogRunning) {
        Rollback_FreezeWatchdogPoll();
        Sleep(100);
    }
    return 0;
}

static bool DualTrainingControlPacketsAllowed()
{
    return GetLaunchOption() == LAUNCH_DUAL_TRAINING;
}

static void LogBattleReadyTx(const BattleReadyPacket& packet, int result, int peerState)
{
    if (!g_readyTxDiagValid || std::memcmp(&g_readyTxDiag, &packet, sizeof(packet)) != 0 ||
        g_readyTxDiagResult != result || g_readyTxDiagPeerState != peerState) {
        HookLog("Netplay: BattleReady TX gen=%u flags=0x%X seed=0x%08X "
                "opening=%u result=%d peerState=%d\n", packet.generation, packet.flags,
                packet.round_seed, packet.opening_member, result, peerState);
    }
    g_readyTxDiagValid = true;
    g_readyTxDiag = packet;
    g_readyTxDiagResult = result;
    g_readyTxDiagPeerState = peerState;
}

static bool SendWire(const void* data, size_t size, uint32_t type)
{
    if (!g_running)
        return false;
    EnetLock lock;
    if (!g_running || !g_host || !g_peer)
        return false;
    const enet_uint32 flags = type == PACKET_INPUT ?
        ENET_PACKET_FLAG_UNSEQUENCED : ENET_PACKET_FLAG_RELIABLE;
    ENetPacket* packet = enet_packet_create(data, size, flags);
    int result = packet ? enet_peer_send(g_peer, 0, packet) : -1;
    if (type == PACKET_BATTLE_READY)
        LogBattleReadyTx(*(const BattleReadyPacket*)data, result, (int)g_peer->state);
    if (result != 0) {
        if (packet) enet_packet_destroy(packet);
        return false;
    }
    // ENet owns reliable buffering/retries. Flush immediately rather than wait
    // for the next game tick (particularly important in input-wait barriers).
    enet_host_flush(g_host);
    return true;
}

template<typename T>
static void SendPacket(uint32_t type, const T& source)
{
    T packet = source;
    packet.type = type;
    SendWire(&packet, sizeof(packet), type);
}

template<typename T>
static bool ReadWire(const ENetPacket& source, T& packet)
{
    if (source.dataLength < sizeof(packet))
        return false;
    std::memcpy(&packet, source.data, sizeof(packet));
    return true;
}

static void MarkPeerDisconnected(ENetPeer* peer, bool disconnectNow)
{
    EnetLock lock;
    if (!g_running || g_peer != peer)
        return;
    g_peerDisconnected = true;
    g_playerDisconnectNotice = true;
    if (disconnectNow && peer)
        enet_peer_disconnect_now(peer, 0);
    if (peer) peer->data = nullptr;
    g_peer = nullptr;
    g_running = false;
}

static void HandleReceivedPacket(const ENetPacket& source, ENetPeer* peer)
{
    if (source.dataLength < sizeof(uint32_t) || source.dataLength > NET_MAX_PACKET_SIZE)
        return;
    ++g_recvCount;
    uint32_t type;
    std::memcpy(&type, source.data, sizeof(type));
    switch (type) {
    case PACKET_INPUT: {
        InputPacket packet;
        if (ReadWire(source, packet)) {
            ++g_recvInputs;
            Controller_ReceiveNetplayInput(packet);
        }
        break;
    }
    case PACKET_DELAY: {
        DelayPacket packet;
        if (ReadWire(source, packet)) FrameStepper_ReceiveNetplayControl(type, &packet);
        break;
    }
    case PACKET_LAUNCH_TRAINING: {
        LaunchTrainingPacket packet;
        if (!ReadWire(source, packet) || packet.autoJump[0] > 1 || packet.autoJump[1] > 1 ||
            packet.autoJumpValidMask > 3 || packet.voiceValidMask > 3)
            break;
        for (int team = 0; team < 2; ++team)
            for (int member = 0; member < 3; ++member)
                if (packet.voiceLanguage[team][member] > 1) return;
        TrainingBoot_ReceiveNetplaySettings(packet);
        break;
    }
    case PACKET_BATTLE_READY: {
        BattleReadyPacket packet;
        if (!ReadWire(source, packet)) break;
        if (!g_readyRxDiagValid || std::memcmp(&g_readyRxDiag, &packet, sizeof(packet)) != 0) {
            HookLog("Netplay: BattleReady RX gen=%u flags=0x%X seed=0x%08X opening=%u\n",
                    packet.generation, packet.flags, packet.round_seed, packet.opening_member);
        }
        g_readyRxDiagValid = true;
        g_readyRxDiag = packet;
        FrameStepper_ReceiveNetplayControl(type, &packet);
        break;
    }
    case PACKET_DESYNC: {
        DesyncPacket packet;
        if (ReadWire(source, packet) &&
            (packet.action == DESYNC_TRIGGER || packet.action == DESYNC_UNPAUSE))
            FrameStepper_ReceiveNetplayControl(type, &packet);
        break;
    }
    case PACKET_RESTART_MATCH: {
        RestartMatchPacket packet;
        if (ReadWire(source, packet)) FrameStepper_ReceiveNetplayControl(type, &packet);
        break;
    }
    case PACKET_MULTI_RESIM: {
        MultiResimPacket packet;
        if (ReadWire(source, packet) && packet.enabled <= 1)
            FrameStepper_ReceiveNetplayControl(type, &packet);
        break;
    }
    case PACKET_SNAPDUMP: {
        SnapDumpPacket packet;
        if (DualTrainingControlPacketsAllowed() && ReadWire(source, packet)) {
            packet.timestamp[sizeof(packet.timestamp) - 1] = '\0';
            FrameStepper_ReceiveNetplayControl(type, &packet);
        }
        break;
    }
    case PACKET_AUTOTEST_START: {
        AutoTestStartPacket packet;
        if (DualTrainingControlPacketsAllowed() && ReadWire(source, packet)) {
            packet.move_name[sizeof(packet.move_name) - 1] = '\0';
            AutoTest_ReceiveNetplayStart(packet);
        }
        break;
    }
    case PACKET_AUTOTEST_FRAME: {
        AutoTestFramePacket packet;
        if (DualTrainingControlPacketsAllowed() && ReadWire(source, packet))
            AutoTest_ReceiveNetplayFrame(packet);
        break;
    }
    case PACKET_AUTOTEST_COMPLETE: {
        AutoTestCompletePacket packet;
        if (DualTrainingControlPacketsAllowed() && ReadWire(source, packet))
            FrameStepper_ReceiveNetplayControl(type, &packet);
        break;
    }
    case PACKET_PLAYER_DISCONNECT:
        HookLog("Netplay: Remote player sent disconnect notice\n");
        MarkPeerDisconnected(peer, true);
        break;
    case PACKET_EXTENSION:
        if (source.dataLength <= MAX_EXTENSION_PACKET_SIZE)
            ChrSelImGui_ReceiveNetplayPacket(source.data, source.dataLength);
        break;
    }
}

void Netplay_Pump()
{
    if (!g_running || g_isResimulating)
        return;
    // Only the live game-thread entry/wait paths call this function. Init can
    // happen on the bootstrap thread; bind ownership on the first live pump.
    const DWORD thread = GetCurrentThreadId();
    DWORD unbound = 0;
    if (g_gameThread.load(std::memory_order_relaxed) == 0)
        g_gameThread.compare_exchange_strong(unbound, thread, std::memory_order_relaxed);
    if (thread != g_gameThread.load(std::memory_order_relaxed) || g_pumping)
        return;
    g_pumping = true;
    for (size_t serviced = 0; serviced < 64 && g_running; ++serviced) {
        ENetEvent event;
        int result;
        {
            EnetLock lock;
            result = g_running && g_host ? enet_host_service(g_host, &event, 0) : 0;
            if (result > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
                event.peer->data = (void*)"connected";
        }
        if (result <= 0) break;
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            HandleReceivedPacket(*event.packet, event.peer);
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            HookLog("Netplay: Remote peer disconnected\n");
            MarkPeerDisconnected(event.peer, false);
        }
    }
    g_pumping = false;
    const uint32_t now = GetTickCount();
    if (now - g_lastStatsTick >= 5000) {
        uint32_t rtt = 0, throttle = 0, throttleLimit = 0;
        {
            EnetLock lock;
            if (g_peer) {
                rtt = g_peer->roundTripTime;
                throttle = g_peer->packetThrottle;
                throttleLimit = g_peer->packetThrottleLimit;
            }
        }
        HookLog("Netplay: received %u packets (%u inputs) in last 5s (total %u), "
                "rtt=%u ms throttle=%u/%u, game-thread pump\n",
                g_recvCount - g_lastRecv, g_recvInputs - g_lastRecvInputs,
                g_recvCount, rtt, throttle, throttleLimit);
        g_lastRecv = g_recvCount;
        g_lastRecvInputs = g_recvInputs;
        g_lastStatsTick = now;
    }
}

void Netplay_ConfigureHostLimits(ENetHost* host)
{
    if (!host) return;
    host->maximumPacketSize = NET_MAX_PACKET_SIZE;
    host->maximumWaitingData = NET_MAX_WAITING_DATA;
}

void Netplay_Init(ENetHost* host, ENetPeer* peer)
{
    {
        EnetLock lock;
        if (g_host || !host || !peer) return;
        g_host = host;
        g_peer = peer;
        g_isHost.store(host->address.port != 0, std::memory_order_relaxed);
        // Native loading has no game-thread pump. Allow startup/roster loading
        // time; the handshake restores the short gameplay timeout.
        enet_peer_timeout(peer, 0, 60000, 60000);
        // The launch handshake is polled quickly, but gameplay ACKs are frame
        // paced. ENet's default RTT throttle mistakes that jump for congestion
        // and deliberately drops our unsequenced input history. Its baseline
        // only refreshes every 5 seconds, producing a long startup slowdown.
        // This small fixed-rate input stream must not be randomly suppressed.
        // Keep bandwidth limits and reliable retransmission behavior intact.
        enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                     ENET_PEER_PACKET_THROTTLE_SCALE, 0);
        peer->packetThrottle = peer->packetThrottleLimit;
        peer->packetThrottleCounter = 0;
    }
    g_gameThread = 0;
    g_pumping = false;
    g_recvCount = g_lastRecv = 0;
    g_recvInputs = g_lastRecvInputs = 0;
    g_lastStatsTick = GetTickCount();
    g_readyTxDiagValid = g_readyRxDiagValid = false;
    g_peerDisconnected = g_playerDisconnectNotice = false;
    Controller_ResetBuffers();
    if constexpr (ROLLBACK_FREEZE_WATCHDOG_ENABLED) {
        g_watchdogRunning = true;
        g_watchdogThread = CreateThread(nullptr, 0, FreezeWatchdogThread, nullptr, 0, nullptr);
        if (!g_watchdogThread) {
            g_watchdogRunning = false;
            HookLog("Netplay: could not start freeze watchdog\n");
        }
    }
    g_running = true;
    HookLog("Netplay: Initialized direct game-thread ENet pump (no mailbox)\n");
}

void Netplay_SetTimeout(uint32_t timeoutMs)
{
    if (!g_running || !timeoutMs) return;
    EnetLock lock;
    if (g_running && g_peer) enet_peer_timeout(g_peer, 0, timeoutMs, timeoutMs);
}

void Netplay_SendPacket(const InputPacket& packet) { SendPacket(PACKET_INPUT, packet); }
void Netplay_SendDelay(int32_t delay, int32_t effectiveFrame)
{
    const DelayPacket packet = {PACKET_DELAY, delay, -1, effectiveFrame};
    SendPacket(PACKET_DELAY, packet);
}
void Netplay_SendRollback(int32_t rollbackFrames, int32_t effectiveFrame)
{
    const DelayPacket packet = {PACKET_DELAY, -1, rollbackFrames, effectiveFrame};
    SendPacket(PACKET_DELAY, packet);
}
void Netplay_SendLaunchTrainingSettings(const LaunchTrainingPacket& packet)
{
    SendPacket(PACKET_LAUNCH_TRAINING, packet);
}
void Netplay_SendMultiResim(bool enabled)
{
    const MultiResimPacket packet = {PACKET_MULTI_RESIM, enabled ? 1u : 0u};
    SendPacket(PACKET_MULTI_RESIM, packet);
}
void Netplay_SendSnapDump(int32_t frame, const char* timestamp)
{
    if (!DualTrainingControlPacketsAllowed()) return;
    SnapDumpPacket packet = {};
    packet.frame = frame;
    if (timestamp) strncpy(packet.timestamp, timestamp, sizeof(packet.timestamp) - 1);
    SendPacket(PACKET_SNAPDUMP, packet);
}
void Netplay_SendAutoTestStart(const AutoTestStartPacket& packet)
{
    if (DualTrainingControlPacketsAllowed()) SendPacket(PACKET_AUTOTEST_START, packet);
}
void Netplay_SendAutoTestFrame(const AutoTestFramePacket& packet)
{
    if (DualTrainingControlPacketsAllowed()) SendPacket(PACKET_AUTOTEST_FRAME, packet);
}
void Netplay_SendAutoTestComplete()
{
    const AutoTestCompletePacket packet = {PACKET_AUTOTEST_COMPLETE};
    if (DualTrainingControlPacketsAllowed()) SendPacket(PACKET_AUTOTEST_COMPLETE, packet);
}
void Netplay_SendBattleReady(uint32_t generation, uint32_t flags, uint32_t roundSeed,
                             uint32_t openingMember)
{
    const BattleReadyPacket packet = {PACKET_BATTLE_READY, generation, flags, roundSeed, openingMember};
    SendPacket(PACKET_BATTLE_READY, packet);
}
void Netplay_SendDesyncTrigger(int32_t frame, uint32_t localChecksum, uint32_t remoteChecksum)
{
    const DesyncPacket packet = {PACKET_DESYNC, FrameStepper_GetBattleGeneration(),
                                DESYNC_TRIGGER, frame, localChecksum, remoteChecksum};
    SendPacket(PACKET_DESYNC, packet);
}
void Netplay_SendDesyncUnpause()
{
    DesyncPacket packet = {};
    packet.battle_generation = FrameStepper_GetBattleGeneration();
    packet.action = DESYNC_UNPAUSE;
    SendPacket(PACKET_DESYNC, packet);
}
void Netplay_SendRestartMatch(int32_t targetFrame, uint32_t roundSeed)
{
    Netplay_SetTimeout(60000);
    const RestartMatchPacket packet = {PACKET_RESTART_MATCH, FrameStepper_GetBattleGeneration(),
                                      targetFrame, roundSeed, RESTART_FULL_MATCH, 0};
    SendPacket(PACKET_RESTART_MATCH, packet);
}
void Netplay_SendTrainingRestart(int32_t targetFrame, uint32_t roundSeed, int32_t buttonRestart)
{
    Netplay_SetTimeout(60000);
    const RestartMatchPacket packet = {PACKET_RESTART_MATCH, FrameStepper_GetBattleGeneration(),
                                      targetFrame, roundSeed, RESTART_TRAINING, buttonRestart};
    SendPacket(PACKET_RESTART_MATCH, packet);
}
bool Netplay_SendExtensionPacket(const void* data, size_t size)
{
    if (!data || size < sizeof(uint32_t) || size > MAX_EXTENSION_PACKET_SIZE) return false;
    uint32_t type;
    std::memcpy(&type, data, sizeof(type));
    return type == PACKET_EXTENSION && SendWire(data, size, type);
}
bool Netplay_IsActive() { return g_running.load(); }
bool Netplay_OwnsHost() { EnetLock lock; return g_host != nullptr; }
bool Netplay_TakePeerDisconnected() { return g_peerDisconnected.exchange(false); }
bool Netplay_TakePlayerDisconnectNotice() { return g_playerDisconnectNotice.exchange(false); }
bool Netplay_SendPlayerDisconnect(uint32_t)
{
    const PlayerDisconnectPacket packet = {PACKET_PLAYER_DISCONNECT};
    return SendWire(&packet, sizeof(packet), PACKET_PLAYER_DISCONNECT);
}
bool Netplay_IsHost()
{
    return g_isHost.load(std::memory_order_relaxed);
}
uint32_t Netplay_GetRTT()
{
    if (SteamTransport_IsActive()) return SteamTransport_GetRTT();
    EnetLock lock;
    return g_peer ? g_peer->roundTripTime : 0;
}
void Netplay_Shutdown()
{
    Netplay_SendPlayerDisconnect();
    g_running = false;
    g_watchdogRunning = false;
    if (g_watchdogThread) {
        if (WaitForSingleObject(g_watchdogThread, 3000) != WAIT_OBJECT_0) {
            HookLog("Netplay: watchdog still exiting; shutdown deferred\n");
            return;
        }
        CloseHandle(g_watchdogThread);
        g_watchdogThread = nullptr;
    }
    SteamLobby_Shutdown(false);
    EnetLock lock;
    if (g_peer) enet_peer_disconnect_now(g_peer, 0);
    if (g_host) enet_host_destroy(g_host);
    g_peer = nullptr;
    g_host = nullptr;
    g_isHost.store(false, std::memory_order_relaxed);
    HookLog("Netplay: Shutdown complete\n");
}
