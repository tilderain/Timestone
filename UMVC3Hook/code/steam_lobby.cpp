#include "steam_lobby.h"

#include "steam_api_compat.h"
#include "steam_transport.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>

extern void HookLog(const char* fmt, ...);

#ifndef UMVC3HOOK_GIT_COMMIT
#define UMVC3HOOK_GIT_COMMIT "unknown"
#endif

namespace {

enum class PendingCall {
    None,
    Search,
    Create,
    Join,
};

struct SteamAPI {
    HMODULE module = nullptr;
    bool (*Init)() = nullptr;
    void (*RunCallbacks)() = nullptr;
    HSteamUser (*GetUser)() = nullptr;
    HSteamPipe (*GetPipe)() = nullptr;
    void* (*GetClient)() = nullptr;
    void* (*GetMatchmaking)(void*, HSteamUser, HSteamPipe, const char*) = nullptr;
    void* (*GetUtils)(void*, HSteamPipe, const char*) = nullptr;
    void* (*GetGeneric)(void*, HSteamUser, HSteamPipe, const char*) = nullptr;

    SteamAPICall_t (*RequestLobbyList)(void*) = nullptr;
    void (*AddStringFilter)(void*, const char*, const char*, int) = nullptr;
    void (*AddSlotsFilter)(void*, int) = nullptr;
    void (*AddResultCountFilter)(void*, int) = nullptr;
    SteamAPICall_t (*CreateLobby)(void*, int, int) = nullptr;
    SteamAPICall_t (*JoinLobby)(void*, uint64_t) = nullptr;
    void (*LeaveLobby)(void*, uint64_t) = nullptr;
    uint64_t (*GetLobbyByIndex)(void*, int) = nullptr;
    int (*GetNumLobbyMembers)(void*, uint64_t) = nullptr;
    uint64_t (*GetLobbyMemberByIndex)(void*, uint64_t, int) = nullptr;
    uint64_t (*GetLobbyOwner)(void*, uint64_t) = nullptr;
    bool (*SetLobbyData)(void*, uint64_t, const char*, const char*) = nullptr;
    const char* (*GetLobbyData)(void*, uint64_t, const char*) = nullptr;
    bool (*SetLobbyJoinable)(void*, uint64_t, bool) = nullptr;

    bool (*IsCallCompleted)(void*, SteamAPICall_t, bool*) = nullptr;
    bool (*GetCallResult)(void*, SteamAPICall_t, void*, int, int, bool*) = nullptr;
};

SteamAPI g_api;
void* g_matchmaking = nullptr;
void* g_utils = nullptr;
ISteamNetworkingSockets* g_sockets = nullptr;
SteamAPICall_t g_pendingHandle = 0;
PendingCall g_pendingCall = PendingCall::None;
uint64_t g_results[50] = {};
size_t g_resultCount = 0;
bool g_resultsChanged = false;
uint64_t g_lobbyId = 0;
bool g_isHost = false;
HSteamListenSocket g_listenSocket = k_HSteamListenSocket_Invalid;
HSteamNetConnection g_connection = k_HSteamNetConnection_Invalid;
ENetHost* g_gameHost = nullptr;
ENetPeer* g_gamePeer = nullptr;
bool g_handlesReady = false;
bool g_bridgeCreated = false;
char g_status[256] = "Steam is not initialized";

template <typename T>
bool Resolve(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(g_api.module, name));
    if (!target) HookLog("SteamLobby: missing export %s\n", name);
    return target != nullptr;
}

void SetStatus(const char* status)
{
    snprintf(g_status, sizeof(g_status), "%s", status);
    HookLog("SteamLobby: %s\n", g_status);
}

void SetStatusf(const char* format, uint64_t value)
{
    snprintf(g_status, sizeof(g_status), format, static_cast<unsigned long long>(value));
    HookLog("SteamLobby: %s\n", g_status);
}

bool ResolveExports()
{
    bool ok = true;
    ok &= Resolve(g_api.Init, "SteamAPI_Init");
    ok &= Resolve(g_api.RunCallbacks, "SteamAPI_RunCallbacks");
    ok &= Resolve(g_api.GetUser, "SteamAPI_GetHSteamUser");
    ok &= Resolve(g_api.GetPipe, "SteamAPI_GetHSteamPipe");
    ok &= Resolve(g_api.GetClient, "SteamClient");
    ok &= Resolve(g_api.GetMatchmaking, "SteamAPI_ISteamClient_GetISteamMatchmaking");
    ok &= Resolve(g_api.GetUtils, "SteamAPI_ISteamClient_GetISteamUtils");
    ok &= Resolve(g_api.GetGeneric, "SteamAPI_ISteamClient_GetISteamGenericInterface");
    ok &= Resolve(g_api.RequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    ok &= Resolve(g_api.AddStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    ok &= Resolve(g_api.AddSlotsFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable");
    ok &= Resolve(g_api.AddResultCountFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    ok &= Resolve(g_api.CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby");
    ok &= Resolve(g_api.JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby");
    ok &= Resolve(g_api.LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby");
    ok &= Resolve(g_api.GetLobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex");
    ok &= Resolve(g_api.GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
    ok &= Resolve(g_api.GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
    ok &= Resolve(g_api.GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
    ok &= Resolve(g_api.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    ok &= Resolve(g_api.GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    ok &= Resolve(g_api.SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    ok &= Resolve(g_api.IsCallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    ok &= Resolve(g_api.GetCallResult, "SteamAPI_ISteamUtils_GetAPICallResult");
    return ok;
}

template <typename T>
bool TakeCallResult(T& result, int callbackId)
{
    bool failed = false;
    if (!g_api.IsCallCompleted(g_utils, g_pendingHandle, &failed)) return false;
    if (failed || !g_api.GetCallResult(g_utils, g_pendingHandle, &result,
                                       sizeof(result), callbackId, &failed) || failed) {
        g_pendingCall = PendingCall::None;
        g_pendingHandle = 0;
        SetStatus("Steam request failed");
        return false;
    }
    return true;
}

void ConnectionOptions(SteamNetworkingConfigValue_t (&options)[2])
{
    options[0] = {};
    options[0].m_eValue = k_ESteamNetworkingConfig_SymmetricConnect;
    options[0].m_eDataType = k_ESteamNetworkingConfig_Int32;
    options[0].m_val.m_int32 = 1;
    options[1] = {};
    options[1].m_eValue = k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable;
    options[1].m_eDataType = k_ESteamNetworkingConfig_Int32;
    options[1].m_val.m_int32 = 0; // Never exchange peer IP candidates; force relay transport.
}

void StartPeerConnection(uint64_t remoteId)
{
    if (!remoteId || g_connection != k_HSteamNetConnection_Invalid) return;

    SteamNetworkingConfigValue_t options[2];
    ConnectionOptions(options);
    if (g_listenSocket == k_HSteamListenSocket_Invalid) {
        g_listenSocket = g_sockets->CreateListenSocketP2P(733, 2, options);
        if (g_listenSocket == k_HSteamListenSocket_Invalid) {
            SetStatus("Failed to create Steam P2P listen socket");
            return;
        }
    }

    SteamNetworkingIdentity identity = {};
    identity.m_eType = k_ESteamNetworkingIdentityType_SteamID;
    identity.m_cbSize = sizeof(uint64_t);
    identity.data.m_steamID64 = remoteId;
    g_connection = g_sockets->ConnectP2P(identity, 733, 2, options);
    if (g_connection == k_HSteamNetConnection_Invalid) {
        SetStatus("Failed to start Steam P2P connection");
        return;
    }
    SetStatus("Establishing Steam relay connection...");
}

void PumpLobbyMembers()
{
    if (!g_lobbyId || g_bridgeCreated || g_connection != k_HSteamNetConnection_Invalid)
        return;
    int count = g_api.GetNumLobbyMembers(g_matchmaking, g_lobbyId);
    if (count < 2) return;

    uint64_t owner = g_api.GetLobbyOwner(g_matchmaking, g_lobbyId);
    uint64_t remote = owner;
    if (g_isHost) {
        remote = 0;
        for (int i = 0; i < count; ++i) {
            uint64_t member = g_api.GetLobbyMemberByIndex(g_matchmaking, g_lobbyId, i);
            if (member && member != owner) {
                remote = member;
                break;
            }
        }
    }
    StartPeerConnection(remote);
}

void PumpConnection()
{
    if (g_connection == k_HSteamNetConnection_Invalid || g_bridgeCreated) return;
    SteamNetConnectionRealTimeStatus_t status = {};
    EResult result = g_sockets->GetConnectionRealTimeStatus(g_connection, &status, 0, nullptr);
    if (result != k_EResultOK) return;
    if (status.m_eState == k_ESteamNetworkingConnectionState_Connected) {
        if (!SteamTransport_CreateBridge(g_isHost, g_sockets, g_connection,
                                         &g_gameHost, &g_gamePeer)) {
            SetStatus("Steam connected, but local transport bridge failed");
            return;
        }
        g_bridgeCreated = true;
        g_handlesReady = true;
        g_api.SetLobbyJoinable(g_matchmaking, g_lobbyId, false);
        SetStatus("Steam peer connected");
    } else if (status.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
               status.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        SetStatus("Steam peer connection failed");
        g_connection = k_HSteamNetConnection_Invalid;
    }
}

} // namespace

bool SteamLobby_Initialize()
{
    if (g_matchmaking && g_sockets) return true;
    g_api.module = GetModuleHandleA("steam_api64e.dll");
    if (!g_api.module) g_api.module = LoadLibraryA("steam_api64e.dll");
    if (!g_api.module) {
        SetStatus("steam_api64e.dll was not found");
        return false;
    }
    if (!ResolveExports() || !g_api.Init()) {
        SetStatus("SteamAPI_Init failed; launch the game through Steam");
        return false;
    }

    void* client = g_api.GetClient();
    HSteamUser user = g_api.GetUser();
    HSteamPipe pipe = g_api.GetPipe();
    g_matchmaking = g_api.GetMatchmaking(client, user, pipe, "SteamMatchMaking009");
    g_utils = g_api.GetUtils(client, pipe, "SteamUtils008");
    g_sockets = static_cast<ISteamNetworkingSockets*>(
        g_api.GetGeneric(client, user, pipe, k_SteamNetworkingSocketsVersion));
    if (!g_matchmaking || !g_utils) {
        SetStatus("Steam matchmaking interfaces are unavailable");
        return false;
    }
    if (!g_sockets) {
        SetStatus("SteamNetworkingSockets012 is unavailable through this Steam client");
        return false;
    }
    SetStatus("Steam ready");
    return true;
}

void SteamLobby_Search()
{
    if (!SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    g_api.AddStringFilter(g_matchmaking, "heronet", "1", 0);
    g_api.AddStringFilter(g_matchmaking, "protocol", "1", 0);
    g_api.AddSlotsFilter(g_matchmaking, 1);
    g_api.AddResultCountFilter(g_matchmaking, 50);
    g_pendingHandle = g_api.RequestLobbyList(g_matchmaking);
    g_pendingCall = PendingCall::Search;
    SetStatus("Searching for HeroNet lobbies...");
}

void SteamLobby_Create()
{
    if (!SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    g_pendingHandle = g_api.CreateLobby(g_matchmaking, 2, 2); // public, two members
    g_pendingCall = PendingCall::Create;
    SetStatus("Creating HeroNet lobby...");
}

void SteamLobby_Join(uint64_t lobbyId)
{
    if (!lobbyId || !SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    g_pendingHandle = g_api.JoinLobby(g_matchmaking, lobbyId);
    g_pendingCall = PendingCall::Join;
    g_isHost = false;
    SetStatus("Joining HeroNet lobby...");
}

void SteamLobby_Pump()
{
    if (!g_matchmaking || !g_utils) return;

    if (g_pendingCall == PendingCall::Search) {
        LobbyMatchList_t result = {};
        if (TakeCallResult(result, k_iCallbackLobbyMatchList)) {
            g_resultCount = result.m_nLobbiesMatching < 50 ? result.m_nLobbiesMatching : 50;
            for (size_t i = 0; i < g_resultCount; ++i)
                g_results[i] = g_api.GetLobbyByIndex(g_matchmaking, static_cast<int>(i));
            g_resultsChanged = true;
            g_pendingCall = PendingCall::None;
            g_pendingHandle = 0;
            if (g_resultCount == 1)
                snprintf(g_status, sizeof(g_status), "Found 1 HeroNet lobby");
            else
                snprintf(g_status, sizeof(g_status), "Found %u HeroNet lobbies",
                         (unsigned)g_resultCount);
        }
    } else if (g_pendingCall == PendingCall::Create) {
        LobbyCreated_t result = {};
        if (TakeCallResult(result, k_iCallbackLobbyCreated)) {
            g_pendingCall = PendingCall::None;
            g_pendingHandle = 0;
            if (result.m_eResult != k_EResultOK || !result.m_ulSteamIDLobby) {
                SetStatus("Steam lobby creation failed");
            } else {
                g_lobbyId = result.m_ulSteamIDLobby;
                g_isHost = true;
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "heronet", "1");
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "protocol", "1");
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "build", UMVC3HOOK_GIT_COMMIT);
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "state", "waiting");
                SetStatusf("Lobby %llu created; waiting for opponent", g_lobbyId);
            }
        }
    } else if (g_pendingCall == PendingCall::Join) {
        LobbyEnter_t result = {};
        if (TakeCallResult(result, k_iCallbackLobbyEnter)) {
            g_pendingCall = PendingCall::None;
            g_pendingHandle = 0;
            if (result.m_EChatRoomEnterResponse != 1 || !result.m_ulSteamIDLobby) {
                SetStatus("Could not enter Steam lobby");
            } else {
                g_lobbyId = result.m_ulSteamIDLobby;
                g_isHost = false;
                SetStatus("Lobby joined; connecting to owner...");
            }
        }
    }

    PumpLobbyMembers();
    PumpConnection();
}

const char* SteamLobby_GetStatus()
{
    return g_status;
}

size_t SteamLobby_GetResultCount()
{
    return g_resultCount;
}

uint64_t SteamLobby_GetResult(size_t index)
{
    return index < g_resultCount ? g_results[index] : 0;
}

bool SteamLobby_TakeResultsChanged()
{
    bool changed = g_resultsChanged;
    g_resultsChanged = false;
    return changed;
}

bool SteamLobby_TakeNetworkHandles(ENetHost** outHost, ENetPeer** outPeer,
                                   bool* outIsHost)
{
    if (!g_handlesReady || !outHost || !outPeer || !outIsHost) return false;
    *outHost = g_gameHost;
    *outPeer = g_gamePeer;
    *outIsHost = g_isHost;
    g_handlesReady = false;
    return true;
}

void SteamLobby_Shutdown(bool preserveConnection)
{
    if (preserveConnection) return;
    SteamTransport_Shutdown();
    if (g_sockets && g_listenSocket != k_HSteamListenSocket_Invalid)
        g_sockets->CloseListenSocket(g_listenSocket);
    g_listenSocket = k_HSteamListenSocket_Invalid;
    g_connection = k_HSteamNetConnection_Invalid;
    if (g_matchmaking && g_lobbyId) g_api.LeaveLobby(g_matchmaking, g_lobbyId);
    g_lobbyId = 0;
    g_gameHost = nullptr;
    g_gamePeer = nullptr;
    g_handlesReady = false;
    g_bridgeCreated = false;
}
