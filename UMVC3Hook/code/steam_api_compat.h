#pragma once

#include <cstddef>
#include <cstdint>

using SteamAPICall_t = uint64_t;
using HSteamUser = int32_t;
using HSteamPipe = int32_t;
using HSteamNetConnection = uint32_t;
using HSteamListenSocket = uint32_t;

constexpr HSteamNetConnection k_HSteamNetConnection_Invalid = 0;
constexpr HSteamListenSocket k_HSteamListenSocket_Invalid = 0;
constexpr const char* k_SteamNetworkingSocketsVersion = "SteamNetworkingSockets012";
constexpr const char* k_SteamNetworkingUtilsVersion = "SteamNetworkingUtils004";
constexpr int k_cchMaxSteamNetworkingPingLocationString = 1024;

enum EResult : int32_t {
    k_EResultOK = 1,
    k_EResultDuplicateRequest = 29,
};

enum ESteamNetworkingConnectionState : int32_t {
    k_ESteamNetworkingConnectionState_None = 0,
    k_ESteamNetworkingConnectionState_Connecting = 1,
    k_ESteamNetworkingConnectionState_FindingRoute = 2,
    k_ESteamNetworkingConnectionState_Connected = 3,
    k_ESteamNetworkingConnectionState_ClosedByPeer = 4,
    k_ESteamNetworkingConnectionState_ProblemDetectedLocally = 5,
};

enum ESteamNetworkingIdentityType : int32_t {
    k_ESteamNetworkingIdentityType_Invalid = 0,
    k_ESteamNetworkingIdentityType_SteamID = 16,
};

enum ESteamNetworkingConfigValue : int32_t {
    k_ESteamNetworkingConfig_Invalid = 0,
    k_ESteamNetworkingConfig_SymmetricConnect = 37,
    k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable = 104,
};

enum ESteamNetworkingConfigDataType : int32_t {
    k_ESteamNetworkingConfig_Int32 = 1,
    k_ESteamNetworkingConfig_Int64 = 2,
    k_ESteamNetworkingConfig_Float = 3,
    k_ESteamNetworkingConfig_String = 4,
    k_ESteamNetworkingConfig_Ptr = 5,
};

constexpr int k_nSteamNetworkingSend_UnreliableNoNagle = 1;
constexpr int k_nSteamNetworkingSend_Reliable = 8;

#pragma pack(push, 1)
struct SteamNetworkingIPAddr {
    uint8_t m_ipv6[16];
    uint16_t m_port;
};

struct SteamNetworkingIdentity {
    ESteamNetworkingIdentityType m_eType;
    int32_t m_cbSize;
    union {
        uint64_t m_steamID64;
        uint8_t m_reserved[128];
    } data;
};
#pragma pack(pop)

struct SteamNetworkingConfigValue_t {
    ESteamNetworkingConfigValue m_eValue;
    ESteamNetworkingConfigDataType m_eDataType;
    union {
        int32_t m_int32;
        int64_t m_int64;
        float m_float;
        const char* m_string;
        void* m_ptr;
    } m_val;
};

struct SteamNetConnectionRealTimeLaneStatus_t;

constexpr int k_cchSteamNetworkingMaxConnectionCloseReason = 128;
constexpr int k_cchSteamNetworkingMaxConnectionDescription = 128;

struct SteamNetConnectionInfo_t {
    SteamNetworkingIdentity m_identityRemote;
    int64_t m_nUserData;
    HSteamListenSocket m_hListenSocket;
    SteamNetworkingIPAddr m_addrRemote;
    uint16_t m_pad;
    uint32_t m_idPOPRemote;
    uint32_t m_idPOPRelay;
    ESteamNetworkingConnectionState m_eState;
    int32_t m_eEndReason;
    char m_szEndDebug[k_cchSteamNetworkingMaxConnectionCloseReason];
    char m_szConnectionDescription[k_cchSteamNetworkingMaxConnectionDescription];
    int32_t m_nFlags;
    uint32_t reserved[63];
};

struct SteamNetConnectionRealTimeStatus_t {
    ESteamNetworkingConnectionState m_eState;
    int32_t m_nPing;
    float m_flConnectionQualityLocal;
    float m_flConnectionQualityRemote;
    float m_flOutPacketsPerSec;
    float m_flOutBytesPerSec;
    float m_flInPacketsPerSec;
    float m_flInBytesPerSec;
    int32_t m_nSendRateBytesPerSecond;
    int32_t m_cbPendingUnreliable;
    int32_t m_cbPendingReliable;
    int32_t m_cbSentUnackedReliable;
    int64_t m_usecQueueTime;
    int32_t m_usecMaxJitter;
    uint32_t reserved[15];
};

struct SteamNetworkingMessage_t {
    void* m_pData;
    int32_t m_cbSize;
    HSteamNetConnection m_conn;
    SteamNetworkingIdentity m_identityPeer;
    int64_t m_nConnUserData;
    int64_t m_usecTimeReceived;
    int64_t m_nMessageNumber;
    void (*m_pfnFreeData)(SteamNetworkingMessage_t*);
    void (*m_pfnRelease)(SteamNetworkingMessage_t*);
    int32_t m_nChannel;
    int32_t m_nFlags;
    int64_t m_nUserData;
    uint16_t m_idxLane;
    uint16_t m_pad;

    void Release() { m_pfnRelease(this); }
};

struct SteamNetworkPingLocation_t {
    uint8_t m_data[512];
};

// The method order must match SteamNetworkingUtils004. Only the prefix used
// by Timestone is declared here.
class ISteamNetworkingUtils {
public:
    virtual SteamNetworkingMessage_t* AllocateMessage(int) = 0;
    virtual int GetRelayNetworkStatus(void*) = 0;
    virtual float GetLocalPingLocation(SteamNetworkPingLocation_t&) = 0;
    virtual int EstimatePingTimeBetweenTwoLocations(const SteamNetworkPingLocation_t&,
                                                    const SteamNetworkPingLocation_t&) = 0;
    virtual int EstimatePingTimeFromLocalHost(const SteamNetworkPingLocation_t&) = 0;
    virtual void ConvertPingLocationToString(const SteamNetworkPingLocation_t&, char*, int) = 0;
    virtual bool ParsePingLocationString(const char*, SteamNetworkPingLocation_t&) = 0;
    virtual bool CheckPingDataUpToDate(float) = 0;
};

// The method order must match SteamNetworkingSockets012. Only the prefix used
// by Timestone is declared here.
class ISteamNetworkingSockets {
public:
    virtual HSteamListenSocket CreateListenSocketIP(const SteamNetworkingIPAddr&, int,
                                                     const SteamNetworkingConfigValue_t*) = 0;
    virtual HSteamNetConnection ConnectByIPAddress(const SteamNetworkingIPAddr&, int,
                                                    const SteamNetworkingConfigValue_t*) = 0;
    virtual HSteamListenSocket CreateListenSocketP2P(int, int,
                                                      const SteamNetworkingConfigValue_t*) = 0;
    virtual HSteamNetConnection ConnectP2P(const SteamNetworkingIdentity&, int, int,
                                           const SteamNetworkingConfigValue_t*) = 0;
    virtual EResult AcceptConnection(HSteamNetConnection) = 0;
    virtual bool CloseConnection(HSteamNetConnection, int, const char*, bool) = 0;
    virtual bool CloseListenSocket(HSteamListenSocket) = 0;
    virtual bool SetConnectionUserData(HSteamNetConnection, int64_t) = 0;
    virtual int64_t GetConnectionUserData(HSteamNetConnection) = 0;
    virtual void SetConnectionName(HSteamNetConnection, const char*) = 0;
    virtual bool GetConnectionName(HSteamNetConnection, char*, int) = 0;
    virtual EResult SendMessageToConnection(HSteamNetConnection, const void*, uint32_t,
                                            int, int64_t*) = 0;
    virtual void SendMessages(int, SteamNetworkingMessage_t**, int64_t*, bool) = 0;
    virtual EResult FlushMessagesOnConnection(HSteamNetConnection) = 0;
    virtual int ReceiveMessagesOnConnection(HSteamNetConnection,
                                             SteamNetworkingMessage_t**, int) = 0;
    virtual bool GetConnectionInfo(HSteamNetConnection, SteamNetConnectionInfo_t*) = 0;
    virtual EResult GetConnectionRealTimeStatus(HSteamNetConnection,
                                                SteamNetConnectionRealTimeStatus_t*, int,
                                                SteamNetConnectionRealTimeLaneStatus_t*) = 0;
};

#pragma pack(push, 8)
struct LobbyMatchList_t {
    uint32_t m_nLobbiesMatching;
};

struct LobbyCreated_t {
    EResult m_eResult;
    uint32_t m_pad;
    uint64_t m_ulSteamIDLobby;
};

struct LobbyEnter_t {
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;
    bool m_bLocked;
    uint8_t m_pad[3];
    uint32_t m_EChatRoomEnterResponse;
};

struct GameLobbyJoinRequestedCompat_t {
    uint64_t m_steamIDLobby;
    uint64_t m_steamIDFriend;
};

struct GameRichPresenceJoinRequestedCompat_t {
    uint64_t m_steamIDFriend;
    char m_rgchConnect[256];
};
#pragma pack(pop)

static_assert(sizeof(SteamNetworkingIdentity) == 136);
static_assert(sizeof(SteamNetworkingConfigValue_t) == 16);
static_assert(sizeof(SteamNetConnectionInfo_t) == 696);
static_assert(sizeof(SteamNetworkPingLocation_t) == 512);
static_assert(sizeof(LobbyMatchList_t) == 4);
static_assert(sizeof(LobbyCreated_t) == 16);
static_assert(sizeof(LobbyEnter_t) == 24);
static_assert(sizeof(GameLobbyJoinRequestedCompat_t) == 16);
static_assert(sizeof(GameRichPresenceJoinRequestedCompat_t) == 264);

constexpr int k_iCallbackLobbyEnter = 504;
constexpr int k_iCallbackLobbyMatchList = 510;
constexpr int k_iCallbackLobbyCreated = 513;
constexpr int k_iCallbackGameLobbyJoinRequested = 333;
constexpr int k_iCallbackGameRichPresenceJoinRequested = 337;
