#include "discord_rich_presence.h"

#include "../IniSafe.h"
#include "steam_lobby.h"
#include "launchmenu_utils.h"
#include "mk10utils.h"
#include "ranked_account.h"

#include <Windows.h>

// cdiscord.h declares every SDK function with DISCORD_API (__declspec(dllexport)
// on Windows). For a runtime-loaded module we only need the prototypes, so the
// macro is emptied before including the header.
#ifndef DISCORD_API
#define DISCORD_API
#endif
#include "../third_party/discord_social_sdk/include/cdiscord.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

extern void HookLog(const char* fmt, ...);

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint64_t kDiscordApplicationId = 1544202390536982610ull;
constexpr uint32_t kSteamApplicationId = 357190; // Ultimate Marvel vs. Capcom 3
constexpr int kPresencePushIntervalSeconds = 15;  // Discord silently drops faster updates
constexpr int kConnectRetrySeconds = 10;
constexpr const char* kAssetKey = "timestone";       // Discord lowercases asset keys
constexpr const char* kAssetLargeText = "Timestone";
constexpr const char* kConsentPrompt =
    "Full Discord Rich Presence will open your browser to link your Discord "
    "account.\n\nContinue?";

// ---------------------------------------------------------------------------
// Runtime-linked SDK surface (signatures match cdiscord.h exactly)
// ---------------------------------------------------------------------------

struct DiscordApi {
    void (*RunCallbacks)() = nullptr;
    void (*SetFreeThreaded)() = nullptr;

    void (*ClientInit)(Discord_Client*) = nullptr;
    void (*ClientDrop)(Discord_Client*) = nullptr;
    void (*ClientSetApplicationId)(Discord_Client*, uint64_t) = nullptr;
    void (*ClientConnect)(Discord_Client*) = nullptr;
    void (*ClientDisconnect)(Discord_Client*) = nullptr;
    Discord_Client_Status (*ClientGetStatus)(Discord_Client*) = nullptr;
    void (*ClientSetStatusChangedCallback)(Discord_Client*,
                                           Discord_Client_OnStatusChanged,
                                           Discord_FreeFn, void*) = nullptr;
    void (*ClientAddLogCallback)(Discord_Client*, Discord_Client_LogCallback,
                                 Discord_FreeFn, void*,
                                 Discord_LoggingSeverity) = nullptr;
    void (*ClientClearRichPresence)(Discord_Client*) = nullptr;
    void (*ClientUpdateRichPresence)(Discord_Client*, Discord_Activity*,
                                     Discord_Client_UpdateRichPresenceCallback,
                                     Discord_FreeFn, void*) = nullptr;
    void (*ClientSetActivityJoinCallback)(Discord_Client*,
                                          Discord_Client_ActivityJoinCallback,
                                          Discord_FreeFn, void*) = nullptr;
    bool (*ClientRegisterLaunchSteamApplication)(Discord_Client*, uint64_t,
                                                 uint32_t) = nullptr;
    void (*ClientStatusToString)(Discord_Client_Status,
                                 Discord_String*) = nullptr;
    void (*ClientErrorToString)(Discord_Client_Error,
                                Discord_String*) = nullptr;

    // Full mode (OAuth). Resolved optionally; missing exports downgrade to Basic.
    void (*ClientAuthorize)(Discord_Client*, Discord_AuthorizationArgs*,
                            Discord_Client_AuthorizationCallback,
                            Discord_FreeFn, void*) = nullptr;
    void (*ClientGetToken)(Discord_Client*, uint64_t, Discord_String,
                           Discord_String, Discord_String,
                           Discord_Client_TokenExchangeCallback,
                           Discord_FreeFn, void*) = nullptr;
    void (*ClientUpdateToken)(Discord_Client*, Discord_AuthorizationTokenType,
                              Discord_String,
                              Discord_Client_UpdateTokenCallback,
                              Discord_FreeFn, void*) = nullptr;
    void (*ClientCreateAuthorizationCodeVerifier)(
        Discord_Client*, Discord_AuthorizationCodeVerifier*) = nullptr;
    void (*ClientGetDefaultPresenceScopes)(Discord_String*) = nullptr;
    void (*AuthorizationArgsInit)(Discord_AuthorizationArgs*) = nullptr;
    void (*AuthorizationArgsDrop)(Discord_AuthorizationArgs*) = nullptr;
    void (*AuthorizationArgsSetClientId)(Discord_AuthorizationArgs*,
                                         uint64_t) = nullptr;
    void (*AuthorizationArgsSetScopes)(Discord_AuthorizationArgs*,
                                       Discord_String) = nullptr;
    void (*VerifierVerifier)(Discord_AuthorizationCodeVerifier*,
                             Discord_String*) = nullptr;
    void (*VerifierDrop)(Discord_AuthorizationCodeVerifier*) = nullptr;

    void (*ActivityInit)(Discord_Activity*) = nullptr;
    void (*ActivityDrop)(Discord_Activity*) = nullptr;
    void (*ActivitySetType)(Discord_Activity*, Discord_ActivityTypes) = nullptr;
    void (*ActivitySetDetails)(Discord_Activity*, Discord_String*) = nullptr;
    void (*ActivitySetState)(Discord_Activity*, Discord_String*) = nullptr;
    void (*ActivitySetAssets)(Discord_Activity*, Discord_ActivityAssets*) = nullptr;
    void (*ActivitySetTimestamps)(Discord_Activity*,
                                  Discord_ActivityTimestamps*) = nullptr;
    void (*ActivitySetParty)(Discord_Activity*, Discord_ActivityParty*) = nullptr;
    void (*ActivitySetSecrets)(Discord_Activity*, Discord_ActivitySecrets*) = nullptr;
    void (*ActivitySetSupportedPlatforms)(Discord_Activity*,
                                          Discord_ActivityGamePlatforms) = nullptr;

    void (*AssetsInit)(Discord_ActivityAssets*) = nullptr;
    void (*AssetsDrop)(Discord_ActivityAssets*) = nullptr;
    void (*AssetsSetLargeImage)(Discord_ActivityAssets*, Discord_String*) = nullptr;
    void (*AssetsSetLargeText)(Discord_ActivityAssets*, Discord_String*) = nullptr;

    void (*TimestampsInit)(Discord_ActivityTimestamps*) = nullptr;
    void (*TimestampsDrop)(Discord_ActivityTimestamps*) = nullptr;
    void (*TimestampsSetStart)(Discord_ActivityTimestamps*, uint64_t) = nullptr;

    void (*PartyInit)(Discord_ActivityParty*) = nullptr;
    void (*PartyDrop)(Discord_ActivityParty*) = nullptr;
    void (*PartySetId)(Discord_ActivityParty*, Discord_String) = nullptr;
    void (*PartySetCurrentSize)(Discord_ActivityParty*, int32_t) = nullptr;
    void (*PartySetMaxSize)(Discord_ActivityParty*, int32_t) = nullptr;
    void (*PartySetPrivacy)(Discord_ActivityParty*,
                            Discord_ActivityPartyPrivacy) = nullptr;

    void (*SecretsInit)(Discord_ActivitySecrets*) = nullptr;
    void (*SecretsDrop)(Discord_ActivitySecrets*) = nullptr;
    void (*SecretsSetJoin)(Discord_ActivitySecrets*, Discord_String) = nullptr;

    bool (*ClientResultSuccessful)(Discord_ClientResult*) = nullptr;
    int32_t (*ClientResultErrorCode)(Discord_ClientResult*) = nullptr;
};

DiscordApi g_api;

HMODULE g_module = nullptr;
bool g_available = false;
bool g_loadAttempted = false;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

int g_desiredMode = 0; // mode the pump should run; mirrors Timestone.ini
int g_activeMode = 0;  // mode of the client that is currently started (0 = none)
bool g_clientActive = false;
bool g_ready = false;
Discord_Client_Error g_lastError = Discord_Client_Error_None;
std::string g_lastErrorText; // distinct-error log throttle
time_t g_nextConnectRetry = 0;
bool g_authenticatedFull = false; // Full-mode OAuth completed this process

Discord_Client g_client{};
Discord_AuthorizationCodeVerifier g_verifier{};
bool g_verifierValid = false;
bool g_authPending = false; // Full mode waiting for the browser flow
bool g_consentAccepted = false;
std::string g_authVerifierString;

std::string g_pendingJoinSecret; // filled by JoinCb, consumed on the pump
std::mutex g_joinMutex;

time_t g_lastPushTime = 0;
std::string g_lastFingerprint;
bool g_presenceDirty = false;

bool g_prevInMatch = false;
time_t g_matchStartTime = 0;
uint64_t g_pumpCounter = 0;

// Strings referenced by the live activity must outlive the push call; the SDK
// may defer reading the spans until a later Discord_RunCallbacks.
std::string g_activityDetails;
std::string g_activityState;
std::string g_activityPartyId;
std::string g_activitySecret;

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

Discord_String DStr(const char* text)
{
    Discord_String result{};
    if (text) {
        result.ptr = reinterpret_cast<uint8_t*>(const_cast<char*>(text));
        result.size = strlen(text);
    }
    return result;
}

Discord_String DStr(const std::string& text)
{
    Discord_String result{};
    result.ptr = reinterpret_cast<uint8_t*>(const_cast<char*>(text.data()));
    result.size = text.size();
    return result;
}

Discord_String DStr(const uint8_t* ptr, size_t size)
{
    Discord_String result{};
    result.ptr = const_cast<uint8_t*>(ptr);
    result.size = size;
    return result;
}

std::string CopyStr(const Discord_String& value)
{
    if (!value.ptr || value.size == 0) return {};
    return std::string(reinterpret_cast<const char*>(value.ptr), value.size);
}

// ---------------------------------------------------------------------------
// Export resolution
// ---------------------------------------------------------------------------

template <typename T>
bool Resolve(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(g_module, name));
    if (!target) HookLog("Discord: missing export %s\n", name);
    return target != nullptr;
}

template <typename T>
void ResolveOptional(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(g_module, name));
    if (!target) HookLog("Discord: optional export %s is unavailable\n", name);
}

bool ResolveRequiredExports()
{
    bool ok = true;
    ok &= Resolve(g_api.RunCallbacks, "Discord_RunCallbacks");
    ok &= Resolve(g_api.SetFreeThreaded, "Discord_SetFreeThreaded");

    ok &= Resolve(g_api.ClientInit, "Discord_Client_Init");
    ok &= Resolve(g_api.ClientDrop, "Discord_Client_Drop");
    ok &= Resolve(g_api.ClientSetApplicationId, "Discord_Client_SetApplicationId");
    ok &= Resolve(g_api.ClientConnect, "Discord_Client_Connect");
    ok &= Resolve(g_api.ClientDisconnect, "Discord_Client_Disconnect");
    ok &= Resolve(g_api.ClientGetStatus, "Discord_Client_GetStatus");
    ok &= Resolve(g_api.ClientSetStatusChangedCallback,
                  "Discord_Client_SetStatusChangedCallback");
    ok &= Resolve(g_api.ClientAddLogCallback, "Discord_Client_AddLogCallback");
    ok &= Resolve(g_api.ClientClearRichPresence, "Discord_Client_ClearRichPresence");
    ok &= Resolve(g_api.ClientUpdateRichPresence,
                  "Discord_Client_UpdateRichPresence");
    ok &= Resolve(g_api.ClientSetActivityJoinCallback,
                  "Discord_Client_SetActivityJoinCallback");
    ok &= Resolve(g_api.ClientStatusToString, "Discord_Client_StatusToString");
    ok &= Resolve(g_api.ClientErrorToString, "Discord_Client_ErrorToString");
    ResolveOptional(g_api.ClientRegisterLaunchSteamApplication,
                    "Discord_Client_RegisterLaunchSteamApplication");

    ok &= Resolve(g_api.ActivityInit, "Discord_Activity_Init");
    ok &= Resolve(g_api.ActivityDrop, "Discord_Activity_Drop");
    ok &= Resolve(g_api.ActivitySetType, "Discord_Activity_SetType");
    ok &= Resolve(g_api.ActivitySetDetails, "Discord_Activity_SetDetails");
    ok &= Resolve(g_api.ActivitySetState, "Discord_Activity_SetState");
    ok &= Resolve(g_api.ActivitySetAssets, "Discord_Activity_SetAssets");
    ok &= Resolve(g_api.ActivitySetTimestamps, "Discord_Activity_SetTimestamps");
    ok &= Resolve(g_api.ActivitySetParty, "Discord_Activity_SetParty");
    ok &= Resolve(g_api.ActivitySetSecrets, "Discord_Activity_SetSecrets");
    ok &= Resolve(g_api.ActivitySetSupportedPlatforms,
                  "Discord_Activity_SetSupportedPlatforms");

    ok &= Resolve(g_api.AssetsInit, "Discord_ActivityAssets_Init");
    ok &= Resolve(g_api.AssetsDrop, "Discord_ActivityAssets_Drop");
    ok &= Resolve(g_api.AssetsSetLargeImage, "Discord_ActivityAssets_SetLargeImage");
    ok &= Resolve(g_api.AssetsSetLargeText, "Discord_ActivityAssets_SetLargeText");

    ok &= Resolve(g_api.TimestampsInit, "Discord_ActivityTimestamps_Init");
    ok &= Resolve(g_api.TimestampsDrop, "Discord_ActivityTimestamps_Drop");
    ok &= Resolve(g_api.TimestampsSetStart, "Discord_ActivityTimestamps_SetStart");

    ok &= Resolve(g_api.PartyInit, "Discord_ActivityParty_Init");
    ok &= Resolve(g_api.PartyDrop, "Discord_ActivityParty_Drop");
    ok &= Resolve(g_api.PartySetId, "Discord_ActivityParty_SetId");
    ok &= Resolve(g_api.PartySetCurrentSize, "Discord_ActivityParty_SetCurrentSize");
    ok &= Resolve(g_api.PartySetMaxSize, "Discord_ActivityParty_SetMaxSize");
    ok &= Resolve(g_api.PartySetPrivacy, "Discord_ActivityParty_SetPrivacy");

    ok &= Resolve(g_api.SecretsInit, "Discord_ActivitySecrets_Init");
    ok &= Resolve(g_api.SecretsDrop, "Discord_ActivitySecrets_Drop");
    ok &= Resolve(g_api.SecretsSetJoin, "Discord_ActivitySecrets_SetJoin");

    ok &= Resolve(g_api.ClientResultSuccessful, "Discord_ClientResult_Successful");
    ok &= Resolve(g_api.ClientResultErrorCode, "Discord_ClientResult_ErrorCode");

    // Full-mode OAuth surface; a missing export here only drops Full -> Basic.
    ResolveOptional(g_api.ClientAuthorize, "Discord_Client_Authorize");
    ResolveOptional(g_api.ClientGetToken, "Discord_Client_GetToken");
    ResolveOptional(g_api.ClientUpdateToken, "Discord_Client_UpdateToken");
    ResolveOptional(g_api.ClientCreateAuthorizationCodeVerifier,
                    "Discord_Client_CreateAuthorizationCodeVerifier");
    ResolveOptional(g_api.ClientGetDefaultPresenceScopes,
                    "Discord_Client_GetDefaultPresenceScopes");
    ResolveOptional(g_api.AuthorizationArgsInit, "Discord_AuthorizationArgs_Init");
    ResolveOptional(g_api.AuthorizationArgsDrop, "Discord_AuthorizationArgs_Drop");
    ResolveOptional(g_api.AuthorizationArgsSetClientId,
                    "Discord_AuthorizationArgs_SetClientId");
    ResolveOptional(g_api.AuthorizationArgsSetScopes,
                    "Discord_AuthorizationArgs_SetScopes");
    ResolveOptional(g_api.VerifierVerifier,
                    "Discord_AuthorizationCodeVerifier_Verifier");
    ResolveOptional(g_api.VerifierDrop, "Discord_AuthorizationCodeVerifier_Drop");
    return ok;
}

void EnsureLoaded()
{
    if (g_loadAttempted) return;
    g_loadAttempted = true;
    g_module = LoadLibraryA("discord_partner_sdk.dll");
    if (!g_module) {
        HookLog("Discord: discord_partner_sdk.dll not found; rich presence disabled\n");
        return;
    }
    g_available = ResolveRequiredExports();
    if (!g_available) {
        HookLog("Discord: required exports missing; rich presence disabled\n");
        return;
    }
    g_api.SetFreeThreaded();
    HookLog("Discord: discord_partner_sdk.dll loaded; rich presence available\n");
}

// ---------------------------------------------------------------------------
// Mode persistence
// ---------------------------------------------------------------------------

bool WriteModeToIni(int mode);

int ReadModeFromIni()
{
    char path[MAX_PATH] = {};
    LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
    int mode = GetPrivateProfileIntA("Settings", "TelemetryMode", -1, path);
    if (mode < 0) {
        // Legacy single-feature key migrates to the TelemetryMode key once;
        // the old key is left in the file and never read again.
        mode = GetPrivateProfileIntA("Settings", "DiscordRichPresence", 0, path);
        if (mode >= 0 && mode <= 2) WriteModeToIni(mode);
        else mode = 0;
    }
    return (mode < 0) ? 0 : ((mode > 3) ? 3 : mode);
}

bool WriteModeToIni(int mode)
{
    char path[MAX_PATH] = {};
    LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
    char value[2] = { static_cast<char>('0' + mode), '\0' };
    return IniSafe_WriteString("Settings", "TelemetryMode", value, path);
}

// ---------------------------------------------------------------------------
// In-match detection (RE notes: doc/area.md). sArea holds the active
// root-to-leaf chain of cArea objects; battle areas are aGame/aAction/aStageNNN.
// The chain length sits at sArea+0x703C and the cArea*[8] array at +0x7048;
// each cArea caches its name pointer at +0x40.
// ---------------------------------------------------------------------------

bool InMatch()
{
    const int64_t sAreaPointerAddress = _addr(0x140e17a80);
    const uint8_t* sArea = *(const uint8_t* const*)sAreaPointerAddress;
    if (!sArea) return false;
    const int count = *reinterpret_cast<const int*>(sArea + 0x703C);
    if (count <= 0 || count > 8) return false;
    const uint8_t* const* chain =
        reinterpret_cast<const uint8_t* const*>(sArea + 0x7048);
    for (int i = 0; i < count; ++i) {
        const uint8_t* area = chain[i];
        if (!area) continue;
        const char* name = *reinterpret_cast<const char* const*>(area + 0x40);
        if (name && (strncmp(name, "aStage", 6) == 0 ||
                     strncmp(name, "aAction", 7) == 0))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SDK callbacks
// ---------------------------------------------------------------------------

void LogCb(Discord_String message, Discord_LoggingSeverity, void*)
{
    const std::string text = CopyStr(message);
    if (!text.empty()) HookLog("Discord: %s\n", text.c_str());
}

void StatusCb(Discord_Client_Status status, Discord_Client_Error error,
              int32_t, void*)
{
    if (status == Discord_Client_Status_Ready) {
        g_ready = true;
        g_lastError = Discord_Client_Error_None;
        g_lastErrorText.clear();
        // Presence should appear promptly after (re)connect.
        g_lastPushTime = 0;
        g_presenceDirty = true;
        HookLog("Discord: connected\n");
    } else if (status == Discord_Client_Status_Disconnected &&
               error != Discord_Client_Error_None) {
        g_ready = false;
        g_lastError = error;
    } else if (status == Discord_Client_Status_Disconnected ||
               status == Discord_Client_Status_Disconnecting) {
        g_ready = false;
    }
}

void JoinCb(Discord_String joinSecret, void*)
{
    const std::string secret = CopyStr(joinSecret);
    if (secret.empty()) return;
    std::lock_guard<std::mutex> guard(g_joinMutex);
    g_pendingJoinSecret = secret;
}

void IgnoreResultCb(Discord_ClientResult*, void*) {}

void ConsumePendingJoin()
{
    std::string secret;
    {
        std::lock_guard<std::mutex> guard(g_joinMutex);
        if (g_pendingJoinSecret.empty()) return;
        secret.swap(g_pendingJoinSecret);
    }
    HookLog("Discord: join requested through rich presence\n");
    SteamLobby_QueueJoinByConnectString(secret.c_str());
}

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

void DropVerifier()
{
    if (g_verifierValid) {
        g_api.VerifierDrop(&g_verifier);
        g_verifierValid = false;
    }
    g_authVerifierString.clear();
}

void FallbackToBasic(const char* reason)
{
    HookLog("Discord: Full mode unavailable (%s); switching to Basic presence\n", reason);
    DropVerifier();
    g_authPending = false;
    DiscordRichPresence_SetMode(1);
}

void StartClient(int mode)
{
    g_client = {};
    g_api.ClientInit(&g_client);
    g_api.ClientAddLogCallback(&g_client, LogCb, nullptr, nullptr,
                               Discord_LoggingSeverity_Warning);
    g_api.ClientSetApplicationId(&g_client, kDiscordApplicationId);
    g_api.ClientSetStatusChangedCallback(&g_client, StatusCb, nullptr, nullptr);
    g_api.ClientSetActivityJoinCallback(&g_client, JoinCb, nullptr, nullptr);
    if (g_api.ClientRegisterLaunchSteamApplication) {
        g_api.ClientRegisterLaunchSteamApplication(
            &g_client, kDiscordApplicationId, kSteamApplicationId);
    }

    g_clientActive = true;
    g_activeMode = mode;
    g_ready = false;
    g_lastError = Discord_Client_Error_None;
    g_lastErrorText.clear();
    g_nextConnectRetry = 0;
    g_lastPushTime = 0;
    g_presenceDirty = true;
    g_lastFingerprint.clear();
    g_prevInMatch = false;
    g_matchStartTime = 0;

    if (mode == 1 || (mode == 2 && g_authenticatedFull)) {
        g_api.ClientConnect(&g_client);
    } else {
        g_authPending = true;
    }
}

void StopClient()
{
    if (!g_clientActive) return;
    if (g_api.ClientClearRichPresence) g_api.ClientClearRichPresence(&g_client);
    g_api.ClientDisconnect(&g_client);
    g_api.ClientDrop(&g_client);
    DropVerifier();
    g_authPending = false;
    g_clientActive = false;
    g_ready = false;
    g_activeMode = 0;
}

bool ConfirmFullConsent()
{
    if (g_consentAccepted) return true;
    const int result = MessageBoxA(nullptr, kConsentPrompt, "Timestone",
                                   MB_OKCANCEL | MB_ICONQUESTION |
                                       MB_SETFOREGROUND | MB_TOPMOST);
    if (result != IDOK) {
        HookLog("Discord: Full mode consent declined\n");
        return false;
    }
    g_consentAccepted = true;
    return true;
}

// ---------------------------------------------------------------------------
// Full-mode OAuth. AdvanceOAuth starts the PKCE browser flow; the callbacks
// chain Authorize -> GetToken -> UpdateToken -> Connect. On any error the
// module drops to Basic (mode 1) so presence still works without a link.
// ---------------------------------------------------------------------------

void TokenCb(Discord_ClientResult* result, Discord_String accessToken,
             Discord_String refreshToken, Discord_AuthorizationTokenType tokenType,
             int32_t expiresIn, Discord_String scopes, void* userData);
void UpdateTokenCb(Discord_ClientResult* result, void* userData);
void AuthCb(Discord_ClientResult* result, Discord_String code,
            Discord_String redirectUri, void* userData);

void AuthCb(Discord_ClientResult* result, Discord_String code,
            Discord_String redirectUri, void*)
{
    if (!g_api.ClientResultSuccessful(result)) {
        const int32_t errorCode =
            g_api.ClientResultErrorCode ? g_api.ClientResultErrorCode(result) : -1;
        HookLog("Discord: authorization failed (code %d)\n", errorCode);
        FallbackToBasic("authorization failed");
        return;
    }
    if (!g_api.ClientGetToken || !g_api.ClientUpdateToken) {
        FallbackToBasic("missing token exports");
        return;
    }
    const Discord_String codeVerifier = DStr(g_authVerifierString);
    g_api.ClientGetToken(&g_client, kDiscordApplicationId, code, codeVerifier,
                         redirectUri, TokenCb, nullptr, nullptr);
}

void TokenCb(Discord_ClientResult* result, Discord_String accessToken,
             Discord_String refreshToken, Discord_AuthorizationTokenType tokenType,
             int32_t, Discord_String, void*)
{
    (void)refreshToken;
    if (!g_api.ClientResultSuccessful(result)) {
        const int32_t errorCode =
            g_api.ClientResultErrorCode ? g_api.ClientResultErrorCode(result) : -1;
        HookLog("Discord: token exchange failed (code %d)\n", errorCode);
        FallbackToBasic("token exchange failed");
        return;
    }
    // The verifier is only needed up to the exchange.
    DropVerifier();
    g_api.ClientUpdateToken(&g_client, tokenType, accessToken,
                            UpdateTokenCb, nullptr, nullptr);
}

void UpdateTokenCb(Discord_ClientResult* result, void*)
{
    if (!g_api.ClientResultSuccessful(result)) {
        const int32_t errorCode =
            g_api.ClientResultErrorCode ? g_api.ClientResultErrorCode(result) : -1;
        HookLog("Discord: token update failed (code %d)\n", errorCode);
        FallbackToBasic("token update failed");
        return;
    }
    g_authenticatedFull = true;
    g_authPending = false;
    HookLog("Discord: Full mode account linked\n");
    g_api.ClientConnect(&g_client);
}

void AdvanceOAuth()
{
    if (!g_consentAccepted && !ConfirmFullConsent()) {
        g_authPending = false;
        FallbackToBasic("consent declined");
        return;
    }
    const bool fullSurfaceReady =
        g_api.ClientAuthorize && g_api.ClientGetToken && g_api.ClientUpdateToken &&
        g_api.ClientCreateAuthorizationCodeVerifier &&
        g_api.ClientGetDefaultPresenceScopes && g_api.AuthorizationArgsInit &&
        g_api.AuthorizationArgsDrop && g_api.AuthorizationArgsSetClientId &&
        g_api.AuthorizationArgsSetScopes && g_api.VerifierVerifier &&
        g_api.VerifierDrop;
    if (!fullSurfaceReady) {
        FallbackToBasic("OAuth exports missing");
        return;
    }

    Discord_AuthorizationCodeVerifier verifier{};
    g_api.ClientCreateAuthorizationCodeVerifier(&g_client, &verifier);
    Discord_String verifierString{};
    g_api.VerifierVerifier(&verifier, &verifierString);
    g_authVerifierString = CopyStr(verifierString);
    if (g_authVerifierString.empty()) {
        g_api.VerifierDrop(&verifier);
        FallbackToBasic("no code verifier");
        return;
    }
    if (g_verifierValid) g_api.VerifierDrop(&g_verifier);
    g_verifier = verifier;
    g_verifierValid = true;

    Discord_AuthorizationArgs args{};
    g_api.AuthorizationArgsInit(&args);
    g_api.AuthorizationArgsSetClientId(&args, kDiscordApplicationId);
    Discord_String scopes{};
    g_api.ClientGetDefaultPresenceScopes(&scopes);
    g_api.AuthorizationArgsSetScopes(&args, scopes);
    g_api.ClientAuthorize(&g_client, &args, AuthCb, nullptr, nullptr);
    g_api.AuthorizationArgsDrop(&args);

    g_authPending = false;
    HookLog("Discord: opened Full-mode authorization (browser link flow)\n");
}

// ---------------------------------------------------------------------------
// Presence push
// ---------------------------------------------------------------------------

struct PresenceState {
    bool inLobby = false;
    int members = 0;
    bool host = false;
    uint64_t lobbyId = 0;
    bool inMatch = false;
};

PresenceState ReadPresenceState()
{
    PresenceState state;
    state.inLobby = SteamLobby_HasLobby();
    state.host = SteamLobby_IsHost();
    state.lobbyId = SteamLobby_GetLobbyId();
    state.members = state.lobbyId ? SteamLobby_GetMemberCount() : 0;
    if (state.members < 0) state.members = 0;
    state.inMatch = InMatch();
    return state;
}

void UpdateMatchStartEdge(const PresenceState& state)
{
    if (state.inMatch && !g_prevInMatch)
        g_matchStartTime = time(nullptr);
    else if (!state.inMatch && g_prevInMatch)
        g_matchStartTime = 0;
    g_prevInMatch = state.inMatch;
}

std::string BuildFingerprint(const PresenceState& state)
{
    char buffer[160] = {};
    snprintf(buffer, sizeof(buffer), "%d;%d;%lld;%d;%d;%d;%llu",
             g_activeMode, state.inMatch ? 1 : 0,
             static_cast<long long>(g_matchStartTime),
             state.inLobby ? 1 : 0, state.members, state.host ? 1 : 0,
             static_cast<unsigned long long>(state.lobbyId));
    return buffer;
}

void PushPresence(const PresenceState& state)
{
    if (!g_api.ClientUpdateRichPresence) return;

    const bool hostWaiting =
        state.inLobby && state.host && state.members <= 1;
    bool showMatchTimer = false;
    bool showParty = false;
    bool showSecret = false;

    g_activityState.clear();
    g_activityPartyId.clear();
    g_activitySecret.clear();

    if (state.inMatch) {
        g_activityDetails = "In a Match";
        showMatchTimer = true;
    } else if (hostWaiting) {
        g_activityDetails = "Hosting a Lobby";
        g_activityState = "Waiting for an opponent";
        g_activityPartyId = std::to_string(state.lobbyId);
        // The join secret rides on a linked Discord account. Hosting while
        // unlinked (or in a non-join tier) keeps the party visible with no
        // secret, so Discord does not advertise an unjoinable game.
        if (g_activeMode == 2 && g_authenticatedFull) {
            g_activitySecret = "+connect_lobby " + std::to_string(state.lobbyId);
            showSecret = true;
        }
        showParty = true;
    } else if (state.inLobby && state.host) {
        g_activityDetails = "Hosting a Lobby";
        g_activityState = "In the lobby";
        g_activityPartyId = std::to_string(state.lobbyId);
        showParty = true;
    } else if (state.inLobby) {
        g_activityDetails = "In a Lobby";
        g_activityState = "Waiting for the host";
        g_activityPartyId = std::to_string(state.lobbyId);
        showParty = true;
    } else {
        g_activityDetails = "Main Menu";
    }

    Discord_Activity activity{};
    g_api.ActivityInit(&activity);
    g_api.ActivitySetType(&activity, Discord_ActivityTypes_Playing);

    // The SDK stores the Discord_String spans (not copies) on the activity, so
    // the backing store must be module-lifetime (the g_activity* members above);
    // these struct instances only need to outlive the UpdateRichPresence call.
    Discord_String details = DStr(g_activityDetails);
    g_api.ActivitySetDetails(&activity, &details);
    if (!g_activityState.empty()) {
        Discord_String state_ = DStr(g_activityState);
        g_api.ActivitySetState(&activity, &state_);
    }

    if (showMatchTimer) {
        Discord_ActivityTimestamps timestamps{};
        g_api.TimestampsInit(&timestamps);
        g_api.TimestampsSetStart(
            &timestamps, static_cast<uint64_t>(g_matchStartTime));
        g_api.ActivitySetTimestamps(&activity, &timestamps);
        g_api.TimestampsDrop(&timestamps);
    }

    if (showParty) {
        Discord_String partyId = DStr(g_activityPartyId);
        Discord_ActivityParty party{};
        g_api.PartyInit(&party);
        g_api.PartySetId(&party, partyId);
        g_api.PartySetCurrentSize(
            &party, hostWaiting ? 1 : (state.members >= 1 ? state.members : 1));
        g_api.PartySetMaxSize(&party, 2);
        g_api.PartySetPrivacy(&party, Discord_ActivityPartyPrivacy_Public);
        g_api.ActivitySetParty(&activity, &party);
        g_api.PartyDrop(&party);
    }

    if (showSecret) {
        Discord_String secret = DStr(g_activitySecret);
        Discord_ActivitySecrets secrets{};
        g_api.SecretsInit(&secrets);
        g_api.SecretsSetJoin(&secrets, secret);
        g_api.ActivitySetSecrets(&activity, &secrets);
        g_api.SecretsDrop(&secrets);
        g_api.ActivitySetSupportedPlatforms(
            &activity, Discord_ActivityGamePlatforms_Desktop);
    }

    Discord_ActivityAssets assets{};
    g_api.AssetsInit(&assets);
    Discord_String assetKey = DStr(kAssetKey);
    Discord_String assetText = DStr(kAssetLargeText);
    g_api.AssetsSetLargeImage(&assets, &assetKey);
    g_api.AssetsSetLargeText(&assets, &assetText);
    g_api.ActivitySetAssets(&activity, &assets);
    g_api.AssetsDrop(&assets);

    g_api.ClientUpdateRichPresence(&g_client, &activity,
                                   IgnoreResultCb, nullptr, nullptr);
    g_api.ActivityDrop(&activity);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

} // namespace

bool DiscordRichPresence_IsAvailable()
{
    EnsureLoaded();
    return g_available;
}

int DiscordRichPresence_GetMode()
{
    EnsureLoaded();
    // Raw stored 0..3 even when the DLL is missing, so the modal can pre-check
    // tier 4 (ranked) without discord_partner_sdk.dll.
    return ReadModeFromIni();
}

void DiscordRichPresence_SetMode(int mode)
{
    EnsureLoaded();
    if (mode < 0 || mode > 3) return;
    if ((mode == 1 || mode == 2) && !g_available) {
        HookLog("Discord: mode %d needs discord_partner_sdk.dll; not enabled\n", mode);
        return;
    }

    if (mode == 2 && !g_consentAccepted && !ConfirmFullConsent()) {
        HookLog("Discord: Full mode not enabled (consent declined)\n");
        return; // nothing persisted; the mode stays at its previous value
    }
    if (mode == 3) {
        // Tier 4 consent + hosted link live in ranked_account; persist only
        // when the link flow actually starts (false = declined / Steam down).
        if (!RankedAccount_SetEnabled(true)) {
            HookLog("Discord: ranked mode not enabled (link declined/unavailable)\n");
            return; // nothing persisted; the mode stays at its previous value
        }
    }
    if (!WriteModeToIni(mode)) {
        HookLog("Discord: failed to save mode %d to Timestone.ini\n", mode);
        return;
    }
    const bool leavingTier4 = (g_desiredMode == 3 && mode != 3);
    g_desiredMode = mode;
    if (leavingTier4) RankedAccount_SetEnabled(false); // stop any in-flight link
    HookLog("Discord: online mode set to %d\n", mode);
}

void DiscordRichPresence_Init()
{
    EnsureLoaded();
    if (!g_available) return;
    g_desiredMode = ReadModeFromIni();
    HookLog("Discord: rich presence mode %d\n", g_desiredMode);
}

void DiscordRichPresence_Pump()
{
    if (!g_available) return;

    ++g_pumpCounter;
    // Cheap ini re-read (every ~4 s at 60 fps) so an external edit to
    // Timestone.ini takes effect without a restart.
    if (g_pumpCounter % 240 == 0) {
        const int iniMode = ReadModeFromIni();
        if (iniMode != g_desiredMode) {
            g_desiredMode = iniMode;
            HookLog("Discord: mode changed to %d via Timestone.ini\n", iniMode);
        }
    }

    // Tier 4 (ranked) runs the Discord SDK off: its stored mode maps to an
    // effective 0 here so the presence client never starts under it. All the
    // driver logic below uses the effective value, never the stored mode.
    const int g_sdkMode = (g_desiredMode == 3) ? 0 : g_desiredMode;

    // Drive the client to match the effective SDK mode.
    if (g_activeMode != g_sdkMode) {
        if (g_clientActive) {
            StopClient();
        } else if (g_sdkMode != 0) {
            StartClient(g_sdkMode);
        }
    }
    if (!g_clientActive) return;

    g_api.RunCallbacks();

    // Reconnect when the status callback reported an error.
    if (!g_ready && g_lastError != Discord_Client_Error_None) {
        const time_t now = time(nullptr);
        if (now >= g_nextConnectRetry) {
            g_api.ClientConnect(&g_client);
            g_nextConnectRetry = now + kConnectRetrySeconds;
            Discord_String errorText{};
            g_api.ClientErrorToString(g_lastError, &errorText);
            const std::string text = CopyStr(errorText);
            if (text != g_lastErrorText) {
                g_lastErrorText = text;
                HookLog("Discord: reconnect attempt (error %s)\n",
                        text.empty() ? "unknown" : text.c_str());
            }
        }
    }

    // Full mode: run the OAuth browser flow once per client start.
    if (g_sdkMode == 2 && g_authPending) AdvanceOAuth();
    if (!g_clientActive || !g_ready) return;

    // Evaluate presence state and push on change (rate-limited to 15 s).
    PresenceState state = ReadPresenceState();
    UpdateMatchStartEdge(state);
    const std::string fingerprint = BuildFingerprint(state);
    if (fingerprint != g_lastFingerprint) {
        g_lastFingerprint = fingerprint;
        g_presenceDirty = true;
    }
    if (g_presenceDirty) {
        const time_t now = time(nullptr);
        if (now - g_lastPushTime >= kPresencePushIntervalSeconds) {
            g_presenceDirty = false;
            g_lastPushTime = now;
            PushPresence(state);
        }
    }

    ConsumePendingJoin();
}
