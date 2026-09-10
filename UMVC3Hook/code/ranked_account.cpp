#include "ranked_account.h"

#include "../IniSafe.h"
#include "launchmenu_utils.h"
#include "steam_lobby.h"

#include <Windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern void HookLog(const char* fmt, ...);

#ifndef UMVC3HOOK_VERSION
#define UMVC3HOOK_VERSION "0.0.0"
#endif

namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr const char* kRankedBaseUrl = "https://mvc3-ranked.hero-rehab.xyz"; // prod
constexpr const char* kRankedLinkPath = "/link";                    // browser page
constexpr const char* kRankedStatusPath = "/link/status";
constexpr const char* kRankedCompletePath = "/link/complete";
constexpr int kPollIntervalMs = 2000; // poll cadence on the background thread
constexpr int kPollMaxAttempts = 240; // 8 minutes (240 x 2 s) then give up
constexpr const char* kStateHex = "0123456789abcdef";

// WinHTTP constants (kept local so the module has no compile-time dependency
// on <winhttp.h>; winhttp.dll is resolved at runtime).
constexpr DWORD kHttpAccessTypeDefaultProxy = 0;
constexpr DWORD kHttpFlagSecure = 0x00800000; // WINHTTP_FLAG_SECURE
constexpr DWORD kHttpQueryStatusCode = 19;    // WINHTTP_QUERY_STATUS_CODE
constexpr DWORD kHttpQueryFlagNumber = 0x10000000;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

enum class RankedState { Idle, Linking, Linked };

std::atomic<RankedState> g_state{RankedState::Idle};
std::atomic<bool> g_workerActive{false};
HANDLE g_stopEvent = nullptr;   // manual-reset; created once, never closed
uint64_t g_pumpCounter = 0;

// Snapshot for the in-flight attempt (written by the game thread before the
// worker starts; copied into worker locals on entry so a later attempt cannot
// disturb a running one).
std::string g_attemptBase;
std::string g_attemptState;
uint64_t g_attemptSteamId = 0;

// ---------------------------------------------------------------------------
// Runtime-linked WinHTTP surface (called from the poll thread only)
// ---------------------------------------------------------------------------

struct WinHttpApi {
    HMODULE module = nullptr;
    bool attempted = false;
    bool ok = false;

    void* (*Open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) = nullptr;
    void* (*Connect)(void*, LPCWSTR, unsigned short, DWORD) = nullptr;
    void* (*OpenRequest)(void*, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR,
                         LPCWSTR*, DWORD) = nullptr;
    bool (*SendRequest)(void*, LPCWSTR, DWORD, void*, DWORD, DWORD,
                        uintptr_t) = nullptr;
    bool (*ReceiveResponse)(void*, void*) = nullptr;
    bool (*QueryDataAvailable)(void*, DWORD*) = nullptr;
    bool (*ReadData)(void*, void*, DWORD, DWORD*) = nullptr;
    bool (*QueryHeaders)(void*, DWORD, LPCWSTR, void*, DWORD*, DWORD*) = nullptr;
    bool (*SetTimeouts)(void*, int, int, int, int) = nullptr;
    bool (*CloseHandle)(void*) = nullptr;
};

WinHttpApi g_http;

template <typename T>
bool ResolveHttp(T& target, const char* name)
{
    target = reinterpret_cast<T>(GetProcAddress(g_http.module, name));
    if (!target) HookLog("Ranked: missing winhttp export %s\n", name);
    return target != nullptr;
}

bool EnsureHttpLoaded()
{
    if (g_http.attempted) return g_http.ok;
    g_http.attempted = true;
    g_http.module = LoadLibraryA("winhttp.dll");
    if (!g_http.module) {
        HookLog("Ranked: winhttp.dll not found; ranked link unavailable\n");
        return false;
    }
    bool ok = true;
    ok &= ResolveHttp(g_http.Open, "WinHttpOpen");
    ok &= ResolveHttp(g_http.Connect, "WinHttpConnect");
    ok &= ResolveHttp(g_http.OpenRequest, "WinHttpOpenRequest");
    ok &= ResolveHttp(g_http.SendRequest, "WinHttpSendRequest");
    ok &= ResolveHttp(g_http.ReceiveResponse, "WinHttpReceiveResponse");
    ok &= ResolveHttp(g_http.QueryDataAvailable, "WinHttpQueryDataAvailable");
    ok &= ResolveHttp(g_http.ReadData, "WinHttpReadData");
    ok &= ResolveHttp(g_http.QueryHeaders, "WinHttpQueryHeaders");
    ok &= ResolveHttp(g_http.SetTimeouts, "WinHttpSetTimeouts");
    ok &= ResolveHttp(g_http.CloseHandle, "WinHttpCloseHandle");
    g_http.ok = ok;
    if (!ok) {
        HookLog("Ranked: winhttp required exports missing; ranked link unavailable\n");
        return false;
    }
    HookLog("Ranked: WinHTTP loaded\n");
    return true;
}

std::string LastErrorText()
{
    const DWORD error = GetLastError();
    if (!error) return "unknown error";
    char buffer[512] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, buffer, sizeof(buffer), nullptr);
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' ||
                       buffer[len - 1] == '.'))
        buffer[--len] = '\0';
    return buffer;
}

std::wstring ToWide(const char* text)
{
    if (!text || !text[0]) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring wide;
    wide.resize(static_cast<size_t>(count) - 1);
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &wide[0], count);
    return wide;
}

// ---------------------------------------------------------------------------
// Ini helpers
// ---------------------------------------------------------------------------

std::string GetIniPath()
{
    char path[MAX_PATH] = {};
    LaunchMenuUtils::GetSettingsPath(path, sizeof(path));
    return path;
}

std::string ReadBaseOverride()
{
    const std::string path = GetIniPath();
    char value[512] = {};
    GetPrivateProfileStringA("Settings", "RankedBaseUrl", "", value,
                             sizeof(value), path.c_str());
    if (value[0]) return value;
    return kRankedBaseUrl;
}

// ---------------------------------------------------------------------------
// Tiny HTTP client (background thread only)
// ---------------------------------------------------------------------------

struct HttpResponse {
    int status = 0; // 0 = transport failure
    std::string body;
    std::string error;
};

// base is a full origin ("https://mvc3-ranked.hero-rehab.xyz" or an override
// "http://127.0.0.1:8787"); pathAndQuery is e.g. "/link/status?state=abc".
// Every WinHTTP handle is created and destroyed inside this call.
void HttpSend(const std::string& base, const char* method,
              const std::string& pathAndQuery, const std::string& body,
              HttpResponse& out)
{
    std::string origin = base;
    while (!origin.empty() && origin.back() == '/') origin.pop_back();

    bool secure = false;
    std::string host;
    unsigned short port = 80;
    const size_t schemeEnd = origin.find("://");
    if (schemeEnd == std::string::npos) {
        out.error = "invalid RankedBaseUrl (no scheme)";
        return;
    }
    const std::string scheme = origin.substr(0, schemeEnd);
    if (scheme == "https") {
        secure = true;
        port = 443;
    } else if (scheme != "http") {
        out.error = "invalid RankedBaseUrl scheme";
        return;
    }
    const size_t hostStart = schemeEnd + 3;
    size_t hostEnd = origin.find('/', hostStart);
    if (hostEnd == std::string::npos) hostEnd = origin.size();
    const std::string hostAndPort = origin.substr(hostStart, hostEnd - hostStart);
    const size_t colon = hostAndPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostAndPort.substr(0, colon);
        const int parsedPort = atoi(hostAndPort.c_str() + colon + 1);
        if (parsedPort > 0 && parsedPort < 65536) port = static_cast<unsigned short>(parsedPort);
    } else {
        host = hostAndPort;
    }
    if (host.empty()) {
        out.error = "invalid RankedBaseUrl (empty host)";
        return;
    }

    const std::wstring wideHost = ToWide(host.c_str());
    const std::wstring widePath = ToWide(pathAndQuery.c_str());
    const std::wstring wideMethod = ToWide(method);
    if (wideHost.empty() || widePath.empty()) {
        out.error = "RankedBaseUrl contains non-ASCII characters";
        return;
    }

    void* session = g_http.Open(L"Timestone", kHttpAccessTypeDefaultProxy,
                                nullptr, nullptr, 0);
    if (!session) {
        out.error = "WinHttpOpen: " + LastErrorText();
        return;
    }
    if (g_http.SetTimeouts)
        g_http.SetTimeouts(session, 3000, 3000, 3000, 3000);

    void* connection = g_http.Connect(session, wideHost.c_str(), port, 0);
    if (!connection) {
        out.error = "WinHttpConnect: " + LastErrorText();
        g_http.CloseHandle(session);
        return;
    }
    static LPCWSTR kAcceptTypes[] = { L"*/*", nullptr };
    void* request = g_http.OpenRequest(
        connection, wideMethod.c_str(), widePath.c_str(), nullptr, nullptr,
        kAcceptTypes, secure ? kHttpFlagSecure : 0);
    if (!request) {
        out.error = "WinHttpOpenRequest: " + LastErrorText();
        g_http.CloseHandle(connection);
        g_http.CloseHandle(session);
        return;
    }

    const bool isPost = body.size() > 0;
    LPCWSTR headers = isPost ? L"Content-Type: application/json" : nullptr;
    void* optionalData = isPost ? const_cast<char*>(body.data()) : nullptr;
    const DWORD optionalLength = isPost ? static_cast<DWORD>(body.size()) : 0;
    if (!g_http.SendRequest(request, headers,
                            headers ? static_cast<DWORD>(-1L) : 0,
                            optionalData, optionalLength, optionalLength, 0)) {
        out.error = "WinHttpSendRequest: " + LastErrorText();
        g_http.CloseHandle(request);
        g_http.CloseHandle(connection);
        g_http.CloseHandle(session);
        return;
    }
    if (!g_http.ReceiveResponse(request, nullptr)) {
        out.error = "WinHttpReceiveResponse: " + LastErrorText();
        g_http.CloseHandle(request);
        g_http.CloseHandle(connection);
        g_http.CloseHandle(session);
        return;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!g_http.QueryHeaders(request, kHttpQueryStatusCode | kHttpQueryFlagNumber,
                             nullptr, &status, &statusSize, nullptr)) {
        out.error = "WinHttpQueryHeaders: " + LastErrorText();
        g_http.CloseHandle(request);
        g_http.CloseHandle(connection);
        g_http.CloseHandle(session);
        return;
    }
    out.status = static_cast<int>(status);

    char buffer[4096];
    DWORD available = 0;
    while (g_http.QueryDataAvailable(request, &available) && available > 0) {
        DWORD toRead = available < sizeof(buffer)
                           ? available
                           : static_cast<DWORD>(sizeof(buffer));
        DWORD readBytes = 0;
        if (!g_http.ReadData(request, buffer, toRead, &readBytes) ||
            readBytes == 0)
            break;
        out.body.append(buffer, readBytes);
        if (available > readBytes) available -= readBytes;
    }

    g_http.CloseHandle(request);
    g_http.CloseHandle(connection);
    g_http.CloseHandle(session);
}

// ---------------------------------------------------------------------------
// State hex + attempt launch
// ---------------------------------------------------------------------------

std::string GenerateStateHex()
{
    uint64_t r = static_cast<uint64_t>(GetTickCount()) ^
                 (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 16; ++i) {
        r = r * 6364136223846793005ull + 1442695040888963407ull;
        out.push_back(kStateHex[(r >> 60) & 0xF]);
    }
    return out;
}

bool StatusIsLinked(const std::string& body)
{
    // Tolerates any JSON whitespace between the key, colon, and value tokens
    // (the dev stub and the real service may format differently).
    const char* text = body.c_str();
    const char* key = strstr(text, "\"status\"");
    if (!key) return false;
    const char* cursor = key + strlen("\"status\"");
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
        ++cursor;
    if (*cursor != ':') return false;
    ++cursor;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
        ++cursor;
    return strncmp(cursor, "\"linked\"", strlen("\"linked\"")) == 0;
}

// ---------------------------------------------------------------------------
// Poll thread
// ---------------------------------------------------------------------------

DWORD WINAPI RankedPollThread(void*)
{
    // Snapshot the attempt so a later (or concurrent) mode change cannot
    // disturb an in-flight poll.
    const std::string base = g_attemptBase;
    const std::string state = g_attemptState;
    const uint64_t steamId = g_attemptSteamId;

    bool completed = false;
    std::string lastPollError;
    int attempt = 0;
    for (; attempt < kPollMaxAttempts; ++attempt) {
        if (WaitForSingleObject(g_stopEvent, kPollIntervalMs) == WAIT_OBJECT_0)
            break; // stopped: SetEnabled(false) or a sibling process linked

        HttpResponse poll;
        const std::string statusPath =
            std::string(kRankedStatusPath) + "?state=" + state;
        HttpSend(base, "GET", statusPath, "", poll);
        if (!poll.status) {
            if (poll.error != lastPollError) {
                lastPollError = poll.error;
                HookLog("Ranked: link status poll error (%s)\n", poll.error.c_str());
            }
            continue; // transport error -> keep polling
        }
        if (poll.status >= 300) {
            HookLog("Ranked: link status HTTP %d\n", poll.status);
            continue; // server unhappy -> keep polling
        }
        if (StatusIsLinked(poll.body)) {
            // The single completion request. The service now knows this
            // Steam account is linked; only a 2xx reply lets us persist.
            char completeBody[256];
            snprintf(completeBody, sizeof(completeBody),
                     "{\"app\":\"umvc3\",\"build\":%s,"
                     "\"steam_id\":%llu,\"state\":\"%s\"}",
                     "\"" UMVC3HOOK_VERSION "\"",
                     static_cast<unsigned long long>(steamId), state.c_str());
            HttpResponse complete;
            HttpSend(base, "POST", kRankedCompletePath, completeBody, complete);
            if (!complete.status) {
                HookLog("Ranked: link complete request failed (%s)\n",
                        complete.error.c_str());
                break; // no retry loop; reselecting tier 4 restarts
            }
            if (complete.status < 200 || complete.status >= 300) {
                HookLog("Ranked: link complete rejected (HTTP %d)\n",
                        complete.status);
                break;
            }
            completed = true;
            break;
        }
        // "pending" (or an unexpected body) -> keep polling
    }

    if (completed) {
        const std::string path = GetIniPath();
        if (IniSafe_WriteString("Settings", "RankedLinked", "1", path.c_str())) {
            g_state.store(RankedState::Linked);
            HookLog("Ranked: account linked and saved\n");
        } else {
            HookLog("Ranked: linked but failed to save RankedLinked\n");
        }
    } else if (attempt >= kPollMaxAttempts) {
        HookLog("Ranked: link timed out; reselect the mode to retry\n");
        g_state.store(RankedState::Idle);
    } else {
        // Stopped: SetEnabled(false) already set Idle, or a sibling process
        // completed the link and Pump set Linked. Leave the state as set.
    }
    g_workerActive.store(false);
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void RankedAccount_Init()
{
    // Intentionally a no-op: a boot-time OnlineMode of 3 must never open a
    // browser or start a poll. The link only begins on an explicit tier-4
    // selection that passes through RankedAccount_SetEnabled(true).
}

bool RankedAccount_IsLinked()
{
    const std::string path = GetIniPath();
    return GetPrivateProfileIntA("Settings", "RankedLinked", 0, path.c_str()) == 1;
}

bool RankedAccount_SetEnabled(bool on)
{
    if (!on) {
        // Leaving tier 4 stops any in-flight link poll. A completed
        // RankedLinked flag is kept so re-entering tier 4 is silent.
        if (g_workerActive.load() && g_stopEvent) SetEvent(g_stopEvent);
        g_state.store(RankedState::Idle);
        return true;
    }

    if (RankedAccount_IsLinked()) {
        g_state.store(RankedState::Linked);
        return true; // already linked in a past session: silent, no browser
    }
    if (g_workerActive.load()) return true; // a link attempt is already running

    const std::string base = ReadBaseOverride();
    char consentText[512];
    snprintf(consentText, sizeof(consentText),
             "Ranked matchmaking links your Steam and Discord accounts. This "
             "will open your browser to %s to finish linking.\n\nContinue?",
             base.c_str());
    const int consent = MessageBoxA(
        nullptr, consentText, "Timestone",
        MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
    if (consent != IDOK) {
        HookLog("Ranked: link consent declined\n");
        return false;
    }

    const uint64_t steamId = SteamLobby_GetLocalSteamId();
    if (!steamId) {
        HookLog("Ranked: Steam not initialized; launch the game through Steam\n");
        return false;
    }
    if (!EnsureHttpLoaded()) return false;

    const std::string state = GenerateStateHex();

    char url[512];
    snprintf(url, sizeof(url), "%s%s?app=umvc3&steam_id=%llu&state=%s",
             base.c_str(), kRankedLinkPath,
             static_cast<unsigned long long>(steamId), state.c_str());
    const INT_PTR result = reinterpret_cast<INT_PTR>(
        ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        HookLog("Ranked: failed to open the link page (error %lld)\n",
                static_cast<long long>(result));
        return false;
    }
    HookLog("Ranked: opened %s for account linking\n", base.c_str());

    if (!g_stopEvent) {
        g_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!g_stopEvent) {
            HookLog("Ranked: could not create the stop event (%lu)\n", GetLastError());
            return false;
        }
    }
    ResetEvent(g_stopEvent);

    g_attemptBase = base;
    g_attemptState = state;
    g_attemptSteamId = steamId;
    g_workerActive.store(true);
    g_state.store(RankedState::Linking);
    if (!CreateThread(nullptr, 0, RankedPollThread, nullptr, 0, nullptr)) {
        HookLog("Ranked: failed to start the link poll thread (%lu)\n", GetLastError());
        g_workerActive.store(false);
        g_state.store(RankedState::Idle);
        return false;
    }
    return true;
}

void RankedAccount_Pump()
{
    if (!g_workerActive.load()) return;
    // Re-check the ini every ~2 s (120 frames): a second game instance
    // (dual training) shares the file and may have completed the link first.
    if (++g_pumpCounter % 120 != 0) return;
    if (RankedAccount_IsLinked()) {
        if (g_stopEvent) SetEvent(g_stopEvent);
        g_state.store(RankedState::Linked);
    }
}
