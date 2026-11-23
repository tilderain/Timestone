#include "steam_lobby.h"
#include "lobby_local.h"

#include "steam_api_compat.h"
#include "steam_transport.h"
#include "lag_diagnostics.h"
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
constexpr ULONGLONG kMatchAnswerWaitMs = 4000; // player 1 waits this long to be called
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
    void (*ActivateOverlayToUser)(void*, const char*, uint64_t) = nullptr;
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
    bool (*SetLobbyType)(void*, uint64_t, int) = nullptr; // optional
    // Room mode (in-game lobby): per-member state and lobby chat.
    void (*SetLobbyMemberData)(void*, uint64_t, const char*, const char*) = nullptr;
    const char* (*GetLobbyMemberData)(void*, uint64_t, uint64_t, const char*) = nullptr;
    bool (*SendLobbyChatMsg)(void*, uint64_t, const void*, int) = nullptr;
    int (*GetLobbyChatEntry)(void*, uint64_t, int, uint64_t*, void*, int, int*) = nullptr;
    int (*GetLobbyMemberLimit)(void*, uint64_t) = nullptr;
    void* (*GetUserInterface)(void*, HSteamUser, HSteamPipe, const char*) = nullptr;
    uint64_t (*GetSteamID)(void*) = nullptr;
    void (*ActivateInviteDialog)(void*, uint64_t) = nullptr;
    bool (*IsOverlayEnabled)(void*) = nullptr;
    bool (*SetRichPresence)(void*, const char*, const char*) = nullptr;
    void (*SetPlayedWith)(void*, uint64_t) = nullptr;

    const char* (*GetPersonaName)(void*) = nullptr;
    const char* (*GetFriendPersonaName)(void*, uint64_t) = nullptr;
    int (*GetMediumFriendAvatar)(void*, uint64_t) = nullptr;
    bool (*RequestUserInformation)(void*, uint64_t, bool) = nullptr;
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
int g_lastJoinRefusal = 0; // EChatRoomEnterResponse of the last refused join
uint64_t g_lobbyId = 0;
bool g_isHost = false;
// Room mode: the lobby is a multi-player room. Membership no longer implies a
// peer connection; one is opened on request between the two seated players.
bool g_roomMode = false;
int g_roomCreateSlots = 2;
bool g_roomCreatePrivate = false;
int g_roomCreateCapacity = 8;
// Steam never lists a lobby whose member limit is reached, so a room's Steam
// limit sits one above the size its host chose and the real size travels in
// the lobby data. A full room stays in everyone's list; whoever enters past
// the real size leaves again by itself.
constexpr int kRoomListingHeadroom = 1;
constexpr int kRoomMaxCapacity = 16; // lobby::kMaxMembers
constexpr const char* kRoomCapacityKey = "capacity";
uint64_t g_roomPeer = 0;
bool g_roomPeerFailed = false;
uint64_t g_localSteamId = 0;
std::atomic<uint32_t> g_chatHead{0}, g_chatTail{0};
int g_chatIds[64] = {};
bool g_roomCallbacksRegistered = false;
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
        // The text is whatever the other player's game published: never
        // read past the field, whether or not it is terminated.
        char connect[sizeof(request->m_rgchConnect) + 1];
        memcpy(connect, request->m_rgchConnect, sizeof(request->m_rgchConnect));
        connect[sizeof(request->m_rgchConnect)] = '\0';
        uint64_t lobbyId = 0;
        if (ParseLobbyConnectString(connect, lobbyId))
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
// Lobby chat arrives as a callback carrying only an entry id. The callback can
// run on the worker pump thread, so it just queues the id for the game thread.
class LobbyChatMsgCallback final : public SteamCallbackBase {
public:
    LobbyChatMsgCallback() { m_iCallback = k_iCallbackLobbyChatMsg; }

    void Run(void* param) override
    {
        if (!param) return;
        auto* message = static_cast<LobbyChatMsgCompat_t*>(param);
        if (message->m_ulSteamIDLobby != g_lobbyId) return;
        const uint32_t head = g_chatHead.load(std::memory_order_relaxed);
        if (head - g_chatTail.load(std::memory_order_acquire) >= 64) return;
        g_chatIds[head % 64] = static_cast<int>(message->m_iChatID);
        g_chatHead.store(head + 1, std::memory_order_release);
    }

    void Run(void* param, bool, SteamAPICall_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return sizeof(LobbyChatMsgCompat_t); }
};

LobbyChatMsgCallback g_lobbyChatMsgCallback;

// Connection state changes. Two uses: every transition of a peer-to-peer
// connection goes in the log (the failing end of a connection otherwise only
// ever reports a timeout), and a request that arrives on one of our listen
// sockets before we have dialled that player is kept, to be accepted when we
// would otherwise dial. Runs on whichever thread pumps Steam's callbacks.
struct IncomingConnection {
    HSteamNetConnection connection;
    HSteamListenSocket listenSocket;
    uint64_t remoteId;
    ULONGLONG tick;
};
std::atomic<uint32_t> g_incomingHead{0}, g_incomingTail{0};
IncomingConnection g_incomingQueue[16] = {};
IncomingConnection g_incomingPending[8] = {}; // game thread
bool g_connectionCallbackRegistered = false;

class ConnectionStatusCallback final : public SteamCallbackBase {
public:
    ConnectionStatusCallback() { m_iCallback = k_iCallbackSteamNetConnectionStatusChanged; }

    void Run(void* param) override
    {
        if (!param) return;
        const auto* change = static_cast<const SteamNetConnectionStatusChangedCompat_t*>(param);
        const SteamNetConnectionInfo_t& info = change->m_info;
        const uint64_t remote = info.m_identityRemote.m_eType == k_ESteamNetworkingIdentityType_SteamID
            ? info.m_identityRemote.data.m_steamID64 : 0;
        HookLog("SteamLobby: connection #%u %d -> %d remote=%llu listen=%u reason=%d \"%s\" (%s)\n",
                change->m_hConn, static_cast<int>(change->m_eOldState),
                static_cast<int>(info.m_eState), static_cast<unsigned long long>(remote),
                info.m_hListenSocket, info.m_eEndReason, info.m_szEndDebug,
                info.m_szConnectionDescription);
        if (change->m_eOldState != k_ESteamNetworkingConnectionState_None ||
            info.m_eState != k_ESteamNetworkingConnectionState_Connecting ||
            info.m_hListenSocket == k_HSteamListenSocket_Invalid || !remote)
            return;
        const uint32_t head = g_incomingHead.load(std::memory_order_relaxed);
        if (head - g_incomingTail.load(std::memory_order_acquire) >= 16) return;
        g_incomingQueue[head % 16] = {change->m_hConn, info.m_hListenSocket, remote, GetTickCount64()};
        g_incomingHead.store(head + 1, std::memory_order_release);
    }

    void Run(void* param, bool, SteamAPICall_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return sizeof(SteamNetConnectionStatusChangedCompat_t); }
};

ConnectionStatusCallback g_connectionStatusCallback;

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
    SteamLobby_FormatVersionMismatch(remoteProtocol, g_versionMismatch, sizeof(g_versionMismatch));
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
    ResolveOptional(g_api.SetLobbyMemberData, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData");
    ResolveOptional(g_api.GetLobbyMemberData, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData");
    ResolveOptional(g_api.SendLobbyChatMsg, "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg");
    ResolveOptional(g_api.SetLobbyType, "SteamAPI_ISteamMatchmaking_SetLobbyType");
    ResolveOptional(g_api.GetLobbyChatEntry, "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry");
    ResolveOptional(g_api.GetLobbyMemberLimit, "SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit");
    ResolveOptional(g_api.GetUserInterface, "SteamAPI_ISteamClient_GetISteamUser");
    ResolveOptional(g_api.GetSteamID, "SteamAPI_ISteamUser_GetSteamID");
    ResolveOptional(g_api.ActivateInviteDialog,
                    "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog");
    ResolveOptional(g_api.IsOverlayEnabled, "SteamAPI_ISteamUtils_IsOverlayEnabled");
    ResolveOptional(g_api.SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence");
    ResolveOptional(g_api.ActivateOverlayToUser,
                    "SteamAPI_ISteamFriends_ActivateGameOverlayToUser");
    ResolveOptional(g_api.SetPlayedWith, "SteamAPI_ISteamFriends_SetPlayedWith");
    ok &= Resolve(g_api.GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName");
    ok &= Resolve(g_api.GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName");
    ResolveOptional(g_api.GetMediumFriendAvatar,
                    "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    ResolveOptional(g_api.RequestUserInformation,
                    "SteamAPI_ISteamFriends_RequestUserInformation");
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
    if (g_roomMode ? !g_lobbyId : (!g_isHost || !g_lobbyId || g_bridgeCreated)) {
        ClearLobbyRichPresence();
        return;
    }

    char connect[64];
    snprintf(connect, sizeof(connect), "+connect_lobby %llu",
             static_cast<unsigned long long>(g_lobbyId));
    SetRichPresence("status", g_roomMode ? "In a Timestone lobby"
                                         : "Waiting in Timestone launch menu");
    // A private room is reached by invitation only: no "Join Game" on the
    // friends list for it. An invitation carries the lobby by itself.
    const char* isPrivate = g_roomMode ? g_api.GetLobbyData(g_matchmaking, g_lobbyId, "private")
                                       : nullptr;
    SetRichPresence("connect", isPrivate && isPrivate[0] == '1' ? "" : connect);
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
    // No handicap on the direct route: ICE and SDR compete on their own scores.
    options[3] = {};
    options[3].m_eValue = k_ESteamNetworkingConfig_P2P_Transport_ICE_Penalty;
    options[3].m_eDataType = k_ESteamNetworkingConfig_Int32;
    options[3].m_val.m_int32 = 0;
}

void RefreshPeerUsername()
{
    const char* username = g_friends && g_peerSteamId
        ? g_api.GetFriendPersonaName(g_friends, g_peerSteamId)
        : nullptr;
    snprintf(g_peerUsername, sizeof(g_peerUsername), "%s",
             username && username[0] ? username : "Unknown");
}

// Game thread: requests that arrived on our listen sockets and have not been
// taken up. One nobody wants is closed after a while.
void DrainIncomingConnections()
{
    const ULONGLONG now = GetTickCount64();
    uint32_t tail = g_incomingTail.load(std::memory_order_relaxed);
    while (tail != g_incomingHead.load(std::memory_order_acquire)) {
        const IncomingConnection incoming = g_incomingQueue[tail % 16];
        g_incomingTail.store(++tail, std::memory_order_release);
        for (IncomingConnection& slot : g_incomingPending) {
            if (slot.connection != k_HSteamNetConnection_Invalid) continue;
            slot = incoming;
            break;
        }
    }
    for (IncomingConnection& slot : g_incomingPending) {
        if (slot.connection == k_HSteamNetConnection_Invalid || now - slot.tick < 20000) continue;
        if (g_sockets) g_sockets->CloseConnection(slot.connection, 1000, "Timestone unanswered", false);
        slot = {};
    }
}

// A waiting request from `remoteId` on `listenSocket`, accepted; or invalid.
HSteamNetConnection AcceptIncomingConnection(uint64_t remoteId, HSteamListenSocket listenSocket)
{
    DrainIncomingConnections();
    for (IncomingConnection& slot : g_incomingPending) {
        if (slot.connection == k_HSteamNetConnection_Invalid || slot.remoteId != remoteId ||
            slot.listenSocket != listenSocket)
            continue;
        const HSteamNetConnection connection = slot.connection;
        slot = {};
        const EResult result = g_sockets->AcceptConnection(connection);
        HookLog("SteamLobby: accepted waiting connection #%u from %llu (result %d)\n", connection,
                static_cast<unsigned long long>(remoteId), static_cast<int>(result));
        if (result == k_EResultOK) return connection;
        g_sockets->CloseConnection(connection, 1000, "Timestone accept failed", false);
    }
    return k_HSteamNetConnection_Invalid;
}

void EnsureMatchListenSocket()
{
    if (g_listenSocket != k_HSteamListenSocket_Invalid || !g_sockets) return;
    SteamNetworkingConfigValue_t options[4];
    ConnectionOptions(options);
    HookLog("SteamLobby: routing policy=%s ICE candidates=%d\n",
            options[1].m_val.m_int32 ? "Automatic" : "Relay Only", options[1].m_val.m_int32);
    g_listenSocket = g_sockets->CreateListenSocketP2P(733, 4, options);
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
    EnsureMatchListenSocket();
    if (g_listenSocket == k_HSteamListenSocket_Invalid) {
        SetStatus("Failed to create Steam P2P listen socket");
        return;
    }
    // The other player may have dialled first: take that connection up
    // instead of starting a second one.
    g_connection = AcceptIncomingConnection(remoteId, g_listenSocket);
    if (g_connection != k_HSteamNetConnection_Invalid) {
        ++g_connectionAttempt;
        SetStatus("Establishing Steam connection...");
        return;
    }
    // In a room only player 2 calls; player 1 answers. Two attempts meeting
    // half-way is what Steam's P2P code crashed on (a connection picked for
    // a ping before its handshake was done). If no call has come after a few
    // seconds player 1 dials after all, as both always did before.
    if (g_roomMode && g_isHost &&
        now + kPeerConnectionTimeoutMs < g_connectionDeadline + kMatchAnswerWaitMs)
        return;

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

void EnsureSpecListenSocket();

void PumpLobbyMembers()
{
    if (g_roomMode) {
        if (g_lobbyId) {
            EnsureMatchListenSocket();
            EnsureSpecListenSocket();
            DrainIncomingConnections();
        }
        // Rooms connect on demand (SteamRoom_ConnectPeer), never on membership.
        if (g_lobbyId && g_roomPeer && !g_bridgeCreated &&
            g_connection == k_HSteamNetConnection_Invalid)
            StartPeerConnection(g_roomPeer);
        return;
    }
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

// A room outlives its creator: Steam hands the lobby to another member, and
// only the owner can write lobby data. The browser shows the host from the
// "username" attribute and estimates ping from "location", both written when
// the room was made, so the member who becomes the owner rewrites them.
void PumpRoomOwner()
{
    static uint64_t trackedLobby = 0;
    static uint64_t trackedOwner = 0;
    if (!g_roomMode || !g_lobbyId || !g_matchmaking) {
        trackedLobby = 0;
        trackedOwner = 0;
        return;
    }
    const uint64_t owner = g_api.GetLobbyOwner(g_matchmaking, g_lobbyId);
    if (!owner) return;
    if (trackedLobby != g_lobbyId) {
        // Just entered: whoever owns it now has already published itself.
        trackedLobby = g_lobbyId;
        trackedOwner = owner;
        return;
    }
    if (owner == trackedOwner) return;
    trackedOwner = owner;
    if (owner != g_localSteamId) return;

    const char* persona = g_friends ? g_api.GetPersonaName(g_friends) : nullptr;
    const char* username = persona && persona[0] ? persona : g_localUsername;
    if (!username[0]) return;
    // A room that was never named carries "<host>'s Lobby"; keep that true.
    char previous[128] = {};
    char name[128] = {};
    const char* value = g_api.GetLobbyData(g_matchmaking, g_lobbyId, "username");
    snprintf(previous, sizeof(previous), "%s", value ? value : "");
    value = g_api.GetLobbyData(g_matchmaking, g_lobbyId, "name");
    snprintf(name, sizeof(name), "%s", value ? value : "");
    char fallback[160];
    snprintf(fallback, sizeof(fallback), "%s's Lobby", previous);
    if (!name[0] || (previous[0] && !strcmp(name, fallback))) {
        snprintf(fallback, sizeof(fallback), "%.110s's Lobby", username);
        g_api.SetLobbyData(g_matchmaking, g_lobbyId, "name", fallback);
    }
    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "username", username);
    // The lobby's ping location is the owner's; publish this member's again.
    g_pingLocationPublished = false;
    g_nextPingLocationAttempt = 0;
    HookLog("SteamLobby: now the lobby host; host name updated\n");
}

void PumpPingLocation()
{
    if (!g_lobbyId || g_pingLocationPublished || !g_networkingUtils)
        return;
    if (!g_roomMode && !g_isHost) return;

    ULONGLONG now = GetTickCount64();
    if (now < g_nextPingLocationAttempt) return;
    g_nextPingLocationAttempt = now + 1000;

    SteamNetworkPingLocation_t location = {};
    if (g_networkingUtils->GetLocalPingLocation(location) < 0.0f) return;

    char encoded[k_cchMaxSteamNetworkingPingLocationString] = {};
    g_networkingUtils->ConvertPingLocationToString(location, encoded, sizeof(encoded));
    if (!encoded[0]) return;
    if (g_roomMode) {
        // Every room member publishes its own location so the member list can
        // show a connection estimate; the owner's also feeds the lobby browser.
        if (!g_api.SetLobbyMemberData) return;
        g_api.SetLobbyMemberData(g_matchmaking, g_lobbyId, "loc", encoded);
        if (g_api.GetLobbyOwner(g_matchmaking, g_lobbyId) == g_localSteamId)
            g_api.SetLobbyData(g_matchmaking, g_lobbyId, "location", encoded);
    } else if (!g_api.SetLobbyData(g_matchmaking, g_lobbyId, "location", encoded)) {
        return;
    }

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

// The room just left, while it still had others in it. Steam goes on counting
// us as a member for a moment after LeaveLobby, so a search started right
// away sees a room we filled the last seat of as full and leaves it out. That
// one search is given the room back.
uint64_t g_leftRoom = 0;
ULONGLONG g_leftRoomTick = 0;
constexpr ULONGLONG kLeftRoomWindowMs = 5000;

void RememberLeftRoom(uint64_t lobbyId)
{
    g_leftRoom = 0;
    if (!g_roomMode || !g_matchmaking || !lobbyId) return;
    if (g_api.GetNumLobbyMembers(g_matchmaking, lobbyId) < 2) return; // it closes with us
    g_leftRoom = lobbyId;
    g_leftRoomTick = GetTickCount64();
}

// The size a room's host chose, or 0 when the lobby does not publish one.
int RoomCapacity(uint64_t lobbyId)
{
    if (!g_roomMode || !g_matchmaking || !lobbyId) return 0;
    const char* value = g_api.GetLobbyData(g_matchmaking, lobbyId, kRoomCapacityKey);
    const int capacity = value ? atoi(value) : 0;
    return capacity >= 2 && capacity <= kRoomMaxCapacity ? capacity : 0;
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

        const char* marker = g_api.GetLobbyData(g_matchmaking, lobbyId,
                                                g_roomMode ? "TimestoneRoom" : "Timestone");
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

// Hands the transport Steam's relay-route estimate to the connected player,
// for the netplay log. The peer's location comes from lobby data, so this
// stays on the thread that pumps the lobby; a room member publishes its own,
// and a plain lobby carries only the host's.
ULONGLONG g_nextRelayEstimateTick = 0;

void RefreshRelayPingEstimate(bool force)
{
    const ULONGLONG now = GetTickCount64();
    if (!force && now < g_nextRelayEstimateTick) return;
    g_nextRelayEstimateTick = now + 5000;
    int estimate = -1;
    if (SteamRoom_IsActive()) estimate = SteamRoom_EstimateMemberPing(g_peerSteamId);
    else if (!g_isHost) estimate = EstimateLobbyPing(g_lobbyId);
    // Keep the last good figure if the lobby data is momentarily unreadable.
    if (estimate >= 0) SteamTransport_SetRelayPingEstimate(estimate);
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
        RefreshRelayPingEstimate(false);
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
        if (g_roomMode) {
            // A failed match connection must not cost the player their room.
            g_roomPeer = 0;
            g_roomPeerFailed = true;
            g_peerSteamId = 0;
            g_peerUsername[0] = '\0';
            g_nextConnectionAttempt = 0;
            g_connectionDeadline = 0;
            g_connectionAttempt = 0;
            SetStatus("Could not connect to the other player");
            return;
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
        RefreshRelayPingEstimate(true);
        if (!g_roomMode) {
            g_api.SetLobbyJoinable(g_matchmaking, g_lobbyId, false);
            ClearLobbyRichPresence();
        }
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
    if (LocalLobby_Enabled()) return LocalLobby_Initialize();
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
    if (LocalLobby_Enabled()) { LocalLobby_Search(); return; }
    if (!SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    // Rooms advertise under their own key so 1v1 launcher clients never see
    // (and auto-connect into) a multi-player room, and vice versa.
    g_api.AddStringFilter(g_matchmaking, g_roomMode ? "TimestoneRoom" : "Timestone", "1", 0);
    if (!g_roomMode) g_api.AddSlotsFilter(g_matchmaking, 1);
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
    if (LocalLobby_Enabled()) { LocalLobby_Join(lobbyId); return; }
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
    g_lastJoinRefusal = 0;
    g_pendingHandle = g_api.JoinLobby(g_matchmaking, lobbyId);
    g_pendingCall = PendingCall::Join;
    g_isHost = false;
    SetStatus("Joining lobby...");
}

void SteamLobby_Pump()
{
    if (LocalLobby_Enabled()) { LocalLobby_Pump(); return; }
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
            if (g_leftRoom && GetTickCount64() - g_leftRoomTick < kLeftRoomWindowMs &&
                !HasResult(g_leftRoom)) {
                const char* marker = g_api.GetLobbyData(g_matchmaking, g_leftRoom, "TimestoneRoom");
                if (marker && strcmp(marker, "1") == 0) {
                    g_api.RequestLobbyData(g_matchmaking, g_leftRoom);
                    AddResult(g_leftRoom);
                    HookLog("SteamLobby: kept lobby %llu, left a moment ago, in the results\n",
                            static_cast<unsigned long long>(g_leftRoom));
                }
            }
            g_leftRoom = 0;
            CollectFriendLobbyCandidates();
            MergeFriendLobbyCandidates();
            g_nextResultPingRefresh = 0;
            g_resultPingRefreshDeadline = GetTickCount64() + 15000;
            g_resultsChanged = true;
            g_pendingCall = PendingCall::None;
            g_pendingHandle = 0;
            UpdateSearchStatus();
            HookLog("SteamLobby: search returned %u public lobbies; %u listed with friends'\n",
                    static_cast<unsigned>(result.m_nLobbiesMatching),
                    static_cast<unsigned>(g_resultCount));
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
                g_api.SetLobbyData(g_matchmaking, g_lobbyId,
                                   g_roomMode ? "TimestoneRoom" : "Timestone", "1");
                if (g_roomMode) {
                    char slots[8];
                    snprintf(slots, sizeof(slots), "%d", g_roomCreateSlots);
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "slots", slots);
                    char capacity[8];
                    snprintf(capacity, sizeof(capacity), "%d", g_roomCreateCapacity);
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, kRoomCapacityKey, capacity);
                    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "private",
                                       g_roomCreatePrivate ? "1" : "0");
                    // Fallback identity when ISteamUser is unavailable: the
                    // creator of a lobby is its first owner.
                    if (!g_localSteamId)
                        g_localSteamId = g_api.GetLobbyOwner(g_matchmaking, g_lobbyId);
                }
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
                if (g_roomMode)
                    SetStatus("Lobby created");
                else
                    SetStatusf("Lobby %llu created; waiting for opponent", g_lobbyId);
            }
        }
    } else if (g_pendingCall == PendingCall::Join) {
        LobbyEnter_t result = {};
        if (TakeCallResult(result, k_iCallbackLobbyEnter)) {
            g_pendingCall = PendingCall::None;
            g_pendingHandle = 0;
            if (result.m_EChatRoomEnterResponse != 1 || !result.m_ulSteamIDLobby) {
                // EChatRoomEnterResponse: 2 gone, 3 not allowed, 4 full, 6 banned,
                // 7 limited account, 10/11 blocked by or blocking a member.
                g_lastJoinRefusal = result.m_EChatRoomEnterResponse == 1
                    ? 2 : static_cast<int>(result.m_EChatRoomEnterResponse);
                HookLog("SteamLobby: lobby %llu refused the join (response %u)\n",
                        static_cast<unsigned long long>(result.m_ulSteamIDLobby),
                        static_cast<unsigned>(result.m_EChatRoomEnterResponse));
                SetStatus("Could not enter Steam lobby");
                SteamLobby_Search();
            } else {
                const char* marker = g_api.GetLobbyData(
                    g_matchmaking, result.m_ulSteamIDLobby,
                    g_roomMode ? "TimestoneRoom" : "Timestone");
                const char* protocol = g_api.GetLobbyData(
                    g_matchmaking, result.m_ulSteamIDLobby, "protocol");
                if (!marker || strcmp(marker, "1") != 0 || !protocol ||
                    strcmp(protocol, kLobbyProtocol) != 0) {
                    SetVersionMismatch(protocol);
                    g_api.LeaveLobby(g_matchmaking, result.m_ulSteamIDLobby);
                    SteamLobby_Search();
                    return;
                }
                // Steam let us in on the listing headroom; the room itself is
                // full, which reads to the player like Steam's own refusal.
                const int capacity = RoomCapacity(result.m_ulSteamIDLobby);
                const int members = g_api.GetNumLobbyMembers(g_matchmaking,
                                                             result.m_ulSteamIDLobby);
                if (capacity && members > capacity) {
                    HookLog("SteamLobby: lobby %llu is full (%d members, room for %d)\n",
                            static_cast<unsigned long long>(result.m_ulSteamIDLobby),
                            members, capacity);
                    g_lastJoinRefusal = 4;
                    RememberLeftRoom(result.m_ulSteamIDLobby);
                    g_api.LeaveLobby(g_matchmaking, result.m_ulSteamIDLobby);
                    SetStatus("Could not enter Steam lobby");
                    SteamLobby_Search();
                    return;
                }
                g_lobbyId = result.m_ulSteamIDLobby;
                g_isHost = false;
                g_pingLocationPublished = false;
                g_nextPingLocationAttempt = 0;
                ClearLobbyRichPresence();
                g_nextConnectionAttempt = 0;
                g_connectionDeadline = g_roomMode ? 0 : GetTickCount64() + kPeerConnectionTimeoutMs;
                g_connectionAttempt = 0;
                SetStatus(g_roomMode ? "Lobby joined" : "Lobby joined; connecting to owner...");
                if (g_roomMode) PublishLobbyRichPresence();
            }
        } else if (g_pendingCall == PendingCall::None) {
            // TakeCallResult clears the pending call when the Steam request fails.
            SteamLobby_Search();
        }
    }

    PumpQueuedJoinRequest();

    PumpRoomOwner();
    PumpPingLocation();
    RefreshResultPings();
    PumpLobbyMembers();
    PumpConnection();
}

const char* SteamLobby_GetStatus()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetStatus();
    return g_status;
}

const char* SteamLobby_GetConnectionType()
{
    LagDiagnosticScope diagnostic(LagMetric::SteamRoute);
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
    if (LocalLobby_Enabled()) return LocalLobby_GetLocalUsername();
    return g_localUsername;
}

uint64_t SteamLobby_GetPeerSteamId()
{
    return g_peerSteamId;
}

const char* SteamLobby_GetPeerUsername()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetPeerUsername();
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

// A member's 64x64 Steam profile picture as raw RGBA. Steam decodes the
// image itself and hands back plain pixels; anything that is not exactly
// 64x64 is refused, so the caller's fixed 64*64*4 buffer is always filled
// exactly and never overrun. False until Steam has the picture cached.
bool SteamLobby_GetAvatar64(uint64_t steamId, uint8_t* rgba)
{
    if (LocalLobby_Enabled() || !rgba || !steamId || !g_friends || !g_utils ||
        !g_api.GetMediumFriendAvatar || !g_api.GetImageSize || !g_api.GetImageRGBA)
        return false;
    const int image = g_api.GetMediumFriendAvatar(g_friends, steamId);
    if (image <= 0) {
        // Someone Steam has not seen yet (the host of a lobby in the list):
        // ask it to fetch their name and picture; a later call finds it.
        if (g_api.RequestUserInformation)
            g_api.RequestUserInformation(g_friends, steamId, false);
        return false;
    }
    uint32_t width = 0, height = 0;
    if (!g_api.GetImageSize(g_utils, image, &width, &height) || width != 64 || height != 64)
        return false;
    return g_api.GetImageRGBA(g_utils, image, rgba, 64 * 64 * 4);
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
    if (LocalLobby_Enabled()) return false;
    for (size_t i = 0; i < g_friendLobbyCandidateCount; ++i) {
        if (g_friendLobbyCandidates[i] == lobbyId) return true;
    }
    return false;
}

bool SteamLobby_IsLobbyVersionCompatible(uint64_t lobbyId)
{
    if (LocalLobby_Enabled()) return true;
    for (size_t i = 0; i < g_resultCount; ++i) {
        if (g_results[i] == lobbyId)
            return strcmp(g_resultProtocols[i], kLobbyProtocol) == 0;
    }
    return false;
}

size_t SteamLobby_GetResultCount()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetResultCount();
    return g_resultCount;
}

uint64_t SteamLobby_GetResult(size_t index)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetResult();
    return index < g_resultCount ? g_results[index] : 0;
}

int SteamLobby_GetResultEstimatedPing(size_t index)
{
    if (LocalLobby_Enabled()) return 2;
    return index < g_resultCount ? g_resultPings[index] : -1;
}

const char* SteamLobby_GetResultName(size_t index)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetResultName();
    return index < g_resultCount ? g_resultNames[index] : "";
}

const char* SteamLobby_GetResultUsername(size_t index)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetResultUsername();
    return index < g_resultCount ? g_resultUsernames[index] : "";
}

int SteamLobby_FormatVersionMismatch(const char* lobbyProtocol, char* out, size_t outSize)
{
    if (!out || !outSize) return 0;
    // The lobby's version is its host's text: only digits and dots are shown.
    char theirs[16] = {};
    size_t length = 0;
    for (const char* c = lobbyProtocol ? lobbyProtocol : ""; *c && length + 1 < sizeof(theirs); ++c)
        if ((*c >= '0' && *c <= '9') || *c == '.') theirs[length++] = *c;
    unsigned a[3] = {}, b[3] = {};
    int order = 0; // the lobby's version against ours
    if (sscanf(theirs, "%u.%u.%u", &a[0], &a[1], &a[2]) == 3 &&
        sscanf(kLobbyProtocol, "%u.%u.%u", &b[0], &b[1], &b[2]) == 3) {
        for (int part = 0; part < 3 && !order; ++part)
            order = a[part] < b[part] ? -1 : a[part] > b[part] ? 1 : 0;
    }
    // Which side is behind decides who has to update.
    snprintf(out, outSize, "This lobby is running %s Timestone version.\nLobby: %s    Yours: %s%s",
             order < 0 ? "an older" : order > 0 ? "a newer" : "a different",
             theirs[0] ? theirs : "unknown", kLobbyProtocol,
             order < 0 ? "\nThe host needs to update."
                       : order > 0 ? "\nUpdate Timestone to join." : "");
    return order;
}

const char* SteamLobby_GetResultProtocol(size_t index)
{
    if (LocalLobby_Enabled()) return kLobbyProtocol;
    return index < g_resultCount ? g_resultProtocols[index] : "";
}

bool SteamLobby_ArePingEstimatesAvailable()
{
    return g_networkingUtils != nullptr;
}

bool SteamLobby_TakeResultsChanged()
{
    if (LocalLobby_Enabled()) return LocalLobby_TakeResultsChanged();
    bool changed = g_resultsChanged;
    g_resultsChanged = false;
    return changed;
}

bool SteamLobby_TakeVersionMismatch(char* message, size_t messageSize)
{
    if (LocalLobby_Enabled()) return false;
    if (!g_versionMismatchPending || !message || messageSize == 0) return false;
    snprintf(message, messageSize, "%s", g_versionMismatch);
    g_versionMismatchPending = false;
    return true;
}

bool SteamLobby_TakeReplacementJoin(uint64_t* lobbyId)
{
    if (LocalLobby_Enabled()) return false;
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
    if (LocalLobby_Enabled()) return LocalLobby_TakeNetworkHandles(outHost, outPeer, outIsHost);
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
    g_roomPeer = 0;
    g_roomPeerFailed = false;
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

// ---------------------------------------------------------------------------
// Session end and search-result details
// ---------------------------------------------------------------------------
void SteamLobby_EndSession()
{
    if (LocalLobby_Enabled()) { LocalLobby_DisconnectPeer(); return; }
    // Room mode never shuts Steam down, in a room or out of one: that would
    // also stop listening for invitations until the next search or join.
    if (g_roomMode)
        SteamRoom_DisconnectPeer();
    else
        SteamLobby_Shutdown(false);
}

int SteamLobby_GetLastJoinRefusal() { return g_lastJoinRefusal; }

int SteamLobby_GetPendingKind()
{
    if (LocalLobby_Enabled()) return 0;
    return static_cast<int>(g_pendingCall);
}

int SteamLobby_GetResultMemberCount(size_t index)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetResultMemberCount();
    if (index >= g_resultCount || !g_matchmaking) return 0;
    return g_api.GetNumLobbyMembers(g_matchmaking, g_results[index]);
}

int SteamLobby_GetResultMemberLimit(size_t index)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetResultMemberLimit();
    if (index >= g_resultCount || !g_matchmaking) return 0;
    if (const int capacity = RoomCapacity(g_results[index])) return capacity;
    if (!g_api.GetLobbyMemberLimit) return 0;
    return g_api.GetLobbyMemberLimit(g_matchmaking, g_results[index]);
}

int SteamLobby_GetResultSlotCount(size_t index)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetResultSlotCount();
    if (index >= g_resultCount || !g_matchmaking) return 0;
    const char* slots = g_api.GetLobbyData(g_matchmaking, g_results[index], "slots");
    return slots ? atoi(slots) : 0;
}

bool SteamLobby_IsResultInMatch(size_t index)
{
    if (LocalLobby_Enabled()) return LocalLobby_IsResultInMatch();
    if (index >= g_resultCount || !g_matchmaking) return false;
    const char* live = g_api.GetLobbyData(g_matchmaking, g_results[index], "live");
    return live && live[0] == '1';
}

// ---------------------------------------------------------------------------
// Room mode
// ---------------------------------------------------------------------------
void SteamRoom_SetMode(bool enabled)
{
    if (LocalLobby_Enabled()) return;
    g_roomMode = enabled;
    if (enabled && g_api.RegisterCallback && !g_roomCallbacksRegistered) {
        g_api.RegisterCallback(&g_lobbyChatMsgCallback, k_iCallbackLobbyChatMsg);
        g_api.RegisterCallback(&g_connectionStatusCallback,
                               k_iCallbackSteamNetConnectionStatusChanged);
        g_roomCallbacksRegistered = true;
    }
    if (enabled && !g_localSteamId && g_api.GetUserInterface && g_api.GetSteamID &&
        g_api.GetClient) {
        static const char* userVersions[] = {"SteamUser019", "SteamUser020", "SteamUser021",
                                             "SteamUser018", "SteamUser017"};
        for (const char* version : userVersions) {
            void* user = g_api.GetUserInterface(g_api.GetClient(), g_api.GetUser(),
                                                g_api.GetPipe(), version);
            if (!user) continue;
            g_localSteamId = g_api.GetSteamID(user);
            break;
        }
        HookLog("SteamLobby: room mode local SteamID %llu\n",
                static_cast<unsigned long long>(g_localSteamId));
    }
}

bool SteamRoom_IsActive()
{
    if (LocalLobby_Enabled()) return LocalLobby_IsActive();
    return g_roomMode && g_lobbyId != 0;
}

uint64_t SteamRoom_GetLobbyId()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetLobbyId();
    return g_roomMode ? g_lobbyId : 0;
}

uint64_t SteamRoom_GetLocalId()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetLocalId();
    return g_localSteamId;
}

uint64_t SteamRoom_GetOwnerId()
{
    if (LocalLobby_Enabled() || !SteamRoom_IsActive() || !g_matchmaking ||
        !g_api.GetLobbyOwner)
        return 0;
    return g_api.GetLobbyOwner(g_matchmaking, g_lobbyId);
}

void SteamRoom_Create(SteamLobbyType type, int maxMembers, int slotCount, const char* name)
{
    if (LocalLobby_Enabled()) { LocalLobby_Create(maxMembers, slotCount, name); return; }
    if (!g_roomMode || !SteamLobby_Initialize() || g_pendingCall != PendingCall::None) return;
    if (g_lobbyId) {
        SetStatus("Already in a Steam lobby");
        return;
    }
    if (maxMembers < 2) maxMembers = 2;
    if (maxMembers > kRoomMaxCapacity) maxMembers = kRoomMaxCapacity;
    g_roomCreateSlots = slotCount < 1 ? 1 : slotCount;
    g_roomCreatePrivate = type == SteamLobbyType::Private;
    snprintf(g_lobbyName, sizeof(g_lobbyName), "%s", name ? name : "");
    g_roomCreateCapacity = maxMembers;
    g_pendingHandle = g_api.CreateLobby(g_matchmaking, static_cast<int>(type),
                                        maxMembers + kRoomListingHeadroom);
    g_pendingCall = PendingCall::Create;
    SetStatus("Creating lobby...");
}

void SteamRoom_Leave()
{
    if (LocalLobby_Enabled()) { LocalLobby_Leave(); return; }
    SteamRoom_DisconnectPeer();
    if (g_sockets && g_listenSocket != k_HSteamListenSocket_Invalid)
        g_sockets->CloseListenSocket(g_listenSocket);
    g_listenSocket = k_HSteamListenSocket_Invalid;
    SteamRoom_SpecCloseAll();
    ClearLobbyRichPresence();
    if (g_matchmaking && g_lobbyId) {
        RememberLeftRoom(g_lobbyId);
        g_api.LeaveLobby(g_matchmaking, g_lobbyId);
        SetStatus("Left Steam lobby");
    }
    g_lobbyId = 0;
    g_isHost = false;
    g_pingLocationPublished = false;
    g_chatTail.store(g_chatHead.load(std::memory_order_acquire), std::memory_order_release);
}

int SteamRoom_GetMemberCount()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetMemberCount();
    return SteamRoom_IsActive() ? g_api.GetNumLobbyMembers(g_matchmaking, g_lobbyId) : 0;
}

uint64_t SteamRoom_GetMember(int index)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetMember(index);
    return SteamRoom_IsActive() ? g_api.GetLobbyMemberByIndex(g_matchmaking, g_lobbyId, index) : 0;
}

uint64_t SteamRoom_GetOwner()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetOwner();
    return SteamRoom_IsActive() ? g_api.GetLobbyOwner(g_matchmaking, g_lobbyId) : 0;
}

int SteamRoom_GetMemberLimit()
{
    if (LocalLobby_Enabled()) return LocalLobby_GetMemberLimit();
    if (!SteamRoom_IsActive()) return 0;
    if (const int capacity = RoomCapacity(g_lobbyId)) return capacity;
    if (!g_api.GetLobbyMemberLimit) return 0;
    return g_api.GetLobbyMemberLimit(g_matchmaking, g_lobbyId);
}

const char* SteamRoom_GetMemberName(uint64_t memberId)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetMemberName(memberId);
    if (memberId == g_localSteamId && g_localUsername[0]) return g_localUsername;
    const char* name = g_friends ? g_api.GetFriendPersonaName(g_friends, memberId) : nullptr;
    return name && name[0] ? name : "Player";
}

const char* SteamRoom_GetMemberData(uint64_t memberId, const char* key)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetMemberData(memberId, key);
    if (!SteamRoom_IsActive() || !g_api.GetLobbyMemberData) return "";
    const char* value = g_api.GetLobbyMemberData(g_matchmaking, g_lobbyId, memberId, key);
    return value ? value : "";
}

void SteamRoom_SetLocalData(const char* key, const char* value)
{
    if (LocalLobby_Enabled()) { LocalLobby_SetLocalData(key, value); return; }
    if (SteamRoom_IsActive() && g_api.SetLobbyMemberData)
        g_api.SetLobbyMemberData(g_matchmaking, g_lobbyId, key, value ? value : "");
}

const char* SteamRoom_GetData(const char* key)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetData(key);
    if (!SteamRoom_IsActive()) return "";
    const char* value = g_api.GetLobbyData(g_matchmaking, g_lobbyId, key);
    return value ? value : "";
}

void SteamRoom_SetName(const char* name)
{
    if (!name || !name[0]) return;
    if (LocalLobby_Enabled()) { LocalLobby_SetData("name", name); return; }
    if (!SteamRoom_IsActive() || SteamRoom_GetOwner() != g_localSteamId) return;
    snprintf(g_lobbyName, sizeof(g_lobbyName), "%s", name);
    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "name", g_lobbyName);
}

bool SteamRoom_SetType(SteamLobbyType type)
{
    const char* isPrivate = type == SteamLobbyType::Private ? "1" : "0";
    if (LocalLobby_Enabled()) { LocalLobby_SetData("private", isPrivate); return true; }
    if (!SteamRoom_IsActive() || SteamRoom_GetOwner() != g_localSteamId || !g_api.SetLobbyType ||
        !g_api.SetLobbyType(g_matchmaking, g_lobbyId, static_cast<int>(type)))
        return false;
    g_roomCreatePrivate = type == SteamLobbyType::Private;
    g_api.SetLobbyData(g_matchmaking, g_lobbyId, "private", isPrivate);
    PublishLobbyRichPresence(); // a private room offers no "Join Game"
    return true;
}

void SteamRoom_SetData(const char* key, const char* value)
{
    if (LocalLobby_Enabled()) { LocalLobby_SetData(key, value); return; }
    if (SteamRoom_IsActive() && SteamRoom_GetOwner() == g_localSteamId)
        g_api.SetLobbyData(g_matchmaking, g_lobbyId, key, value ? value : "");
}

int SteamRoom_EstimateMemberPing(uint64_t memberId)
{
    if (LocalLobby_Enabled()) return 2;
    if (!g_networkingUtils || !SteamRoom_IsActive()) return -1;
    const char* encoded = SteamRoom_GetMemberData(memberId, "loc");
    if (!encoded[0]) return -1;
    SteamNetworkPingLocation_t location = {};
    if (!g_networkingUtils->ParsePingLocationString(encoded, location)) return -1;
    // The estimate is a round trip, and is reported as one.
    const int ping = g_networkingUtils->EstimatePingTimeFromLocalHost(location);
    return ping >= 0 ? ping : -1;
}

bool SteamRoom_SendChat(const void* data, int size)
{
    if (LocalLobby_Enabled()) return LocalLobby_SendChat(data, size);
    if (!SteamRoom_IsActive() || !g_api.SendLobbyChatMsg || !data || size <= 0) return false;
    return g_api.SendLobbyChatMsg(g_matchmaking, g_lobbyId, data, size);
}

bool SteamRoom_PollChat(uint64_t* sender, char* buffer, int capacity, int* size)
{
    if (LocalLobby_Enabled()) return LocalLobby_PollChat(sender, buffer, capacity, size);
    if (!sender || !buffer || capacity <= 0 || !size) return false;
    uint32_t tail = g_chatTail.load(std::memory_order_relaxed);
    while (tail != g_chatHead.load(std::memory_order_acquire)) {
        const int chatId = g_chatIds[tail % 64];
        ++tail;
        g_chatTail.store(tail, std::memory_order_release);
        if (!SteamRoom_IsActive() || !g_api.GetLobbyChatEntry) continue;
        int type = 0;
        uint64_t user = 0;
        const int length = g_api.GetLobbyChatEntry(g_matchmaking, g_lobbyId, chatId, &user,
                                                   buffer, capacity, &type);
        // 1 = k_EChatEntryTypeChatMsg; ignore typing notices and system entries.
        if (length <= 0 || type != 1) continue;
        *sender = user;
        *size = length;
        return true;
    }
    return false;
}

bool SteamRoom_InviteFriends()
{
    if (LocalLobby_Enabled()) return true;
    if (!SteamRoom_IsActive() || !g_friends || !g_api.ActivateInviteDialog) return false;
    g_api.ActivateInviteDialog(g_friends, g_lobbyId);
    // Without the overlay the request above does nothing at all.
    return !g_api.IsOverlayEnabled || !g_utils || g_api.IsOverlayEnabled(g_utils);
}

void SteamLobby_QueueJoin(uint64_t lobbyId)
{
    if (LocalLobby_Enabled()) return;
    QueueJoinLobby(lobbyId);
}

bool SteamLobby_QueueJoinConnectString(const char* connect)
{
    uint64_t lobbyId = 0;
    if (!connect || !ParseLobbyConnectString(connect, lobbyId)) return false;
    SteamLobby_QueueJoin(lobbyId);
    return true;
}

uint64_t SteamLobby_QueuedJoin()
{
    if (LocalLobby_Enabled()) return 0;
    return g_pendingJoinLobby.load(std::memory_order_acquire);
}

bool SteamRoom_GetJoinLink(char* link, size_t capacity)
{
    if (!link || capacity == 0) return false;
    link[0] = '\0';
    if (LocalLobby_Enabled() || !SteamRoom_IsActive()) return false;
    const uint64_t owner = g_api.GetLobbyOwner(g_matchmaking, g_lobbyId);
    if (!owner) return false;
    const int length = snprintf(link, capacity, "steam://joinlobby/357190/%llu/%llu",
        static_cast<unsigned long long>(g_lobbyId),
        static_cast<unsigned long long>(owner));
    return length > 0 && static_cast<size_t>(length) < capacity;
}

bool SteamRoom_ViewProfile(uint64_t memberId)
{
    if (LocalLobby_Enabled()) return false;
    if (!memberId || !g_friends || !g_api.ActivateOverlayToUser) return false;
    g_api.ActivateOverlayToUser(g_friends, "steamid", memberId);
    return true;
}

const char* SteamRoom_GetLobbyData(uint64_t lobbyId, const char* key)
{
    if (LocalLobby_Enabled()) return LocalLobby_GetLobbyData(lobbyId, key);
    if (!lobbyId || !g_matchmaking || !g_api.GetLobbyData) return "";
    const char* value = g_api.GetLobbyData(g_matchmaking, lobbyId, key);
    return value ? value : "";
}

bool SteamRoom_ConnectPeer(uint64_t memberId, bool asHost)
{
    if (LocalLobby_Enabled()) return LocalLobby_ConnectPeer(memberId, asHost);
    if (!SteamRoom_IsActive() || !memberId || !g_sockets) return false;
    if (g_bridgeCreated || g_connection != k_HSteamNetConnection_Invalid) {
        // A previous match connection is still up; drop it first.
        SteamRoom_DisconnectPeer();
    }
    g_isHost = asHost;
    g_roomPeer = memberId;
    g_roomPeerFailed = false;
    g_handlesReady = false;
    g_nextConnectionAttempt = 0;
    g_connectionDeadline = 0;
    g_connectionAttempt = 0;
    StartPeerConnection(memberId);
    return true;
}

void SteamRoom_DisconnectPeer()
{
    if (LocalLobby_Enabled()) { LocalLobby_DisconnectPeer(); return; }
    const bool bridgeActive = SteamTransport_IsActive();
    if (bridgeActive) SteamTransport_Shutdown();
    if (!bridgeActive && g_sockets && g_connection != k_HSteamNetConnection_Invalid)
        g_sockets->CloseConnection(g_connection, 1000, "Timestone match ended", false);
    // In a room the listen socket stays up between matches, so a player who
    // dials before we have seen the pairing is still heard.
    if (!(g_roomMode && g_lobbyId)) {
        if (g_sockets && g_listenSocket != k_HSteamListenSocket_Invalid)
            g_sockets->CloseListenSocket(g_listenSocket);
        g_listenSocket = k_HSteamListenSocket_Invalid;
    }
    g_connection = k_HSteamNetConnection_Invalid;
    g_gameHost = nullptr;
    g_gamePeer = nullptr;
    g_roomPeer = 0;
    g_peerSteamId = 0;
    g_peerUsername[0] = '\0';
    g_handlesReady = false;
    g_bridgeCreated = false;
    g_nextConnectionAttempt = 0;
    g_connectionDeadline = 0;
    g_connectionAttempt = 0;
}

bool SteamRoom_TakePeerFailed()
{
    if (LocalLobby_Enabled()) return LocalLobby_TakePeerFailed();
    const bool failed = g_roomPeerFailed;
    g_roomPeerFailed = false;
    return failed;
}

// ---------------------------------------------------------------------------
// Spectator links
// ---------------------------------------------------------------------------
namespace {

constexpr int kSpecVirtualPort = 734; // the match connection uses 733
constexpr int kMaxSpecLinks = 8;
constexpr ULONGLONG kSpecConnectTimeoutMs = 15000;

struct SpecLink {
    uint64_t id = 0;
    HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
    int state = 0;
    ULONGLONG nextAttempt = 0;
    ULONGLONG deadline = 0;
    ULONGLONG opened = 0;
    bool dial = false;
    unsigned polls = 0;
};
SpecLink g_specLinks[kMaxSpecLinks];
// The answering end dials after all if no call has arrived by then (the
// caller's request can be lost, or the callback that reports it missing).
constexpr ULONGLONG kSpecAnswerWaitMs = 4000;
HSteamListenSocket g_specListen = k_HSteamListenSocket_Invalid;

SpecLink* FindSpecLink(uint64_t id)
{
    if (!id) return nullptr;
    for (SpecLink& link : g_specLinks)
        if (link.id == id) return &link;
    return nullptr;
}

void CloseSpecConnection(SpecLink& link, const char* reason)
{
    if (g_sockets && link.connection != k_HSteamNetConnection_Invalid)
        g_sockets->CloseConnection(link.connection, 1000, reason, false);
    link.connection = k_HSteamNetConnection_Invalid;
}

// Symmetric connect, as for the match: both ends dial and Steam pairs the two
// attempts into one connection.
void EnsureSpecListenSocket()
{
    if (g_specListen != k_HSteamListenSocket_Invalid || !g_sockets) return;
    SteamNetworkingConfigValue_t options[4];
    ConnectionOptions(options);
    g_specListen = g_sockets->CreateListenSocketP2P(kSpecVirtualPort, 4, options);
    if (g_specListen == k_HSteamListenSocket_Invalid)
        HookLog("SteamLobby: could not create the spectator listen socket\n");
}

void DialSpecLink(SpecLink& link)
{
    SteamNetworkingConfigValue_t options[4];
    ConnectionOptions(options);
    EnsureSpecListenSocket();
    if (g_specListen == k_HSteamListenSocket_Invalid) {
        link.nextAttempt = GetTickCount64() + 1000;
        return;
    }
    // The other end may have dialled first.
    link.connection = AcceptIncomingConnection(link.id, g_specListen);
    if (link.connection != k_HSteamNetConnection_Invalid) return;
    SteamNetworkingIdentity identity = {};
    identity.m_eType = k_ESteamNetworkingIdentityType_SteamID;
    identity.m_cbSize = sizeof(uint64_t);
    identity.data.m_steamID64 = link.id;
    link.connection = g_sockets->ConnectP2P(identity, kSpecVirtualPort, 4, options);
    if (link.connection == k_HSteamNetConnection_Invalid)
        link.nextAttempt = GetTickCount64() + 1000;
}

} // namespace

void SteamRoom_SpecOpen(uint64_t memberId, bool dial)
{
    if (LocalLobby_Enabled()) { LocalLobby_SpecOpen(memberId); return; }
    if (!g_sockets || !memberId || FindSpecLink(memberId)) return;
    for (SpecLink& link : g_specLinks) {
        if (link.id) continue;
        link = SpecLink{};
        link.id = memberId;
        link.state = 1;
        link.dial = dial;
        link.opened = GetTickCount64();
        link.deadline = link.opened + kSpecConnectTimeoutMs;
        return;
    }
}

void SteamRoom_SpecClose(uint64_t memberId)
{
    if (LocalLobby_Enabled()) { LocalLobby_SpecClose(memberId); return; }
    if (SpecLink* link = FindSpecLink(memberId)) {
        CloseSpecConnection(*link, "Timestone spectator link closed");
        *link = SpecLink{};
    }
}

void SteamRoom_SpecCloseAll()
{
    if (LocalLobby_Enabled()) { LocalLobby_SpecCloseAll(); return; }
    for (SpecLink& link : g_specLinks) {
        if (!link.id) continue;
        CloseSpecConnection(link, "Timestone spectator link closed");
        link = SpecLink{};
    }
    if (g_sockets && g_specListen != k_HSteamListenSocket_Invalid)
        g_sockets->CloseListenSocket(g_specListen);
    g_specListen = k_HSteamListenSocket_Invalid;
}

int SteamRoom_SpecState(uint64_t memberId)
{
    if (LocalLobby_Enabled()) return LocalLobby_SpecState(memberId);
    const SpecLink* link = FindSpecLink(memberId);
    return link ? link->state : 0;
}

bool SteamRoom_SpecSend(uint64_t memberId, const void* data, int size)
{
    if (LocalLobby_Enabled()) return LocalLobby_SpecSend(memberId, data, size);
    const SpecLink* link = FindSpecLink(memberId);
    if (!link || link->state != 2 || !g_sockets || !data || size <= 0) return false;
    return g_sockets->SendMessageToConnection(link->connection, data,
                                              static_cast<uint32_t>(size),
                                              k_nSteamNetworkingSend_Reliable,
                                              nullptr) == k_EResultOK;
}

bool SteamRoom_SpecReceive(uint64_t* memberId, void* buffer, int capacity, int* size)
{
    if (LocalLobby_Enabled()) return LocalLobby_SpecReceive(memberId, buffer, capacity, size);
    if (!g_sockets || !memberId || !buffer || !size || capacity <= 0) return false;
    for (SpecLink& link : g_specLinks) {
        if (!link.id || link.state != 2) continue;
        SteamNetworkingMessage_t* message = nullptr;
        if (g_sockets->ReceiveMessagesOnConnection(link.connection, &message, 1) != 1 || !message)
            continue;
        const int length = static_cast<int>(message->m_cbSize);
        const bool fits = length <= capacity;
        if (fits) memcpy(buffer, message->m_pData, static_cast<size_t>(length));
        message->Release();
        if (!fits) continue; // nothing we send is that large
        *memberId = link.id;
        *size = length;
        return true;
    }
    return false;
}

void SteamRoom_SpecPump()
{
    if (LocalLobby_Enabled()) { LocalLobby_SpecPump(); return; }
    if (!g_sockets) return;
    const ULONGLONG now = GetTickCount64();
    for (SpecLink& link : g_specLinks) {
        if (!link.id || link.state == 3) continue;
        if (link.state == 1 && link.connection == k_HSteamNetConnection_Invalid) {
            if (now >= link.deadline) {
                link.state = 3;
                continue;
            }
            if (!link.dial && now - link.opened < kSpecAnswerWaitMs) {
                // The answering end: take the call when it comes.
                EnsureSpecListenSocket();
                link.connection = AcceptIncomingConnection(link.id, g_specListen);
            } else if (now >= link.nextAttempt) {
                DialSpecLink(link);
            }
            continue;
        }
        // A live link is only checked now and then; a failure also shows up
        // as sends that stop being accepted.
        if (link.state == 2 && ++link.polls % 30 != 0) continue;
        SteamNetConnectionRealTimeStatus_t status = {};
        if (g_sockets->GetConnectionRealTimeStatus(link.connection, &status, 0, nullptr) != k_EResultOK)
            continue;
        const bool ended = status.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ||
                           status.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally ||
                           status.m_eState == k_ESteamNetworkingConnectionState_None;
        if (link.state == 1) {
            if (status.m_eState == k_ESteamNetworkingConnectionState_Connected) {
                link.state = 2;
                HookLog("SteamLobby: spectator link to %llu connected\n",
                        static_cast<unsigned long long>(link.id));
            } else if (ended) {
                // The other end may simply not have dialled yet: try again.
                CloseSpecConnection(link, "Timestone spectator link retry");
                link.nextAttempt = now + 1000;
            } else if (now >= link.deadline) {
                CloseSpecConnection(link, "Timestone spectator link timed out");
                link.state = 3;
            }
        } else if (ended) {
            HookLog("SteamLobby: spectator link to %llu ended (state %d)\n",
                    static_cast<unsigned long long>(link.id), static_cast<int>(status.m_eState));
            CloseSpecConnection(link, "Timestone spectator link ended");
            link.state = 3;
        }
    }
}
