#include "steam_transport.h"

#include <Windows.h>
#include <atomic>

extern void HookLog(const char* fmt, ...);

static ISteamNetworkingSockets* g_sockets = nullptr;
static HSteamNetConnection g_connection = k_HSteamNetConnection_Invalid;
static ENetHost* g_bridgeHost = nullptr;
static ENetPeer* g_bridgePeer = nullptr;
static HANDLE g_bridgeThread = nullptr;
static std::atomic<bool> g_bridgeRunning{false};
static std::atomic<uint32_t> g_steamRTT{0};

static DWORD WINAPI BridgeThread(LPVOID)
{
    HookLog("SteamTransport: bridge thread started\n");
    while (g_bridgeRunning) {
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

        SteamNetConnectionRealTimeStatus_t status = {};
        EResult statusResult = g_sockets->GetConnectionRealTimeStatus(
            g_connection, &status, 0, nullptr);
        if (statusResult == k_EResultOK) {
            if (status.m_nPing > 0) g_steamRTT = static_cast<uint32_t>(status.m_nPing);
            if (status.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                status.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                HookLog("SteamTransport: Steam connection ended, state=%d\n",
                        (int)status.m_eState);
                if (g_bridgePeer) enet_peer_disconnect_now(g_bridgePeer, 0);
                g_bridgeRunning = false;
            }
        }
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

bool SteamTransport_IsActive()
{
    return g_bridgeRunning || g_bridgeThread != nullptr;
}

uint32_t SteamTransport_GetRTT()
{
    return g_steamRTT.load();
}

void SteamTransport_Shutdown()
{
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
    if (g_sockets && g_connection != k_HSteamNetConnection_Invalid)
        g_sockets->CloseConnection(g_connection, 1000, "HeroNet shutdown", false);
    g_connection = k_HSteamNetConnection_Invalid;
    g_sockets = nullptr;
    g_steamRTT = 0;
}
