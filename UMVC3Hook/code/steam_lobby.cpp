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
    void* (*GetFriends)(void*, HSteamUser, HSteamPipe, const char*) = nullptr;
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
    bool (*RequestLobbyData)(void*, uint64_t) = nullptr;
    bool (*SetLobbyData)(void*, uint64_t, const char*, const char*) = nullptr;
    const char* (*GetLobbyData)(void*, uint64_t, const char*) = nullptr;
    bool (*SetLobbyJoinable)(void*, uint64_t, bool) = nullptr;

    const char* (*GetPersonaName)(void*) = nullptr;
    const char* (*GetFriendPersonaName)(void*, uint64_t) = nullptr;

    bool (*IsCallCompleted)(void*, SteamAPICall_t, bool*) = nullptr;
    bool (*GetCallResult)(void*, SteamAPICall_t, void*, int, int, bool*) = nullptr;
};

SteamAPI g_api;
void* g_matchmaking = nullptr;
void* g_utils = nullptr;
void* g_friends = nullptr;
ISteamNetworkingSockets* g_sockets = nullptr;
ISteamNetworkingUtils* g_networkingUtils = nullptr;
SteamAPICall_t g_pendingHandle = 0;
PendingCall g_pendingCall = PendingCall::None;
uint64_t g_results[50] = {};
int g_resultPings[50] = {};
char g_resultNames[50][128] = {};
char g_resultUsernames[50][128] = {};
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
bool g_pingLocationPublished = false;
ULONGLONG g_nextPingLocationAttempt = 0;
ULONGLONG g_nextResultPingRefresh = 0;
ULONGLONG g_resultPingRefreshDeadline = 0;
HANDLE g_callbackThread = nullptr;
volatile LONG g_stopCallbackThread = 0;
bool g_callbacksOnUiThread = false;
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
    ok &= Resolve(g_api.GetFriends, "SteamAPI_ISteamClient_GetISteamFriends");
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
    ok &= Resolve(g_api.RequestLobbyData, "SteamAPI_ISteamMatchmaking_RequestLobbyData");
    ok &= Resolve(g_api.SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData");
    ok &= Resolve(g_api.GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData");
    ok &= Resolve(g_api.SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable");
    ok &= Resolve(g_api.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    ok &= Resolve(g_api.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ok &= Resolve(g_api.IsCallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    ok &= Resolve(g_api.GetCallResult, "SteamAPI_ISteamUtils_GetAPICallResult");
    return ok;
}

DWORD WINAPI CallbackPumpThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_stopCallbackThread, 0, 0) == 0) {
        g_api.RunCallbacks();
        Sleep(10);
    }
    return 0;
}

bool IsValveSteamApi()
{
    char path[MAX_PATH];
    if (!GetModuleFileNameA(g_api.module, path, sizeof(path))) return false;

    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeA(path, &ignored);
    if (!size) return false;
    void* versionInfo = HeapAlloc(GetProcessHeap(), 0, size);
    if (!versionInfo) return false;

    bool isValve = false;
    if (GetFileVersionInfoA(path, 0, size, versionInfo)) {
        struct Translation { WORD language; WORD codePage; };
        Translation* translations = nullptr;
        UINT translationBytes = 0;
        if (VerQueryValueA(versionInfo, "\\VarFileInfo\\Translation",
                           reinterpret_cast<void**>(&translations), &translationBytes)) {
            size_t count = translationBytes / sizeof(Translation);
            for (size_t i = 0; i < count && !isValve; ++i) {
                char query[64];
                snprintf(query, sizeof(query), "\\StringFileInfo\\%04x%04x\\CompanyName",
                         translations[i].language, translations[i].codePage);
                char* company = nullptr;
                UINT companyLength = 0;
                if (VerQueryValueA(versionInfo, query, reinterpret_cast<void**>(&company),
                                   &companyLength) && company) {
                    for (const char* p = company; *p; ++p) {
                        if (_strnicmp(p, "Valve", 5) == 0) {
                            isValve = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, versionInfo);
    return isValve;
}

void StartCallbackPump()
{
    if (g_callbacksOnUiThread || g_callbackThread) return;
    InterlockedExchange(&g_stopCallbackThread, 0);
    g_callbackThread = CreateThread(nullptr, 0, CallbackPumpThread, nullptr, 0, nullptr);
    if (!g_callbackThread)
        HookLog("SteamLobby: failed to start Steam callback pump\n");
}

void StopCallbackPump()
{
    if (!g_callbackThread) return;
    InterlockedExchange(&g_stopCallbackThread, 1);
    WaitForSingleObject(g_callbackThread, 250);
    CloseHandle(g_callbackThread);
    g_callbackThread = nullptr;
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

void PumpPingLocation()
{
    if (!g_isHost || !g_lobbyId || g_pingLocationPublished || !g_networkingUtils)
        return;

    ULONGLONG now = GetTickCount64();
    if (now < g_nextPingLocationAttempt) return;
    g_nextPingLocationAttempt = now + 1000;

    SteamNetworkPingLocation_t location = {};
    if (g_networkingUtils->GetLocalPingLocation(location) < 0.0f) return;

    char encoded[k_cchMaxSteamNetworkingPingLocationString] = {};
    g_networkingUtils->ConvertPingLocationToString(location, encoded, sizeof(encoded));
    if (!encoded[0] || !g_api.SetLobbyData(g_matchmaking, g_lobbyId, "location", encoded))
        return;

    g_pingLocationPublished = true;
    HookLog("SteamLobby: published ping location %s\n", encoded);
}

int EstimateLobbyPing(uint64_t lobbyId)
{
    if (!g_networkingUtils || !lobbyId) return -1;
    const char* encoded = g_api.GetLobbyData(g_matchmaking, lobbyId, "location");
    if (!encoded || !encoded[0]) return -1;

    SteamNetworkPingLocation_t location = {};
    if (!g_networkingUtils->ParsePingLocationString(encoded, location)) return -1;
    int ping = g_networkingUtils->EstimatePingTimeFromLocalHost(location);
    return ping >= 0 ? ping : -1;
}

bool CopyResultText(char* destination, size_t destinationSize, const char* source)
{
    if (!source) source = "";
    if (strncmp(destination, source, destinationSize) == 0) return false;
    snprintf(destination, destinationSize, "%s", source);
    return true;
}

bool RefreshResultMetadata(size_t index)
{
    uint64_t lobbyId = g_results[index];
    bool changed = CopyResultText(g_resultNames[index], sizeof(g_resultNames[index]),
                                  g_api.GetLobbyData(g_matchmaking, lobbyId, "name"));

    const char* username = g_api.GetLobbyData(g_matchmaking, lobbyId, "username");
    if ((!username || !username[0]) && g_friends) {
        uint64_t owner = g_api.GetLobbyOwner(g_matchmaking, lobbyId);
        username = owner ? g_api.GetFriendPersonaName(g_friends, owner) : "";
    }
    changed |= CopyResultText(g_resultUsernames[index], sizeof(g_resultUsernames[index]),
                              username);
    return changed;
}

void RefreshResultPings()
{
    ULONGLONG now = GetTickCount64();
    if (!g_resultCount || now < g_nextResultPingRefresh ||
        now >= g_resultPingRefreshDeadline)
        return;

    g_nextResultPingRefresh = now + 1000;
    bool changed = false;
    bool pending = false;
    for (size_t i = 0; i < g_resultCount; ++i) {
        changed |= RefreshResultMetadata(i);
        if (g_resultPings[i] >= 0) continue;
        int ping = EstimateLobbyPing(g_results[i]);
        if (ping >= 0) {
            g_resultPings[i] = ping;
            changed = true;
            HookLog("SteamLobby: estimated lobby %llu ping at %d ms\n",
                    static_cast<unsigned long long>(g_results[i]), ping);
        } else {
            pending = true;
            g_api.RequestLobbyData(g_matchmaking, g_results[i]);
        }
    }
    if (changed) g_resultsChanged = true;
    if (!pending) g_resultPingRefreshDeadline = 0;
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
    if (g_matchmaking && g_sockets) {
        StartCallbackPump();
        return true;
    }
    g_api.module = GetModuleHandleA("steam_api64.dll");
    if (!g_api.module) g_api.module = LoadLibraryA("steam_api64.dll");
    if (!g_api.module) {
        SetStatus("steam_api64.dll was not found");
        return false;
    }
    if (!ResolveExports() || !g_api.Init()) {
        SetStatus("SteamAPI_Init failed; launch the game through Steam");
        return false;
    }
    g_callbacksOnUiThread = IsValveSteamApi();
    HookLog("SteamLobby: callback pump mode: %s\n",
            g_callbacksOnUiThread ? "launch thread (Valve Steam API)" : "worker thread (compatibility)");

    void* client = g_api.GetClient();
    HSteamUser user = g_api.GetUser();
    HSteamPipe pipe = g_api.GetPipe();
    static const char* friendVersions[] = {
        "SteamFriends017", "SteamFriends016", "SteamFriends015", "SteamFriends014"
    };
    for (const char* version : friendVersions) {
        g_friends = g_api.GetFriends(client, user, pipe, version);
        if (g_friends) break;
    }
    g_matchmaking = g_api.GetMatchmaking(client, user, pipe, "SteamMatchMaking009");
    g_utils = g_api.GetUtils(client, pipe, "SteamUtils008");
    g_sockets = static_cast<ISteamNetworkingSockets*>(
        g_api.GetGeneric(client, user, pipe, k_SteamNetworkingSocketsVersion));
    g_networkingUtils = static_cast<ISteamNetworkingUtils*>(
        g_api.GetGeneric(client, user, pipe, k_SteamNetworkingUtilsVersion));
    if (!g_matchmaking || !g_utils) {
        SetStatus("Steam matchmaking interfaces are unavailable");
        return false;
    }
    if (!g_sockets) {
        SetStatus("SteamNetworkingSockets012 is unavailable through this Steam client");
        return false;
    }
    if (!g_networkingUtils) {
        StartCallbackPump();
        SetStatus("Steam ready; ping estimates unavailable");
        return true;
    }
    g_networkingUtils->CheckPingDataUpToDate(1.0e10f);
    StartCallbackPump();
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
    if (g_callbacksOnUiThread) g_api.RunCallbacks();

    if (g_pendingCall == PendingCall::Search) {
        LobbyMatchList_t result = {};
        if (TakeCallResult(result, k_iCallbackLobbyMatchList)) {
            g_resultCount = result.m_nLobbiesMatching < 50 ? result.m_nLobbiesMatching : 50;
            for (size_t i = 0; i < g_resultCount; ++i) {
                g_results[i] = g_api.GetLobbyByIndex(g_matchmaking, static_cast<int>(i));
                g_api.RequestLobbyData(g_matchmaking, g_results[i]);
                g_resultPings[i] = EstimateLobbyPing(g_results[i]);
                g_resultNames[i][0] = '\0';
                g_resultUsernames[i][0] = '\0';
                RefreshResultMetadata(i);
            }
            g_nextResultPingRefresh = 0;
            g_resultPingRefreshDeadline = GetTickCount64() + 15000;
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
                g_pingLocationPublished = false;
                g_nextPingLocationAttempt = 0;
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "heronet", "1");
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "protocol", "1");
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "build", UMVC3HOOK_GIT_COMMIT);
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "state", "waiting");
                const char* username = g_friends ? g_api.GetPersonaName(g_friends) : "";
                if (username && username[0]) {
                    char lobbyName[128];
                    snprintf(lobbyName, sizeof(lobbyName), "%s's Lobby", username);
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "name", lobbyName);
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "username", username);
                }
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

    PumpPingLocation();
    RefreshResultPings();
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

int SteamLobby_GetResultEstimatedPing(size_t index)
{
    return index < g_resultCount ? g_resultPings[index] : -1;
}

const char* SteamLobby_GetResultName(size_t index)
{
    return index < g_resultCount ? g_resultNames[index] : "";
}

const char* SteamLobby_GetResultUsername(size_t index)
{
    return index < g_resultCount ? g_resultUsernames[index] : "";
}

bool SteamLobby_ArePingEstimatesAvailable()
{
    return g_networkingUtils != nullptr;
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
    StopCallbackPump();
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
    g_pingLocationPublished = false;
    g_nextPingLocationAttempt = 0;
    g_nextResultPingRefresh = 0;
    g_resultPingRefreshDeadline = 0;
}
