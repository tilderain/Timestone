#include "LaunchMenu.h"
#include "../IniSafe.h"
#include "LaunchMenu_compact.h"
#include "LaunchMenu_settings.h"
#include "assist_names.h"
#include "controller.h"
#include "launch_music.h"
#include "launchmenu_dark.h"
#include "trainingboot.h"
#include "netplay.h"
#include "savedata.h"
#include "steam_lobby.h"
#include "steam_transport.h"
#include "updater.h"
#include "umvc3.h"
#include <Windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// HookLog from dllmain.cpp
extern void HookLog(const char* fmt, ...);
extern void HookLogSetFile(const char* filename);
extern void HookLogFlush();

// ---------------------------------------------------------------------------
// Character and stage name tables
// ---------------------------------------------------------------------------
const char* kCharNames[] = {
    "None",
    "Ryu", "Chun-Li", "Akuma", "Chris", "Wesker",
    "Viewtiful Joe", "Dante", "Trish", "Frank West", "Spencer",
    "Arthur", "Amaterasu", "Zero", "Tron", "Morrigan",
    "Hsien-Ko", "Felicia", "C. Viper", "Haggar", "Jill",
    "Strider", "Vergil", "Phoenix Wright", "Firebrand", "Nemesis",
    "Spider-Man", "Captain America", "Wolverine", "Magneto", "Hulk",
    "She-Hulk", "Taskmaster", "Iron Man", "Thor", "Dr. Doom",
    "Phoenix", "Shuma-Gorath", "MODOK", "Dormammu", "Deadpool",
    "Storm", "Super-Skrull", "Sentinel", "X-23", "Nova",
    "Rocket Raccoon", "Ghost Rider", "Iron Fist", "Dr. Strange", "Hawkeye",
    "Galactus"
};
const int kCharNameCount = sizeof(kCharNames) / sizeof(kCharNames[0]);

const StageEntry kStages[] = {
    { "Training Room",            0   },
    { "Daily Bugle",              2   },
    { "Metro City",               3   },
    { "Demon Village",            4   },
    { "Kattlelox Island",         5   },
    { "Hand Hideout",             6   },
    { "S.H.I.E.L.D. Helicarrier", 7   },
    { "Asgard",                   9   },
    { "Tricell Labs",             10  },
    { "Fate of the World",        11  },
    { "Danger Room",              100 },
    { "City That Never Sleeps",   102 },
    { "Days of Future Past",      103 },
    { "Demon Village Redux",      104 },
    { "Bonne Wonderland",         105 },
    { "Shadowland",               106 },
    { "S.H.I.E.L.D. Air Show",   107 },
    { "Asgard: Sea of Space",     109 },
    { "Chaos at Tricell",         110 },
};
const int kStageCount = sizeof(kStages) / sizeof(kStages[0]);

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static LaunchOption g_launchOption = LAUNCH_NORMAL;
static LaunchDialogConfig g_dialogConfig = {
    {1, 1, 1},   // p1Chars (Ryu, Ryu, Ryu)
    {1, 1, 1},   // p2Chars (Ryu, Ryu, Ryu)
    {0, 0, 0},   // p1Colors
    {0, 0, 0},   // p2Colors
    {0, 0, 0},   // p1Assists (Alpha)
    {0, 0, 0},   // p2Assists (Alpha)
    {false, false}, // Simple Mode controls
    STAGE_BONNE_WONDERLAND,
    "",          // hostIp
    7777,       // hostPort
    false,       // hostEnabled
    nullptr,     // enetHost
    nullptr      // enetPeer
};
static bool      g_isHost = false;          // true if we clicked Host, false if Connect
static bool      g_localReady = false;
static bool      g_peerReady = false;
static bool      g_autoTestLaunch = false;
static COLORREF  g_statusColor = RGB(210, 210, 210);
static HWND      g_hChatWindow = nullptr;
static HWND      g_hChatHistory = nullptr;
static HWND      g_hChatInput = nullptr;
static HBITMAP   g_hChatAvatar = nullptr;
static uint64_t  g_chatAvatarSteamId = 0;
static WNDPROC   g_chatInputWndProc = nullptr;
static char      g_chatOpponentName[128] = "Opponent";
static ULONGLONG g_chatRateWindowStart = 0;
static unsigned  g_chatRateCount = 0;
static SavedRoster g_savedRosters[3] = {};
static size_t     g_savedRosterCount = 0;
static bool       g_savedRostersLoaded = false;

static COLORREF GetPulsingColor(COLORREF low, COLORREF high)
{
    constexpr ULONGLONG periodMs = 2400;
    constexpr double twoPi = 6.283185307179586;
    const double phase = static_cast<double>(GetTickCount64() % periodMs) /
                         static_cast<double>(periodMs) * twoPi;
    const double blend = (std::sin(phase) + 1.0) * 0.5;
    const auto channel = [blend](BYTE lowValue, BYTE highValue) {
        return static_cast<BYTE>(lowValue + (highValue - lowValue) * blend);
    };
    return RGB(channel(GetRValue(low), GetRValue(high)),
               channel(GetGValue(low), GetGValue(high)),
               channel(GetBValue(low), GetBValue(high)));
}

static uint64_t GetInviteLobbyId()
{
    const char* commandLine = GetCommandLineA();
    constexpr const char* option = "+connect_lobby";
    constexpr size_t optionLength = 14;

    for (const char* match = commandLine; (match = strstr(match, option)) != nullptr;
         match += optionLength) {
        bool tokenStart = match == commandLine || match[-1] == ' ' || match[-1] == '\t';
        const char* value = match + optionLength;
        if (!tokenStart || (*value != ' ' && *value != '\t')) continue;
        while (*value == ' ' || *value == '\t') ++value;

        char* end = nullptr;
        unsigned long long lobbyId = strtoull(value, &end, 10);
        if (end != value && lobbyId != 0 &&
            (*end == '\0' || *end == ' ' || *end == '\t' || *end == '"'))
            return static_cast<uint64_t>(lobbyId);
    }
    return 0;
}

static int GetCharacterArchiveVariantCount(int characterId)
{
    if (characterId == 51) return 1;
    if (characterId == 3 || characterId == 5 ||
        characterId == 35 || characterId == 39) return 9;
    return characterId > 0 && characterId < kCharNameCount ? 8 : 1;
}

static bool IsValidTrainingLaunchPacket(const LaunchTrainingPacket& packet)
{
    if (packet.type != PACKET_LAUNCH_TRAINING ||
        packet.simpleMode[0] > 1 || packet.simpleMode[1] > 1 ||
        packet.autoJump[0] > 1 || packet.autoJump[1] > 1 ||
        packet.autoJumpValidMask > 3 || packet.voiceValidMask > 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (packet.p1Chars[i] < 0 || packet.p1Chars[i] >= kCharNameCount ||
            packet.p2Chars[i] < 0 || packet.p2Chars[i] >= kCharNameCount ||
            packet.p1Colors[i] < 0 ||
            packet.p1Colors[i] >= GetCharacterArchiveVariantCount(packet.p1Chars[i]) ||
            packet.p2Colors[i] < 0 ||
            packet.p2Colors[i] >= GetCharacterArchiveVariantCount(packet.p2Chars[i]) ||
            packet.p1Assists[i] < 0 || packet.p1Assists[i] > 2 ||
            packet.p2Assists[i] < 0 || packet.p2Assists[i] > 2 ||
            packet.voiceLanguage[0][i] > 1 ||
            packet.voiceLanguage[1][i] > 1)
            return false;
    }
    return true;
}

// Custom timer and window messages
#define ID_NET_POLL_TIMER       5001
#define ID_STEAM_POLL_TIMER     5002
static constexpr char kChatWindowClass[] = "UMVC3LaunchChat";
static constexpr int kChatTranscriptLimit = 16 * 1024;
static constexpr unsigned kChatMessagesPerSecond = 20;
static volatile bool g_networkPolling = false;
static HANDLE g_hPollThread = NULL;
static bool g_isLaunching = false;
static bool g_battleSetupOnly = false;
static bool g_compactMode = false;
static bool g_compactAdvancedRequest = false;
static bool g_compactBattleSetupRequest = false;
static bool g_battleLobbyReturnRequest = false;
static bool g_regularCompactRequest = false;
static bool g_directConnectExpanded = false;
static int g_lobbyTypeSelection = 0;
static char g_lobbyName[128] = {};
static constexpr size_t kPingSampleCapacity = 120;
static constexpr size_t kMinPingSamples = 60;
static constexpr ULONGLONG kPingSampleIntervalMs = 50;
static ENetPeer* g_pingSamplePeer = nullptr;
static enet_uint32 g_pingSamples[kPingSampleCapacity] = {};
static size_t g_pingSampleCount = 0;
static size_t g_nextPingSample = 0;
static uint64_t g_pingSampleSum = 0;
static ULONGLONG g_lastPingSampleTime = 0;

struct AutomaticNetplaySettings {
    int32_t delay;
    int32_t rollback;
};

static AutomaticNetplaySettings GetAutomaticNetplaySettings(enet_uint32 pingMs)
{
    if (pingMs < 24)  return {2, 0};
    if (pingMs < 36) return {3, 0};
    if (pingMs < 48)  return {3, 2};
    if (pingMs < 60)  return {3, 3};
    if (pingMs < 100) return {3, 7};
    if (pingMs < 140) return {3, 7};
    if (pingMs < 190) return {4, 7};
    if (pingMs < 250) return {5, 7};
    return {6, 7};
}

static AutomaticNetplaySettings ApplyAutomaticNetplaySettings(enet_uint32 pingMs)
{
    const AutomaticNetplaySettings settings = GetAutomaticNetplaySettings(pingMs);
    Controller_SetDelayFrames(settings.delay);
    Controller_SetRollbackFrames(settings.rollback);
    return settings;
}

static void ResetPingSamples(ENetPeer* peer = nullptr)
{
    g_pingSamplePeer = peer;
    g_pingSampleCount = 0;
    g_nextPingSample = 0;
    g_pingSampleSum = 0;
    g_lastPingSampleTime = 0;
}

static enet_uint32 GetLaunchPingMs(ENetPeer* peer)
{
    const uint32_t roundTripTime = SteamTransport_IsActive()
        ? SteamTransport_GetRTT()
        : peer ? peer->roundTripTime : 0;
    return roundTripTime / 2;
}

static void CollectPingSample(ENetPeer* peer)
{
    if (peer != g_pingSamplePeer)
        ResetPingSamples(peer);

    const ULONGLONG now = GetTickCount64();
    if (g_lastPingSampleTime != 0 &&
        now - g_lastPingSampleTime < kPingSampleIntervalMs)
        return;
    g_lastPingSampleTime = now;

    const enet_uint32 pingMs = GetLaunchPingMs(peer);
    if (pingMs == 0) return;
    if (g_pingSampleCount == kPingSampleCapacity)
        g_pingSampleSum -= g_pingSamples[g_nextPingSample];
    else
        ++g_pingSampleCount;

    g_pingSamples[g_nextPingSample] = pingMs;
    g_pingSampleSum += pingMs;
    g_nextPingSample = (g_nextPingSample + 1) % kPingSampleCapacity;
}

static void GetSettingsPath(const char* filename, char* path, size_t pathSize)
{
    DWORD length = GetModuleFileNameA(nullptr, path, (DWORD)pathSize);
    if (length == 0 || length >= pathSize) {
        path[0] = '\0';
        return;
    }

    char* slash = strrchr(path, '\\');
    char* forwardSlash = strrchr(path, '/');
    if (!slash || (forwardSlash && forwardSlash > slash)) slash = forwardSlash;
    char* filenameStart = slash ? slash + 1 : path;
    snprintf(filenameStart, pathSize - (filenameStart - path), "%s", filename);
}

static void CALLBACK LayoutDisclaimerMessageBox(HWND dialog, UINT, UINT_PTR timerId, DWORD)
{
    KillTimer(dialog, timerId);

    HWND acceptButton = GetDlgItem(dialog, IDYES);
    HWND rejectButton = GetDlgItem(dialog, IDNO);
    if (!acceptButton || !rejectButton)
        return;

    SetWindowTextA(acceptButton, "Sign my soul away boss");
    SetWindowTextA(rejectButton, "No Wtf??");

    RECT windowRect = {};
    RECT clientRect = {};
    GetWindowRect(dialog, &windowRect);
    GetClientRect(dialog, &clientRect);
    constexpr int desiredClientWidth = 600;
    if (clientRect.right < desiredClientWidth) {
        const int extraWidth = desiredClientWidth - clientRect.right;
        SetWindowPos(dialog, NULL, windowRect.left - extraWidth / 2, windowRect.top,
                     windowRect.right - windowRect.left + extraWidth,
                     windowRect.bottom - windowRect.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        GetClientRect(dialog, &clientRect);
    }

    RECT acceptRect = {};
    GetWindowRect(acceptButton, &acceptRect);
    MapWindowPoints(NULL, dialog, reinterpret_cast<POINT*>(&acceptRect), 2);
    constexpr int acceptWidth = 190;
    constexpr int rejectWidth = 90;
    constexpr int centeredGap = 28;
    const int x = (clientRect.right - acceptWidth - centeredGap - rejectWidth) / 2;
    const int rejectX = clientRect.right - rejectWidth - 12;
    HDWP positions = BeginDeferWindowPos(2);
    positions = DeferWindowPos(
        positions, acceptButton, NULL, x, acceptRect.top, acceptWidth,
        acceptRect.bottom - acceptRect.top, SWP_NOACTIVATE | SWP_NOZORDER);
    positions = DeferWindowPos(
        positions, rejectButton, NULL, rejectX, acceptRect.top, rejectWidth,
        acceptRect.bottom - acceptRect.top, SWP_NOACTIVATE | SWP_NOZORDER);
    EndDeferWindowPos(positions);
    RedrawWindow(dialog, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static LRESULT CALLBACK DisclaimerMessageBoxHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HCBT_ACTIVATE)
        SetTimer(reinterpret_cast<HWND>(wParam), 0, 1, LayoutDisclaimerMessageBox);
    return CallNextHookEx(NULL, code, wParam, lParam);
}

bool ConfirmLaunchDisclaimer()
{
    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    if (GetPrivateProfileIntA("General", "SoulSignedAway", 0, settingsPath) != 0 ||
        GetPrivateProfileIntA("General", "DisclaimerAccepted", 0, settingsPath) != 0)
        return true;

    HHOOK hook = SetWindowsHookExA(WH_CBT, DisclaimerMessageBoxHook, NULL,
                                   GetCurrentThreadId());
    const int result = MessageBoxA(
        NULL,
        "This is buggy, broken, and untested! If you have comments, questions, "
        "death threats, I will listen to them, but I have the right not to fix "
        "them immediately!\n\nDo you agree?",
        "Warning",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    if (hook)
        UnhookWindowsHookEx(hook);
    if (result != IDYES)
        return false;

    if (!IniSafe_WriteString(
            "General", "SoulSignedAway", "1", settingsPath)) {
        HookLog("UMVC3Hook: Failed to save disclaimer acceptance to %s\n",
                settingsPath);
    }
    return true;
}

static void LoadRollbackSettings()
{
    char rollbackPath[MAX_PATH] = {};
    GetSettingsPath("rollbacksetting.ini", rollbackPath, sizeof(rollbackPath));
    for (int i = 0; i < 3; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "Slot%d", i + 1);
        g_dialogConfig.p1Chars[i] = GetPrivateProfileIntA("P1", key, g_dialogConfig.p1Chars[i], rollbackPath);
        g_dialogConfig.p2Chars[i] = GetPrivateProfileIntA("P2", key, g_dialogConfig.p2Chars[i], rollbackPath);
        snprintf(key, sizeof(key), "Color%d", i + 1);
        g_dialogConfig.p1Colors[i] = GetPrivateProfileIntA("P1", key, g_dialogConfig.p1Colors[i], rollbackPath);
        g_dialogConfig.p2Colors[i] = GetPrivateProfileIntA("P2", key, g_dialogConfig.p2Colors[i], rollbackPath);
        snprintf(key, sizeof(key), "Assist%d", i + 1);
        g_dialogConfig.p1Assists[i] = GetPrivateProfileIntA("P1", key, g_dialogConfig.p1Assists[i], rollbackPath);
        g_dialogConfig.p2Assists[i] = GetPrivateProfileIntA("P2", key, g_dialogConfig.p2Assists[i], rollbackPath);

        g_dialogConfig.p1Colors[i] = std::clamp(g_dialogConfig.p1Colors[i], 0,
            GetCharacterArchiveVariantCount(g_dialogConfig.p1Chars[i]) - 1);
        g_dialogConfig.p2Colors[i] = std::clamp(g_dialogConfig.p2Colors[i], 0,
            GetCharacterArchiveVariantCount(g_dialogConfig.p2Chars[i]) - 1);
        g_dialogConfig.p1Assists[i] = std::clamp(g_dialogConfig.p1Assists[i], 0, 2);
        g_dialogConfig.p2Assists[i] = std::clamp(g_dialogConfig.p2Assists[i], 0, 2);
    }
    g_dialogConfig.simpleMode[0] = GetPrivateProfileIntA(
        "P1", "SimpleMode", g_dialogConfig.simpleMode[0], rollbackPath) != 0;
    g_dialogConfig.simpleMode[1] = GetPrivateProfileIntA(
        "P2", "SimpleMode", g_dialogConfig.simpleMode[1], rollbackPath) != 0;
    g_dialogConfig.stageId = GetPrivateProfileIntA(
        "Training", "StageId", STAGE_BONNE_WONDERLAND, rollbackPath);

    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    GetPrivateProfileStringA("Network", "HostIp", "",
                             g_dialogConfig.hostIp,
                             sizeof(g_dialogConfig.hostIp), settingsPath);
    if (g_dialogConfig.hostIp[0] == '\0') {
        GetPrivateProfileStringA("Network", "HostIp", "127.0.0.1",
                                 g_dialogConfig.hostIp,
                                 sizeof(g_dialogConfig.hostIp), rollbackPath);
    }

    char port[16] = {};
    GetPrivateProfileStringA("Network", "HostPort", "", port, sizeof(port), settingsPath);
    g_dialogConfig.hostPort = port[0] != '\0'
        ? atoi(port)
        : GetPrivateProfileIntA("Network", "HostPort", g_dialogConfig.hostPort, rollbackPath);
    g_directConnectExpanded = GetPrivateProfileIntA(
        "Network", "DirectConnectExpanded", 0, settingsPath) != 0;
    g_lobbyTypeSelection = std::clamp(
        static_cast<int>(GetPrivateProfileIntA(
            "Network", "LobbyType", 0, settingsPath)), 0, 2);
    GetPrivateProfileStringA("Network", "LobbyName", "", g_lobbyName,
                             sizeof(g_lobbyName), settingsPath);
}

static void SaveLobbyTypeSelection()
{
    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    char value[2] = { static_cast<char>('0' + g_lobbyTypeSelection), '\0' };
    if (!IniSafe_WriteString(
            "Network", "LobbyType", value, settingsPath)) {
        HookLog("UMVC3Hook: Failed to save lobby type to %s\n", settingsPath);
    }
}

static void SaveLobbyName()
{
    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    if (!IniSafe_WriteString(
            "Network", "LobbyName", g_lobbyName, settingsPath)) {
        HookLog("UMVC3Hook: Failed to save lobby name to %s\n", settingsPath);
    }
}

static void SaveRollbackSettings()
{
    char rollbackPath[MAX_PATH] = {};
    GetSettingsPath("rollbacksetting.ini", rollbackPath, sizeof(rollbackPath));
    for (int i = 0; i < 3; ++i) {
        char key[16], value[16];
        snprintf(key, sizeof(key), "Slot%d", i + 1);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p1Chars[i]);
        IniSafe_WriteString("P1", key, value, rollbackPath);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p2Chars[i]);
        IniSafe_WriteString("P2", key, value, rollbackPath);
        snprintf(key, sizeof(key), "Color%d", i + 1);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p1Colors[i]);
        IniSafe_WriteString("P1", key, value, rollbackPath);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p2Colors[i]);
        IniSafe_WriteString("P2", key, value, rollbackPath);
        snprintf(key, sizeof(key), "Assist%d", i + 1);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p1Assists[i]);
        IniSafe_WriteString("P1", key, value, rollbackPath);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p2Assists[i]);
        IniSafe_WriteString("P2", key, value, rollbackPath);
    }
    IniSafe_WriteString("P1", "SimpleMode",
                               g_dialogConfig.simpleMode[0] ? "1" : "0", rollbackPath);
    IniSafe_WriteString("P2", "SimpleMode",
                               g_dialogConfig.simpleMode[1] ? "1" : "0", rollbackPath);
    char stageId[16];
    snprintf(stageId, sizeof(stageId), "%d", g_dialogConfig.stageId);
    IniSafe_WriteString("Training", "StageId", stageId, rollbackPath);

    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    IniSafe_WriteString("Network", "HostIp", g_dialogConfig.hostIp, settingsPath);
    char port[16];
    snprintf(port, sizeof(port), "%d", g_dialogConfig.hostPort);
    IniSafe_WriteString("Network", "HostPort", port, settingsPath);
}

void SaveRosterToIni(const int32_t chars[2][3],
                     const int32_t colors[2][3],
                     const int32_t assists[2][3])
{
    for (int i = 0; i < 3; ++i) {
        g_dialogConfig.p1Chars[i] = chars[0][i];
        g_dialogConfig.p2Chars[i] = chars[1][i];
        g_dialogConfig.p1Colors[i] = colors[0][i];
        g_dialogConfig.p2Colors[i] = colors[1][i];
        g_dialogConfig.p1Assists[i] = assists[0][i];
        g_dialogConfig.p2Assists[i] = assists[1][i];
    }
    SaveRollbackSettings();
}

void SendTrainingLaunchPacket()
{
    if (!g_dialogConfig.enetPeer || !g_dialogConfig.enetHost) return;

    DelayPacket settingsPacket = {};
    settingsPacket.type = PACKET_DELAY;
    settingsPacket.delay = Controller_GetDelayFrames();
    settingsPacket.rollbackFrames = Controller_GetRollbackFrames();
    settingsPacket.effective_frame = -1;
    ENetPacket* enetSettings = enet_packet_create(
        &settingsPacket, sizeof(settingsPacket), ENET_PACKET_FLAG_RELIABLE);
    if (!enetSettings ||
        enet_peer_send(g_dialogConfig.enetPeer, 0, enetSettings) != 0) {
        if (enetSettings) enet_packet_destroy(enetSettings);
        HookLog("UMVC3Hook: Failed to send automatic netplay settings\n");
        return;
    }

    LaunchTrainingPacket packet = {};
    packet.type = PACKET_LAUNCH_TRAINING;
    for (int i = 0; i < 3; ++i) {
        packet.p1Chars[i] = g_dialogConfig.p1Chars[i];
        packet.p2Chars[i] = g_dialogConfig.p2Chars[i];
        packet.p1Colors[i] = g_dialogConfig.p1Colors[i];
        packet.p2Colors[i] = g_dialogConfig.p2Colors[i];
        packet.p1Assists[i] = g_dialogConfig.p1Assists[i];
        packet.p2Assists[i] = g_dialogConfig.p2Assists[i];
    }
    packet.stageId = g_dialogConfig.stageId;
    packet.simpleMode[0] = g_dialogConfig.simpleMode[0];
    packet.simpleMode[1] = g_dialogConfig.simpleMode[1];
    switch (TrainingBoot_GetCpuMode()) {
    case TRAINING_CPU_P2_ONLY:
        packet.flags = LAUNCH_TRAINING_P2_CPU;
        break;
    case TRAINING_CPU_BOTH:
        packet.flags = LAUNCH_TRAINING_BOTH_CPUS;
        break;
    default:
        packet.flags = 0;
        break;
    }
    if (TrainingBoot_GetFiveBarStart())
        packet.flags |= LAUNCH_TRAINING_FIVE_BARS;
    ENetPacket* enetPacket = enet_packet_create(&packet, sizeof(packet), ENET_PACKET_FLAG_RELIABLE);
    if (!enetPacket || enet_peer_send(g_dialogConfig.enetPeer, 0, enetPacket) != 0) {
        if (enetPacket) enet_packet_destroy(enetPacket);
        HookLog("UMVC3Hook: Failed to send training launch packet\n");
        return;
    }
    enet_host_flush(g_dialogConfig.enetHost);
    HookLog("UMVC3Hook: Sent training launch packet P1=%d/%d/%d P2=%d/%d/%d stage=%d\n",
            packet.p1Chars[0], packet.p1Chars[1], packet.p1Chars[2],
            packet.p2Chars[0], packet.p2Chars[1], packet.p2Chars[2], packet.stageId);
}

bool ApplyTrainingLaunchPacket(const LaunchTrainingPacket& packet)
{
    if (!IsValidTrainingLaunchPacket(packet)) {
        HookLog("UMVC3Hook: Rejected invalid training launch packet\n");
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        g_dialogConfig.p1Chars[i] = packet.p1Chars[i];
        g_dialogConfig.p2Chars[i] = packet.p2Chars[i];
        g_dialogConfig.p1Colors[i] = packet.p1Colors[i];
        g_dialogConfig.p2Colors[i] = packet.p2Colors[i];
        g_dialogConfig.p1Assists[i] = packet.p1Assists[i];
        g_dialogConfig.p2Assists[i] = packet.p2Assists[i];
    }
    g_dialogConfig.stageId = packet.stageId;
    g_dialogConfig.simpleMode[0] = packet.simpleMode[0] != 0;
    g_dialogConfig.simpleMode[1] = packet.simpleMode[1] != 0;
    TrainingBoot_SetCpuMode(
        (packet.flags & LAUNCH_TRAINING_BOTH_CPUS) ? TRAINING_CPU_BOTH :
        (packet.flags & LAUNCH_TRAINING_P2_CPU) ? TRAINING_CPU_P2_ONLY :
        TRAINING_CPU_NONE);
    TrainingBoot_SetFiveBarStart((packet.flags & LAUNCH_TRAINING_FIVE_BARS) != 0);

    TrainingBootConfig cfg = {};
    for (int i = 0; i < 3; ++i) {
        cfg.p1Chars[i] = packet.p1Chars[i];
        cfg.p2Chars[i] = packet.p2Chars[i];
        cfg.p1Colors[i] = packet.p1Colors[i];
        cfg.p2Colors[i] = packet.p2Colors[i];
        cfg.p1Assists[i] = packet.p1Assists[i];
        cfg.p2Assists[i] = packet.p2Assists[i];
        if ((packet.voiceValidMask & 1) != 0)
            cfg.voiceLanguage[0][i] = packet.voiceLanguage[0][i];
        if ((packet.voiceValidMask & 2) != 0)
            cfg.voiceLanguage[1][i] = packet.voiceLanguage[1][i];
    }
    cfg.stageId = packet.stageId;
    cfg.simpleMode[0] = packet.simpleMode[0] != 0;
    cfg.simpleMode[1] = packet.simpleMode[1] != 0;
    if ((packet.autoJumpValidMask & 1) != 0)
        cfg.autoJump[0] = packet.autoJump[0] != 0;
    if ((packet.autoJumpValidMask & 2) != 0)
        cfg.autoJump[1] = packet.autoJump[1] != 0;
    cfg.timerValue = 99;
    TrainingBoot_SetConfig(cfg);
    HookLog("UMVC3Hook: Applied launch packet P1=%d/%d/%d P2=%d/%d/%d stage=%d\n",
            packet.p1Chars[0], packet.p1Chars[1], packet.p1Chars[2],
            packet.p2Chars[0], packet.p2Chars[1], packet.p2Chars[2], packet.stageId);
    return true;
}

// Process-wide cleanup may be reached through multiple shutdown paths.
static volatile LONG g_exitCleanupStarted = 0;
static volatile LONG g_returnToLaunchMenuArmed = 0;
static volatile LONG g_returnToLaunchMenuStarted = 0;
static volatile LONG g_returnToLaunchMenuWatcherStarted = 0;
static HANDLE g_returnToLaunchMenuCancelEvent = nullptr;

void CleanupNetworkOnExit()
{
    if (InterlockedExchange(&g_exitCleanupStarted, 1) != 0)
        return;

    // If netplay took over the host/peer, let it do the cleanup
    if (Netplay_IsActive()) {
        Netplay_Shutdown();
        g_dialogConfig.enetHost = nullptr;
        g_dialogConfig.enetPeer = nullptr;
        return;
    }

    g_networkPolling = false;
    if (g_hPollThread) {
        WaitForSingleObject(g_hPollThread, 1000);
        CloseHandle(g_hPollThread);
        g_hPollThread = NULL;
    }
    if (g_dialogConfig.enetHost) {
        HookLog("UMVC3Hook: CleanupNetworkOnExit - destroying enet host\n");
        enet_host_destroy(g_dialogConfig.enetHost);
        g_dialogConfig.enetHost = nullptr;
        g_dialogConfig.enetPeer = nullptr;
    }
    SteamLobby_Shutdown(false);
}

void EnableReturnToLaunchMenuOnExit()
{
    if (GetLaunchOption() == LAUNCH_DUAL_TRAINING)
        return;

    InterlockedExchange(&g_returnToLaunchMenuArmed, 1);

    wchar_t gamePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, gamePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return;

    wchar_t updaterPath[MAX_PATH] = {};
    wcscpy(updaterPath, gamePath);
    wchar_t* filename = wcsrchr(updaterPath, L'\\');
    if (!filename)
        filename = wcsrchr(updaterPath, L'/');
    if (!filename)
        return;
    wcscpy(filename + 1, L"TimestoneUpdater.exe");

    wchar_t eventName[96] = {};
    swprintf(eventName, sizeof(eventName) / sizeof(eventName[0]),
             L"Local\\TimestoneReturnMenu_%lu", GetCurrentProcessId());
    g_returnToLaunchMenuCancelEvent = CreateEventW(
        nullptr, TRUE, FALSE, eventName);
    if (!g_returnToLaunchMenuCancelEvent) {
        HookLog("UMVC3Hook: Failed to create launch-menu watchdog event (error %lu)\n",
                GetLastError());
        return;
    }

    wchar_t commandLine[MAX_PATH * 2 + 192] = {};
    swprintf(commandLine, sizeof(commandLine) / sizeof(commandLine[0]),
             L"\"%ls\" --return-menu-watch --pid %lu --game-exe \"%ls\" "
             L"--cancel-event \"%ls\"", updaterPath, GetCurrentProcessId(),
             gamePath, eventName);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    const DWORD creationFlags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(updaterPath, commandLine, nullptr, nullptr, FALSE,
                        creationFlags, nullptr, nullptr, &startup, &process)) {
        HookLog("UMVC3Hook: Failed to start launch-menu watchdog (error %lu)\n",
                GetLastError());
        CloseHandle(g_returnToLaunchMenuCancelEvent);
        g_returnToLaunchMenuCancelEvent = nullptr;
        return;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    InterlockedExchange(&g_returnToLaunchMenuWatcherStarted, 1);
    HookLog("UMVC3Hook: Launch-menu watchdog started\n");
}

void DisableReturnToLaunchMenuOnExit()
{
    InterlockedExchange(&g_returnToLaunchMenuArmed, 0);
    if (g_returnToLaunchMenuCancelEvent)
        SetEvent(g_returnToLaunchMenuCancelEvent);
}

void PrepareForGameExit()
{
    CleanupNetworkOnExit();
    if (InterlockedCompareExchange(&g_returnToLaunchMenuArmed, 0, 1) != 1 ||
        InterlockedExchange(&g_returnToLaunchMenuStarted, 1) != 0) {
        HookLogFlush();
        return;
    }

    // The detached watcher survives forced console termination and relaunches
    // only after this process has fully exited.
    if (InterlockedCompareExchange(&g_returnToLaunchMenuWatcherStarted, 0, 0) != 0) {
        HookLogFlush();
        return;
    }

    wchar_t exePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        HookLog("UMVC3Hook: Failed to resolve executable for launch-menu return (error %lu)\n",
                GetLastError());
        HookLogFlush();
        return;
    }

    wchar_t commandLine[MAX_PATH + 32] = {};
    swprintf(commandLine, sizeof(commandLine) / sizeof(commandLine[0]),
             L"\"%ls\" -timestone-return-menu", exePath);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    const DWORD creationFlags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(exePath, commandLine, nullptr, nullptr, FALSE,
                        creationFlags, nullptr, nullptr, &startup, &process)) {
        HookLog("UMVC3Hook: Failed to return to launch menu (error %lu)\n",
                GetLastError());
        HookLogFlush();
        return;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    HookLog("UMVC3Hook: Started fresh launch-menu process\n");
    HookLogFlush();
}

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------
enum CtrlId {
    // Launch buttons
    ID_LAUNCH_NORMAL      = 1001,
    ID_LAUNCH_TRAINING    = 1002,
    ID_LAUNCH_DUAL        = 1003,
    ID_LAUNCH_DUAL_CPUS   = 1004,
    ID_LAUNCH_DUAL_P2_CPU = 1005,
    // Character combos
    ID_P1_SLOT1           = 2001,
    ID_P1_SLOT2           = 2002,
    ID_P1_SLOT3           = 2003,
    ID_P2_SLOT1           = 2004,
    ID_P2_SLOT2           = 2005,
    ID_P2_SLOT3           = 2006,
    ID_P1_COLOR1          = 2011,
    ID_P1_COLOR2          = 2012,
    ID_P1_COLOR3          = 2013,
    ID_P2_COLOR1          = 2014,
    ID_P2_COLOR2          = 2015,
    ID_P2_COLOR3          = 2016,
    ID_P1_ASSIST1         = 2021,
    ID_P1_ASSIST2         = 2022,
    ID_P1_ASSIST3         = 2023,
    ID_P2_ASSIST1         = 2024,
    ID_P2_ASSIST2         = 2025,
    ID_P2_ASSIST3         = 2026,
    ID_P1_SIMPLE_MODE     = 2027,
    ID_P2_SIMPLE_MODE     = 2028,
    ID_RANDOMIZE_CHARS    = 2030,
    ID_P2_LABEL           = 2031,
    ID_PEER_READY         = 2032,
    ID_PING_LABEL         = 2033,
    ID_P1_SAVED_ROSTER    = 2034,
    ID_P2_SAVED_ROSTER    = 2035,
    // Stage combo
    ID_STAGE              = 2101,
    // Network controls
    ID_NET_IP             = 2201,
    ID_NET_PORT           = 2202,
    ID_NET_HOST_BTN       = 2203,
    ID_NET_STATUS         = 2204,
    ID_NET_CONNECT_BTN    = 2205,
    ID_NET_TOGGLE         = 2206,
    ID_NET_IP_LABEL       = 2207,
    ID_NET_PORT_LABEL     = 2208,
    // Steam lobby controls
    ID_STEAM_SEARCH_BTN   = 2301,
    ID_STEAM_CREATE_BTN   = 2302,
    ID_STEAM_LIST         = 2303,
    ID_STEAM_STATUS       = 2304,
    ID_STEAM_JOIN_BTN     = 2305,
    ID_STEAM_TYPE         = 2306,
    ID_STEAM_NAME         = 2307,
    ID_STEAM_COPY_LINK    = 2308,
    ID_LAUNCH_PROMPT      = 2401,
    ID_VERSION_LABEL      = 2402,
    ID_ADVANCED_MENU      = 2403,
    ID_BATTLE_LEAVE       = 2404,
    ID_ADVANCED_CONFIG    = 2405,
    ID_LOBBY_MUSIC_MUTE   = 2406,
    // Peer chat controls
    ID_CHAT_CONNECTION    = 2501,
    ID_CHAT_PROFILE       = 2502,
    ID_CHAT_HISTORY       = 2503,
    ID_CHAT_INPUT         = 2504,
    ID_CHAT_SEND          = 2505,
    ID_CHAT_AVATAR        = 2506,
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LaunchDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ChatWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ChatInputWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void PopulateCharCombo(HWND hCombo, int defaultCharId);
static void PopulateColorCombo(HWND hCombo, int characterId, int defaultColor);
static void PopulateAssistCombo(HWND hCombo, int characterId, int defaultAssist);
static void PopulateStageCombo(HWND hCombo, int defaultStageId);
static int FindStageIndex(int stageId);
static int FindComboItemByData(HWND hCombo, int data);
static void UpdateCharFromCombo(HWND hDlg, int comboId, int slotIndex, int team);
static void UpdateSlotOptionsFromCombos(HWND hDlg);
static void RandomizeCharacters(HWND hDlg);
static void CreateSavedRosterCombo(HWND hDlg, int team, int x, int y);
static void ApplySavedRoster(HWND hDlg, int team, size_t preset);
static void ConfigureConnectedControls(HWND hDlg);
static void FocusLaunchMenu(HWND hDlg);
static void ShowChatWindow(HWND hDlg);
static void DestroyChatWindow();
static void UpdateChatAvatar();
static void ReceiveChatPacket(const LaunchChatPacket& packet);
static void ResetLaunchMenuButtonState(HWND hDlg);
static void UpdatePingLabel(HWND hDlg);
static void SetDirectConnectExpanded(HWND hDlg, bool expanded);
static void SendP1Selection(HWND hDlg);
static void SendP2Selection(HWND hDlg);
static bool ApplyP1Selection(HWND hDlg, const LaunchP1SelectionPacket& packet);
static bool ApplyP2Selection(HWND hDlg, const LaunchP2SelectionPacket& packet);
static DWORD WINAPI NetworkPollThread(LPVOID lpParam);
static void CleanupNetwork(HWND hDlg, bool forceCleanup = false);
static void CreateBattleSetupControls(HWND hDlg);
static UINT GetLaunchWindowDpi(HWND hWnd);
static void ApplyLaunchMenuFont(HWND hDlg);
static void InitializeDpiLayout(HWND hWnd);
static void UpdateDpiLayout(HWND hWnd, UINT newDpi);
static void DestroyDpiLayout(HWND hWnd);
static void ReturnBattleSetupToLobbies(HWND hDlg);
static void UpdateLobbyMusicCheckbox(HWND hDlg);

static bool IsCompatibleProtocol(const char (&protocol)[32])
{
    return memchr(protocol, '\0', sizeof(protocol)) != nullptr &&
           strcmp(protocol, UMVC3HOOK_VERSION) == 0;
}

static void ShowVersionMismatch(HWND hDlg, const char* remoteVersion)
{
    unsigned remoteMajor = 0, remoteMinor = 0, remotePatch = 0;
    unsigned localMajor = 0, localMinor = 0, localPatch = 0;
    bool parsedRemote = remoteVersion &&
        sscanf(remoteVersion, "%u.%u.%u", &remoteMajor, &remoteMinor, &remotePatch) == 3;
    bool parsedLocal = sscanf(UMVC3HOOK_VERSION, "%u.%u.%u",
                              &localMajor, &localMinor, &localPatch) == 3;
    bool remoteOlder = !parsedRemote || (parsedLocal &&
        (remoteMajor < localMajor ||
         (remoteMajor == localMajor && remoteMinor < localMinor) ||
         (remoteMajor == localMajor && remoteMinor == localMinor &&
          remotePatch < localPatch)));
    char message[256];
    snprintf(message, sizeof(message),
             "%s\n\nYour version: %s\nTheir version: %s",
             remoteOlder
                 ? "The other player is running an older version."
                 : "Your version does not match, please update Timestone",
             UMVC3HOOK_VERSION,
             remoteVersion && remoteVersion[0] ? remoteVersion : "unknown");
    if (g_isHost) {
        HookLog("UMVC3Hook: Rejected incompatible peer: %s\n", message);
        bool steamHost = SteamLobby_ResetHostPeer();
        if (!steamHost && g_dialogConfig.enetPeer)
            enet_peer_disconnect_now(g_dialogConfig.enetPeer, 0);
        if (steamHost && g_dialogConfig.enetHost)
            enet_host_destroy(g_dialogConfig.enetHost);
        g_dialogConfig.enetPeer = nullptr;
        if (steamHost) g_dialogConfig.enetHost = nullptr;
        g_localReady = false;
        g_peerReady = false;
        ResetLaunchMenuButtonState(hDlg);
        EnableWindow(GetDlgItem(hDlg, ID_NET_HOST_BTN), FALSE);
        EnableWindow(GetDlgItem(hDlg, ID_NET_CONNECT_BTN), FALSE);
        HWND status = GetDlgItem(hDlg, ID_NET_STATUS);
        if (status) SetWindowTextA(status,
            steamHost ? "Rejected incompatible player; waiting in Steam lobby"
                      : "Rejected incompatible player; still listening");
        return;
    }
    MessageBoxA(hDlg, message, "Timestone Version Mismatch", MB_OK | MB_ICONERROR);
    CleanupNetwork(hDlg, true);
}

static void UpdateReadyButton(HWND hDlg)
{
    HWND button = GetDlgItem(hDlg, ID_PEER_READY);
    if (!button) return;

    const bool ready = g_isHost ? g_peerReady : g_localReady;
    const char* text = g_isHost
        ? (ready ? "Peer ready" : "Peer not ready")
        : (ready ? "Ready" : "Click to Ready!");
    SetWindowTextA(button, text);
    ShowWindow(button, g_dialogConfig.enetPeer ? SW_SHOW : SW_HIDE);
    InvalidateRect(button, NULL, TRUE);
}

static void UpdatePingLabel(HWND hDlg)
{
    HWND label = GetDlgItem(hDlg, ID_PING_LABEL);
    if (!label) return;

    ENetPeer* peer = g_dialogConfig.enetPeer;
    if (!peer || peer->state != ENET_PEER_STATE_CONNECTED) {
        ResetPingSamples();
        ShowWindow(label, SW_HIDE);
        return;
    }

    CollectPingSample(peer);
    char text[64];
    const enet_uint32 currentPingMs = GetLaunchPingMs(peer);
    if (currentPingMs == 0) {
        snprintf(text, sizeof(text), "Ping: measuring Steam connection...");
    } else if (g_pingSampleCount < kMinPingSamples) {
        snprintf(text, sizeof(text), "Ping: %u ms (Sampling %zu/%zu)",
                 currentPingMs, g_pingSampleCount, kMinPingSamples);
    } else {
        const enet_uint32 pingMs = static_cast<enet_uint32>(
            (g_pingSampleSum + g_pingSampleCount / 2) / g_pingSampleCount);
        const AutomaticNetplaySettings settings =
            ApplyAutomaticNetplaySettings(pingMs);
        snprintf(text, sizeof(text), "Ping: %u ms (Auto %dD/%dR)",
                 pingMs, settings.delay, settings.rollback);
    }
    SetWindowTextA(label, text);
    ShowWindow(label, SW_SHOW);
}

static ULONGLONG g_lobbyLinkCopiedUntil = 0;

static void UpdateSteamLobbyButton(HWND hDlg)
{
    HWND button = GetDlgItem(hDlg, ID_STEAM_CREATE_BTN);
    if (!button) return;
    bool hasLobby = SteamLobby_HasLobby();
    SetWindowTextA(button, hasLobby ? "Leave Lobby" : "Create Lobby");
    EnableWindow(button, hasLobby || !SteamLobby_IsCreating());
    EnableWindow(GetDlgItem(hDlg, ID_STEAM_TYPE),
                  !hasLobby && !SteamLobby_IsCreating());
    EnableWindow(GetDlgItem(hDlg, ID_STEAM_NAME),
                 !hasLobby && !SteamLobby_IsCreating());
    char link[128] = {};
    ShowWindow(GetDlgItem(hDlg, ID_STEAM_COPY_LINK),
               SteamLobby_GetHostedJoinLink(link, sizeof(link)) ? SW_SHOW : SW_HIDE);
}

static bool CopySteamLobbyJoinLink(HWND hDlg)
{
    char link[128] = {};
    if (!SteamLobby_GetHostedJoinLink(link, sizeof(link))) return false;
    const size_t size = strlen(link) + 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return false;
    void* data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        return false;
    }
    memcpy(data, link, size);
    GlobalUnlock(memory);
    bool copied = false;
    if (OpenClipboard(hDlg)) {
        copied = EmptyClipboard() && SetClipboardData(CF_TEXT, memory) != NULL;
        CloseClipboard();
    }
    if (!copied) GlobalFree(memory);
    return copied;
}

static void JoinSelectedSteamLobby(HWND hDlg)
{
    HWND list = GetDlgItem(hDlg, ID_STEAM_LIST);
    if (!list) return;
    int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (row < 0) return;

    LVITEMA item = {};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(list, &item) || !item.lParam) return;

    uint64_t lobbyId = static_cast<uint64_t>(item.lParam);
    if (SteamLobby_IsHostedLobby(lobbyId)) return;
    if (SteamLobby_HasLobby() || g_dialogConfig.enetHost)
        CleanupNetwork(hDlg, true);
    SteamLobby_Join(lobbyId);
}

static void ConfigureConnectedControls(HWND hDlg)
{
    for (HWND child = GetWindow(hDlg, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        char className[32] = {};
        GetClassNameA(child, className, sizeof(className));
        if (_stricmp(className, "BUTTON") == 0 ||
            _stricmp(className, "COMBOBOX") == 0 ||
            _stricmp(className, "EDIT") == 0 ||
            _stricmp(className, "LISTBOX") == 0) {
            EnableWindow(child, FALSE);
        }
    }

    if (g_isHost) {
        for (int i = 0; i < 3; ++i) {
            EnableWindow(GetDlgItem(hDlg, ID_P1_SLOT1 + i), TRUE);
            EnableWindow(GetDlgItem(hDlg, ID_P1_COLOR1 + i), TRUE);
            EnableWindow(GetDlgItem(hDlg, ID_P1_ASSIST1 + i), TRUE);
        }
        EnableWindow(GetDlgItem(hDlg, ID_STAGE), TRUE);
        EnableWindow(GetDlgItem(hDlg, ID_P1_SIMPLE_MODE), TRUE);
        EnableWindow(GetDlgItem(hDlg, ID_P1_SAVED_ROSTER), TRUE);
        EnableWindow(GetDlgItem(hDlg, ID_LAUNCH_TRAINING), g_peerReady ? TRUE : FALSE);
        HookLog("UMVC3Hook: Host owns P1; waiting for peer P2 selection\n");
    } else {
        for (int i = 0; i < 3; ++i) {
            EnableWindow(GetDlgItem(hDlg, ID_P2_SLOT1 + i), TRUE);
            EnableWindow(GetDlgItem(hDlg, ID_P2_COLOR1 + i), TRUE);
            EnableWindow(GetDlgItem(hDlg, ID_P2_ASSIST1 + i), TRUE);
        }
        EnableWindow(GetDlgItem(hDlg, ID_PEER_READY), TRUE);
        EnableWindow(GetDlgItem(hDlg, ID_P2_SIMPLE_MODE), TRUE);
        EnableWindow(GetDlgItem(hDlg, ID_P2_SAVED_ROSTER), TRUE);
        HookLog("UMVC3Hook: Client controls enabled for P2 selection\n");
    }
    if (g_battleSetupOnly)
        EnableWindow(GetDlgItem(hDlg, ID_BATTLE_LEAVE), TRUE);
    EnableWindow(GetDlgItem(hDlg, ID_RANDOMIZE_CHARS), TRUE);
    UpdateLobbyMusicCheckbox(hDlg);
    UpdateReadyButton(hDlg);
    UpdatePingLabel(hDlg);
}

static void FocusLaunchMenu(HWND hDlg)
{
    if (IsIconic(hDlg))
        ShowWindow(hDlg, SW_RESTORE);

    // Briefly use the topmost band so the menu is raised even if another
    // process owns the foreground, then return it to the normal Z-order.
    SetWindowPos(hDlg, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(hDlg, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hDlg);
}

static bool IsValidChatText(const char* text, size_t length)
{
    if (!text || length == 0 || length > LAUNCH_CHAT_MAX_MESSAGE)
        return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char value = static_cast<unsigned char>(text[i]);
        if (value < 0x20 || value == 0x7f)
            return false;
    }
    return true;
}

static void AppendChatText(const char* text)
{
    if (!g_hChatHistory || !text) return;

    const int currentLength = GetWindowTextLengthA(g_hChatHistory);
    const int addedLength = static_cast<int>(strlen(text));
    const int excess = currentLength + addedLength - kChatTranscriptLimit;
    if (excess > 0) {
        SendMessageA(g_hChatHistory, EM_SETSEL, 0, excess);
        SendMessageA(g_hChatHistory, EM_REPLACESEL, FALSE,
                     reinterpret_cast<LPARAM>(""));
    }
    SendMessageA(g_hChatHistory, EM_SETSEL, -1, -1);
    SendMessageA(g_hChatHistory, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(text));
}

static void AppendChatMessage(const char* sender, const char* message, size_t length)
{
    char line[LAUNCH_CHAT_MAX_MESSAGE + sizeof(g_chatOpponentName) + 8];
    snprintf(line, sizeof(line), "%s: %.*s\r\n", sender,
             static_cast<int>(length), message);
    AppendChatText(line);
}

static void SendChatMessage()
{
    if (!g_hChatInput || !g_dialogConfig.enetHost || !g_dialogConfig.enetPeer)
        return;

    char message[LAUNCH_CHAT_MAX_MESSAGE + 1] = {};
    const int length = GetWindowTextA(g_hChatInput, message, sizeof(message));
    if (!IsValidChatText(message, length))
        return;

    LaunchChatPacket packet = {};
    packet.type = PACKET_LAUNCH_CHAT;
    packet.length = static_cast<uint16_t>(length);
    memcpy(packet.message, message, length);
    ENetPacket* enetPacket = enet_packet_create(
        &packet, sizeof(packet), ENET_PACKET_FLAG_RELIABLE);
    if (!enetPacket || enet_peer_send(g_dialogConfig.enetPeer, 0, enetPacket) != 0) {
        if (enetPacket) enet_packet_destroy(enetPacket);
        HookLog("UMVC3Hook: Failed to send launch chat packet\n");
        return;
    }

    enet_host_flush(g_dialogConfig.enetHost);
    AppendChatMessage("You", message, length);
    SetWindowTextA(g_hChatInput, "");
    SetFocus(g_hChatInput);
}

static void ReceiveChatPacket(const LaunchChatPacket& packet)
{
    if (packet.type != PACKET_LAUNCH_CHAT || packet.reserved != 0 ||
        !IsValidChatText(packet.message, packet.length)) {
        HookLog("UMVC3Hook: Rejected malformed launch chat packet\n");
        return;
    }
    for (size_t i = packet.length; i < sizeof(packet.message); ++i) {
        if (packet.message[i] != '\0') {
            HookLog("UMVC3Hook: Rejected non-canonical launch chat packet\n");
            return;
        }
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_chatRateWindowStart >= 1000) {
        g_chatRateWindowStart = now;
        g_chatRateCount = 0;
    }
    if (g_chatRateCount >= kChatMessagesPerSecond) {
        if (g_chatRateCount == kChatMessagesPerSecond)
            HookLog("UMVC3Hook: Rate-limiting launch chat packets\n");
        ++g_chatRateCount;
        return;
    }
    ++g_chatRateCount;
    AppendChatMessage(g_chatOpponentName, packet.message, packet.length);
}

static void ClearChatAvatar()
{
    if (g_hChatWindow) {
        HWND avatar = GetDlgItem(g_hChatWindow, ID_CHAT_AVATAR);
        if (avatar) {
            SendMessageA(avatar, STM_SETIMAGE, IMAGE_BITMAP, 0);
            ShowWindow(avatar, SW_HIDE);
        }
    }
    if (g_hChatAvatar) {
        DeleteObject(g_hChatAvatar);
        g_hChatAvatar = nullptr;
    }
    g_chatAvatarSteamId = 0;
}

static void UpdateChatAvatar()
{
    if (!g_hChatWindow) return;
    const uint64_t steamId = SteamLobby_GetPeerSteamId();
    if (!steamId) {
        ClearChatAvatar();
        return;
    }
    if (g_hChatAvatar && g_chatAvatarSteamId == steamId) return;
    if (g_chatAvatarSteamId != steamId) ClearChatAvatar();

    uint8_t rgba[64 * 64 * 4];
    uint32_t width = 0;
    uint32_t height = 0;
    if (!SteamLobby_GetPeerAvatarRGBA(rgba, sizeof(rgba), &width, &height)) return;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        return;
    }

    uint8_t* bgra = static_cast<uint8_t*>(pixels);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixelCount; ++i) {
        bgra[i * 4] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4];
        bgra[i * 4 + 3] = 0xff;
    }

    g_hChatAvatar = bitmap;
    g_chatAvatarSteamId = steamId;
    HWND avatar = GetDlgItem(g_hChatWindow, ID_CHAT_AVATAR);
    SendMessageA(avatar, STM_SETIMAGE, IMAGE_BITMAP,
                 reinterpret_cast<LPARAM>(bitmap));
    ShowWindow(avatar, SW_SHOW);
}

static void ShowChatWindow(HWND hDlg)
{
    const char* opponent = SteamLobby_GetPeerSteamId()
        ? SteamLobby_GetPeerUsername() : "Opponent";
    if (!opponent || !opponent[0]) opponent = "Opponent";
    size_t opponentLength = strnlen(opponent, sizeof(g_chatOpponentName) - 1);
    for (size_t i = 0; i < opponentLength; ++i) {
        const unsigned char value = static_cast<unsigned char>(opponent[i]);
        g_chatOpponentName[i] = value < 0x20 || value == 0x7f ? '?' : opponent[i];
    }
    g_chatOpponentName[opponentLength] = '\0';

    if (!g_hChatWindow) {
        const UINT dpi = GetLaunchWindowDpi(hDlg);
        if (g_battleSetupOnly) {
            RECT clientRect = {};
            GetClientRect(hDlg, &clientRect);
            const int x = MulDiv(830, dpi, 96);
            g_hChatWindow = CreateWindowExA(
                WS_EX_CONTROLPARENT, kChatWindowClass, "Opp Chat",
                WS_CHILD | WS_VISIBLE,
                x, 0, std::max(1L, clientRect.right - x), clientRect.bottom,
                hDlg, nullptr, GetModuleHandle(NULL), nullptr);
        } else {
            RECT ownerRect = {};
            RECT workArea = {};
            GetWindowRect(hDlg, &ownerRect);
            SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
            const int width = MulDiv(370, dpi, 96);
            const int height = MulDiv(440, dpi, 96);
            const int gap = MulDiv(12, dpi, 96);
            int x = ownerRect.right + gap;
            if (x + width > workArea.right)
                x = ownerRect.left - width - gap;
            x = std::clamp(x, static_cast<int>(workArea.left),
                          static_cast<int>(workArea.right) - width);
            int y = std::clamp(static_cast<int>(ownerRect.top),
                               static_cast<int>(workArea.top),
                               static_cast<int>(workArea.bottom) - height);
            g_hChatWindow = CreateWindowExA(
                WS_EX_TOOLWINDOW, kChatWindowClass, "Peer Chat",
                WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                x, y, width, height, hDlg, nullptr, GetModuleHandle(NULL), nullptr);
        }
    }

    if (!g_hChatWindow) return;
    char connection[sizeof(g_chatOpponentName) + 40];
    snprintf(connection, sizeof(connection), "You are now connected to %s",
             g_chatOpponentName);
    SetWindowTextA(GetDlgItem(g_hChatWindow, ID_CHAT_CONNECTION), connection);
    const bool hasSteamPeer = SteamLobby_GetPeerSteamId() != 0;
    HWND connectionLabel = GetDlgItem(g_hChatWindow, ID_CHAT_CONNECTION);
    HWND profileButton = GetDlgItem(g_hChatWindow, ID_CHAT_PROFILE);
    const UINT chatDpi = GetLaunchWindowDpi(g_hChatWindow);
    SetWindowPos(connectionLabel, nullptr,
                 MulDiv(hasSteamPeer ? 84 : 12, chatDpi, 96),
                 MulDiv(12, chatDpi, 96),
                 MulDiv(hasSteamPeer ? 264 : 336, chatDpi, 96),
                 MulDiv(34, chatDpi, 96), SWP_NOZORDER);
    SetWindowPos(profileButton, nullptr,
                 MulDiv(hasSteamPeer ? 84 : 12, chatDpi, 96),
                 MulDiv(50, chatDpi, 96),
                 MulDiv(hasSteamPeer ? 264 : 336, chatDpi, 96),
                 MulDiv(28, chatDpi, 96), SWP_NOZORDER);
    EnableWindow(profileButton, hasSteamPeer ? TRUE : FALSE);
    UpdateChatAvatar();
    char systemLine[sizeof(connection) + 4];
    snprintf(systemLine, sizeof(systemLine), "%s\r\n", connection);
    AppendChatText(systemLine);
    ShowWindow(g_hChatWindow, SW_SHOW);
}

static void DestroyChatWindow()
{
    if (g_hChatWindow)
        DestroyWindow(g_hChatWindow);
    g_hChatWindow = nullptr;
    g_hChatHistory = nullptr;
    g_hChatInput = nullptr;
    ClearChatAvatar();
    g_chatRateWindowStart = 0;
    g_chatRateCount = 0;
}

static LRESULT CALLBACK ChatInputWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (wParam == VK_RETURN && msg == WM_KEYDOWN) {
        SendChatMessage();
        return 0;
    }
    if (wParam == VK_RETURN && msg == WM_CHAR)
        return 0;
    return g_chatInputWndProc
        ? CallWindowProcA(g_chatInputWndProc, hWnd, msg, wParam, lParam)
        : DefWindowProcA(hWnd, msg, wParam, lParam);
}

static LRESULT CALLBACK ChatWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindowA("STATIC", "", WS_CHILD | SS_BITMAP | SS_CENTERIMAGE,
                      12, 12, 64, 64, hWnd,
                      reinterpret_cast<HMENU>(ID_CHAT_AVATAR), nullptr, nullptr);
        CreateWindowA("STATIC", "", WS_VISIBLE | WS_CHILD,
                      12, 12, 336, 34, hWnd,
                      reinterpret_cast<HMENU>(ID_CHAT_CONNECTION), nullptr, nullptr);
        CreateWindowA("BUTTON", "Open Opponent Steam Profile",
                      WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                      12, 50, 336, 28, hWnd,
                      reinterpret_cast<HMENU>(ID_CHAT_PROFILE), nullptr, nullptr);
        g_hChatHistory = CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "",
            WS_VISIBLE | WS_CHILD | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_READONLY,
            12, 86, 336, 260, hWnd,
            reinterpret_cast<HMENU>(ID_CHAT_HISTORY), nullptr, nullptr);
        g_hChatInput = CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 354, 256, 26, hWnd,
            reinterpret_cast<HMENU>(ID_CHAT_INPUT), nullptr, nullptr);
        SendMessageA(g_hChatInput, EM_SETLIMITTEXT, LAUNCH_CHAT_MAX_MESSAGE, 0);
        g_chatInputWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
            g_hChatInput, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(ChatInputWndProc)));
        CreateWindowA("BUTTON", "Send",
                      WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      276, 353, 72, 28, hWnd,
                      reinterpret_cast<HMENU>(ID_CHAT_SEND), nullptr, nullptr);
        InitializeDpiLayout(hWnd);
        ApplyLaunchMenuFont(hWnd);
        ApplyLaunchMenuDarkTheme(hWnd);
        return 0;

    case WM_DPICHANGED:
    {
        const UINT dpi = HIWORD(wParam);
        UpdateDpiLayout(hWnd, dpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hWnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        ApplyLaunchMenuFont(hWnd);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_CHAT_SEND && HIWORD(wParam) == BN_CLICKED) {
            SendChatMessage();
            return 0;
        }
        if (LOWORD(wParam) == ID_CHAT_PROFILE && HIWORD(wParam) == BN_CLICKED) {
            SteamLobby_OpenPeerProfile();
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleLaunchMenuDarkControlColor(msg, wParam);

    case WM_ERASEBKGND:
        PaintLaunchMenuDarkBackground(hWnd, reinterpret_cast<HDC>(wParam));
        return TRUE;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        DestroyDpiLayout(hWnd);
        if (g_hChatWindow == hWnd) {
            ClearChatAvatar();
            g_hChatWindow = nullptr;
            g_hChatHistory = nullptr;
            g_hChatInput = nullptr;
        }
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void ResetLaunchMenuButtonState(HWND hDlg)
{
    if (!hDlg) return;
    for (HWND child = GetWindow(hDlg, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        char className[32] = {};
        GetClassNameA(child, className, sizeof(className));
        if (_stricmp(className, "BUTTON") == 0 ||
            _stricmp(className, "COMBOBOX") == 0 ||
            _stricmp(className, "EDIT") == 0 ||
            _stricmp(className, "LISTBOX") == 0) {
            EnableWindow(child, TRUE);
        }
    }
    ShowWindow(GetDlgItem(hDlg, ID_PEER_READY), SW_HIDE);
    ShowWindow(GetDlgItem(hDlg, ID_PING_LABEL), SW_HIDE);
}

static void UpdateLobbyMusicCheckbox(HWND hDlg)
{
    HWND checkbox = GetDlgItem(hDlg, ID_LOBBY_MUSIC_MUTE);
    if (!checkbox) return;
    if (!g_dialogConfig.enetPeer) {
        ShowWindow(checkbox, SW_HIDE);
        return;
    }
    SendMessageA(checkbox, BM_SETCHECK,
                 LaunchMusic_IsMuted() ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(checkbox, TRUE);
    ShowWindow(checkbox, SW_SHOW);
}

static void SetDirectConnectExpanded(HWND hDlg, bool expanded)
{
    if (g_directConnectExpanded == expanded) return;

    UINT layoutDpi = static_cast<UINT>(reinterpret_cast<INT_PTR>(
        GetPropA(hDlg, "TimestoneLayoutDpi")));
    if (!layoutDpi) layoutDpi = 96;
    const int expandedHeight = MulDiv(56, layoutDpi, 96);
    const int directControls[] = {
        ID_NET_IP_LABEL, ID_NET_IP, ID_NET_PORT_LABEL, ID_NET_PORT,
        ID_NET_HOST_BTN, ID_NET_CONNECT_BTN, ID_NET_STATUS,
    };
    for (int id : directControls)
        ShowWindow(GetDlgItem(hDlg, id), expanded ? SW_SHOW : SW_HIDE);

    const int launchControls[] = {
        ID_LAUNCH_PROMPT, ID_LAUNCH_NORMAL, ID_LAUNCH_TRAINING,
        ID_LAUNCH_DUAL, ID_LAUNCH_DUAL_CPUS, ID_LAUNCH_DUAL_P2_CPU,
        ID_VERSION_LABEL, ID_ADVANCED_MENU, ID_ADVANCED_CONFIG,
    };
    const int offset = expanded ? expandedHeight : -expandedHeight;
    for (int id : launchControls) {
        HWND control = GetDlgItem(hDlg, id);
        if (!control) continue;
        RECT rect = {};
        GetWindowRect(control, &rect);
        MapWindowPoints(NULL, hDlg, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(control, NULL, rect.left, rect.top + offset, 0, 0,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    }

    HWND toggle = GetDlgItem(hDlg, ID_NET_TOGGLE);
    if (toggle)
        SetWindowTextA(toggle, expanded ? "Hide IP Connect"
                                        : "Advanced: IP Connect");

    RECT windowRect = {};
    GetWindowRect(hDlg, &windowRect);
    SetWindowPos(hDlg, NULL, 0, 0,
                 windowRect.right - windowRect.left,
                 windowRect.bottom - windowRect.top + offset,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    g_directConnectExpanded = expanded;

    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    IniSafe_WriteString("Network", "DirectConnectExpanded",
                               expanded ? "1" : "0", settingsPath);

    RedrawWindow(hDlg, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void SendP2Selection(HWND hDlg)
{
    if (g_isHost || !g_dialogConfig.enetHost || !g_dialogConfig.enetPeer) return;

    for (int i = 0; i < 3; ++i)
        UpdateCharFromCombo(hDlg, ID_P2_SLOT1 + i, i, 1);
    UpdateSlotOptionsFromCombos(hDlg);

    LaunchP2SelectionPacket packet = {};
    packet.type = PACKET_LAUNCH_P2_SELECTION;
    snprintf(packet.protocol, sizeof(packet.protocol), "%s", UMVC3HOOK_VERSION);
    for (int i = 0; i < 3; ++i) {
        packet.chars[i] = g_dialogConfig.p2Chars[i];
        packet.colors[i] = g_dialogConfig.p2Colors[i];
        packet.assists[i] = g_dialogConfig.p2Assists[i];
    }
    packet.ready = g_localReady ? 1 : 0;
    packet.simpleMode = g_dialogConfig.simpleMode[1];

    ENetPacket* enetPacket = enet_packet_create(
        &packet, sizeof(packet), ENET_PACKET_FLAG_RELIABLE);
    if (!enetPacket || enet_peer_send(g_dialogConfig.enetPeer, 0, enetPacket) != 0) {
        if (enetPacket) enet_packet_destroy(enetPacket);
        HookLog("UMVC3Hook: Failed to send P2 launch selection\n");
        return;
    }
    enet_host_flush(g_dialogConfig.enetHost);
}

static void SendP1Selection(HWND hDlg)
{
    if (!g_isHost || !g_dialogConfig.enetHost || !g_dialogConfig.enetPeer) return;

    for (int i = 0; i < 3; ++i)
        UpdateCharFromCombo(hDlg, ID_P1_SLOT1 + i, i, 0);
    UpdateSlotOptionsFromCombos(hDlg);
    HWND stage = GetDlgItem(hDlg, ID_STAGE);
    int stageSelection = stage
        ? static_cast<int>(SendMessageA(stage, CB_GETCURSEL, 0, 0))
        : CB_ERR;
    if (stageSelection != CB_ERR)
        g_dialogConfig.stageId = static_cast<int>(
            SendMessageA(stage, CB_GETITEMDATA, stageSelection, 0));

    LaunchP1SelectionPacket packet = {};
    packet.type = PACKET_LAUNCH_P1_SELECTION;
    snprintf(packet.protocol, sizeof(packet.protocol), "%s", UMVC3HOOK_VERSION);
    for (int i = 0; i < 3; ++i) {
        packet.chars[i] = g_dialogConfig.p1Chars[i];
        packet.colors[i] = g_dialogConfig.p1Colors[i];
        packet.assists[i] = g_dialogConfig.p1Assists[i];
    }
    packet.simpleMode = g_dialogConfig.simpleMode[0];
    packet.stageId = g_dialogConfig.stageId;

    ENetPacket* enetPacket = enet_packet_create(
        &packet, sizeof(packet), ENET_PACKET_FLAG_RELIABLE);
    if (!enetPacket || enet_peer_send(g_dialogConfig.enetPeer, 0, enetPacket) != 0) {
        if (enetPacket) enet_packet_destroy(enetPacket);
        HookLog("UMVC3Hook: Failed to send P1 launch selection\n");
        return;
    }
    enet_host_flush(g_dialogConfig.enetHost);
}

static bool ApplyP1Selection(HWND hDlg, const LaunchP1SelectionPacket& packet)
{
    if (g_isHost || packet.type != PACKET_LAUNCH_P1_SELECTION ||
        !IsCompatibleProtocol(packet.protocol) || packet.simpleMode > 1) return false;
    for (int i = 0; i < 3; ++i) {
        if (packet.chars[i] < 0 || packet.chars[i] >= kCharNameCount ||
            packet.colors[i] < 0 ||
            packet.colors[i] >= GetCharacterArchiveVariantCount(packet.chars[i]) ||
            packet.assists[i] < 0 || packet.assists[i] > 2)
            return false;
    }
    const int stageIndex = FindStageIndex(packet.stageId);
    if (kStages[stageIndex].id != packet.stageId) return false;

    for (int i = 0; i < 3; ++i) {
        g_dialogConfig.p1Chars[i] = packet.chars[i];
        g_dialogConfig.p1Colors[i] = packet.colors[i];
        g_dialogConfig.p1Assists[i] = packet.assists[i];
        SendMessageA(GetDlgItem(hDlg, ID_P1_SLOT1 + i),
                     CB_SETCURSEL, packet.chars[i], 0);
        PopulateColorCombo(GetDlgItem(hDlg, ID_P1_COLOR1 + i),
                           packet.chars[i], packet.colors[i]);
        PopulateAssistCombo(GetDlgItem(hDlg, ID_P1_ASSIST1 + i),
                            packet.chars[i], packet.assists[i]);
    }
    g_dialogConfig.simpleMode[0] = packet.simpleMode != 0;
    SendMessageA(GetDlgItem(hDlg, ID_P1_SIMPLE_MODE), BM_SETCHECK,
                 g_dialogConfig.simpleMode[0] ? BST_CHECKED : BST_UNCHECKED, 0);
    g_dialogConfig.stageId = packet.stageId;
    SendMessageA(GetDlgItem(hDlg, ID_STAGE), CB_SETCURSEL,
                 FindComboItemByData(GetDlgItem(hDlg, ID_STAGE), packet.stageId), 0);
    return true;
}

static bool ApplyP2Selection(HWND hDlg, const LaunchP2SelectionPacket& packet)
{
    if (!g_isHost || packet.type != PACKET_LAUNCH_P2_SELECTION ||
        !IsCompatibleProtocol(packet.protocol) ||
        packet.ready > 1 || packet.simpleMode > 1)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (packet.chars[i] < 0 || packet.chars[i] >= kCharNameCount ||
            packet.colors[i] < 0 ||
            packet.colors[i] >= GetCharacterArchiveVariantCount(packet.chars[i]) ||
            packet.assists[i] < 0 || packet.assists[i] > 2)
            return false;
    }
    g_dialogConfig.simpleMode[1] = packet.simpleMode != 0;
    SendMessageA(GetDlgItem(hDlg, ID_P2_SIMPLE_MODE), BM_SETCHECK,
                 g_dialogConfig.simpleMode[1] ? BST_CHECKED : BST_UNCHECKED, 0);

    for (int i = 0; i < 3; ++i) {
        g_dialogConfig.p2Chars[i] = packet.chars[i];
        g_dialogConfig.p2Colors[i] = packet.colors[i];
        g_dialogConfig.p2Assists[i] = packet.assists[i];
        SendMessageA(GetDlgItem(hDlg, ID_P2_SLOT1 + i),
                     CB_SETCURSEL, packet.chars[i], 0);
        PopulateColorCombo(GetDlgItem(hDlg, ID_P2_COLOR1 + i),
                           packet.chars[i], packet.colors[i]);
        PopulateAssistCombo(GetDlgItem(hDlg, ID_P2_ASSIST1 + i),
                            packet.chars[i], packet.assists[i]);
    }
    g_peerReady = packet.ready != 0;
    EnableWindow(GetDlgItem(hDlg, ID_LAUNCH_TRAINING), g_peerReady ? TRUE : FALSE);
    UpdateReadyButton(hDlg);
    HookLog("UMVC3Hook: Applied peer P2 selection ready=%u\n", packet.ready);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static LaunchOption ShowLaunchMenuWindow(bool battleSetupOnly, bool compactMode)
{
    InitializeLaunchMenuDarkMode();
    HookLog("UMVC3Hook: Launch window setup begin compact=%d battleSetup=%d\n",
            compactMode, battleSetupOnly);
    Updater_Init();
    HookLog("UMVC3Hook: Updater startup check complete\n");
    g_isLaunching = false;
    g_battleSetupOnly = battleSetupOnly;
    g_compactMode = compactMode;
    if (!g_compactMode && !g_battleSetupOnly)
        g_regularCompactRequest = false;
    if (g_battleSetupOnly)
        g_battleLobbyReturnRequest = false;
    if (g_compactMode) {
        g_compactAdvancedRequest = false;
        g_compactBattleSetupRequest = false;
    }
    g_localReady = false;
    g_peerReady = false;
    HINSTANCE hInst = GetModuleHandle(NULL);
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);

    // Default config from training boot
    const TrainingBootConfig& def = TrainingBoot_GetDefaultConfig();
    for (int i = 0; i < 3; i++) {
        g_dialogConfig.p1Chars[i] = def.p1Chars[i];
        g_dialogConfig.p2Chars[i] = def.p2Chars[i];
        g_dialogConfig.p1Colors[i] = def.p1Colors[i];
        g_dialogConfig.p2Colors[i] = def.p2Colors[i];
        g_dialogConfig.p1Assists[i] = def.p1Assists[i];
        g_dialogConfig.p2Assists[i] = def.p2Assists[i];
    }
    g_dialogConfig.stageId = def.stageId;
    g_dialogConfig.simpleMode[0] = def.simpleMode[0];
    g_dialogConfig.simpleMode[1] = def.simpleMode[1];

    // Default network settings, then restore the last target from disk.
    strcpy(g_dialogConfig.hostIp, "127.0.0.1");
    g_dialogConfig.hostPort = 7777;
    LoadRollbackSettings();
    g_dialogConfig.hostEnabled = false;
    g_dialogConfig.enetHost = nullptr;
    g_dialogConfig.enetPeer = nullptr;
    HookLog("UMVC3Hook: Initializing Steam lobby support\n");
    const bool steamLobbyReady = SteamLobby_Initialize();
    HookLog("UMVC3Hook: Steam lobby initialization %s\n",
            steamLobbyReady ? "complete" : "unavailable");
    if (steamLobbyReady && !g_savedRostersLoaded) {
        g_savedRosterCount = Savedata_LoadRosters(
            g_savedRosters, sizeof(g_savedRosters) / sizeof(g_savedRosters[0]));
        g_savedRostersLoaded = true;
    }
    uint64_t inviteLobbyId = g_battleSetupOnly ? 0 : GetInviteLobbyId();
    if (inviteLobbyId) {
        HookLog("SteamLobby: received +connect_lobby invite for %llu\n",
                static_cast<unsigned long long>(inviteLobbyId));
        SteamLobby_Join(inviteLobbyId);
    }

    // Create the launch dialog window
    const char CLASS_NAME[] = "UMVC3LaunchMenu";

    WNDCLASS wc = {};
    wc.lpfnWndProc = LaunchDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    // The launcher class is unregistered between menu transitions. Do not give
    // it the shared dark brush because Windows deletes class brushes on unregister.
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    const ATOM windowClass = RegisterClassA(&wc);
    if (!windowClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        HookLog("UMVC3Hook: Launch window class registration failed error=%lu\n",
                GetLastError());

    WNDCLASSA chatClass = {};
    chatClass.lpfnWndProc = ChatWndProc;
    chatClass.hInstance = hInst;
    chatClass.lpszClassName = kChatWindowClass;
    chatClass.hbrBackground = nullptr;
    chatClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    const ATOM registeredChatClass = RegisterClassA(&chatClass);
    if (!registeredChatClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        HookLog("UMVC3Hook: Chat window class registration failed error=%lu\n",
                GetLastError());

    using SetThreadDpiAwarenessContextFn = HANDLE (WINAPI*)(HANDLE);
    const auto setThreadDpiAwarenessContext =
        reinterpret_cast<SetThreadDpiAwarenessContextFn>(GetProcAddress(
            GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext"));
    HANDLE previousDpiContext = nullptr;
    if (setThreadDpiAwarenessContext) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. Resolve it dynamically
        // so the project can retain its Windows 7 compile target.
        previousDpiContext = setThreadDpiAwarenessContext(
            reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    }

    HMODULE asiModule = nullptr;
    HANDLE visualStylesContext = INVALID_HANDLE_VALUE;
    ULONG_PTR visualStylesCookie = 0;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ShowLaunchMenuWindow), &asiModule)) {
        ACTCTXA context = {};
        context.cbSize = sizeof(context);
        context.dwFlags = ACTCTX_FLAG_HMODULE_VALID |
                          ACTCTX_FLAG_RESOURCE_NAME_VALID;
        context.hModule = asiModule;
        context.lpResourceName = MAKEINTRESOURCEA(2);
        visualStylesContext = CreateActCtxA(&context);
        if (visualStylesContext != INVALID_HANDLE_VALUE &&
            !ActivateActCtx(visualStylesContext, &visualStylesCookie)) {
            ReleaseActCtx(visualStylesContext);
            visualStylesContext = INVALID_HANDLE_VALUE;
        }
    }

    HDC screenDc = GetDC(nullptr);
    const int dpi = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) ReleaseDC(nullptr, screenDc);
    int w = MulDiv(g_compactMode ? 656 : (g_battleSetupOnly ? 1220 : 840), dpi, 96);
    int h = MulDiv(g_compactMode ? 448 : (g_battleSetupOnly ? 430 : 780), dpi, 96);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hWnd = CreateWindowExA(
        0, CLASS_NAME, g_compactMode ? "Timestone"
            : g_battleSetupOnly ? "Battle Setup" : "Launch Mode",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_MINIMIZEBOX,
        x, y, w, h,
        NULL, NULL, hInst, NULL
    );

    if (!hWnd) {
        HookLog("UMVC3Hook: Launch window creation failed error=%lu; continuing normal launch\n",
                GetLastError());
        g_launchOption = LAUNCH_NORMAL;
    } else {
        HookLog("UMVC3Hook: Launch window created successfully\n");
        MSG msg = {};
        BOOL messageResult = 0;
        while ((messageResult = GetMessage(&msg, NULL, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (messageResult < 0) {
            HookLog("UMVC3Hook: Launch window message loop failed error=%lu; continuing normal launch\n",
                    GetLastError());
            g_launchOption = LAUNCH_NORMAL;
        }
    }

    if (visualStylesCookie)
        DeactivateActCtx(0, visualStylesCookie);
    if (visualStylesContext != INVALID_HANDLE_VALUE)
        ReleaseActCtx(visualStylesContext);
    if (setThreadDpiAwarenessContext && previousDpiContext)
        setThreadDpiAwarenessContext(previousDpiContext);

    UnregisterClassA(CLASS_NAME, hInst);
    UnregisterClassA(kChatWindowClass, hInst);

    // Only clean up network if NOT launching into gameplay
    if (!g_isLaunching || !g_dialogConfig.hostEnabled) {
        g_networkPolling = false;
        if (g_hPollThread) {
            WaitForSingleObject(g_hPollThread, 1000);
            CloseHandle(g_hPollThread);
            g_hPollThread = NULL;
        }
        if (g_dialogConfig.enetHost) {
            HookLog("UMVC3Hook: Launch menu closing, cleaning up unused enet host\n");
            enet_host_destroy(g_dialogConfig.enetHost);
            g_dialogConfig.enetHost = nullptr;
            g_dialogConfig.enetPeer = nullptr;
        }
    } else {
        HookLog("UMVC3Hook: Launch menu closing, netplay will use enet host from game thread\n");
        // Don't start a background polling thread — netplay.cpp handles ENet I/O
        // synchronously from the game thread (HookedMove). The enet host and peer
        // survive in g_dialogConfig for Netplay_Init to pick up later.
        g_dialogConfig.hostEnabled = true;
    }

    return g_launchOption;
}

LaunchOption ShowRegularLaunchMenu(bool battleSetupOnly)
{
    return ShowLaunchMenuWindow(battleSetupOnly, false);
}

LaunchOption ShowCompactLaunchMenu()
{
    return ShowLaunchMenuWindow(false, true);
}

bool TakeCompactAdvancedRequest()
{
    const bool requested = g_compactAdvancedRequest;
    g_compactAdvancedRequest = false;
    return requested;
}

bool TakeCompactBattleSetupRequest()
{
    const bool requested = g_compactBattleSetupRequest;
    g_compactBattleSetupRequest = false;
    return requested;
}

bool TakeBattleLobbyReturnRequest()
{
    const bool requested = g_battleLobbyReturnRequest;
    g_battleLobbyReturnRequest = false;
    return requested;
}

bool TakeRegularCompactRequest()
{
    const bool requested = g_regularCompactRequest;
    g_regularCompactRequest = false;
    return requested;
}

LaunchOption GetLaunchOption()
{
    return g_launchOption;
}

void SetLaunchOption(LaunchOption opt)
{
    g_launchOption = opt;
}

bool ConfigureAutoTestDualTraining(int32_t characterId, int32_t hostPort)
{
    if (characterId < 1 || characterId >= kCharNameCount ||
        hostPort < 0 || hostPort > 65535)
        return false;

    LoadRollbackSettings();
    if (hostPort != 0)
        g_dialogConfig.hostPort = hostPort;
    const int32_t autoTestAssist = characterId == 23 ? 2 : 0;
    for (int i = 0; i < 3; ++i) {
        int32_t slotCharacter = characterId == 17 && i > 0 ? 1 : characterId;
        g_dialogConfig.p1Chars[i] = slotCharacter;
        g_dialogConfig.p2Chars[i] = slotCharacter;
        g_dialogConfig.p1Colors[i] = 0;
        g_dialogConfig.p2Colors[i] = 0;
        g_dialogConfig.p1Assists[i] = autoTestAssist;
        g_dialogConfig.p2Assists[i] = autoTestAssist;
    }
    g_dialogConfig.simpleMode[0] = false;
    g_dialogConfig.simpleMode[1] = false;
    g_dialogConfig.stageId = 0;
    g_launchOption = LAUNCH_DUAL_TRAINING;
    g_autoTestLaunch = true;

    TrainingBootConfig cfg = {};
    for (int i = 0; i < 3; ++i) {
        int32_t slotCharacter = characterId == 17 && i > 0 ? 1 : characterId;
        cfg.p1Chars[i] = slotCharacter;
        cfg.p2Chars[i] = slotCharacter;
        cfg.p1Colors[i] = 0;
        cfg.p2Colors[i] = 0;
        cfg.p1Assists[i] = autoTestAssist;
        cfg.p2Assists[i] = autoTestAssist;
    }
    cfg.stageId = 0;
    cfg.timerValue = 99;
    TrainingBoot_SetConfig(cfg);
    return true;
}

void ConfigureCommandLineDualTraining()
{
    LoadRollbackSettings();
    if (g_dialogConfig.hostIp[0] == '\0')
        strcpy(g_dialogConfig.hostIp, "127.0.0.1");
    if (g_dialogConfig.hostPort <= 0 || g_dialogConfig.hostPort > 65535)
        g_dialogConfig.hostPort = 7777;

    TrainingBootConfig cfg = {};
    for (int i = 0; i < 3; ++i) {
        cfg.p1Chars[i] = g_dialogConfig.p1Chars[i];
        cfg.p2Chars[i] = g_dialogConfig.p2Chars[i];
        cfg.p1Colors[i] = g_dialogConfig.p1Colors[i];
        cfg.p2Colors[i] = g_dialogConfig.p2Colors[i];
        cfg.p1Assists[i] = g_dialogConfig.p1Assists[i];
        cfg.p2Assists[i] = g_dialogConfig.p2Assists[i];
    }
    cfg.stageId = g_dialogConfig.stageId;
    cfg.simpleMode[0] = g_dialogConfig.simpleMode[0];
    cfg.simpleMode[1] = g_dialogConfig.simpleMode[1];
    cfg.timerValue = 99;
    TrainingBoot_SetConfig(cfg);
    g_launchOption = LAUNCH_DUAL_TRAINING;
}

bool IsAutoTestLaunchEnabled()
{
    return g_autoTestLaunch;
}

const LaunchDialogConfig& GetLaunchDialogConfig()
{
    return g_dialogConfig;
}

void SetNetworkHandles(ENetHost* host, ENetPeer* peer)
{
    g_dialogConfig.enetHost = host;
    g_dialogConfig.enetPeer = peer;
    if (host) g_dialogConfig.hostEnabled = true;
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
static BOOL CALLBACK SetLaunchControlFont(HWND control, LPARAM font)
{
    SendMessageA(control, WM_SETFONT, font, TRUE);
    return TRUE;
}

static UINT GetLaunchWindowDpi(HWND hWnd)
{
    using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
    const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
    if (getDpiForWindow) {
        const UINT dpi = getDpiForWindow(hWnd);
        if (dpi) return dpi;
    }

    HDC dc = GetDC(hWnd);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(hWnd, dc);
    return dpi ? dpi : 96;
}

static void ScaleLaunchMenuChildren(HWND hWnd, UINT oldDpi, UINT newDpi)
{
    if (!oldDpi || oldDpi == newDpi) return;

    for (HWND child = GetWindow(hWnd, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect = {};
        GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, hWnd, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(child, nullptr,
                     MulDiv(rect.left, newDpi, oldDpi),
                     MulDiv(rect.top, newDpi, oldDpi),
                     MulDiv(rect.right - rect.left, newDpi, oldDpi),
                     MulDiv(rect.bottom - rect.top, newDpi, oldDpi),
                     SWP_NOACTIVATE | SWP_NOZORDER);

        char className[32] = {};
        GetClassNameA(child, className, sizeof(className));
        if (_stricmp(className, WC_LISTVIEWA) == 0) {
            HWND header = ListView_GetHeader(child);
            const int columnCount = header ? Header_GetItemCount(header) : 0;
            for (int column = 0; column < columnCount; ++column) {
                ListView_SetColumnWidth(child, column, MulDiv(
                    ListView_GetColumnWidth(child, column), newDpi, oldDpi));
            }
        } else if (_stricmp(className, "COMBOBOX") == 0) {
            const LRESULT width = SendMessageA(child, CB_GETDROPPEDWIDTH, 0, 0);
            if (width > 0) {
                SendMessageA(child, CB_SETDROPPEDWIDTH,
                             MulDiv(static_cast<int>(width), newDpi, oldDpi), 0);
            }
        }
    }
}

static void InitializeDpiLayout(HWND hWnd)
{
    const UINT dpi = GetLaunchWindowDpi(hWnd);
    SetPropA(hWnd, "TimestoneLayoutDpi",
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(dpi)));
    ScaleLaunchMenuChildren(hWnd, 96, dpi);
}

static void UpdateDpiLayout(HWND hWnd, UINT newDpi)
{
    const UINT oldDpi = static_cast<UINT>(reinterpret_cast<INT_PTR>(
        GetPropA(hWnd, "TimestoneLayoutDpi")));
    ScaleLaunchMenuChildren(hWnd, oldDpi ? oldDpi : 96, newDpi);
    SetPropA(hWnd, "TimestoneLayoutDpi",
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(newDpi)));
}

static void DestroyDpiLayout(HWND hWnd)
{
    HFONT font = reinterpret_cast<HFONT>(GetPropA(hWnd, "TimestoneLaunchFont"));
    if (font) DeleteObject(font);
    RemovePropA(hWnd, "TimestoneLaunchFont");
    RemovePropA(hWnd, "TimestoneLayoutDpi");
}

static void ApplyLaunchMenuFont(HWND hDlg)
{
    HFONT oldFont = reinterpret_cast<HFONT>(
        GetPropA(hDlg, "TimestoneLaunchFont"));
    LOGFONTA font = {};
    font.lfHeight = -MulDiv(10, GetLaunchWindowDpi(hDlg), 72);
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    snprintf(font.lfFaceName, sizeof(font.lfFaceName), "MS Shell Dlg 2");
    HFONT launchFont = CreateFontIndirectA(&font);
    if (!launchFont) return;
    SetPropA(hDlg, "TimestoneLaunchFont", launchFont);
    SendMessageA(hDlg, WM_SETFONT, reinterpret_cast<WPARAM>(launchFont), TRUE);
    EnumChildWindows(hDlg, SetLaunchControlFont,
                     reinterpret_cast<LPARAM>(launchFont));
    if (oldFont) DeleteObject(oldFont);
}

static void CreateBattleSetupControls(HWND hDlg)
{
    constexpr int leftX = 20;
    constexpr int rightX = 430;
    constexpr int contentW = 800;

    CreateWindowA("STATIC", "Choose characters and stage for this battle:",
        WS_VISIBLE | WS_CHILD, leftX, 12, 440, 18, hDlg, NULL, NULL, NULL);
    CreateWindowA("BUTTON", "Mute Lobby Music",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        leftX + 470, 8, 140, 26,
        hDlg, (HMENU)ID_LOBBY_MUSIC_MUTE, NULL, NULL);
    CreateWindowA("BUTTON", "Randomize Characters",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        leftX + 620, 8, 180, 26,
        hDlg, (HMENU)ID_RANDOMIZE_CHARS, NULL, NULL);

    CreateWindowA("BUTTON", "Character Select",
        WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, 38, contentW, 190,
        hDlg, NULL, NULL, NULL);

    for (int team = 0; team < 2; ++team) {
        const int x = team == 0 ? leftX + 10 : rightX;
        CreateWindowA("STATIC", team == 0 ? "P1" : "P2",
            WS_VISIBLE | WS_CHILD, x, 58, 22, 18,
            hDlg, team == 0 ? NULL : (HMENU)ID_P2_LABEL, NULL, NULL);
        CreateWindowA("STATIC", "Character", WS_VISIBLE | WS_CHILD,
            x + 24, 58, 130, 18, hDlg, NULL, NULL, NULL);
        CreateWindowA("STATIC", "Color", WS_VISIBLE | WS_CHILD,
            x + 158, 58, 45, 18, hDlg, NULL, NULL, NULL);
        CreateWindowA("STATIC", "Assist", WS_VISIBLE | WS_CHILD,
            x + 207, 58, 170, 18, hDlg, NULL, NULL, NULL);

        for (int slot = 0; slot < 3; ++slot) {
            const int index = team * 3 + slot;
            const int y = 78 + slot * 30;
            char slotLabel[4];
            snprintf(slotLabel, sizeof(slotLabel), "%d", slot + 1);
            CreateWindowA("STATIC", slotLabel, WS_VISIBLE | WS_CHILD,
                x, y + 4, 20, 18, hDlg, NULL, NULL, NULL);

            const int character = team == 0 ? g_dialogConfig.p1Chars[slot]
                                             : g_dialogConfig.p2Chars[slot];
            const int color = team == 0 ? g_dialogConfig.p1Colors[slot]
                                         : g_dialogConfig.p2Colors[slot];
            const int assist = team == 0 ? g_dialogConfig.p1Assists[slot]
                                          : g_dialogConfig.p2Assists[slot];
            HWND charCombo = CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_VSCROLL |
                    CBS_DROPDOWN | CBS_HASSTRINGS | CBS_AUTOHSCROLL,
                x + 24, y, 130, 400, hDlg,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_P1_SLOT1 + index)),
                NULL, NULL);
            PopulateCharCombo(charCombo, character);
            SendMessageA(charCombo, CB_SETDROPPEDWIDTH, 180, 0);

            HWND colorCombo = CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                x + 158, y, 45, 200, hDlg,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_P1_COLOR1 + index)),
                NULL, NULL);
            PopulateColorCombo(colorCombo, character, color);

            HWND assistCombo = CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                x + 207, y, 170, 200, hDlg,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_P1_ASSIST1 + index)),
                NULL, NULL);
            PopulateAssistCombo(assistCombo, character, assist);
            SendMessageA(assistCombo, CB_SETDROPPEDWIDTH, 220, 0);
        }

        HWND simple = CreateWindowA("BUTTON", "Simple Mode Controls",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            x + 24, 174, 180, 20, hDlg,
            (HMENU)(team == 0 ? ID_P1_SIMPLE_MODE : ID_P2_SIMPLE_MODE), NULL, NULL);
        SendMessageA(simple, BM_SETCHECK,
            g_dialogConfig.simpleMode[team] ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateSavedRosterCombo(hDlg, team, x + 214, 202);
    }

    CreateWindowA("STATIC", "Ping: -- ms", WS_CHILD,
        leftX + 230, 176, 190, 18, hDlg, (HMENU)ID_PING_LABEL, NULL, NULL);

    CreateWindowA("BUTTON", "Stage Select", WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
        leftX, 236, contentW, 52, hDlg, NULL, NULL, NULL);
    CreateWindowA("STATIC", "Stage:", WS_VISIBLE | WS_CHILD,
        leftX + 10, 258, 50, 18, hDlg, NULL, NULL, NULL);
    HWND stage = CreateWindowA("COMBOBOX", "",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_VSCROLL |
            CBS_DROPDOWN | CBS_HASSTRINGS | CBS_AUTOHSCROLL,
        leftX + 65, 254, 600, 200, hDlg, (HMENU)ID_STAGE, NULL, NULL);
    PopulateStageCombo(stage, g_dialogConfig.stageId);

    CreateWindowA("BUTTON", "Leave Lobby",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        leftX, 294, 130, 28, hDlg, (HMENU)ID_BATTLE_LEAVE, NULL, NULL);
    CreateWindowA("STATIC", SteamLobby_GetStatus(), WS_VISIBLE | WS_CHILD,
        leftX + 140, 294, 445, 34, hDlg, (HMENU)ID_NET_STATUS, NULL, NULL);
    CreateWindowA("BUTTON", "Not Ready",
        WS_CHILD | BS_OWNERDRAW | WS_DISABLED,
        leftX + 610, 294, 190, 28, hDlg, (HMENU)ID_PEER_READY, NULL, NULL);
    CreateWindowA("BUTTON", "Launch into Battle",
        WS_VISIBLE | WS_CHILD | BS_OWNERDRAW | WS_DISABLED,
        leftX, 330, contentW, 36, hDlg, (HMENU)ID_LAUNCH_TRAINING, NULL, NULL);

    SetTimer(hDlg, ID_STEAM_POLL_TIMER, 50, NULL);
}

static void ReturnBattleSetupToLobbies(HWND hDlg)
{
    if (!g_battleSetupOnly || g_battleLobbyReturnRequest) return;
    g_battleLobbyReturnRequest = true;
    g_launchOption = LAUNCH_CANCELLED;
    DestroyWindow(hDlg);
    PostQuitMessage(0);
}

static INT_PTR CALLBACK LaunchDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    case WM_INITDIALOG:
    {
        if (g_compactMode) {
            const CompactLaunchMenuControlIds ids = {
                ID_STEAM_SEARCH_BTN, ID_STEAM_CREATE_BTN, ID_STEAM_JOIN_BTN,
                ID_STEAM_TYPE, ID_STEAM_NAME, ID_STEAM_STATUS, ID_STEAM_LIST,
                ID_LAUNCH_NORMAL, ID_ADVANCED_CONFIG, ID_ADVANCED_MENU,
                ID_LOBBY_MUSIC_MUTE,
                ID_STEAM_COPY_LINK,
            };
            CreateCompactLaunchMenuControls(hDlg, ids, g_lobbyTypeSelection,
                                             g_lobbyName);
            InitializeDpiLayout(hDlg);
            ApplyLaunchMenuFont(hDlg);
            ApplyLaunchMenuDarkTheme(hDlg);
            UpdateLobbyMusicCheckbox(hDlg);
            SetTimer(hDlg, ID_STEAM_POLL_TIMER, 50, NULL);
            SteamLobby_Search();
            return msg == WM_INITDIALOG ? TRUE : 0;
        }

        if (g_battleSetupOnly) {
            CreateBattleSetupControls(hDlg);
            InitializeDpiLayout(hDlg);
            ApplyLaunchMenuFont(hDlg);
            ApplyLaunchMenuDarkTheme(hDlg);
            UpdateLobbyMusicCheckbox(hDlg);
            return msg == WM_INITDIALOG ? TRUE : 0;
        }

        int y = 15;
        const int leftX = 20;
        const int left2X = 430; // second column start
        const int contentW = 800;

        // ---- Title ----
        CreateWindowA("STATIC", "Configure launch mode and match settings:",
            WS_VISIBLE | WS_CHILD, leftX, y, 440, 18, hDlg, NULL, NULL, NULL);
        CreateWindowA("BUTTON", "Mute Lobby Music",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            leftX + 470, y - 4, 140, 26,
            hDlg, (HMENU)ID_LOBBY_MUSIC_MUTE, NULL, NULL);
        CreateWindowA("BUTTON", "Randomize Characters",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            leftX + 620, y - 4, 180, 26,
            hDlg, (HMENU)ID_RANDOMIZE_CHARS, NULL, NULL);
        y += 28;

        // ---- Character Select Section ----
        const int characterSectionTop = y;
        CreateWindowA("BUTTON", "Character Select",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, contentW, 160,
            hDlg, NULL, NULL, NULL);
        y += 18;

        struct CharSlot { int ctrlId; int team; int slot; };
        CharSlot slots[6] = {
            {ID_P1_SLOT1, 0, 0},
            {ID_P1_SLOT2, 0, 1},
            {ID_P1_SLOT3, 0, 2},
            {ID_P2_SLOT1, 1, 0},
            {ID_P2_SLOT2, 1, 1},
            {ID_P2_SLOT3, 1, 2},
        };

        for (int col = 0; col < 2; ++col) {
            int cx = col == 0 ? leftX + 10 : left2X;
            CreateWindowA("STATIC", col == 0 ? "P1" : "P2",
                WS_VISIBLE | WS_CHILD, cx, y, 22, 18,
                hDlg, col == 0 ? NULL : (HMENU)ID_P2_LABEL, NULL, NULL);
            CreateWindowA("STATIC", "Character",
                WS_VISIBLE | WS_CHILD, cx + 24, y, 130, 18, hDlg, NULL, NULL, NULL);
            CreateWindowA("STATIC", "Color",
                WS_VISIBLE | WS_CHILD, cx + 158, y, 45, 18, hDlg, NULL, NULL, NULL);
            CreateWindowA("STATIC", "Assist",
                WS_VISIBLE | WS_CHILD, cx + 207, y, 170, 18, hDlg, NULL, NULL, NULL);
        }
        y += 18;

        for (int i = 0; i < 6; i++) {
            int col = i < 3 ? 0 : 1;
            int row = i % 3;
            int cx = col == 0 ? leftX + 10 : left2X;
            int cy = y + row * 30;

            char slotLabel[4];
            snprintf(slotLabel, sizeof(slotLabel), "%d", row + 1);
            CreateWindowA("STATIC", slotLabel,
                WS_VISIBLE | WS_CHILD, cx, cy + 4, 20, 18,
                hDlg, NULL, NULL, NULL);

            HWND hChar = CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_VSCROLL |
                CBS_DROPDOWN | CBS_HASSTRINGS | CBS_AUTOHSCROLL,
                cx + 24, cy, 130, 400,
                hDlg, (HMENU)slots[i].ctrlId, NULL, NULL);

            int defaultCharId = (slots[i].team == 0)
                ? g_dialogConfig.p1Chars[slots[i].slot]
                : g_dialogConfig.p2Chars[slots[i].slot];
            int defaultColor = (slots[i].team == 0)
                ? g_dialogConfig.p1Colors[slots[i].slot]
                : g_dialogConfig.p2Colors[slots[i].slot];
            int defaultAssist = (slots[i].team == 0)
                ? g_dialogConfig.p1Assists[slots[i].slot]
                : g_dialogConfig.p2Assists[slots[i].slot];
            PopulateCharCombo(hChar, defaultCharId);
            SendMessageA(hChar, CB_SETDROPPEDWIDTH, 180, 0);

            HWND hColor = CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                cx + 158, cy, 45, 200,
                hDlg, (HMENU)(ID_P1_COLOR1 + i), NULL, NULL);
            PopulateColorCombo(hColor, defaultCharId, defaultColor);

            HWND hAssist = CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                cx + 207, cy, 170, 200,
                hDlg, (HMENU)(ID_P1_ASSIST1 + i), NULL, NULL);
            PopulateAssistCombo(hAssist, defaultCharId, defaultAssist);
            SendMessageA(hAssist, CB_SETDROPPEDWIDTH, 220, 0);
        }
        y += 90;

        HWND p1Simple = CreateWindowA("BUTTON", "Simple Mode Controls",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            leftX + 34, y, 180, 20,
            hDlg, (HMENU)ID_P1_SIMPLE_MODE, NULL, NULL);
        HWND p2Simple = CreateWindowA("BUTTON", "Simple Mode Controls",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            left2X + 34, y, 180, 20,
            hDlg, (HMENU)ID_P2_SIMPLE_MODE, NULL, NULL);
        CreateSavedRosterCombo(hDlg, 0, leftX + 224, y + 25);
        CreateSavedRosterCombo(hDlg, 1, left2X + 224, y + 25);
        CreateWindowA("STATIC", "Ping: -- ms",
            WS_CHILD,
            leftX + 220, y + 2, 200, 18,
            hDlg, (HMENU)ID_PING_LABEL, NULL, NULL);
        SendMessageA(p1Simple, BM_SETCHECK,
                     g_dialogConfig.simpleMode[0] ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(p2Simple, BM_SETCHECK,
                     g_dialogConfig.simpleMode[1] ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 50;

        // ---- Stage Select ----
        y += 5;
        CreateWindowA("BUTTON", "Stage Select",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, contentW, 52,
            hDlg, NULL, NULL, NULL);
        y += 18;

        CreateWindowA("STATIC", "Stage:",
            WS_VISIBLE | WS_CHILD, leftX + 10, y + 4, 50, 18,
            hDlg, NULL, NULL, NULL);

        HWND hStage = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_VSCROLL |
            CBS_DROPDOWN | CBS_HASSTRINGS | CBS_AUTOHSCROLL,
            leftX + 65, y, 600, 200,
            hDlg, (HMENU)ID_STAGE, NULL, NULL);
        PopulateStageCombo(hStage, g_dialogConfig.stageId);
        y += 35;

        // ---- Steam Lobby Section ----
        y += 5;
        CreateWindowA("BUTTON", "Steam Lobby",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, contentW, 235,
            hDlg, NULL, NULL, NULL);
        y += 18;

        CreateWindowA("BUTTON", "Search Lobbies",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX + 10, y, 120, 26,
            hDlg, (HMENU)ID_STEAM_SEARCH_BTN, NULL, NULL);

        CreateWindowA("BUTTON", "Create Lobby",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX + 140, y, 120, 26,
            hDlg, (HMENU)ID_STEAM_CREATE_BTN, NULL, NULL);

        CreateWindowA("STATIC", "Type:",
            WS_VISIBLE | WS_CHILD,
            leftX + 270, y + 4, 35, 18,
            hDlg, NULL, NULL, NULL);

        HWND hLobbyType = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            leftX + 305, y, 120, 100,
            hDlg, (HMENU)ID_STEAM_TYPE, NULL, NULL);
        SendMessageA(hLobbyType, CB_ADDSTRING, 0, (LPARAM)"Public");
        SendMessageA(hLobbyType, CB_ADDSTRING, 0, (LPARAM)"Friends Only");
        SendMessageA(hLobbyType, CB_ADDSTRING, 0, (LPARAM)"Private");
        SendMessageA(hLobbyType, CB_SETCURSEL, g_lobbyTypeSelection, 0);

        CreateWindowA("BUTTON", "Join Lobby",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_DISABLED,
            leftX + 435, y, 120, 26,
            hDlg, (HMENU)ID_STEAM_JOIN_BTN, NULL, NULL);

        CreateWindowA("STATIC", "Name:",
            WS_VISIBLE | WS_CHILD,
            leftX + 565, y + 4, 40, 18,
            hDlg, NULL, NULL, NULL);

        HWND hLobbyName = CreateWindowA("EDIT", g_lobbyName,
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            leftX + 610, y + 2, 180, 22,
            hDlg, (HMENU)ID_STEAM_NAME, NULL, NULL);
        SendMessageA(hLobbyName, EM_SETLIMITTEXT,
                     (WPARAM)(sizeof(g_lobbyName) - 1), 0);

        y += 30;

        CreateWindowA("STATIC", SteamLobby_GetStatus(),
            WS_VISIBLE | WS_CHILD, leftX + 10, y + 2, 644, 18,
            hDlg, (HMENU)ID_STEAM_STATUS, NULL, NULL);
        CreateWindowA("BUTTON", "Copy Lobby Link",
            WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            leftX + 660, y, 130, 22,
            hDlg, (HMENU)ID_STEAM_COPY_LINK, NULL, NULL);
        y += 22;

        // Lobby results table
        HWND hLobbyList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_VSCROLL |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            leftX + 10, y, 780, 150,
            hDlg, (HMENU)ID_STEAM_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(hLobbyList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        const char* columnNames[] = {"Lobby Name", "Username", "Ping"};
        const int columnWidths[] = {315, 290, 125};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNA column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<char*>(columnNames[i]);
            column.cx = columnWidths[i];
            column.iSubItem = i;
            ListView_InsertColumn(hLobbyList, i, &column);
        }
        y += 155;

        // ---- Direct Connect Section ----
        y += 8;
        CreateWindowA("BUTTON", "Advanced: IP Connect",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            leftX, y, contentW, 26,
            hDlg, (HMENU)ID_NET_TOGGLE, NULL, NULL);
        y += 34;

        CreateWindowA("STATIC", "IP:",
            WS_CHILD, leftX + 10, y + 4, 25, 18,
            hDlg, (HMENU)ID_NET_IP_LABEL, NULL, NULL);

        HWND hIpEdit = CreateWindowA("EDIT", g_dialogConfig.hostIp,
            WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NOHIDESEL,
            leftX + 35, y, 300, 22,
            hDlg, (HMENU)ID_NET_IP, NULL, NULL);
        if (hIpEdit) {
            // Keep clipboard pastes bounded and leave native clipboard handling intact.
            SendMessageA(hIpEdit, EM_SETLIMITTEXT,
                         (WPARAM)(sizeof(g_dialogConfig.hostIp) - 1), 0);
        }

        CreateWindowA("STATIC", "Port:",
            WS_CHILD, leftX + 345, y + 4, 35, 18,
            hDlg, (HMENU)ID_NET_PORT_LABEL, NULL, NULL);

        char portStr[16];
        snprintf(portStr, sizeof(portStr), "%d", g_dialogConfig.hostPort);
        CreateWindowA("EDIT", portStr,
            WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            leftX + 380, y, 60, 22,
            hDlg, (HMENU)ID_NET_PORT, NULL, NULL);

        CreateWindowA("BUTTON", "Host Game",
            WS_CHILD | BS_PUSHBUTTON,
            leftX + 500, y - 2, 130, 26,
            hDlg, (HMENU)ID_NET_HOST_BTN, NULL, NULL);

        CreateWindowA("BUTTON", "Connect",
            WS_CHILD | BS_PUSHBUTTON,
            leftX + 640, y - 2, 130, 26,
            hDlg, (HMENU)ID_NET_CONNECT_BTN, NULL, NULL);

        CreateWindowA("STATIC", "Ready",
            WS_CHILD, leftX + 10, y + 32, 780, 18,
            hDlg, (HMENU)ID_NET_STATUS, NULL, NULL);

        // ---- Launch Buttons ----
        CreateWindowA("STATIC", "Choose how to launch the game:",
            WS_VISIBLE | WS_CHILD, leftX, y, 300, 18,
            hDlg, (HMENU)ID_LAUNCH_PROMPT, NULL, NULL);
        CreateWindowA("BUTTON", "Not Ready",
            WS_CHILD | BS_OWNERDRAW | WS_DISABLED,
            leftX + contentW - 160, characterSectionTop + 124, 150, 26,
            hDlg, (HMENU)ID_PEER_READY, NULL, NULL);
        y += 25;

        CreateWindowA("BUTTON", "Launch Normally",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX, y, 395, 32, hDlg, (HMENU)ID_LAUNCH_NORMAL, NULL, NULL);

        CreateWindowA("BUTTON", "Launch into Battle",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            leftX + 405, y, 395, 32, hDlg, (HMENU)ID_LAUNCH_TRAINING, NULL, NULL);
        y += 38;

        CreateWindowA("BUTTON", "Training Mode + Spawn 2nd Instance",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX, y, contentW, 32, hDlg, (HMENU)ID_LAUNCH_DUAL, NULL, NULL);

        y += 38;

        CreateWindowA("BUTTON", "Launch Dual Training with CPUs",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX, y, contentW, 32, hDlg, (HMENU)ID_LAUNCH_DUAL_CPUS, NULL, NULL);
        y += 38;

        CreateWindowA("BUTTON", "Launch Dual Training with P2 CPU",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX, y, contentW, 32, hDlg, (HMENU)ID_LAUNCH_DUAL_P2_CPU, NULL, NULL);

        char versionText[64] = {};
        snprintf(versionText, sizeof(versionText), "Timestone v%s", UMVC3HOOK_VERSION);
        CreateWindowA("STATIC", versionText,
            WS_VISIBLE | WS_CHILD | SS_RIGHT,
            leftX + 600, y + 40, 200, 18,
            hDlg, (HMENU)ID_VERSION_LABEL, NULL, NULL);

        char settingsPath[MAX_PATH] = {};
        GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
        HWND advancedMenu = CreateWindowA("BUTTON", "Advanced launch menu",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            leftX, y + 36, 300, 22,
            hDlg, (HMENU)ID_ADVANCED_MENU, NULL, NULL);
        SendMessageA(advancedMenu, BM_SETCHECK,
            GetPrivateProfileIntA("General", "UseRegularLaunchMenu", 0,
                                  settingsPath) != 0
                ? BST_CHECKED : BST_UNCHECKED,
            0);
        CreateWindowA("BUTTON", "Configure settings",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            leftX + 310, y + 34, 180, 26,
            hDlg, (HMENU)ID_ADVANCED_CONFIG, NULL, NULL);

        InitializeDpiLayout(hDlg);
        if (g_directConnectExpanded) {
            g_directConnectExpanded = false;
            SetDirectConnectExpanded(hDlg, true);
        }

        ApplyLaunchMenuFont(hDlg);
        ApplyLaunchMenuDarkTheme(hDlg);
        UpdateLobbyMusicCheckbox(hDlg);
        SetTimer(hDlg, ID_STEAM_POLL_TIMER, 50, NULL);
        return msg == WM_INITDIALOG ? TRUE : 0;
    }

    case WM_DPICHANGED:
    {
        const UINT dpi = HIWORD(wParam);
        UpdateDpiLayout(hDlg, dpi);
        if (g_hChatWindow && GetParent(g_hChatWindow) == hDlg)
            UpdateDpiLayout(g_hChatWindow, dpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hDlg, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        ApplyLaunchMenuFont(hDlg);
        return 0;
    }

    case WM_TIMER:
        if (wParam == ID_STEAM_POLL_TIMER) {
            if (!g_isHost && g_dialogConfig.enetPeer && !g_localReady)
                InvalidateRect(GetDlgItem(hDlg, ID_PEER_READY), NULL, FALSE);
            if (g_isHost && g_dialogConfig.enetPeer && g_peerReady)
                InvalidateRect(GetDlgItem(hDlg, ID_LAUNCH_TRAINING), NULL, FALSE);

            SteamLobby_Pump();
            UpdateChatAvatar();
            UpdateLobbyMusicCheckbox(hDlg);
            if (g_battleSetupOnly && SteamLobby_GetPeerSteamId() == 0) {
                ReturnBattleSetupToLobbies(hDlg);
                return 0;
            }
            uint64_t replacementLobby = 0;
            if (SteamLobby_TakeReplacementJoin(&replacementLobby)) {
                CleanupNetwork(hDlg, true);
                SteamLobby_Join(replacementLobby);
            }
            char mismatch[256];
            if (SteamLobby_TakeVersionMismatch(mismatch, sizeof(mismatch)))
                MessageBoxA(hDlg, mismatch, "Timestone Version Mismatch",
                            MB_OK | MB_ICONERROR);
            HWND hSteamStatus = GetDlgItem(hDlg, ID_STEAM_STATUS);
            if (!SteamLobby_HasLobby()) g_lobbyLinkCopiedUntil = 0;
            if (hSteamStatus) SetWindowTextA(hSteamStatus,
                GetTickCount64() < g_lobbyLinkCopiedUntil ? "Copied!" : SteamLobby_GetStatus());
            if (g_battleSetupOnly && !g_dialogConfig.enetPeer) {
                HWND hNetStatus = GetDlgItem(hDlg, ID_NET_STATUS);
                if (hNetStatus) SetWindowTextA(hNetStatus, SteamLobby_GetStatus());
            }
            UpdateSteamLobbyButton(hDlg);

            if (g_compactMode && SteamLobby_GetPeerSteamId() != 0) {
                g_compactBattleSetupRequest = true;
                DestroyWindow(hDlg);
                PostQuitMessage(0);
                return 0;
            }

            if (SteamLobby_TakeResultsChanged()) {
                HWND hList = GetDlgItem(hDlg, ID_STEAM_LIST);
                if (hList) {
                    ListView_DeleteAllItems(hList);
                    for (size_t i = 0; i < SteamLobby_GetResultCount(); ++i) {
                        uint64_t lobbyId = SteamLobby_GetResult(i);
                        int ping = SteamLobby_GetResultEstimatedPing(i);
                        char pingText[32];
                        if (ping >= 0) {
                            snprintf(pingText, sizeof(pingText), "~%d ms", ping / 2);
                        } else if (SteamLobby_ArePingEstimatesAvailable()) {
                            snprintf(pingText, sizeof(pingText), "Pending");
                        } else {
                            snprintf(pingText, sizeof(pingText), "Unavailable");
                        }

                        LVITEMA item = {};
                        item.mask = LVIF_TEXT | LVIF_PARAM;
                        item.iItem = static_cast<int>(i);
                        item.pszText = const_cast<char*>(SteamLobby_GetResultName(i));
                        item.lParam = static_cast<LPARAM>(lobbyId);
                        int row = ListView_InsertItem(hList, &item);
                        if (row >= 0) {
                            ListView_SetItemText(hList, row, 1,
                                const_cast<char*>(SteamLobby_GetResultUsername(i)));
                            ListView_SetItemText(hList, row, 2, pingText);
                        }
                    }
                }
            }

            ENetHost* steamHost = nullptr;
            ENetPeer* steamPeer = nullptr;
            bool steamIsHost = false;
            if (SteamLobby_TakeNetworkHandles(&steamHost, &steamPeer, &steamIsHost)) {
                g_dialogConfig.enetHost = steamHost;
                g_dialogConfig.enetPeer = steamPeer;
                g_dialogConfig.hostEnabled = true;
                g_isHost = steamIsHost;
                if (steamPeer) steamPeer->data = (void*)"connected";
                LaunchMusic_Start();
                SetTimer(hDlg, ID_NET_POLL_TIMER, 10, NULL);

                HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);
                if (hStatus) {
                    char peerStatus[256];
                    snprintf(peerStatus, sizeof(peerStatus),
                             "Steam connected: %s. Make your selection, NOW",
                             SteamLobby_GetPeerUsername());
                    SetWindowTextA(hStatus, peerStatus);
                    g_statusColor = steamIsHost ? RGB(0, 120, 215) : RGB(200, 180, 0);
                    InvalidateRect(hStatus, NULL, TRUE);
                }
                ConfigureConnectedControls(hDlg);
                if (steamIsHost) SendP1Selection(hDlg);
                else SendP2Selection(hDlg);
                if (steamPeer) ShowChatWindow(hDlg);
                FocusLaunchMenu(hDlg);
            }
            UpdatePingLabel(hDlg);
            return 0;
        }
        if (wParam == ID_NET_POLL_TIMER) {
            if (g_dialogConfig.enetHost) {
                ENetEvent event;
                int result = enet_host_service(g_dialogConfig.enetHost, &event, 0);

                if (result > 0) {
                    if (event.type == ENET_EVENT_TYPE_CONNECT) {
                        uint8_t* ip = (uint8_t*)&event.peer->address.host;
                        HookLog("UMVC3Hook: CONNECT event from %u.%u.%u.%u:%u (g_isHost=%d)\n",
                            ip[0], ip[1], ip[2], ip[3],
                            event.peer->address.port, g_isHost);
                        g_dialogConfig.enetPeer = event.peer;
                        event.peer->data = (void*)"connected";
                        LaunchMusic_Start();

                        // Keep a generous timeout through loading and the intro barrier.
                        enet_peer_timeout(event.peer, 0, 15000, 15000);

                        HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);
                        if (hStatus) {
                            if (g_isHost) {
                                SetWindowTextA(hStatus, "Client connected: make your selection, NOW");
                                g_statusColor = RGB(0, 120, 215);
                                g_peerReady = false;
                                ConfigureConnectedControls(hDlg);
                                SendP1Selection(hDlg);
                            } else {
                                SetWindowTextA(hStatus, "Connected: make your selection, NOW");
                                g_statusColor = RGB(200, 180, 0);
                                g_localReady = false;
                                ConfigureConnectedControls(hDlg);
                                SendP2Selection(hDlg);
                            }
                            InvalidateRect(hStatus, NULL, TRUE);
                        }
                        ShowChatWindow(hDlg);
                        FocusLaunchMenu(hDlg);
                    }
                    else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                        uint8_t* ip = (uint8_t*)&event.peer->address.host;
                        HookLog("UMVC3Hook: DISCONNECT event from %u.%u.%u.%u:%u\n",
                            ip[0], ip[1], ip[2], ip[3],
                            event.peer->address.port);
                        g_dialogConfig.enetPeer = nullptr;
                        UpdateLobbyMusicCheckbox(hDlg);
                        g_localReady = false;
                        g_peerReady = false;
                        DestroyChatWindow();
                        if (g_battleSetupOnly) {
                            ReturnBattleSetupToLobbies(hDlg);
                            return 0;
                        }
                        CleanupNetwork(hDlg, true);
                        SteamLobby_Initialize();
                        SteamLobby_Search();
                    }
                    else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                        uint32_t packetType = event.packet->dataLength >= sizeof(uint32_t)
                            ? *(uint32_t*)event.packet->data : UINT32_MAX;
                        if (packetType == PACKET_LAUNCH_CHAT) {
                            if (event.packet->dataLength == sizeof(LaunchChatPacket)) {
                                const auto& chat =
                                    *reinterpret_cast<const LaunchChatPacket*>(
                                        event.packet->data);
                                ReceiveChatPacket(chat);
                            } else {
                                HookLog("UMVC3Hook: Rejected wrong-sized launch chat packet\n");
                            }
                        }
                        else if (packetType == PACKET_LAUNCH_P1_SELECTION &&
                            event.packet->dataLength != sizeof(LaunchP1SelectionPacket)) {
                            enet_packet_destroy(event.packet);
                            ShowVersionMismatch(hDlg, nullptr);
                            return 0;
                        }
                        if (event.packet->dataLength == sizeof(LaunchP1SelectionPacket) &&
                            packetType == PACKET_LAUNCH_P1_SELECTION) {
                            const auto& selection =
                                *(const LaunchP1SelectionPacket*)event.packet->data;
                            if (!IsCompatibleProtocol(selection.protocol)) {
                                char remoteVersion[sizeof(selection.protocol) + 1] = {};
                                memcpy(remoteVersion, selection.protocol,
                                       sizeof(selection.protocol));
                                enet_packet_destroy(event.packet);
                                ShowVersionMismatch(hDlg, remoteVersion);
                                return 0;
                            }
                            if (!ApplyP1Selection(hDlg,
                                    selection))
                                HookLog("UMVC3Hook: Rejected invalid P1 launch selection\n");
                        }
                        else if (packetType == PACKET_LAUNCH_P2_SELECTION &&
                                 event.packet->dataLength != sizeof(LaunchP2SelectionPacket)) {
                            enet_packet_destroy(event.packet);
                            ShowVersionMismatch(hDlg, nullptr);
                            return 0;
                        }
                        else if (event.packet->dataLength == sizeof(LaunchP2SelectionPacket) &&
                                  packetType == PACKET_LAUNCH_P2_SELECTION) {
                            const auto& selection =
                                *(const LaunchP2SelectionPacket*)event.packet->data;
                            if (!IsCompatibleProtocol(selection.protocol)) {
                                char remoteVersion[sizeof(selection.protocol) + 1] = {};
                                memcpy(remoteVersion, selection.protocol,
                                       sizeof(selection.protocol));
                                enet_packet_destroy(event.packet);
                                ShowVersionMismatch(hDlg, remoteVersion);
                                return 0;
                            }
                             if (!ApplyP2Selection(hDlg,
                                     selection))
                                 HookLog("UMVC3Hook: Rejected invalid P2 launch selection\n");
                         }
                        else if (event.packet->dataLength == sizeof(DelayPacket) &&
                                 packetType == PACKET_DELAY) {
                            const auto& settings =
                                *(const DelayPacket*)event.packet->data;
                            if (settings.delay >= 0 && settings.delay <= 15 &&
                                settings.rollbackFrames >= 0 &&
                                settings.rollbackFrames <= 16 &&
                                (settings.delay != 0 || settings.rollbackFrames != 0)) {
                                Controller_SetDelayFrames(settings.delay);
                                Controller_SetRollbackFrames(settings.rollbackFrames);
                                HookLog("UMVC3Hook: Applied host netplay settings %dD/%dR\n",
                                        settings.delay, settings.rollbackFrames);
                            } else {
                                HookLog("UMVC3Hook: Rejected invalid host netplay settings\n");
                            }
                        }
                        else if (event.packet->dataLength == sizeof(LaunchTrainingPacket) &&
                            *(uint32_t*)event.packet->data == PACKET_LAUNCH_TRAINING) {
                            const LaunchTrainingPacket* launch =
                                (const LaunchTrainingPacket*)event.packet->data;
                            if (!ApplyTrainingLaunchPacket(*launch)) {
                                enet_packet_destroy(event.packet);
                                return 0;
                            }
                            SaveRollbackSettings();
                            g_isLaunching = true;
                            g_launchOption = LAUNCH_TRAINING;
                            HookLog("UMVC3Hook: Received training launch packet\n");
                            enet_packet_destroy(event.packet);
                            DestroyWindow(hDlg);
                            PostQuitMessage(0);
                            return 0;
                        }
                        enet_packet_destroy(event.packet);
                    }
                } else if (result < 0) {
                    HookLog("UMVC3Hook: enet_host_service error: %d\n", result);
                    if (g_battleSetupOnly) {
                        ReturnBattleSetupToLobbies(hDlg);
                        return 0;
                    }
                    CleanupNetwork(hDlg, true);
                    SteamLobby_Initialize();
                    SteamLobby_Search();
                }
            }
            UpdatePingLabel(hDlg);
        }
        return 0;

    case WM_NOTIFY:
    {
        const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
        if (!header || header->idFrom != ID_STEAM_LIST) break;

        if (header->code == LVN_ITEMCHANGED) {
            HWND hList = header->hwndFrom;
            int row = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
            uint64_t lobbyId = 0;
            if (row >= 0) {
                LVITEMA item = {};
                item.mask = LVIF_PARAM;
                item.iItem = row;
                if (ListView_GetItem(hList, &item))
                    lobbyId = static_cast<uint64_t>(item.lParam);
            }
            EnableWindow(GetDlgItem(hDlg, ID_STEAM_JOIN_BTN),
                         lobbyId && !SteamLobby_IsHostedLobby(lobbyId));
            return 0;
        }

        if (header->code == NM_DBLCLK) {
            const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
            if (activate->iItem >= 0) JoinSelectedSteamLobby(hDlg);
            return 0;
        }

        if (header->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                const bool selected = (ListView_GetItemState(header->hwndFrom,
                    static_cast<int>(draw->nmcd.dwItemSpec), LVIS_SELECTED) &
                    LVIS_SELECTED) != 0;
                // Keep native selection painting from replacing the dark palette.
                if (IsLaunchMenuDarkModeEnabled())
                    draw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_HOT);
                draw->clrTextBk = selected && IsLaunchMenuDarkModeEnabled() ? RGB(53, 75, 99)
                                          : LaunchMenuDarkControlColor();
                draw->clrText = LaunchMenuDarkTextColor();
                LVITEMA item = {};
                item.mask = LVIF_PARAM;
                item.iItem = static_cast<int>(draw->nmcd.dwItemSpec);
                if (ListView_GetItem(header->hwndFrom, &item)) {
                    uint64_t lobbyId = static_cast<uint64_t>(item.lParam);
                    if (SteamLobby_IsHostedLobby(lobbyId))
                        draw->clrText = IsLaunchMenuDarkModeEnabled() ? RGB(136, 136, 136) : GetSysColor(COLOR_GRAYTEXT);
                    else if (!SteamLobby_IsLobbyVersionCompatible(lobbyId))
                        draw->clrText = IsLaunchMenuDarkModeEnabled() ? RGB(230, 140, 140) : RGB(180, 70, 70);
                    else if (SteamLobby_IsFriendLobby(lobbyId))
                        draw->clrText = IsLaunchMenuDarkModeEnabled() ? RGB(143, 195, 143) : RGB(0, 128, 0);
                }
            }
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_ADVANCED_CONFIG:
            ShowAdvancedConfigModal(hDlg);
            return 0;

        case ID_RANDOMIZE_CHARS:
            RandomizeCharacters(hDlg);
            if (g_dialogConfig.enetPeer) {
                if (g_isHost) {
                    SendP1Selection(hDlg);
                } else {
                    g_localReady = false;
                    UpdateReadyButton(hDlg);
                    SendP2Selection(hDlg);
                }
            }
            return 0;

        case ID_PEER_READY:
            if (!g_isHost && g_dialogConfig.enetPeer) {
                g_localReady = !g_localReady;
                UpdateReadyButton(hDlg);
                if (g_localReady) SaveRollbackSettings();
                SendP2Selection(hDlg);
            }
            return 0;

        case ID_P1_SIMPLE_MODE:
        case ID_P2_SIMPLE_MODE:
            if (HIWORD(wParam) == BN_CLICKED) {
                const int team = LOWORD(wParam) == ID_P1_SIMPLE_MODE ? 0 : 1;
                g_dialogConfig.simpleMode[team] = SendMessageA(
                    (HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (g_dialogConfig.enetPeer) {
                    if (team == 0 && g_isHost)
                        SendP1Selection(hDlg);
                    if (team == 1 && !g_isHost) {
                        g_localReady = false;
                        UpdateReadyButton(hDlg);
                        SendP2Selection(hDlg);
                    }
                }
            }
            return 0;

        case ID_P1_SAVED_ROSTER:
        case ID_P2_SAVED_ROSTER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const int selection = static_cast<int>(SendMessageA(
                    reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0));
                if (selection > 0) {
                    const int team = LOWORD(wParam) == ID_P1_SAVED_ROSTER ? 0 : 1;
                    ApplySavedRoster(hDlg, team, static_cast<size_t>(selection - 1));
                }
            }
            return 0;

        // ---- Launch buttons ----
        case ID_LAUNCH_NORMAL:
        {
            if (g_compactMode) {
                g_isLaunching = true;
                g_launchOption = LAUNCH_NORMAL;
                DestroyWindow(hDlg);
                PostQuitMessage(0);
                return 0;
            }

            for (int i = 0; i < 3; ++i) {
                UpdateCharFromCombo(hDlg, ID_P1_SLOT1 + i, i, 0);
                UpdateCharFromCombo(hDlg, ID_P2_SLOT1 + i, i, 1);
            }
            UpdateSlotOptionsFromCombos(hDlg);
            SaveRollbackSettings();
            // Apply network config before launching
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            if (hIp) GetWindowTextA(hIp, g_dialogConfig.hostIp, sizeof(g_dialogConfig.hostIp));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.hostPort = atoi(buf);
            }
            SaveRollbackSettings();

            g_isLaunching = true;
            g_launchOption = LAUNCH_NORMAL;
            DestroyWindow(hDlg);
            PostQuitMessage(0);
            return 0;
        }
        case ID_LAUNCH_TRAINING:
        case ID_LAUNCH_DUAL:
        case ID_LAUNCH_DUAL_CPUS:
        case ID_LAUNCH_DUAL_P2_CPU:
        {
            if (LOWORD(wParam) == ID_LAUNCH_TRAINING && g_isHost &&
                g_dialogConfig.enetPeer && !g_peerReady) {
                HookLog("UMVC3Hook: Launch ignored because peer is not ready\n");
                return 0;
            }
            // Read character selections from combo boxes
            for (int i = 0; i < 3; i++) {
                UpdateCharFromCombo(hDlg, ID_P1_SLOT1 + i, i, 0);
                UpdateCharFromCombo(hDlg, ID_P2_SLOT1 + i, i, 1);
            }
            UpdateSlotOptionsFromCombos(hDlg);

            // Read stage selection
            HWND hStage = GetDlgItem(hDlg, ID_STAGE);
            if (hStage) {
                int sel = (int)SendMessageA(hStage, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR)
                    g_dialogConfig.stageId = static_cast<int>(
                        SendMessageA(hStage, CB_GETITEMDATA, sel, 0));
            }

            // Read network config
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            if (hIp) GetWindowTextA(hIp, g_dialogConfig.hostIp, sizeof(g_dialogConfig.hostIp));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.hostPort = atoi(buf);
            }

            // Apply config to training boot
            SaveRollbackSettings();
            TrainingBoot_SetCpuMode(
                LOWORD(wParam) == ID_LAUNCH_DUAL_CPUS ? TRAINING_CPU_BOTH :
                LOWORD(wParam) == ID_LAUNCH_DUAL_P2_CPU ? TRAINING_CPU_P2_ONLY :
                TRAINING_CPU_NONE);
            SendTrainingLaunchPacket();
            TrainingBootConfig cfg = {};
            for (int i = 0; i < 3; i++) {
                cfg.p1Chars[i] = g_dialogConfig.p1Chars[i];
                cfg.p2Chars[i] = g_dialogConfig.p2Chars[i];
                cfg.p1Colors[i] = g_dialogConfig.p1Colors[i];
                cfg.p2Colors[i] = g_dialogConfig.p2Colors[i];
                cfg.p1Assists[i] = g_dialogConfig.p1Assists[i];
                cfg.p2Assists[i] = g_dialogConfig.p2Assists[i];
            }
            cfg.stageId = g_dialogConfig.stageId;
            cfg.simpleMode[0] = g_dialogConfig.simpleMode[0];
            cfg.simpleMode[1] = g_dialogConfig.simpleMode[1];
            cfg.timerValue = 99;
            TrainingBoot_SetConfig(cfg);

            g_isLaunching = true;
            g_launchOption = (LOWORD(wParam) == ID_LAUNCH_TRAINING)
                ? LAUNCH_TRAINING : LAUNCH_DUAL_TRAINING;
            DestroyWindow(hDlg);
            PostQuitMessage(0);
            return 0;
        }

        // ---- Character combo box selection ----
        case ID_P1_SLOT1: case ID_P1_SLOT2: case ID_P1_SLOT3:
        case ID_P2_SLOT1: case ID_P2_SLOT2: case ID_P2_SLOT3:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                HWND hCombo = (HWND)lParam;
                int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    int charId = (int)SendMessageA(hCombo, CB_GETITEMDATA, sel, 0);
                    int ctrlId = LOWORD(wParam);
                    // Determine which slot this is
                    int idx = ctrlId - ID_P1_SLOT1;
                    if (idx >= 0 && idx < 3) {
                        g_dialogConfig.p1Chars[idx] = charId;
                    } else if (idx >= 3 && idx < 6) {
                        g_dialogConfig.p2Chars[idx - 3] = charId;
                    }
                    int defaultColor = idx < 3
                        ? g_dialogConfig.p1Colors[idx]
                        : g_dialogConfig.p2Colors[idx - 3];
                    PopulateColorCombo(GetDlgItem(hDlg, ID_P1_COLOR1 + idx),
                                       charId, defaultColor);
                    int defaultAssist = idx < 3
                        ? g_dialogConfig.p1Assists[idx]
                        : g_dialogConfig.p2Assists[idx - 3];
                    PopulateAssistCombo(GetDlgItem(hDlg, ID_P1_ASSIST1 + idx),
                                        charId, defaultAssist);
                }
            }
            if (HIWORD(wParam) == CBN_EDITCHANGE) {
                HWND hCombo = (HWND)lParam;
                char buf[64];
                GetWindowTextA(hCombo, buf, sizeof(buf));
                // Try to find matching character name
                for (int i = 0; i < kCharNameCount; i++) {
                    if (_stricmp(buf, kCharNames[i]) == 0) {
                        int ctrlId = LOWORD(wParam);
                        int idx = ctrlId - ID_P1_SLOT1;
                        if (idx >= 0 && idx < 3) {
                            g_dialogConfig.p1Chars[idx] = i;
                        } else if (idx >= 3 && idx < 6) {
                            g_dialogConfig.p2Chars[idx - 3] = i;
                        }
                        int defaultColor = idx < 3
                            ? g_dialogConfig.p1Colors[idx]
                            : g_dialogConfig.p2Colors[idx - 3];
                        PopulateColorCombo(GetDlgItem(hDlg, ID_P1_COLOR1 + idx),
                                           i, defaultColor);
                        int defaultAssist = idx < 3
                            ? g_dialogConfig.p1Assists[idx]
                            : g_dialogConfig.p2Assists[idx - 3];
                        PopulateAssistCombo(GetDlgItem(hDlg, ID_P1_ASSIST1 + idx),
                                            i, defaultAssist);
                        break;
                    }
                }
            }
            if (HIWORD(wParam) == CBN_SELCHANGE && !g_isHost &&
                LOWORD(wParam) >= ID_P2_SLOT1 &&
                LOWORD(wParam) <= ID_P2_SLOT3 && g_dialogConfig.enetPeer) {
                g_localReady = false;
                UpdateReadyButton(hDlg);
                SendP2Selection(hDlg);
            }
            if (HIWORD(wParam) == CBN_SELCHANGE && g_isHost &&
                LOWORD(wParam) >= ID_P1_SLOT1 &&
                LOWORD(wParam) <= ID_P1_SLOT3 && g_dialogConfig.enetPeer)
                SendP1Selection(hDlg);
            return 0;

        case ID_P1_COLOR1: case ID_P1_COLOR2: case ID_P1_COLOR3:
        case ID_P2_COLOR1: case ID_P2_COLOR2: case ID_P2_COLOR3:
        case ID_P1_ASSIST1: case ID_P1_ASSIST2: case ID_P1_ASSIST3:
        case ID_P2_ASSIST1: case ID_P2_ASSIST2: case ID_P2_ASSIST3:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                UpdateSlotOptionsFromCombos(hDlg);
                int controlId = LOWORD(wParam);
                if (!g_isHost &&
                    ((controlId >= ID_P2_COLOR1 && controlId <= ID_P2_COLOR3) ||
                     (controlId >= ID_P2_ASSIST1 && controlId <= ID_P2_ASSIST3)) &&
                     g_dialogConfig.enetPeer) {
                    g_localReady = false;
                    UpdateReadyButton(hDlg);
                    SendP2Selection(hDlg);
                }
                if (g_isHost &&
                    ((controlId >= ID_P1_COLOR1 && controlId <= ID_P1_COLOR3) ||
                     (controlId >= ID_P1_ASSIST1 && controlId <= ID_P1_ASSIST3)) &&
                    g_dialogConfig.enetPeer)
                    SendP1Selection(hDlg);
            }
            return 0;

        // ---- Stage combo box ----
        case ID_STAGE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                HWND hCombo = (HWND)lParam;
                int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    g_dialogConfig.stageId = static_cast<int>(
                        SendMessageA(hCombo, CB_GETITEMDATA, sel, 0));
                    if (g_isHost && g_dialogConfig.enetPeer)
                        SendP1Selection(hDlg);
                }
            }
            return 0;

        case ID_NET_TOGGLE:
            SetDirectConnectExpanded(hDlg, !g_directConnectExpanded);
            return 0;

        case ID_LOBBY_MUSIC_MUTE:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool muted = SendMessageA(
                    reinterpret_cast<HWND>(lParam), BM_GETCHECK, 0, 0) == BST_CHECKED;
                LaunchMusic_SetMuted(muted);
                UpdateLobbyMusicCheckbox(hDlg);
            }
            return 0;

        case ID_ADVANCED_MENU:
            if (HIWORD(wParam) == BN_CLICKED) {
                char settingsPath[MAX_PATH] = {};
                GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
                const bool enabled = SendMessageA(
                    reinterpret_cast<HWND>(lParam), BM_GETCHECK, 0, 0) == BST_CHECKED;
                IniSafe_WriteString("General", "UseRegularLaunchMenu",
                                           enabled ? "1" : "0", settingsPath);
                if (g_compactMode && enabled) {
                    g_compactAdvancedRequest = true;
                    DestroyWindow(hDlg);
                    PostQuitMessage(0);
                } else if (!g_compactMode && !g_battleSetupOnly && !enabled) {
                    g_regularCompactRequest = true;
                    g_launchOption = LAUNCH_CANCELLED;
                    DestroyWindow(hDlg);
                    PostQuitMessage(0);
                }
            }
            return 0;

        case ID_BATTLE_LEAVE:
            if (g_battleSetupOnly) {
                ReturnBattleSetupToLobbies(hDlg);
            }
            return 0;

        // ---- Network Host button ----
        case ID_NET_HOST_BTN:
        {
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);

            if (hIp) GetWindowTextA(hIp, g_dialogConfig.hostIp, sizeof(g_dialogConfig.hostIp));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.hostPort = atoi(buf);
            }
            SaveRollbackSettings();

            // Switch log to host-specific file
            HookLogSetFile("Timestone_host.log");
            HookLog("UMVC3Hook: Host button clicked, port %d\n", g_dialogConfig.hostPort);

            // Create enet host (server mode)
            if (g_dialogConfig.enetHost) {
                HookLog("UMVC3Hook: Already have a host, cleaning up first\n");
                CleanupNetwork(hDlg);
            }

            ENetAddress addr;
            addr.host = ENET_HOST_ANY;
            addr.port = (enet_uint16)g_dialogConfig.hostPort;

            ENetHost* server = enet_host_create(&addr, 1, 2, 0, 0);
            if (!server) {
                HookLog("UMVC3Hook: enet_host_create FAILED on port %d\n", g_dialogConfig.hostPort);
                if (hStatus) SetWindowTextA(hStatus, "Host: enet_host_create failed!");
                return 0;
            }
            Netplay_ConfigureHostLimits(server);

            HookLog("UMVC3Hook: enet_host_create OK on port %d, host addr=%u.%u.%u.%u:%u\n",
                g_dialogConfig.hostPort,
                (server->address.host >> 0) & 0xFF,
                (server->address.host >> 8) & 0xFF,
                (server->address.host >> 16) & 0xFF,
                (server->address.host >> 24) & 0xFF,
                server->address.port);
            g_dialogConfig.enetHost = server;
            g_dialogConfig.enetPeer = nullptr;
            g_isHost = true;
            g_statusColor = LaunchMenuDarkTextColor();

            if (hStatus) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Listening on port %d...", g_dialogConfig.hostPort);
                SetWindowTextA(hStatus, msg);
            }

            // Start non-threaded polling timer inside launch window
            SetTimer(hDlg, ID_NET_POLL_TIMER, 10, NULL);
            EnableWindow(GetDlgItem(hDlg, ID_NET_HOST_BTN), FALSE);
            EnableWindow(GetDlgItem(hDlg, ID_NET_CONNECT_BTN), FALSE);
            g_dialogConfig.hostEnabled = true;
            return 0;
        }

        // ---- Network Connect button ----
        case ID_NET_CONNECT_BTN:
        {
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);

            if (hIp) GetWindowTextA(hIp, g_dialogConfig.hostIp, sizeof(g_dialogConfig.hostIp));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.hostPort = atoi(buf);
            }
            SaveRollbackSettings();

            // Switch log to client-specific file
            HookLogSetFile("Timestone_client.log");
            HookLog("UMVC3Hook: Connect button clicked, target %s:%d\n",
                g_dialogConfig.hostIp, g_dialogConfig.hostPort);

            // Create client host
            if (g_dialogConfig.enetHost) {
                HookLog("UMVC3Hook: Already have a host, cleaning up first\n");
                CleanupNetwork(hDlg);
            }

            ENetHost* client = enet_host_create(NULL, 1, 2, 0, 0);
            if (!client) {
                HookLog("UMVC3Hook: Client enet_host_create FAILED\n");
                if (hStatus) SetWindowTextA(hStatus, "Connect: enet_host_create failed!");
                return 0;
            }
            Netplay_ConfigureHostLimits(client);

            HookLog("UMVC3Hook: Client enet_host_create OK, client addr=%u.%u.%u.%u:%u\n",
                (client->address.host >> 0) & 0xFF,
                (client->address.host >> 8) & 0xFF,
                (client->address.host >> 16) & 0xFF,
                (client->address.host >> 24) & 0xFF,
                client->address.port);
            g_dialogConfig.enetHost = client;
            g_dialogConfig.enetPeer = nullptr;

            // Resolve target address
            ENetAddress targetAddr;
            if (enet_address_set_host_ip(&targetAddr, g_dialogConfig.hostIp) != 0) {
                if (enet_address_set_host(&targetAddr, g_dialogConfig.hostIp) != 0) {
                    HookLog("UMVC3Hook: Failed to resolve address '%s'\n", g_dialogConfig.hostIp);
                    if (hStatus) SetWindowTextA(hStatus, "Invalid IP address!");
                    enet_host_destroy(client);
                    g_dialogConfig.enetHost = nullptr;
                    return 0;
                }
            }
            targetAddr.port = (enet_uint16)g_dialogConfig.hostPort;

            // Initiate connection
            ENetPeer* peer = enet_host_connect(client, &targetAddr, 2, 0);
            if (!peer) {
                HookLog("UMVC3Hook: enet_host_connect returned NULL\n");
                if (hStatus) SetWindowTextA(hStatus, "Connect failed!");
                enet_host_destroy(client);
                g_dialogConfig.enetHost = nullptr;
                return 0;
            }

            HookLog("UMVC3Hook: enet_host_connect OK, polling via timer\n");
            // Set connect timeout to 1000ms (default is ~15s)
            enet_peer_timeout(peer, 0, 1000, 1000);
            g_isHost = false;
            g_statusColor = LaunchMenuDarkTextColor();
            if (hStatus) SetWindowTextA(hStatus, "Connecting...");

            // Start non-threaded polling timer inside launch window
            SetTimer(hDlg, ID_NET_POLL_TIMER, 10, NULL);
            EnableWindow(GetDlgItem(hDlg, ID_NET_HOST_BTN), FALSE);
            EnableWindow(GetDlgItem(hDlg, ID_NET_CONNECT_BTN), FALSE);
            g_dialogConfig.hostEnabled = true;
            return 0;
        }

        // ---- Steam Lobby buttons ----
        case ID_STEAM_TYPE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int selection = static_cast<int>(SendMessageA(
                    reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0));
                if (selection >= 0 && selection <= 2) {
                    g_lobbyTypeSelection = selection;
                    SaveLobbyTypeSelection();
                }
            }
            return 0;

        case ID_STEAM_NAME:
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                GetWindowTextA(reinterpret_cast<HWND>(lParam), g_lobbyName,
                               sizeof(g_lobbyName));
                SaveLobbyName();
                SteamLobby_SetName(g_lobbyName);
            }
            return 0;

        case ID_STEAM_SEARCH_BTN:
        {
            HWND hList = GetDlgItem(hDlg, ID_STEAM_LIST);
            if (hList) ListView_DeleteAllItems(hList);
            SteamLobby_Search();
            return 0;
        }

        case ID_STEAM_CREATE_BTN:
        {
            if (SteamLobby_HasLobby())
                CleanupNetwork(hDlg, true);
            else {
                GetWindowTextA(GetDlgItem(hDlg, ID_STEAM_NAME), g_lobbyName,
                               sizeof(g_lobbyName));
                SaveLobbyName();
                SteamLobby_SetName(g_lobbyName);
                int selection = static_cast<int>(SendMessageA(
                    GetDlgItem(hDlg, ID_STEAM_TYPE), CB_GETCURSEL, 0, 0));
                SteamLobbyType type = selection == 1
                    ? SteamLobbyType::FriendsOnly
                    : selection == 2 ? SteamLobbyType::Private
                                     : SteamLobbyType::Public;
                SteamLobby_Create(type);
            }
            UpdateSteamLobbyButton(hDlg);
            return 0;
        }

        case ID_STEAM_COPY_LINK:
            if (CopySteamLobbyJoinLink(hDlg)) {
                g_lobbyLinkCopiedUntil = GetTickCount64() + 2000;
                SetWindowTextA(GetDlgItem(hDlg, ID_STEAM_STATUS), "Copied!");
            } else
                MessageBoxA(hDlg, "Could not copy the lobby join link. Please try again.",
                            "Copy Lobby Link", MB_OK | MB_ICONWARNING);
            return 0;

        case ID_STEAM_JOIN_BTN:
        {
            JoinSelectedSteamLobby(hDlg);
            return 0;
        }
        }
        break;

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (draw->CtlID == ID_LAUNCH_TRAINING) {
            RECT rect = draw->rcItem;
            const bool launchReady = g_isHost && g_dialogConfig.enetPeer && g_peerReady;
            if (launchReady) {
                HBRUSH brush = CreateSolidBrush(GetPulsingColor(
                    RGB(70, 125, 165), RGB(95, 155, 190)));
                FillRect(draw->hDC, &rect, brush);
                DeleteObject(brush);
                DrawEdge(draw->hDC, &rect,
                         (draw->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED,
                         BF_RECT);
                SetTextColor(draw->hDC, RGB(255, 255, 255));
            } else {
                UINT state = DFCS_BUTTONPUSH;
                if (draw->itemState & ODS_SELECTED) state |= DFCS_PUSHED;
                if (draw->itemState & ODS_DISABLED) state |= DFCS_INACTIVE;
                HBRUSH brush = CreateSolidBrush(LaunchMenuDarkControlColor());
                FillRect(draw->hDC, &rect, brush);
                DeleteObject(brush);
                DrawEdge(draw->hDC, &rect,
                         (state & DFCS_PUSHED) ? EDGE_SUNKEN : EDGE_RAISED,
                         BF_RECT);
                SetTextColor(draw->hDC, (draw->itemState & ODS_DISABLED)
                    ? RGB(125, 125, 125) : LaunchMenuDarkTextColor());
            }

            SetBkMode(draw->hDC, TRANSPARENT);
            if (draw->itemState & ODS_SELECTED)
                OffsetRect(&rect, 1, 1);
            DrawTextA(draw->hDC, "Launch into Battle", -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (draw->itemState & ODS_FOCUS) {
                InflateRect(&rect, -3, -3);
                DrawFocusRect(draw->hDC, &rect);
            }
            return TRUE;
        }

        if (draw->CtlID != ID_PEER_READY) break;

        RECT rect = draw->rcItem;
        const bool ready = g_isHost ? g_peerReady : g_localReady;
        COLORREF background = ready ? RGB(35, 150, 65) : RGB(190, 45, 45);
        if (!g_isHost && !ready)
            background = GetPulsingColor(RGB(145, 65, 65), RGB(185, 90, 90));
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(draw->hDC, &rect, brush);
        DeleteObject(brush);

        DrawEdge(draw->hDC, &rect,
                 (draw->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED,
                 BF_RECT);

        char text[32] = {};
        GetWindowTextA(draw->hwndItem, text, sizeof(text));
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, RGB(255, 255, 255));
        if (draw->itemState & ODS_SELECTED)
            OffsetRect(&rect, 1, 1);
        DrawTextA(draw->hDC, text, -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (draw->itemState & ODS_FOCUS) {
            InflateRect(&rect, -3, -3);
            DrawFocusRect(draw->hDC, &rect);
        }
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
    {
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == GetDlgItem(hDlg, ID_PING_LABEL)) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(100, 210, 120));
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(LaunchMenuDarkBackgroundBrush());
        }
        if (hCtrl == GetDlgItem(hDlg, ID_NET_STATUS)) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, g_statusColor);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(LaunchMenuDarkBackgroundBrush());
        }
        return HandleLaunchMenuDarkControlColor(msg, wParam);
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleLaunchMenuDarkControlColor(msg, wParam);

    case WM_ERASEBKGND:
        PaintLaunchMenuDarkBackground(hDlg, reinterpret_cast<HDC>(wParam));
        return TRUE;

    case WM_CLOSE:
        if (g_battleSetupOnly) {
            ReturnBattleSetupToLobbies(hDlg);
            return 0;
        }
        g_launchOption = LAUNCH_CANCELLED;
        DestroyWindow(hDlg);
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        DestroyDpiLayout(hDlg);
        KillTimer(hDlg, ID_STEAM_POLL_TIMER);
        if (g_compactMode &&
            (g_compactAdvancedRequest || g_compactBattleSetupRequest)) {
            // The next launch window continues using this Steam session.
        } else if (!g_isLaunching) {
            CleanupNetwork(hDlg, true);  // user closed/cancelled — full cleanup
        } else {
            CleanupNetwork(hDlg, false); // launching — preserve host for gameplay
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hDlg, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void PopulateCharCombo(HWND hCombo, int defaultCharId)
{
    if (!hCombo) return;

    // Clamp default char ID
    if (defaultCharId < 0 || defaultCharId >= kCharNameCount)
        defaultCharId = 1;

    int selectIdx = 0;
    for (int i = 0; i < kCharNameCount; i++) {
        int idx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)kCharNames[i]);
        SendMessageA(hCombo, CB_SETITEMDATA, idx, i);
        if (i == defaultCharId) {
            selectIdx = idx;
        }
    }

    SendMessageA(hCombo, CB_SETCURSEL, selectIdx, 0);
}

static void PopulateColorCombo(HWND hCombo, int characterId, int defaultColor)
{
    if (!hCombo) return;

    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    int variantCount = GetCharacterArchiveVariantCount(characterId);
    defaultColor = std::clamp(defaultColor, 0, variantCount - 1);
    for (int color = 0; color < variantCount; ++color) {
        char label[8];
        snprintf(label, sizeof(label), "%d", color + 1);
        int idx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageA(hCombo, CB_SETITEMDATA, idx, color);
    }
    SendMessageA(hCombo, CB_SETCURSEL, defaultColor, 0);
}

static void PopulateAssistCombo(HWND hCombo, int characterId, int defaultAssist)
{
    if (!hCombo) return;

    static const char* kFallbackNames[] = {"Alpha", "Beta", "Gamma"};
    SendMessageA(hCombo, CB_RESETCONTENT, 0, 0);
    defaultAssist = std::clamp(defaultAssist, 0, 2);
    for (int assist = 0; assist < 3; ++assist) {
        const char* name = characterId >= 0 && characterId < 52
            ? kAssistMoveNames[characterId].names[assist]
            : kFallbackNames[assist];
        int idx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0,
                                    (LPARAM)name);
        SendMessageA(hCombo, CB_SETITEMDATA, idx, assist);
    }
    SendMessageA(hCombo, CB_SETCURSEL, defaultAssist, 0);
}

static void PopulateStageCombo(HWND hCombo, int defaultStageId)
{
    if (!hCombo) return;

    const int visibleStageIds[] = {STAGE_BONNE_WONDERLAND, 0, 100};
    int selectIdx = 0;
    for (int stageId : visibleStageIds) {
        const int stageIndex = FindStageIndex(stageId);
        int idx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0,
                                    (LPARAM)kStages[stageIndex].name);
        SendMessageA(hCombo, CB_SETITEMDATA, idx, stageId);
        if (stageId == defaultStageId) {
            selectIdx = idx;
        }
    }

    SendMessageA(hCombo, CB_SETCURSEL, selectIdx, 0);
}

static int FindStageIndex(int stageId)
{
    for (int i = 0; i < kStageCount; i++) {
        if (kStages[i].id == stageId)
            return i;
    }
    return 0;
}

static int FindComboItemByData(HWND hCombo, int data)
{
    if (!hCombo) return CB_ERR;
    const int count = static_cast<int>(SendMessageA(hCombo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        if (static_cast<int>(SendMessageA(hCombo, CB_GETITEMDATA, i, 0)) == data)
            return i;
    }
    return CB_ERR;
}

static void UpdateCharFromCombo(HWND hDlg, int comboId, int slotIndex, int team)
{
    HWND hCombo = GetDlgItem(hDlg, comboId);
    if (!hCombo) return;

    // First try to get the selected item
    int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) {
        int charId = (int)SendMessageA(hCombo, CB_GETITEMDATA, sel, 0);
        if (team == 0)
            g_dialogConfig.p1Chars[slotIndex] = charId;
        else
            g_dialogConfig.p2Chars[slotIndex] = charId;
        return;
    }

    // If no selection, try to match the edit text
    char buf[64];
    GetWindowTextA(hCombo, buf, sizeof(buf));
    for (int i = 0; i < kCharNameCount; i++) {
        if (_stricmp(buf, kCharNames[i]) == 0) {
            if (team == 0)
                g_dialogConfig.p1Chars[slotIndex] = i;
            else
                g_dialogConfig.p2Chars[slotIndex] = i;
            return;
        }
    }

    // No match found — keep default
    if (team == 0)
        g_dialogConfig.p1Chars[slotIndex] = 1;
    else
        g_dialogConfig.p2Chars[slotIndex] = 1;
}

static void UpdateSlotOptionsFromCombos(HWND hDlg)
{
    for (int index = 0; index < 6; ++index) {
        HWND hColor = GetDlgItem(hDlg, ID_P1_COLOR1 + index);
        HWND hAssist = GetDlgItem(hDlg, ID_P1_ASSIST1 + index);
        int color = 0;
        int assist = 0;

        if (hColor) {
            int selected = (int)SendMessageA(hColor, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                color = (int)SendMessageA(hColor, CB_GETITEMDATA, selected, 0);
        }
        if (hAssist) {
            int selected = (int)SendMessageA(hAssist, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
                assist = (int)SendMessageA(hAssist, CB_GETITEMDATA, selected, 0);
        }

        if (index < 3) {
            g_dialogConfig.p1Colors[index] = color;
            g_dialogConfig.p1Assists[index] = assist;
        } else {
            g_dialogConfig.p2Colors[index - 3] = color;
            g_dialogConfig.p2Assists[index - 3] = assist;
        }
    }
}

static void CreateSavedRosterCombo(HWND hDlg, int team, int x, int y)
{
    if (g_savedRosterCount == 0) return;
    CreateWindowA("STATIC", "Preset:", WS_VISIBLE | WS_CHILD,
                  x, y + 2, 45, 18, hDlg, nullptr, nullptr, nullptr);
    HWND combo = CreateWindowA(
        "COMBOBOX", "", WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST,
        x + 48, y - 2, 115, 100, hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(
            team == 0 ? ID_P1_SAVED_ROSTER : ID_P2_SAVED_ROSTER)),
        nullptr, nullptr);
    SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Reserve Unit"));
    for (size_t i = 0; i < g_savedRosterCount; ++i) {
        char label[24];
        snprintf(label, sizeof(label), "Team %c", static_cast<char>('A' + i));
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    SendMessageA(combo, CB_SETCURSEL, 0, 0);
}

static void ApplySavedRoster(HWND hDlg, int team, size_t preset)
{
    if (team < 0 || team > 1 || preset >= g_savedRosterCount) return;
    const SavedRoster& roster = g_savedRosters[preset];
    int* chars = team == 0 ? g_dialogConfig.p1Chars : g_dialogConfig.p2Chars;
    int* colors = team == 0 ? g_dialogConfig.p1Colors : g_dialogConfig.p2Colors;
    int* assists = team == 0 ? g_dialogConfig.p1Assists : g_dialogConfig.p2Assists;
    for (int slot = 0; slot < 3; ++slot) {
        const int index = team * 3 + slot;
        chars[slot] = roster.chars[slot];
        colors[slot] = std::clamp(roster.colors[slot], 0,
            GetCharacterArchiveVariantCount(chars[slot]) - 1);
        assists[slot] = std::clamp(roster.assists[slot], 0, 2);
        HWND charCombo = GetDlgItem(hDlg, ID_P1_SLOT1 + index);
        SendMessageA(charCombo, CB_SETCURSEL,
                     FindComboItemByData(charCombo, chars[slot]), 0);
        PopulateColorCombo(GetDlgItem(hDlg, ID_P1_COLOR1 + index),
                           chars[slot], colors[slot]);
        PopulateAssistCombo(GetDlgItem(hDlg, ID_P1_ASSIST1 + index),
                            chars[slot], assists[slot]);
    }

    if (!g_dialogConfig.enetPeer) return;
    if (team == 0 && g_isHost) SendP1Selection(hDlg);
    if (team == 1 && !g_isHost) {
        g_localReady = false;
        UpdateReadyButton(hDlg);
        SendP2Selection(hDlg);
    }
}

static uint32_t NextCharacterRandom()
{
    static uint32_t state = 0;
    if (state == 0) {
        LARGE_INTEGER counter = {};
        QueryPerformanceCounter(&counter);
        state = static_cast<uint32_t>(counter.QuadPart) ^
                static_cast<uint32_t>(counter.QuadPart >> 32) ^
                static_cast<uint32_t>(GetTickCount64());
        if (state == 0) state = 1;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void RandomizeCharacters(HWND hDlg)
{
    constexpr int kPlayableCharacterCount = 50;
    const int firstTeam = g_dialogConfig.enetPeer ? (g_isHost ? 0 : 1) : 0;
    const int teamCount = g_dialogConfig.enetPeer ? 1 : 2;
    for (int team = firstTeam; team < firstTeam + teamCount; ++team) {
        int characters[kPlayableCharacterCount];
        for (int i = 0; i < kPlayableCharacterCount; ++i)
            characters[i] = i + 1;

        for (int slot = 0; slot < 3; ++slot) {
            int selected = slot + static_cast<int>(
                NextCharacterRandom() % (kPlayableCharacterCount - slot));
            std::swap(characters[slot], characters[selected]);
            int characterId = characters[slot];
            int index = team * 3 + slot;
            int* teamCharacters = team == 0
                ? g_dialogConfig.p1Chars : g_dialogConfig.p2Chars;
            int* teamColors = team == 0
                ? g_dialogConfig.p1Colors : g_dialogConfig.p2Colors;
            int* teamAssists = team == 0
                ? g_dialogConfig.p1Assists : g_dialogConfig.p2Assists;

            teamCharacters[slot] = characterId;
            teamColors[slot] = static_cast<int>(NextCharacterRandom() %
                GetCharacterArchiveVariantCount(characterId));
            teamAssists[slot] = static_cast<int>(NextCharacterRandom() % 3);
            SendMessageA(GetDlgItem(hDlg, ID_P1_SLOT1 + index),
                         CB_SETCURSEL, characterId, 0);
            PopulateColorCombo(GetDlgItem(hDlg, ID_P1_COLOR1 + index),
                               characterId, teamColors[slot]);
            PopulateAssistCombo(GetDlgItem(hDlg, ID_P1_ASSIST1 + index),
                                characterId, teamAssists[slot]);
        }
    }
}

// ---------------------------------------------------------------------------
// Network helpers (timer-based polling during dialog, thread after launch)
// ---------------------------------------------------------------------------

static void CleanupNetwork(HWND hDlg, bool forceCleanup) {
    DestroyChatWindow();

    // When launching into the game with an active connection, skip destruction
    // so the enet host/peer survive for gameplay.
    if (g_dialogConfig.hostEnabled && !forceCleanup) {
        HookLog("UMVC3Hook: CleanupNetwork - skipping (host persists for gameplay)\n");
        SteamLobby_Shutdown(true);
        return;
    }

    if (hDlg) {
        KillTimer(hDlg, ID_NET_POLL_TIMER);
    }

    g_networkPolling = false;
    if (g_hPollThread) {
        WaitForSingleObject(g_hPollThread, 1000);
        CloseHandle(g_hPollThread);
        g_hPollThread = NULL;
    }
    if (g_dialogConfig.enetHost) {
        HookLog("UMVC3Hook: CleanupNetwork - destroying enet host\n");
        enet_host_destroy(g_dialogConfig.enetHost);
        g_dialogConfig.enetHost = nullptr;
        g_dialogConfig.enetPeer = nullptr;
    }
    g_dialogConfig.hostEnabled = false;
    g_localReady = false;
    g_peerReady = false;
    SteamLobby_Shutdown(false);

    if (hDlg) {
        HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);
        g_statusColor = LaunchMenuDarkTextColor();
        if (hStatus) {
            SetWindowTextA(hStatus, "Ready");
            InvalidateRect(hStatus, NULL, TRUE);
        }
        ResetLaunchMenuButtonState(hDlg);
        UpdateReadyButton(hDlg);
    }
}

static DWORD WINAPI NetworkPollThread(LPVOID lpParam) {
    HookLog("UMVC3Hook: NetworkPollThread started\n");
    while (g_networkPolling && g_dialogConfig.enetHost) {
        ENetEvent event;
        int result = enet_host_service(g_dialogConfig.enetHost, &event, 0);

        if (result > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                uint8_t* ip = (uint8_t*)&event.peer->address.host;
                HookLog("UMVC3Hook: CONNECT event from %u.%u.%u.%u:%u (g_isHost=%d)\n",
                    ip[0], ip[1], ip[2], ip[3],
                    event.peer->address.port, g_isHost);
                g_dialogConfig.enetPeer = event.peer;
                event.peer->data = (void*)"connected";
            }
            else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                uint8_t* ip = (uint8_t*)&event.peer->address.host;
                HookLog("UMVC3Hook: DISCONNECT event from %u.%u.%u.%u:%u\n",
                    ip[0], ip[1], ip[2], ip[3],
                    event.peer->address.port);
                g_dialogConfig.enetPeer = nullptr;
            }
            else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        } else if (result < 0) {
            HookLog("UMVC3Hook: enet_host_service error: %d\n", result);
            break;
        }
        Sleep(10);
    }
    HookLog("UMVC3Hook: NetworkPollThread exiting\n");
    return 0;
}
