#include "steam_lobby.h"

#include "steam_api_compat.h"
#include "steam_transport.h"
#include "launchmenu_utils.h"
#include "umvc3.h"
#include <Windows.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void HookLog(const char* fmt, ...);

#ifndef UMVC3HOOK_GIT_COMMIT
#define UMVC3HOOK_GIT_COMMIT "unknown"
#endif

namespace {

constexpr const char* kLobbyProtocol = UMVC3HOOK_VERSION;
constexpr ULONGLONG kPeerConnectionTimeoutMs = 25000;
constexpr size_t kMaxPublicLobbyResults = 50;
constexpr size_t kMaxFriendLobbyCandidates = 50;
constexpr size_t kMaxLobbyResults = kMaxPublicLobbyResults + kMaxFriendLobbyCandidates;
constexpr int k_EFriendFlagImmediate = 0x04;

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
    void (*RegisterCallback)(void*, int) = nullptr;
    void (*UnregisterCallback)(void*) = nullptr;

    SteamAPICall_t (*RequestLobbyList)(void*) = nullptr;
    void (*AddStringFilter)(void*, const char*, const char*, int) = nullptr;
    void (*AddSlotsFilter)(void*, int) = nullptr;
    void (*AddResultCountFilter)(void*, int) = nullptr;
    void (*AddDistanceFilter)(void*, int) = nullptr;
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
    bool (*SetRichPresence)(void*, const char*, const char*) = nullptr;
    void (*SetPlayedWith)(void*, uint64_t) = nullptr;

    const char* (*GetPersonaName)(void*) = nullptr;
    const char* (*GetFriendPersonaName)(void*, uint64_t) = nullptr;
    int (*GetMediumFriendAvatar)(void*, uint64_t) = nullptr;
    int (*GetFriendCount)(void*, int) = nullptr;
    uint64_t (*GetFriendByIndex)(void*, int, int) = nullptr;
    bool (*GetFriendGamePlayed)(void*, uint64_t, FriendGameInfo_t*) = nullptr;

    bool (*GetImageSize)(void*, int, uint32_t*, uint32_t*) = nullptr;
    bool (*GetImageRGBA)(void*, int, uint8_t*, int) = nullptr;

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
uint64_t g_results[kMaxLobbyResults] = {};
int g_resultPings[kMaxLobbyResults] = {};
char g_resultNames[kMaxLobbyResults][128] = {};
char g_resultUsernames[kMaxLobbyResults][128] = {};
char g_resultProtocols[kMaxLobbyResults][32] = {};
size_t g_resultCount = 0;
uint64_t g_friendLobbyCandidates[kMaxFriendLobbyCandidates] = {};
size_t g_friendLobbyCandidateCount = 0;
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
ULONGLONG g_nextConnectionAttempt = 0;
ULONGLONG g_connectionDeadline = 0;
unsigned g_connectionAttempt = 0;
ULONGLONG g_nextResultPingRefresh = 0;
ULONGLONG g_resultPingRefreshDeadline = 0;
HANDLE g_callbackThread = nullptr;
volatile LONG g_stopCallbackThread = 0;
bool g_callbacksOnUiThread = false;
bool g_joinCallbacksRegistered = false;
std::atomic<uint64_t> g_pendingJoinLobby{0};
std::atomic<uint64_t> g_replacementJoinLobby{0};
uint64_t g_peerSteamId = 0;
uint64_t g_gamePeerSteamId = 0;
uint64_t g_rejectedPeerSteamId = 0;
char g_peerUsername[128] = {};
char g_localUsername[128] = {};
char g_lobbyName[128] = {};
char g_status[256] = "Steam is not initialized";
char g_versionMismatch[256] = {};
bool g_versionMismatchPending = false;

class SteamCallbackBase {
public:
    SteamCallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, SteamAPICall_t call) = 0;
    int GetICallback() { return m_iCallback; }
    virtual int GetCallbackSizeBytes() = 0;

protected:
    uint8_t m_nCallbackFlags;
    int m_iCallback;
};

void QueueJoinLobby(uint64_t lobbyId)
{
    if (lobbyId) g_pendingJoinLobby.store(lobbyId, std::memory_order_release);
}

bool IsSpace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool ParseLobbyConnectString(const char* connect, uint64_t& lobbyId)
{
    if (!connect) return false;

    constexpr char prefix[] = "+connect_lobby";
    constexpr size_t prefixLength = sizeof(prefix) - 1;
    while (IsSpace(*connect)) ++connect;
    if (strncmp(connect, prefix, prefixLength) != 0) return false;
    connect += prefixLength;
    if (!IsSpace(*connect)) return false;
    while (IsSpace(*connect)) ++connect;

    char* end = nullptr;
    unsigned long long parsed = strtoull(connect, &end, 10);
    if (end == connect || parsed == 0) return false;
    while (IsSpace(*end)) ++end;
    if (*end != '\0' && *end != '"') return false;

    lobbyId = static_cast<uint64_t>(parsed);
    return true;
}

class GameLobbyJoinRequestedCallback final : public SteamCallbackBase {
public:
    GameLobbyJoinRequestedCallback()
    {
        m_iCallback = k_iCallbackGameLobbyJoinRequested;
    }

    void Run(void* param) override
    {
        if (!param) return;
        auto* request = static_cast<GameLobbyJoinRequestedCompat_t*>(param);
        QueueJoinLobby(request->m_steamIDLobby);
    }

    void Run(void* param, bool, SteamAPICall_t) override { Run(param); }

    int GetCallbackSizeBytes() override
    {
        return sizeof(GameLobbyJoinRequestedCompat_t);
    }
};

class GameRichPresenceJoinRequestedCallback final : public SteamCallbackBase {
public:
    GameRichPresenceJoinRequestedCallback()
    {
        m_iCallback = k_iCallbackGameRichPresenceJoinRequested;
    }

    void Run(void* param) override
    {
        if (!param) return;
        auto* request = static_cast<GameRichPresenceJoinRequestedCompat_t*>(param);
        uint64_t lobbyId = 0;
        if (ParseLobbyConnectString(request->m_rgchConnect, lobbyId))
            QueueJoinLobby(lobbyId);
    }

    void Run(void* param, bool, SteamAPICall_t) override { Run(param); }

    int GetCallbackSizeBytes() override
    {
        return sizeof(GameRichPresenceJoinRequestedCompat_t);
    }
};

GameLobbyJoinRequestedCallback g_gameLobbyJoinRequestedCallback;
GameRichPresenceJoinRequestedCallback g_gameRichPresenceJoinRequestedCallback;
static_assert(sizeof(GameLobbyJoinRequestedCallback) == 16);
static_assert(sizeof(GameRichPresenceJoinRequestedCallback) == 16);

template <typename T>
bool Resolve(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(g_api.module, name));
    if (!target) HookLog("SteamLobby: missing export %s\n", name);
    return target != nullptr;
}

template <typename T>
void ResolveOptional(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(g_api.module, name));
    if (!target) HookLog("SteamLobby: optional export %s is unavailable\n", name);
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

void SetVersionMismatch(const char* remoteProtocol)
{
    unsigned remoteMajor = 0, remoteMinor = 0, remotePatch = 0;
    unsigned localMajor = 0, localMinor = 0, localPatch = 0;
    bool parsedRemote = remoteProtocol &&
        sscanf(remoteProtocol, "%u.%u.%u", &remoteMajor, &remoteMinor, &remotePatch) == 3;
    bool parsedLocal = sscanf(kLobbyProtocol, "%u.%u.%u",
                              &localMajor, &localMinor, &localPatch) == 3;
    bool remoteOlder = !parsedRemote || (parsedLocal &&
        (remoteMajor < localMajor ||
         (remoteMajor == localMajor && remoteMinor < localMinor) ||
         (remoteMajor == localMajor && remoteMinor == localMinor &&
          remotePatch < localPatch)));
    snprintf(g_versionMismatch, sizeof(g_versionMismatch),
             "%s\n\nYour version: %s\nTheir version: %s",
             remoteOlder
                 ? "The other player is running an older version."
                 : "Your version does not match, please update Timestone",
             kLobbyProtocol,
             remoteProtocol && remoteProtocol[0] ? remoteProtocol : "unknown");
    g_versionMismatchPending = true;
    SetStatus("Steam lobby version does not match");
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
    ok &= Resolve(g_api.RegisterCallback, "SteamAPI_RegisterCallback");
    ok &= Resolve(g_api.UnregisterCallback, "SteamAPI_UnregisterCallback");
    ok &= Resolve(g_api.RequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList");
    ok &= Resolve(g_api.AddStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter");
    ok &= Resolve(g_api.AddSlotsFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable");
    ok &= Resolve(g_api.AddResultCountFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListResultCountFilter");
    ok &= Resolve(g_api.AddDistanceFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter");
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
    ResolveOptional(g_api.SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    ResolveOptional(g_api.SetPlayedWith, "SteamAPI_ISteamFriends_SetPlayedWith");
    ok &= Resolve(g_api.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    ok &= Resolve(g_api.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ResolveOptional(g_api.GetMediumFriendAvatar,
                    "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    ResolveOptional(g_api.GetFriendCount, "SteamAPI_ISteamFriends_GetFriendCount");
    ResolveOptional(g_api.GetFriendByIndex, "SteamAPI_ISteamFriends_GetFriendByIndex");
    ResolveOptional(g_api.GetFriendGamePlayed, "SteamAPI_ISteamFriends_GetFriendGamePlayed");
    ResolveOptional(g_api.GetImageSize, "SteamAPI_ISteamUtils_GetImageSize");
    ResolveOptional(g_api.GetImageRGBA, "SteamAPI_ISteamUtils_GetImageRGBA");
    ok &= Resolve(g_api.IsCallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted");
    ok &= Resolve(g_api.GetCallResult, "SteamAPI_ISteamUtils_GetAPICallResult");
    return ok;
}

bool RegisterJoinCallbacks()
{
    if (g_joinCallbacksRegistered) return true;
    if (!g_api.RegisterCallback || !g_api.UnregisterCallback) return false;

    g_api.RegisterCallback(&g_gameLobbyJoinRequestedCallback,
                           k_iCallbackGameLobbyJoinRequested);
    g_api.RegisterCallback(&g_gameRichPresenceJoinRequestedCallback,
                           k_iCallbackGameRichPresenceJoinRequested);
    g_joinCallbacksRegistered = true;
    HookLog("SteamLobby: registered Steam join callbacks\n");
    return true;
}

void UnregisterJoinCallbacks()
{
    if (!g_joinCallbacksRegistered || !g_api.UnregisterCallback) return;
    g_api.UnregisterCallback(&g_gameLobbyJoinRequestedCallback);
    g_api.UnregisterCallback(&g_gameRichPresenceJoinRequestedCallback);
    g_joinCallbacksRegistered = false;
}

void SetRichPresence(const char* key, const char* value)
{
    if (g_friends && g_api.SetRichPresence)
        g_api.SetRichPresence(g_friends, key, value ? value : "");
}

void ClearLobbyRichPresence();

void PublishLobbyRichPresence()
{
    if (!g_isHost || !g_lobbyId || g_bridgeCreated) {
        ClearLobbyRichPresence();
        return;
    }

    char connect[64];
    snprintf(connect, sizeof(connect), "+connect_lobby %llu",
             static_cast<unsigned long long>(g_lobbyId));
    SetRichPresence("status", "Waiting in Timestone launch menu");
    SetRichPresence("connect", connect);
}

void ClearLobbyRichPresence()
{
    SetRichPresence("connect", "");
    SetRichPresence("status", "");
}

void PumpQueuedJoinRequest()
{
    if (g_pendingCall != PendingCall::None) return;

    uint64_t lobbyId = g_pendingJoinLobby.exchange(0, std::memory_order_acq_rel);
    if (!lobbyId) return;
    HookLog("SteamLobby: joining queued lobby invite %llu\n",
            static_cast<unsigned long long>(lobbyId));
    SteamLobby_Join(lobbyId);
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

void ConnectionOptions(SteamNetworkingConfigValue_t (&options)[4])
{
    options[0] = {};
    options[0].m_eValue = k_ESteamNetworkingConfig_SymmetricConnect;
    options[0].m_eDataType = k_ESteamNetworkingConfig_Int32;
    options[0].m_val.m_int32 = 1;
    options[1] = {};
    options[1].m_eValue = k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable;
    options[1].m_eDataType = k_ESteamNetworkingConfig_Int32;
    // SDR remains available independently of ICE. TURN requires configured servers.
    options[1].m_val.m_int32 = SteamLobby_IsRelayOnly() ? 0 :
        k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay |
        k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Private |
        k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Public;
    // Direct UDP defaults to omitting the timing data the peer needs for jitter.
    options[2] = {};
    options[2].m_eValue = k_ESteamNetworkingConfig_SendTimeSincePreviousPacket;
    options[2].m_eDataType = k_ESteamNetworkingConfig_Int32;
    options[2].m_val.m_int32 = 1;
    // Prefer SDR unless ICE's route score is better by at least 34 ms.
    options[3] = {};
    options[3].m_eValue = k_ESteamNetworkingConfig_P2P_Transport_ICE_Penalty;
    options[3].m_eDataType = k_ESteamNetworkingConfig_Int32;
    options[3].m_val.m_int32 = 42;
}

void RefreshPeerUsername()
{
    const char* username = g_friends && g_peerSteamId
        ? g_api.GetFriendPersonaName(g_friends, g_peerSteamId)
        : nullptr;
    snprintf(g_peerUsername, sizeof(g_peerUsername), "%s",
             username && username[0] ? username : "Unknown");
}

void StartPeerConnection(uint64_t remoteId)
{
    if (!remoteId || g_connection != k_HSteamNetConnection_Invalid) return;
    ULONGLONG now = GetTickCount64();
    if (!g_connectionDeadline) {
        g_connectionDeadline = now + kPeerConnectionTimeoutMs;
        HookLog("SteamLobby: peer connection deadline started (%llu ms)\n",
                static_cast<unsigned long long>(kPeerConnectionTimeoutMs));
    }
    if (now < g_nextConnectionAttempt) return;

    if (g_peerSteamId != remoteId) {
        g_peerSteamId = remoteId;
        if (g_friends && g_api.SetPlayedWith) {
            g_api.SetPlayedWith(g_friends, g_peerSteamId);
            HookLog("SteamLobby: reported lobby peer %llu to Steam Recent Players\n",
                    static_cast<unsigned long long>(g_peerSteamId));
        }
    }
    RefreshPeerUsername();

    SteamNetworkingConfigValue_t options[4];
    ConnectionOptions(options);
    if (g_listenSocket == k_HSteamListenSocket_Invalid) {
        HookLog("SteamLobby: routing policy=%s ICE candidates=%d\n",
                options[1].m_val.m_int32 ? "Automatic" : "Relay Only",
                options[1].m_val.m_int32);
        g_listenSocket = g_sockets->CreateListenSocketP2P(733, 4, options);
        if (g_listenSocket == k_HSteamListenSocket_Invalid) {
            SetStatus("Failed to create Steam P2P listen socket");
            return;
        }
    }

    SteamNetworkingIdentity identity = {};
    identity.m_eType = k_ESteamNetworkingIdentityType_SteamID;
    identity.m_cbSize = sizeof(uint64_t);
    identity.data.m_steamID64 = remoteId;
    ++g_connectionAttempt;
    g_connection = g_sockets->ConnectP2P(identity, 733, 4, options);
    if (g_connection == k_HSteamNetConnection_Invalid) {
        g_nextConnectionAttempt = GetTickCount64() + 1000;
        snprintf(g_status, sizeof(g_status),
                 "Steam failed: ConnectP2P returned no connection. Trying again... (attempt #%u)",
                 g_connectionAttempt + 1);
        HookLog("SteamLobby: %s\n", g_status);
        return;
    }
    if (g_connectionAttempt == 1)
        SetStatus(options[1].m_val.m_int32 ? "Establishing Steam connection (automatic routing)..."
                                         : "Establishing Steam relay connection...");
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
        if (remote == g_rejectedPeerSteamId) return;
        g_rejectedPeerSteamId = 0;
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
    changed |= CopyResultText(g_resultProtocols[index], sizeof(g_resultProtocols[index]),
                              g_api.GetLobbyData(g_matchmaking, lobbyId, "protocol"));

    const char* username = g_api.GetLobbyData(g_matchmaking, lobbyId, "username");
    if ((!username || !username[0]) && g_friends) {
        uint64_t owner = g_api.GetLobbyOwner(g_matchmaking, lobbyId);
        username = owner ? g_api.GetFriendPersonaName(g_friends, owner) : "";
    }
    changed |= CopyResultText(g_resultUsernames[index], sizeof(g_resultUsernames[index]),
                              username);
    return changed;
}

bool HasResult(uint64_t lobbyId)
{
    for (size_t i = 0; i < g_resultCount; ++i) {
        if (g_results[i] == lobbyId) return true;
    }
    return false;
}

void AddResult(uint64_t lobbyId)
{
    if (!lobbyId || HasResult(lobbyId) || g_resultCount >= kMaxLobbyResults) return;
    size_t index = g_resultCount++;
    g_results[index] = lobbyId;
    g_resultPings[index] = EstimateLobbyPing(lobbyId);
    g_resultNames[index][0] = '\0';
    g_resultUsernames[index][0] = '\0';
    g_resultProtocols[index][0] = '\0';
    RefreshResultMetadata(index);
}

void UpdateSearchStatus()
{
    if (g_resultCount == 1)
        snprintf(g_status, sizeof(g_status), "Found 1 lobby");
    else
        snprintf(g_status, sizeof(g_status), "Found %u lobbies",
                 static_cast<unsigned>(g_resultCount));
}

void CollectFriendLobbyCandidates()
{
    g_friendLobbyCandidateCount = 0;
    if (!g_friends || !g_api.GetFriendCount || !g_api.GetFriendByIndex ||
        !g_api.GetFriendGamePlayed)
        return;

    int friendCount = g_api.GetFriendCount(g_friends, k_EFriendFlagImmediate);
    for (int i = 0; i < friendCount &&
                    g_friendLobbyCandidateCount < kMaxFriendLobbyCandidates; ++i) {
        uint64_t friendId = g_api.GetFriendByIndex(g_friends, i, k_EFriendFlagImmediate);
        FriendGameInfo_t gameInfo = {};
        if (!friendId || !g_api.GetFriendGamePlayed(g_friends, friendId, &gameInfo) ||
            !gameInfo.m_steamIDLobby)
            continue;

        bool duplicate = false;
        for (size_t candidate = 0; candidate < g_friendLobbyCandidateCount; ++candidate) {
            if (g_friendLobbyCandidates[candidate] == gameInfo.m_steamIDLobby) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        g_friendLobbyCandidates[g_friendLobbyCandidateCount++] = gameInfo.m_steamIDLobby;
        g_api.RequestLobbyData(g_matchmaking, gameInfo.m_steamIDLobby);
    }
    HookLog("SteamLobby: found %u friend lobby candidates\n",
            static_cast<unsigned>(g_friendLobbyCandidateCount));
}

bool MergeFriendLobbyCandidates()
{
    bool changed = false;
    for (size_t i = 0; i < g_friendLobbyCandidateCount; ++i) {
        uint64_t lobbyId = g_friendLobbyCandidates[i];
        if (HasResult(lobbyId)) continue;

        const char* marker = g_api.GetLobbyData(g_matchmaking, lobbyId, "Timestone");
        if (marker && strcmp(marker, "1") == 0) {
            AddResult(lobbyId);
            changed = true;
            HookLog("SteamLobby: added friend lobby %llu to search results\n",
                    static_cast<unsigned long long>(lobbyId));
        } else {
            g_api.RequestLobbyData(g_matchmaking, lobbyId);
        }
    }
    if (changed) UpdateSearchStatus();
    return changed;
}

void RefreshResultPings()
{
    ULONGLONG now = GetTickCount64();
    if (now < g_nextResultPingRefresh || now >= g_resultPingRefreshDeadline)
        return;

    g_nextResultPingRefresh = now + 1000;
    bool changed = MergeFriendLobbyCandidates();
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
    for (size_t i = 0; i < g_friendLobbyCandidateCount; ++i)
        pending |= !HasResult(g_friendLobbyCandidates[i]);
    if (changed) g_resultsChanged = true;
    if (!pending) g_resultPingRefreshDeadline = 0;
}

void RefreshConnectedStatus()
{
    SteamNetConnectionInfo_t info = {};
    if (g_sockets->GetConnectionInfo(g_connection, &info)) {
        if (info.m_eState != k_ESteamNetworkingConnectionState_Connected) return;
    }
    snprintf(g_status, sizeof(g_status), "Connected to %s [%s] (SteamID %llu)",
             g_peerUsername, SteamLobby_GetConnectionType(),
             static_cast<unsigned long long>(g_peerSteamId));
}

void PumpConnection()
{
    if (g_bridgeCreated) {
        RefreshConnectedStatus();
        return;
    }
    if (g_connectionDeadline && GetTickCount64() >= g_connectionDeadline) {
        if (g_connection != k_HSteamNetConnection_Invalid) {
            g_sockets->CloseConnection(g_connection, 1002,
                                       "Timestone connection timed out", false);
            g_connection = k_HSteamNetConnection_Invalid;
        }
        if (g_listenSocket != k_HSteamListenSocket_Invalid) {
            g_sockets->CloseListenSocket(g_listenSocket);
            g_listenSocket = k_HSteamListenSocket_Invalid;
        }
        ClearLobbyRichPresence();
        if (g_lobbyId) g_api.LeaveLobby(g_matchmaking, g_lobbyId);
        g_lobbyId = 0;
        g_isHost = false;
        g_peerSteamId = 0;
        g_peerUsername[0] = '\0';
        g_nextConnectionAttempt = 0;
        g_connectionDeadline = 0;
        g_connectionAttempt = 0;
        SetStatus("Steam peer connection timed out; left lobby");
        return;
    }
    if (g_connection == k_HSteamNetConnection_Invalid) return;
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
        g_connectionDeadline = 0;
        g_api.SetLobbyJoinable(g_matchmaking, g_lobbyId, false);
        ClearLobbyRichPresence();
        RefreshPeerUsername();
        RefreshConnectedStatus();
        HookLog("SteamLobby: peer connected username=\"%s\" steamid=%llu\n",
                g_peerUsername,
                static_cast<unsigned long long>(g_peerSteamId));
    } else if (status.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
               status.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
        HSteamNetConnection failedConnection = g_connection;
        SteamNetConnectionInfo_t info = {};
        bool hasInfo = g_sockets->GetConnectionInfo(failedConnection, &info);
        if (hasInfo) {
            HookLog("SteamLobby: peer connection failed state=%d reason=%d debug=\"%s\" connection=\"%s\"\n",
                    static_cast<int>(info.m_eState), info.m_eEndReason,
                    info.m_szEndDebug, info.m_szConnectionDescription);
            if (info.m_szEndDebug[0]) {
                snprintf(g_status, sizeof(g_status),
                         "Steam failed (%d): %.32s. Trying again... (attempt #%u)",
                         info.m_eEndReason, info.m_szEndDebug,
                         g_connectionAttempt + 1);
            } else {
                snprintf(g_status, sizeof(g_status),
                         "Steam failed (reason %d; no debug text). Trying again... (attempt #%u)",
                         info.m_eEndReason, g_connectionAttempt + 1);
            }
        } else {
            HookLog("SteamLobby: peer connection failed state=%d; connection details unavailable\n",
                    static_cast<int>(status.m_eState));
            snprintf(g_status, sizeof(g_status),
                     "Steam failed (state %d; details unavailable). Trying again... (attempt #%u)",
                     static_cast<int>(status.m_eState), g_connectionAttempt + 1);
            HookLog("SteamLobby: %s\n", g_status);
        }
        g_connection = k_HSteamNetConnection_Invalid;
        g_sockets->CloseConnection(failedConnection, 1001,
                                   "Timestone connection retry", false);
        g_nextConnectionAttempt = GetTickCount64() + 1000;
    }
}

} // namespace

bool SteamLobby_IsRelayOnly()
{
    char path[MAX_PATH] = {};
    LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
    return GetPrivateProfileIntA("Network", "SteamRelayOnly", 0, path) != 0;
}

bool SteamLobby_Initialize()
{
    if (g_matchmaking && g_sockets) {
        if (!RegisterJoinCallbacks()) {
            SetStatus("Steam join callbacks are unavailable");
            return false;
        }
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
    const char* localUsername = g_friends ? g_api.GetPersonaName(g_friends) : nullptr;
    snprintf(g_localUsername, sizeof(g_localUsername), "%s",
             localUsername && localUsername[0] ? localUsername : "");
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
    if (!RegisterJoinCallbacks()) {
        SetStatus("Steam join callbacks are unavailable");
        return false;
    }
    ClearLobbyRichPresence();
    if (!g_networkingUtils) {
        StartCallbackPump();
        SetStatus("Steam ready; ping estimates unavailable");
        return true;
    }
    g_networkingUtils->InitRelayNetworkAccess();
    StartCallbackPump();
    SetStatus("Steam ready");
    return true;
}

void SteamLobby_Search()
{
    if (!SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    g_api.AddStringFilter(g_matchmaking, "Timestone", "1", 0);
    g_api.AddSlotsFilter(g_matchmaking, 1);
    g_api.AddResultCountFilter(g_matchmaking, 50);
    g_api.AddDistanceFilter(g_matchmaking, 3); // k_ELobbyDistanceFilterWorldwide
    g_pendingHandle = g_api.RequestLobbyList(g_matchmaking);
    g_pendingCall = PendingCall::Search;
    SetStatus("Searching for lobbies...");
}

void SteamLobby_SetName(const char* name)
{
    snprintf(g_lobbyName, sizeof(g_lobbyName), "%s", name ? name : "");
    if (g_isHost && g_lobbyId && g_matchmaking)
        g_api.SetLobbyData(g_matchmaking, g_lobbyId, "name", g_lobbyName);
}

void SteamLobby_Create(SteamLobbyType type)
{
    if (!SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    if (g_lobbyId) {
        SetStatus("Already in a Steam lobby");
        return;
    }
    g_pendingHandle = g_api.CreateLobby(g_matchmaking, static_cast<int>(type), 2);
    g_pendingCall = PendingCall::Create;
    SetStatus("Creating lobby...");
}

void SteamLobby_Join(uint64_t lobbyId)
{
    if (!lobbyId || !SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    if (HasResult(lobbyId) && !SteamLobby_IsLobbyVersionCompatible(lobbyId)) {
        const char* protocol = g_api.GetLobbyData(g_matchmaking, lobbyId, "protocol");
        SetVersionMismatch(protocol);
        return;
    }
    if (lobbyId == g_lobbyId) {
        SetStatus("Already in the selected Steam lobby");
        return;
    }
    if (g_lobbyId || g_bridgeCreated) {
        g_replacementJoinLobby.store(lobbyId, std::memory_order_release);
        SetStatus("Leaving current lobby before joining another...");
        return;
    }
    ClearLobbyRichPresence();
    g_pendingHandle = g_api.JoinLobby(g_matchmaking, lobbyId);
    g_pendingCall = PendingCall::Join;
    g_isHost = false;
    SetStatus("Joining lobby...");
}

void SteamLobby_Pump()
{
    if (!g_matchmaking || !g_utils) return;
    if (g_callbacksOnUiThread) g_api.RunCallbacks();
    SteamTransport_Pump();

    if (g_pendingCall == PendingCall::Search) {
        LobbyMatchList_t result = {};
        if (TakeCallResult(result, k_iCallbackLobbyMatchList)) {
            g_resultCount = 0;
            size_t publicCount = result.m_nLobbiesMatching < kMaxPublicLobbyResults
                ? result.m_nLobbiesMatching : kMaxPublicLobbyResults;
            for (size_t i = 0; i < publicCount; ++i) {
                uint64_t lobbyId = g_api.GetLobbyByIndex(g_matchmaking, static_cast<int>(i));
                g_api.RequestLobbyData(g_matchmaking, lobbyId);
                AddResult(lobbyId);
            }
            CollectFriendLobbyCandidates();
            MergeFriendLobbyCandidates();
            g_nextResultPingRefresh = 0;
            g_resultPingRefreshDeadline = GetTickCount64() + 15000;
            g_resultsChanged = true;
            g_pendingCall = PendingCall::None;
            g_pendingHandle = 0;
            UpdateSearchStatus();
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
                g_rejectedPeerSteamId = 0;
                g_nextConnectionAttempt = 0;
                g_connectionDeadline = 0;
                g_connectionAttempt = 0;
                g_pingLocationPublished = false;
                g_nextPingLocationAttempt = 0;
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "Timestone", "1");
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "protocol", kLobbyProtocol);
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "build", UMVC3HOOK_GIT_COMMIT);
                g_api.SetLobbyData(g_matchmaking, g_lobbyId, "state", "waiting");
                const char* username = g_friends ? g_api.GetPersonaName(g_friends) : "";
                if (username && username[0]) {
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "username", username);
                }
                if (g_lobbyName[0]) {
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "name", g_lobbyName);
                } else if (username && username[0]) {
                    char fallbackName[128];
                    snprintf(fallbackName, sizeof(fallbackName), "%s's Lobby", username);
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "name", fallbackName);
                }
                PublishLobbyRichPresence();
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
                SteamLobby_Search();
            } else {
                const char* marker = g_api.GetLobbyData(
                    g_matchmaking, result.m_ulSteamIDLobby, "Timestone");
                const char* protocol = g_api.GetLobbyData(
                    g_matchmaking, result.m_ulSteamIDLobby, "protocol");
                if (!marker || strcmp(marker, "1") != 0 || !protocol ||
                    strcmp(protocol, kLobbyProtocol) != 0) {
                    SetVersionMismatch(protocol);
                    g_api.LeaveLobby(g_matchmaking, result.m_ulSteamIDLobby);
                    SteamLobby_Search();
                    return;
                }
                g_lobbyId = result.m_ulSteamIDLobby;
                g_isHost = false;
                ClearLobbyRichPresence();
                g_nextConnectionAttempt = 0;
                g_connectionDeadline = GetTickCount64() + kPeerConnectionTimeoutMs;
                g_connectionAttempt = 0;
                SetStatus("Lobby joined; connecting to owner...");
            }
        } else if (g_pendingCall == PendingCall::None) {
            // TakeCallResult clears the pending call when the Steam request fails.
            SteamLobby_Search();
        }
    }

    PumpQueuedJoinRequest();

    PumpPingLocation();
    RefreshResultPings();
    PumpLobbyMembers();
    PumpConnection();
}

const char* SteamLobby_GetStatus()
{
    return g_status;
}

const char* SteamLobby_GetConnectionType()
{
    SteamNetConnectionInfo_t info = {};
    if (!g_sockets || g_connection == k_HSteamNetConnection_Invalid ||
        !g_sockets->GetConnectionInfo(g_connection, &info) ||
        info.m_eState != k_ESteamNetworkingConnectionState_Connected)
        return "Unknown";
    if (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers)
        return "Loopback";
    if (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed)
        return info.m_idPOPRelay ? "Relay (SDR)" : "Relay";
    return "Direct";
}

const char* SteamLobby_GetLocalUsername()
{
    return g_localUsername;
}

uint64_t SteamLobby_GetPeerSteamId()
{
    return g_peerSteamId;
}

const char* SteamLobby_GetPeerUsername()
{
    return g_peerUsername;
}

bool SteamLobby_GetPeerAvatarRGBA(uint8_t* rgba, size_t capacity,
                                  uint32_t* width, uint32_t* height)
{
    if (!rgba || !width || !height || !g_peerSteamId || !g_friends || !g_utils ||
        !g_api.GetMediumFriendAvatar || !g_api.GetImageSize || !g_api.GetImageRGBA)
        return false;

    const int image = g_api.GetMediumFriendAvatar(g_friends, g_peerSteamId);
    if (image <= 0 || !g_api.GetImageSize(g_utils, image, width, height) ||
        *width == 0 || *height == 0 || *width > 64 || *height > 64)
        return false;

    const size_t bytes = static_cast<size_t>(*width) * *height * 4;
    return bytes <= capacity &&
           g_api.GetImageRGBA(g_utils, image, rgba, static_cast<int>(bytes));
}

bool SteamLobby_OpenPeerProfile()
{
    const uint64_t profileSteamId = g_peerSteamId
        ? g_peerSteamId : g_gamePeerSteamId;
    if (!profileSteamId) return false;

    char profileUrl[96];
    snprintf(profileUrl, sizeof(profileUrl),
             "https://steamcommunity.com/profiles/%llu",
             static_cast<unsigned long long>(profileSteamId));
    return reinterpret_cast<INT_PTR>(ShellExecuteA(
               nullptr, "open", profileUrl, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

bool SteamLobby_HasPeerProfile()
{
    return g_peerSteamId != 0 || g_gamePeerSteamId != 0;
}

bool SteamLobby_HasLobby()
{
    return g_lobbyId != 0;
}

bool SteamLobby_IsCreating()
{
    return g_pendingCall == PendingCall::Create;
}

bool SteamLobby_IsHostedLobby(uint64_t lobbyId)
{
    return g_isHost && lobbyId != 0 && lobbyId == g_lobbyId;
}

bool SteamLobby_IsHost()
{
    return g_isHost && g_lobbyId != 0;
}

bool SteamLobby_GetHostedJoinLink(char* link, size_t capacity)
{
    if (!link || capacity == 0) return false;
    link[0] = '\0';
    if (!g_isHost || !g_lobbyId || !g_matchmaking) return false;
    const uint64_t owner = g_api.GetLobbyOwner(g_matchmaking, g_lobbyId);
    if (!owner) return false;
    const int length = snprintf(link, capacity, "steam://joinlobby/357190/%llu/%llu",
        static_cast<unsigned long long>(g_lobbyId),
        static_cast<unsigned long long>(owner));
    return length > 0 && static_cast<size_t>(length) < capacity;
}

bool SteamLobby_IsFriendLobby(uint64_t lobbyId)
{
    for (size_t i = 0; i < g_friendLobbyCandidateCount; ++i) {
        if (g_friendLobbyCandidates[i] == lobbyId) return true;
    }
    return false;
}

bool SteamLobby_IsLobbyVersionCompatible(uint64_t lobbyId)
{
    for (size_t i = 0; i < g_resultCount; ++i) {
        if (g_results[i] == lobbyId)
            return strcmp(g_resultProtocols[i], kLobbyProtocol) == 0;
    }
    return false;
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

bool SteamLobby_TakeVersionMismatch(char* message, size_t messageSize)
{
    if (!g_versionMismatchPending || !message || messageSize == 0) return false;
    snprintf(message, messageSize, "%s", g_versionMismatch);
    g_versionMismatchPending = false;
    return true;
}

bool SteamLobby_TakeReplacementJoin(uint64_t* lobbyId)
{
    if (!lobbyId) return false;
    *lobbyId = g_replacementJoinLobby.exchange(0, std::memory_order_acq_rel);
    return *lobbyId != 0;
}

bool SteamLobby_ResetHostPeer()
{
    if (!g_isHost || !g_lobbyId) return false;

    g_rejectedPeerSteamId = g_peerSteamId;
    SteamTransport_Shutdown();
    g_connection = k_HSteamNetConnection_Invalid;
    g_gameHost = nullptr;
    g_gamePeer = nullptr;
    g_peerSteamId = 0;
    g_peerUsername[0] = '\0';
    g_handlesReady = false;
    g_bridgeCreated = false;
    g_nextConnectionAttempt = 0;
    g_connectionDeadline = 0;
    g_connectionAttempt = 0;
    g_api.SetLobbyJoinable(g_matchmaking, g_lobbyId, true);
    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "state", "waiting");
    PublishLobbyRichPresence();
    SetStatus("Rejected incompatible player; waiting for opponent");
    return true;
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
    if (preserveConnection) {
        g_gamePeerSteamId = g_peerSteamId;
        HookLog("SteamLobby: transferred live connection to gameplay\n");
        return;
    }
    bool leftLobby = g_lobbyId != 0;
    StopCallbackPump();
    UnregisterJoinCallbacks();
    bool bridgeActive = SteamTransport_IsActive();
    SteamTransport_Shutdown();
    if (!bridgeActive && g_sockets &&
        g_connection != k_HSteamNetConnection_Invalid) {
        g_sockets->CloseConnection(g_connection, 1000,
                                   "Timestone shutdown", false);
    }
    if (g_sockets && g_listenSocket != k_HSteamListenSocket_Invalid)
        g_sockets->CloseListenSocket(g_listenSocket);
    g_listenSocket = k_HSteamListenSocket_Invalid;
    g_connection = k_HSteamNetConnection_Invalid;
    ClearLobbyRichPresence();
    if (g_matchmaking && g_lobbyId)
        g_api.LeaveLobby(g_matchmaking, g_lobbyId);
    g_pendingJoinLobby.store(0, std::memory_order_release);
    g_replacementJoinLobby.store(0, std::memory_order_release);
    g_pendingCall = PendingCall::None;
    g_pendingHandle = 0;
    g_lobbyId = 0;
    g_isHost = false;
    g_gameHost = nullptr;
    g_gamePeer = nullptr;
    g_peerSteamId = 0;
    g_rejectedPeerSteamId = 0;
    g_peerUsername[0] = '\0';
    g_versionMismatch[0] = '\0';
    g_versionMismatchPending = false;
    g_handlesReady = false;
    g_bridgeCreated = false;
    g_pingLocationPublished = false;
    g_nextPingLocationAttempt = 0;
    g_nextConnectionAttempt = 0;
    g_connectionDeadline = 0;
    g_connectionAttempt = 0;
    g_nextResultPingRefresh = 0;
    g_resultPingRefreshDeadline = 0;
    if (leftLobby) SetStatus("Left Steam lobby");
}
