#include "steam_transport.h"
#include "lobby_local.h"
#include "launch_music.h"
#include "netplay.h"

#include <Windows.h>
#include <atomic>

extern void HookLog(const char* fmt, ...);

static ISteamNetworkingSockets* g_sockets = nullptr;
static HSteamNetConnection g_connection = k_HSteamNetConnection_Invalid;
static ENetHost* g_bridgeHost = nullptr;
static ENetPeer* g_bridgePeer = nullptr;
static HANDLE g_bridgeThread = nullptr;
static std::atomic<bool> g_bridgeRunning{false};
static std::atomic<bool> g_bridgeThreadStarted{false};
static std::atomic_flag g_bridgePumpLock = ATOMIC_FLAG_INIT;
static std::atomic<uint32_t> g_steamRTT{0};
static std::atomic<float> g_steamPacketLoss{-1.0f};
static std::atomic<float> g_steamRemotePacketLoss{-1.0f};
static constexpr DWORD STATUS_POLL_INTERVAL_MS = 500;
static DWORD g_lastStatusPollTick = 0;
static std::atomic<int32_t> g_steamJitter{-1};
static constexpr DWORD JITTER_WINDOW_MS = 500;
static int32_t g_jitterWindowPeakUsec = -1;
static DWORD g_jitterWindowStartTick = 0;
static DWORD g_bridgeCreatedTick = 0;

// The route Steam is carrying the connection on. Steam may move a live
// connection between its direct and relayed transports at any time.
enum SteamRoute : int {
    kRouteUnknown, kRouteDirect, kRouteRelay, kRouteRelaySdr, kRouteLoopback
};
static const char* const kRouteNames[] = {
    "Unknown", "Direct", "Relay", "Relay (SDR)", "Loopback"
};
static std::atomic<int> g_steamRoute{kRouteUnknown};
static uint32_t g_steamRelayPop = 0; // bridge thread only
static std::atomic<int32_t> g_logJitterPeakUsec{-1};
static std::atomic<int32_t> g_relayPingEstimate{-1};

// A relay's data-center code is up to four characters packed into an integer.
static void FormatPop(uint32_t pop, char out[5])
{
    int length = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        const char c = static_cast<char>((pop >> shift) & 0xFF);
        if (c) out[length++] = c;
    }
    out[length] = '\0';
}

// Runs on the status poll, twice a second: reads the route and logs when it
// differs from the one last seen.
// What a route change cost, written once the new route has had time to show
// it: Steam's ping is smoothed, so the figure at the change itself is still
// the old route's. Bridge thread only.
static constexpr DWORD ROUTE_SETTLE_MS = 10 * 1000;
static int g_routePolledPing = -1;      // the poll before this one
static int g_routePingBefore = -1;      // on the route that was left
static DWORD g_routeChangeTick = 0;     // 0: no report owed
static float g_routeWorstLossIn = 0.0f, g_routeWorstLossOut = 0.0f;
static int g_routeWorstPing = 0;

static void ReportSettledRoute(const SteamNetConnectionRealTimeStatus_t& status, int route)
{
    if (!g_routeChangeTick) return;
    const float in = g_steamPacketLoss.load(std::memory_order_relaxed);
    const float out = g_steamRemotePacketLoss.load(std::memory_order_relaxed);
    if (in > g_routeWorstLossIn) g_routeWorstLossIn = in;
    if (out > g_routeWorstLossOut) g_routeWorstLossOut = out;
    if (status.m_nPing > g_routeWorstPing) g_routeWorstPing = status.m_nPing;
    if (GetTickCount() - g_routeChangeTick < ROUTE_SETTLE_MS) return;
    g_routeChangeTick = 0;
    HookLog("[Netplay] Route settled: %s, %lu s after the change | RTT: %d -> %d ms "
            "(worst %d) | relay-est=%d ms | worst loss in=%.1f%% out=%.1f%%\n",
            kRouteNames[route], ROUTE_SETTLE_MS / 1000, g_routePingBefore, status.m_nPing,
            g_routeWorstPing, g_relayPingEstimate.load(std::memory_order_relaxed),
            g_routeWorstLossIn, g_routeWorstLossOut);
}

static void PollRoute(const SteamNetConnectionRealTimeStatus_t& status)
{
    SteamNetConnectionInfo_t info = {};
    if (status.m_eState != k_ESteamNetworkingConnectionState_Connected ||
        !g_sockets->GetConnectionInfo(g_connection, &info))
        return;

    int route = kRouteDirect;
    if (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers)
        route = kRouteLoopback;
    else if (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed)
        route = info.m_idPOPRelay ? kRouteRelaySdr : kRouteRelay;
    const uint32_t pop = route == kRouteRelaySdr ? info.m_idPOPRelay : 0;

    const int previous = g_steamRoute.load(std::memory_order_relaxed);
    if (route == previous && pop == g_steamRelayPop) {
        ReportSettledRoute(status, route);
        g_routePolledPing = status.m_nPing;
        return;
    }

    char popText[5], previousPopText[5];
    FormatPop(pop, popText);
    FormatPop(g_steamRelayPop, previousPopText);
    const double seconds = (GetTickCount() - g_bridgeCreatedTick) / 1000.0;
    if (previous == kRouteUnknown) {
        HookLog("[Netplay] Route: %s%s%s | RTT: %d ms | relay-est=%d ms\n",
                kRouteNames[route], pop ? " pop=" : "", popText, status.m_nPing,
                g_relayPingEstimate.load(std::memory_order_relaxed));
    } else {
        // A second change inside the window keeps the first one's "before".
        if (!g_routeChangeTick)
            g_routePingBefore = g_routePolledPing > 0 ? g_routePolledPing : status.m_nPing;
        HookLog("[Netplay] Route changed: %s%s%s -> %s%s%s at +%.1f s | RTT before: %d ms | "
                "relay-est=%d ms | loss in=%.1f%% out=%.1f%%\n",
                kRouteNames[previous], g_steamRelayPop ? " pop=" : "", previousPopText,
                kRouteNames[route], pop ? " pop=" : "", popText, seconds,
                g_routePingBefore, g_relayPingEstimate.load(std::memory_order_relaxed),
                g_steamPacketLoss.load(std::memory_order_relaxed),
                g_steamRemotePacketLoss.load(std::memory_order_relaxed));
        g_routeChangeTick = GetTickCount();
        if (!g_routeChangeTick) g_routeChangeTick = 1;
        g_routeWorstLossIn = g_routeWorstLossOut = 0.0f;
        g_routeWorstPing = 0;
    }
    g_routePolledPing = status.m_nPing;
    g_steamRelayPop = pop;
    g_steamRoute.store(route, std::memory_order_relaxed);
}

// Steam's full statistics for the connection: lifetime ping and quality
// histograms, packet totals, and the relays used. Must run before the
// connection is closed, and never while a match is being played.
static void LogDetailedStatus()
{
    static char text[16 * 1024];
    if (!g_sockets || g_connection == k_HSteamNetConnection_Invalid) return;
    if (g_sockets->GetDetailedConnectionStatus(g_connection, text, sizeof(text)) < 0)
        return;
    text[sizeof(text) - 1] = '\0';
    HookLog("[Netplay] Steam connection summary after %.1f s, route=%s:\n%s\n"
            "[Netplay] End of Steam connection summary\n",
            (GetTickCount() - g_bridgeCreatedTick) / 1000.0,
            kRouteNames[g_steamRoute.load()], text);
}

// The loopback hop can go unserviced while launch hooks finish installing.
// Steam owns real peer liveness, so its local proxy must outlive that handoff.
static constexpr enet_uint32 kLoopbackHandoffTimeoutMs = 10 * 60 * 1000;

static bool PumpBridgeOnce()
{
    if (!g_bridgeRunning || g_bridgePumpLock.test_and_set()) return false;

    ENetEvent event;
    while (g_bridgeHost && enet_host_service(g_bridgeHost, &event, 0) > 0) {
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            int flags = (event.packet->flags & ENET_PACKET_FLAG_RELIABLE)
                ? k_nSteamNetworkingSend_Reliable
                : k_nSteamNetworkingSend_UnreliableNoNagle;
            EResult result = g_sockets->SendMessageToConnection(
                g_connection, event.packet->data,
                static_cast<uint32_t>(event.packet->dataLength), flags, nullptr);
            if (result != k_EResultOK)
                HookLog("SteamTransport: send failed result=%d\n", (int)result);
            enet_packet_destroy(event.packet);
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            HookLog("SteamTransport: local ENet bridge peer disconnected after %lu ms "
                    "state=%d data=%u\n",
                    GetTickCount() - g_bridgeCreatedTick,
                    event.peer ? (int)event.peer->state : -1, event.data);
            g_bridgeRunning = false;
        }
    }

    SteamNetworkingMessage_t* messages[32] = {};
    int count = g_sockets->ReceiveMessagesOnConnection(g_connection, messages, 32);
    for (int i = 0; i < count; ++i) {
        SteamNetworkingMessage_t* message = messages[i];
        enet_uint32 flags = (message->m_nFlags & k_nSteamNetworkingSend_Reliable)
            ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
        ENetPacket* packet = enet_packet_create(message->m_pData, message->m_cbSize, flags);
        if (!packet || enet_peer_send(g_bridgePeer, 0, packet) != 0) {
            if (packet) enet_packet_destroy(packet);
            HookLog("SteamTransport: failed to forward inbound message\n");
        }
        message->Release();
    }
    if (g_bridgeHost) enet_host_flush(g_bridgeHost);

    // Keep telemetry polling out of the low-latency packet forwarding cadence.
    const DWORD statusTick = GetTickCount();
    if (statusTick - g_lastStatusPollTick < STATUS_POLL_INTERVAL_MS) {
        g_bridgePumpLock.clear();
        return true;
    }
    g_lastStatusPollTick = statusTick;

    SteamNetConnectionRealTimeStatus_t status = {};
    EResult statusResult = g_sockets->GetConnectionRealTimeStatus(
        g_connection, &status, 0, nullptr);
    if (statusResult == k_EResultOK) {
        if (status.m_nPing > 0) g_steamRTT = static_cast<uint32_t>(status.m_nPing);
        // Steam measures packets delivered in order, so this includes reordering.
        const float quality = status.m_flConnectionQualityLocal;
        g_steamPacketLoss = quality >= 0.0f && quality <= 1.0f
            ? (1.0f - quality) * 100.0f : -1.0f;
        const float remoteQuality = status.m_flConnectionQualityRemote;
        g_steamRemotePacketLoss = remoteQuality >= 0.0f && remoteQuality <= 1.0f
            ? (1.0f - remoteQuality) * 100.0f : -1.0f;
        // Steam clears its jitter peak on each poll. Aggregate for a readable HUD.
        if (status.m_usecMaxJitter > g_jitterWindowPeakUsec)
            g_jitterWindowPeakUsec = status.m_usecMaxJitter;
        // The log line keeps its own peak, spanning the time between lines.
        int32_t logPeak = g_logJitterPeakUsec.load(std::memory_order_relaxed);
        while (status.m_usecMaxJitter > logPeak &&
               !g_logJitterPeakUsec.compare_exchange_weak(
                   logPeak, status.m_usecMaxJitter, std::memory_order_relaxed)) {}
        PollRoute(status);
        const DWORD now = GetTickCount();
        if (now - g_jitterWindowStartTick >= JITTER_WINDOW_MS) {
            g_steamJitter = g_jitterWindowPeakUsec < 0
                ? -1 : g_jitterWindowPeakUsec / 1000;
            g_jitterWindowPeakUsec = -1;
            g_jitterWindowStartTick = now;
        }
        if (status.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            status.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
            HookLog("SteamTransport: Steam connection ended, state=%d\n",
                    (int)status.m_eState);
            if (g_bridgePeer) enet_peer_disconnect_now(g_bridgePeer, 0);
            g_bridgeRunning = false;
        }
    }
    else {
        g_steamPacketLoss = -1.0f;
        g_steamRemotePacketLoss = -1.0f;
    }
    g_bridgePumpLock.clear();
    return true;
}

static DWORD WINAPI BridgeThread(LPVOID)
{
    g_bridgeThreadStarted = true;
    HookLog("SteamTransport: bridge thread started\n");
    while (g_bridgeRunning) {
        PumpBridgeOnce();
        Sleep(1);
    }
    HookLog("SteamTransport: bridge thread stopped\n");
    return 0;
}

static ENetHost* CreateLoopbackServer(enet_uint16& outPort)
{
    ENetAddress address = {};
    enet_address_set_host_ip(&address, "127.0.0.1");
    const enet_uint16 firstPort = static_cast<enet_uint16>(38000 + (GetCurrentProcessId() % 1000));
    for (int i = 0; i < 1000; ++i) {
        address.port = static_cast<enet_uint16>(38000 + ((firstPort - 38000 + i) % 1000));
        ENetHost* host = enet_host_create(&address, 1, 2, 0, 0);
        if (host) {
            Netplay_ConfigureHostLimits(host);
            outPort = address.port;
            return host;
        }
    }
    return nullptr;
}

bool SteamTransport_CreateBridge(bool isHost, ISteamNetworkingSockets* sockets,
                                 HSteamNetConnection connection,
                                 ENetHost** outGameHost, ENetPeer** outGamePeer)
{
    if (!sockets || connection == k_HSteamNetConnection_Invalid ||
        !outGameHost || !outGamePeer || g_bridgeRunning)
        return false;

    enet_uint16 port = 0;
    ENetHost* server = CreateLoopbackServer(port);
    ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
    Netplay_ConfigureHostLimits(client);
    if (!server || !client) {
        if (server) enet_host_destroy(server);
        if (client) enet_host_destroy(client);
        return false;
    }

    ENetAddress target = {};
    enet_address_set_host_ip(&target, "127.0.0.1");
    target.port = port;
    ENetPeer* clientPeer = enet_host_connect(client, &target, 2, 0);
    ENetPeer* serverPeer = nullptr;
    bool clientConnected = false;
    DWORD deadline = GetTickCount() + 3000;
    while (GetTickCount() < deadline && (!clientConnected || !serverPeer)) {
        ENetEvent event;
        while (enet_host_service(client, &event, 1) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) clientConnected = true;
        }
        while (enet_host_service(server, &event, 1) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) serverPeer = event.peer;
        }
    }
    if (!clientConnected || !serverPeer) {
        enet_host_destroy(client);
        enet_host_destroy(server);
        return false;
    }

    enet_peer_timeout(clientPeer, 0, kLoopbackHandoffTimeoutMs,
                      kLoopbackHandoffTimeoutMs);
    enet_peer_timeout(serverPeer, 0, kLoopbackHandoffTimeoutMs,
                      kLoopbackHandoffTimeoutMs);
    // Neither loopback hop may discard forwarded input packets.
    Netplay_DisablePacketThrottle(clientPeer);
    Netplay_DisablePacketThrottle(serverPeer);

    if (isHost) {
        *outGameHost = server;
        *outGamePeer = serverPeer;
        g_bridgeHost = client;
        g_bridgePeer = clientPeer;
    } else {
        *outGameHost = client;
        *outGamePeer = clientPeer;
        g_bridgeHost = server;
        g_bridgePeer = serverPeer;
    }

    g_sockets = sockets;
    g_connection = connection;
    g_steamRTT = 0;
    g_steamPacketLoss = -1.0f;
    g_steamRemotePacketLoss = -1.0f;
    g_steamJitter = -1;
    g_jitterWindowPeakUsec = -1;
    g_jitterWindowStartTick = GetTickCount();
    g_steamRoute = kRouteUnknown;
    g_steamRelayPop = 0;
    g_routePolledPing = -1;
    g_routeChangeTick = 0;
    g_logJitterPeakUsec = -1;
    g_bridgeCreatedTick = GetTickCount();
    g_lastStatusPollTick = g_bridgeCreatedTick - STATUS_POLL_INTERVAL_MS;
    g_bridgeThreadStarted = false;
    g_bridgeRunning = true;
    g_bridgeThread = CreateThread(nullptr, 0, BridgeThread, nullptr, 0, nullptr);
    if (!g_bridgeThread) {
        g_bridgeRunning = false;
        enet_host_destroy(g_bridgeHost);
        g_bridgeHost = nullptr;
        g_bridgePeer = nullptr;
        return false;
    }
    HookLog("SteamTransport: local ENet bridge ready (%s)\n", isHost ? "host" : "client");
    return true;
}

void SteamTransport_Pump()
{
    // A thread created during DLL initialization cannot run until the loader
    // lock is released, so the launch dialog services the bridge temporarily.
    if (!g_bridgeThreadStarted) PumpBridgeOnce();
}

bool SteamTransport_IsActive()
{
    return g_bridgeRunning || g_bridgeThread != nullptr;
}

uint32_t SteamTransport_GetRTT()
{
    // Local test lobby: the match runs over localhost, there is no Steam route.
    if (LocalLobby_Enabled()) return 2;
    return g_steamRTT.load();
}

float SteamTransport_GetPacketLoss()
{
    return g_steamPacketLoss.load();
}

float SteamTransport_GetRemotePacketLoss()
{
    return g_steamRemotePacketLoss.load();
}

int32_t SteamTransport_GetJitter()
{
    return g_steamJitter.load();
}

const char* SteamTransport_GetRouteName()
{
    if (LocalLobby_Enabled()) return "Local";
    return kRouteNames[g_steamRoute.load(std::memory_order_relaxed)];
}

int32_t SteamTransport_TakeLogJitterPeakUsec()
{
    return g_logJitterPeakUsec.exchange(-1, std::memory_order_relaxed);
}

void SteamTransport_SetRelayPingEstimate(int32_t pingMs)
{
    g_relayPingEstimate.store(pingMs, std::memory_order_relaxed);
}

int32_t SteamTransport_GetRelayPingEstimate()
{
    return g_relayPingEstimate.load(std::memory_order_relaxed);
}

void SteamTransport_Shutdown()
{
    HookLog("SteamTransport: shutdown requested (running=%d, thread=%p)\n",
            g_bridgeRunning.load() ? 1 : 0, g_bridgeThread);
    // Let the music fade in the background while returning to the lobby browser.
    LaunchMusic_BeginFadeOut();
    const DWORD flushDeadline = GetTickCount() + 50;
    while (g_bridgeRunning && !PumpBridgeOnce() &&
           GetTickCount() < flushDeadline)
        Sleep(1);
    g_bridgeRunning = false;
    if (g_bridgeThread) {
        WaitForSingleObject(g_bridgeThread, 3000);
        CloseHandle(g_bridgeThread);
        g_bridgeThread = nullptr;
    }
    if (g_bridgeHost) {
        enet_host_destroy(g_bridgeHost);
        g_bridgeHost = nullptr;
        g_bridgePeer = nullptr;
    }
    // The bridge thread has stopped, so nothing else is using the connection.
    LogDetailedStatus();
    if (g_sockets && g_connection != k_HSteamNetConnection_Invalid)
        g_sockets->CloseConnection(g_connection, 1000, "Timestone shutdown", false);
    g_connection = k_HSteamNetConnection_Invalid;
    g_sockets = nullptr;
    g_bridgeThreadStarted = false;
    g_steamRTT = 0;
    g_steamPacketLoss = -1.0f;
    g_steamRemotePacketLoss = -1.0f;
    g_steamJitter = -1;
    g_steamRoute = kRouteUnknown;
    g_logJitterPeakUsec = -1;
    g_relayPingEstimate = -1;
}
