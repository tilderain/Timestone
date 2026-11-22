#include "discord_presence.h"

#include "LaunchMenu.h"
#include "discord_presence_format.h"
#include "launchmenu_utils.h"
#include "lobby_flow.h"
#include "netplay.h"
#include "steam_lobby.h"
#include "../IniSafe.h"

#include <Windows.h>
#include <atomic>
#include <ctime>
#include <cstring>
#include <mutex>
#include <string>

extern void HookLog(const char* fmt, ...);

namespace {

const char* const kClientId = "1544202390536982610";
const char* const kSection = "Discord";
const char* const kKey = "RichPresence";
const DWORD kPublishIntervalMs = 5000; // Discord takes 5 updates per 20 s
const DWORD kReadyTimeoutMs = 10000;
const DWORD kRetryMs = 15000;
const DWORD kMaxFrame = 64 * 1024;
const int kTickDivider = 30; // snapshot every 30 game ticks

std::atomic<int> g_level{-1}; // -1 = not read from Timestone.ini yet
std::atomic<HANDLE> g_wake{nullptr};
std::atomic<bool> g_workerRunning{false};
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_refused{false}; // the client refused presence this session
std::mutex g_startMutex;
std::mutex g_snapMutex;
PresenceSnapshot g_snapshot;

void SettingsPath(char* path, size_t size)
{
    path[0] = '\0';
    LaunchMenuUtils::GetSettingsPath(path, size);
}

void EnsureWorker();

uint32_t ReadLE32(const char* bytes)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(bytes[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(bytes[3])) << 24);
}

bool ReadExact(HANDLE pipe, char* buffer, DWORD size)
{
    DWORD done = 0;
    while (done < size) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, buffer + done, size - done, &chunk, nullptr) || chunk == 0)
            return false;
        done += chunk;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const std::string& data)
{
    DWORD sent = 0;
    return WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &sent, nullptr) != 0 &&
           sent == data.size();
}

// HKCU\Software\Classes\discord-<id> is what the client runs for a Join, so it
// starts the game the same way the Steam invite does.
void RegisterLaunchProtocol()
{
    wchar_t steamExe[2048] = {};
    DWORD size = sizeof(steamExe);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe", RRF_RT_REG_SZ,
                     nullptr, steamExe, &size) != ERROR_SUCCESS ||
        !steamExe[0]) {
        HookLog("Discord: Steam path not found; Join cannot start the game\n");
        return;
    }

    wchar_t command[2200] = {};
    swprintf(command, sizeof(command) / sizeof(command[0]), L"\"%ls\" steam://rungameid/357190",
             steamExe);

    const wchar_t* key = L"Software\\Classes\\discord-1544202390536982610";
    wchar_t current[2200] = {};
    DWORD currentSize = sizeof(current);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Classes\\discord-1544202390536982610\\shell\\open\\command",
                     nullptr, RRF_RT_REG_SZ, nullptr, current, &currentSize) == ERROR_SUCCESS &&
        wcscmp(current, command) == 0)
        return;

    HKEY root = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_WRITE, nullptr, &root, nullptr) !=
        ERROR_SUCCESS) {
        HookLog("Discord: could not register the Join protocol\n");
        return;
    }
    const wchar_t* description = L"URL:Run game 1544202390536982610 protocol";
    bool ok = RegSetKeyValueW(root, nullptr, nullptr, REG_SZ, description,
                              static_cast<DWORD>((wcslen(description) + 1) * sizeof(wchar_t))) ==
                  ERROR_SUCCESS &&
              RegSetKeyValueW(root, nullptr, L"URL Protocol", REG_SZ, L"", 0) == ERROR_SUCCESS;

    HKEY commandKey = nullptr;
    if (ok && RegCreateKeyExW(root, L"shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr,
                              &commandKey, nullptr) == ERROR_SUCCESS) {
        ok = RegSetKeyValueW(commandKey, nullptr, nullptr, REG_SZ, command,
                             static_cast<DWORD>((wcslen(command) + 1) * sizeof(wchar_t))) ==
             ERROR_SUCCESS;
        RegCloseKey(commandKey);
    } else {
        ok = false;
    }
    RegCloseKey(root);
    HookLog(ok ? "Discord: registered the Join protocol\n"
               : "Discord: could not register the Join command\n");
}

// One pipe to the desktop client, and the loop that keeps it current.
struct PresenceWorker {
    HANDLE pipe = nullptr;
    int state = 0; // 0 closed, 1 awaiting READY, 2 ready
    ULONGLONG readyDeadline = 0;
    ULONGLONG nextConnect = 0;
    ULONGLONG lastSend = 0;
    uint32_t nonce = 0;
    std::string lastSent;
    bool sentOnce = false;
    bool missingLogged = false;
    long long lastErrorCode = 0;
    long long lastCloseCode = 0;

    void Refuse()
    {
        HookLog("Discord refused presence without sign-in; presence is off for this session\n");
        g_refused.store(true);
    }

    void Close()
    {
        if (pipe) CloseHandle(pipe);
        pipe = nullptr;
        state = 0;
    }

    void Send(const std::string& frame)
    {
        if (pipe && !WriteAll(pipe, frame)) {
            Close();
            nextConnect = GetTickCount64() + kRetryMs;
        }
    }

    void Subscribe(const char* evt) { Send(DiscordPresence_Frame(1, DiscordPresence_SubscribeJson(evt, ++nonce))); }

    void Connect(ULONGLONG now)
    {
        for (int i = 0; i < 10 && !pipe; ++i) {
            wchar_t name[64] = {};
            swprintf(name, sizeof(name) / sizeof(name[0]), L"\\\\?\\pipe\\discord-ipc-%d", i);
            HANDLE handle = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                        OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE) pipe = handle;
        }
        if (!pipe) {
            if (!missingLogged) {
                HookLog("Discord: desktop app not found; presence waits for it\n");
                missingLogged = true;
            }
            nextConnect = now + kRetryMs;
            return;
        }
        missingLogged = false;
        state = 1;
        readyDeadline = now + kReadyTimeoutMs;
        lastSent.clear();
        sentOnce = false;
        Send(DiscordPresence_Frame(0, DiscordPresence_HandshakeJson(kClientId)));
    }

    void HandleDispatch(const std::string& body);

    // Reads whatever the client has sent; false when the pipe went away.
    void PumpInput(ULONGLONG now)
    {
        for (;;) {
            // Peek the header and read only whole frames, so a slow client
            // never leaves the worker blocked in ReadFile (Shutdown cannot
            // interrupt it).
            char header[8] = {};
            DWORD peeked = 0, available = 0;
            if (!PeekNamedPipe(pipe, header, sizeof(header), &peeked, &available, nullptr)) {
                Close();
                nextConnect = now + kRetryMs;
                return;
            }
            if (peeked < sizeof(header)) return;
            const uint32_t opcode = ReadLE32(header);
            const uint32_t length = ReadLE32(header + 4);
            if (length > kMaxFrame) {
                HookLog("Discord: oversized frame, dropping the connection\n");
                Close();
                nextConnect = now + kRetryMs;
                return;
            }
            if (available < sizeof(header) + length) return;
            if (!ReadExact(pipe, header, sizeof(header))) {
                Close();
                nextConnect = now + kRetryMs;
                return;
            }
            std::string body(length, '\0');
            if (length && !ReadExact(pipe, &body[0], length)) {
                Close();
                nextConnect = now + kRetryMs;
                return;
            }
            if (opcode == 1)
                HandleDispatch(body);
            else if (opcode == 2) {
                long long code = 0;
                DiscordPresence_JsonInt(body, "code", code);
                std::string message;
                DiscordPresence_JsonString(body, "message", message);
                if (code != lastCloseCode) {
                    lastCloseCode = code;
                    HookLog("Discord: connection closed (%lld): %s\n", code, message.c_str());
                }
                if (code == 4006) Refuse();
                Close();
                nextConnect = now + kRetryMs;
                return;
            } else if (opcode == 3) {
                Send(DiscordPresence_Frame(4, body));
            }
        }
    }

    void Publish(ULONGLONG now)
    {
        PresenceSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(g_snapMutex);
            snapshot = g_snapshot;
        }
        const std::string json = DiscordPresence_ActivityJson(snapshot);
        if (sentOnce && json == lastSent) return;
        if (!sentOnce && json.empty()) return;
        if (sentOnce && now - lastSend < kPublishIntervalMs) return;
        Send(DiscordPresence_Frame(
            1, DiscordPresence_SetActivityJson(json, GetCurrentProcessId(), ++nonce)));
        lastSent = json;
        lastSend = now;
        sentOnce = true;
    }

    void Run()
    {
        RegisterLaunchProtocol();
        while (!g_shutdown.load() && DiscordPresence_GetLevel() == DiscordPresenceLevel::Basic &&
               !g_refused.load()) {
            const ULONGLONG now = GetTickCount64();
            if (!pipe && now >= nextConnect) Connect(now);
            if (pipe) PumpInput(now);
            if (pipe && state == 2) Publish(now);
            if (pipe && state == 1 && now >= readyDeadline) {
                HookLog("Discord: handshake timed out\n");
                Close();
                nextConnect = now + kRetryMs;
            }
            if (g_shutdown.load()) break;
            if (HANDLE wake = g_wake.load())
                WaitForSingleObject(wake, 250);
            else
                Sleep(250);
        }

        if (pipe && state == 2) {
            // Leaving with an activity set would leave it on the profile.
            Send(DiscordPresence_Frame(
                1, DiscordPresence_SetActivityJson("", GetCurrentProcessId(), ++nonce)));
        }
        Close();
        g_workerRunning.store(false);
    }
};

void PresenceWorker::HandleDispatch(const std::string& body)
{
    // Command replies nest the subscribed event under data.evt, so events are
    // only taken from DISPATCH messages; errors come back on the failed command.
    std::string cmd;
    std::string event;
    if (!DiscordPresence_JsonString(body, "cmd", cmd)) return;
    if (!DiscordPresence_JsonString(body, "evt", event)) return;
    const bool dispatch = cmd == "DISPATCH";

    if (dispatch && event == "READY") {
        state = 2;
        lastSent.clear();
        sentOnce = false;
        Subscribe("ACTIVITY_JOIN");
        Subscribe("ACTIVITY_JOIN_REQUEST");
        HookLog("Discord: connected\n");
        return;
    }

    if (event == "ERROR") {
        long long code = 0;
        if (!DiscordPresence_JsonInt(body, "code", code)) return;
        std::string message;
        DiscordPresence_JsonString(body, "message", message);
        if (code != lastErrorCode) {
            lastErrorCode = code;
            HookLog("Discord: error %lld on %s: %s\n", code, cmd.c_str(), message.c_str());
        }
        // Only a refused SET_ACTIVITY ends presence; a refused join reply or
        // subscription leaves a working presence alone.
        if (code == 4006 && cmd == "SET_ACTIVITY") Refuse();
        return;
    }

    if (dispatch && event == "ACTIVITY_JOIN") {
        std::string secret;
        if (!DiscordPresence_JsonString(body, "secret", secret)) return;
        if (SteamLobby_QueueJoinConnectString(secret.c_str()))
            HookLog("Discord: joining a lobby from Discord\n");
        else
            HookLog("Discord: ignored an invalid join\n");
        return;
    }

    if (dispatch && event == "ACTIVITY_JOIN_REQUEST") {
        const std::size_t user = body.find("\"user\"");
        std::string id;
        if (user == std::string::npos || !DiscordPresence_JsonString(body, "id", id, user)) return;
        if (id.empty() || id.size() > 20) return;
        for (char c : id)
            if (c < '0' || c > '9') return;

        PresenceSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(g_snapMutex);
            snapshot = g_snapshot;
        }
        const bool accept = DiscordPresence_IsJoinable(snapshot);
        Send(DiscordPresence_Frame(1, DiscordPresence_JoinReplyJson(id, accept, ++nonce)));
        HookLog("Discord: join request %s\n", accept ? "accepted" : "refused (the room is not joinable)");
    }
}

DWORD WINAPI WorkerThread(LPVOID)
{
    PresenceWorker worker;
    worker.Run();
    return 0;
}

void EnsureWorker()
{
    if (g_shutdown.load() || g_refused.load()) return;
    if (DiscordPresence_GetLevel() != DiscordPresenceLevel::Basic) return;

    std::lock_guard<std::mutex> lock(g_startMutex);
    if (g_workerRunning.load()) return;

    if (!g_wake.load()) {
        HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake) {
            HookLog("Discord: could not create the presence event\n");
            return;
        }
        g_wake.store(wake);
    }

    // Set before the thread exists so a second tick cannot spawn another one.
    g_workerRunning.store(true);
    HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!thread) {
        g_workerRunning.store(false);
        HookLog("Discord: could not start the presence thread\n");
        return;
    }
    CloseHandle(thread);
}

} // namespace

DiscordPresenceLevel DiscordPresence_GetLevel()
{
    const int loaded = g_level.load(std::memory_order_acquire);
    if (loaded >= 0) return loaded ? DiscordPresenceLevel::Basic : DiscordPresenceLevel::Off;

    char path[MAX_PATH] = {};
    SettingsPath(path, sizeof(path));
    const int value = GetPrivateProfileIntA(kSection, kKey, 0, path) == 1 ? 1 : 0;
    int expected = -1;
    g_level.compare_exchange_strong(expected, value);
    return g_level.load(std::memory_order_acquire) ? DiscordPresenceLevel::Basic
                                                   : DiscordPresenceLevel::Off;
}

bool DiscordPresence_SetLevel(DiscordPresenceLevel level)
{
    if (level == DiscordPresence_GetLevel()) return true;

    char path[MAX_PATH] = {};
    SettingsPath(path, sizeof(path));
    if (!IniSafe_WriteString(kSection, kKey, level == DiscordPresenceLevel::Basic ? "1" : "0", path))
        return false;

    g_level.store(level == DiscordPresenceLevel::Basic ? 1 : 0, std::memory_order_release);
    if (HANDLE wake = g_wake.load()) SetEvent(wake);
    return true;
}

void DiscordPresence_GameTick()
{
    // Same reason as the lobby's own tick: cheap enough to run often, but the
    // presence has no use for every frame.
    static int counter = 0;
    if (++counter < kTickDivider) return;
    counter = 0;

    if (DiscordPresence_GetLevel() != DiscordPresenceLevel::Basic) return;

    PresenceSnapshot snapshot;
    if (LobbyFlow_IsEnabled())
        LobbyFlow_FillPresence(&snapshot);
    else if (GetLaunchOption() == LAUNCH_DUAL_TRAINING)
        snapshot.activity = Netplay_IsHost() ? PresenceActivity::DualTraining
                                             : PresenceActivity::None;
    else if (Netplay_IsActive())
        snapshot.activity = PresenceActivity::DirectMatch;
    else if (GetLaunchOption() == LAUNCH_TRAINING)
        snapshot.activity = PresenceActivity::Training;

    // A set's clock starts when the match does and restarts when the opponent
    // changes (rematch against the same player keeps counting, as the set does).
    static PresenceActivity previous = PresenceActivity::None;
    static char previousOpponent[kPresenceNameBytes] = {};
    static int64_t setStart = 0;
    const bool timed = snapshot.activity == PresenceActivity::InMatch ||
                       snapshot.activity == PresenceActivity::DirectMatch;
    if (timed) {
        if (snapshot.activity != previous || strcmp(snapshot.opponent, previousOpponent) != 0)
            setStart = static_cast<int64_t>(std::time(nullptr));
        snapshot.startUnix = setStart;
    } else {
        setStart = 0;
    }
    previous = snapshot.activity;
    snprintf(previousOpponent, sizeof(previousOpponent), "%s", snapshot.opponent);

    snprintf(snapshot.version, sizeof(snapshot.version), "%s", UMVC3HOOK_VERSION);

    {
        std::lock_guard<std::mutex> lock(g_snapMutex);
        g_snapshot = snapshot;
    }
    EnsureWorker();
}

void DiscordPresence_Shutdown()
{
    if (g_shutdown.exchange(true)) return;
    if (HANDLE wake = g_wake.load()) SetEvent(wake);
    // Never join the thread: this also runs under the loader lock.
    for (int i = 0; i < 50 && g_workerRunning.load(); ++i) Sleep(10);
}
