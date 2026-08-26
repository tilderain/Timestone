#include "LaunchMenu.h"
#include "assist_names.h"
#include "trainingboot.h"
#include "netplay.h"
#include "steam_lobby.h"
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
static COLORREF  g_statusColor = RGB(0, 0, 0); // default black for status text
static HBRUSH    g_hStatusBrush = nullptr;

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
        packet.simpleMode[0] > 1 || packet.simpleMode[1] > 1)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (packet.p1Chars[i] < 0 || packet.p1Chars[i] >= kCharNameCount ||
            packet.p2Chars[i] < 0 || packet.p2Chars[i] >= kCharNameCount ||
            packet.p1Colors[i] < 0 ||
            packet.p1Colors[i] >= GetCharacterArchiveVariantCount(packet.p1Chars[i]) ||
            packet.p2Colors[i] < 0 ||
            packet.p2Colors[i] >= GetCharacterArchiveVariantCount(packet.p2Chars[i]) ||
            packet.p1Assists[i] < 0 || packet.p1Assists[i] > 2 ||
            packet.p2Assists[i] < 0 || packet.p2Assists[i] > 2)
            return false;
    }
    return true;
}

// Custom timer and window messages
#define ID_NET_POLL_TIMER       5001
#define ID_STEAM_POLL_TIMER     5002
static volatile bool g_networkPolling = false;
static HANDLE g_hPollThread = NULL;
static bool g_isLaunching = false;
static bool g_directConnectExpanded = false;
static int g_lobbyTypeSelection = 0;

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

static bool ConfirmDisclaimer()
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

    if (!WritePrivateProfileStringA(
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
}

static void SaveLobbyTypeSelection()
{
    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    char value[2] = { static_cast<char>('0' + g_lobbyTypeSelection), '\0' };
    if (!WritePrivateProfileStringA(
            "Network", "LobbyType", value, settingsPath)) {
        HookLog("UMVC3Hook: Failed to save lobby type to %s\n", settingsPath);
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
        WritePrivateProfileStringA("P1", key, value, rollbackPath);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p2Chars[i]);
        WritePrivateProfileStringA("P2", key, value, rollbackPath);
        snprintf(key, sizeof(key), "Color%d", i + 1);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p1Colors[i]);
        WritePrivateProfileStringA("P1", key, value, rollbackPath);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p2Colors[i]);
        WritePrivateProfileStringA("P2", key, value, rollbackPath);
        snprintf(key, sizeof(key), "Assist%d", i + 1);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p1Assists[i]);
        WritePrivateProfileStringA("P1", key, value, rollbackPath);
        snprintf(value, sizeof(value), "%d", g_dialogConfig.p2Assists[i]);
        WritePrivateProfileStringA("P2", key, value, rollbackPath);
    }
    WritePrivateProfileStringA("P1", "SimpleMode",
                               g_dialogConfig.simpleMode[0] ? "1" : "0", rollbackPath);
    WritePrivateProfileStringA("P2", "SimpleMode",
                               g_dialogConfig.simpleMode[1] ? "1" : "0", rollbackPath);
    char stageId[16];
    snprintf(stageId, sizeof(stageId), "%d", g_dialogConfig.stageId);
    WritePrivateProfileStringA("Training", "StageId", stageId, rollbackPath);

    char settingsPath[MAX_PATH] = {};
    GetSettingsPath("Timestone.ini", settingsPath, sizeof(settingsPath));
    WritePrivateProfileStringA("Network", "HostIp", g_dialogConfig.hostIp, settingsPath);
    char port[16];
    snprintf(port, sizeof(port), "%d", g_dialogConfig.hostPort);
    WritePrivateProfileStringA("Network", "HostPort", port, settingsPath);
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
    }
    cfg.stageId = packet.stageId;
    cfg.simpleMode[0] = packet.simpleMode[0] != 0;
    cfg.simpleMode[1] = packet.simpleMode[1] != 0;
    cfg.timerValue = 99;
    TrainingBoot_SetConfig(cfg);
    HookLog("UMVC3Hook: Applied launch packet P1=%d/%d/%d P2=%d/%d/%d stage=%d\n",
            packet.p1Chars[0], packet.p1Chars[1], packet.p1Chars[2],
            packet.p2Chars[0], packet.p2Chars[1], packet.p2Chars[2], packet.stageId);
    return true;
}

// Declare global cleanup for DLL detach
void CleanupNetworkOnExit()
{
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
    ID_LAUNCH_PROMPT      = 2401,
    ID_VERSION_LABEL      = 2402,
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LaunchDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void PopulateCharCombo(HWND hCombo, int defaultCharId);
static void PopulateColorCombo(HWND hCombo, int characterId, int defaultColor);
static void PopulateAssistCombo(HWND hCombo, int characterId, int defaultAssist);
static void PopulateStageCombo(HWND hCombo, int defaultStageId);
static int FindStageIndex(int stageId);
static int FindComboItemByData(HWND hCombo, int data);
static void UpdateCharFromCombo(HWND hDlg, int comboId, int slotIndex, int team);
static void UpdateSlotOptionsFromCombos(HWND hDlg);
static void RandomizeCharacters(HWND hDlg);
static void ConfigureConnectedControls(HWND hDlg);
static void ResetLaunchMenuButtonState(HWND hDlg);
static void SetDirectConnectExpanded(HWND hDlg, bool expanded);
static void SendP1Selection(HWND hDlg);
static void SendP2Selection(HWND hDlg);
static bool ApplyP1Selection(HWND hDlg, const LaunchP1SelectionPacket& packet);
static bool ApplyP2Selection(HWND hDlg, const LaunchP2SelectionPacket& packet);
static DWORD WINAPI NetworkPollThread(LPVOID lpParam);
static void CleanupNetwork(HWND hDlg, bool forceCleanup = false);

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

static void UpdateSteamLobbyButton(HWND hDlg)
{
    HWND button = GetDlgItem(hDlg, ID_STEAM_CREATE_BTN);
    if (!button) return;
    bool hasLobby = SteamLobby_HasLobby();
    SetWindowTextA(button, hasLobby ? "Leave Lobby" : "Create Lobby");
    EnableWindow(button, hasLobby || !SteamLobby_IsCreating());
    EnableWindow(GetDlgItem(hDlg, ID_STEAM_TYPE),
                 !hasLobby && !SteamLobby_IsCreating());
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
        HookLog("UMVC3Hook: Client controls enabled for P2 selection\n");
    }
    UpdateReadyButton(hDlg);
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
}

static void SetDirectConnectExpanded(HWND hDlg, bool expanded)
{
    if (g_directConnectExpanded == expanded) return;

    constexpr int expandedHeight = 56;
    const int directControls[] = {
        ID_NET_IP_LABEL, ID_NET_IP, ID_NET_PORT_LABEL, ID_NET_PORT,
        ID_NET_HOST_BTN, ID_NET_CONNECT_BTN, ID_NET_STATUS,
    };
    for (int id : directControls)
        ShowWindow(GetDlgItem(hDlg, id), expanded ? SW_SHOW : SW_HIDE);

    const int launchControls[] = {
        ID_LAUNCH_PROMPT, ID_LAUNCH_NORMAL, ID_LAUNCH_TRAINING,
        ID_LAUNCH_DUAL, ID_LAUNCH_DUAL_CPUS, ID_LAUNCH_DUAL_P2_CPU,
        ID_VERSION_LABEL,
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
    WritePrivateProfileStringA("Network", "DirectConnectExpanded",
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
LaunchOption ShowLaunchMenu()
{
    if (!ConfirmDisclaimer())
        return LAUNCH_CANCELLED;

    Updater_Init();
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
    SteamLobby_Initialize();
    uint64_t inviteLobbyId = GetInviteLobbyId();
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
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClassA(&wc);

    int w = 840, h = 780;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hWnd = CreateWindowExA(
        0, CLASS_NAME, "UMVC3Hook - Launch Mode",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_MINIMIZEBOX,
        x, y, w, h,
        NULL, NULL, hInst, NULL
    );

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterClassA(CLASS_NAME, hInst);

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
static INT_PTR CALLBACK LaunchDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    case WM_INITDIALOG:
    {
        int y = 15;
        const int leftX = 20;
        const int left2X = 430; // second column start
        const int contentW = 800;

        // ---- Title ----
        CreateWindowA("STATIC", "Configure launch mode and match settings:",
            WS_VISIBLE | WS_CHILD, leftX, y, 600, 18, hDlg, NULL, NULL, NULL);
        CreateWindowA("BUTTON", "Randomize Characters",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            leftX + 620, y - 4, 180, 26,
            hDlg, (HMENU)ID_RANDOMIZE_CHARS, NULL, NULL);
        y += 28;

        // ---- Character Select Section ----
        const int characterSectionTop = y;
        CreateWindowA("BUTTON", "Character Select",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, contentW, 200,
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
        SendMessageA(p1Simple, BM_SETCHECK,
                     g_dialogConfig.simpleMode[0] ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageA(p2Simple, BM_SETCHECK,
                     g_dialogConfig.simpleMode[1] ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 25;

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

        y += 30;

        CreateWindowA("STATIC", SteamLobby_GetStatus(),
            WS_VISIBLE | WS_CHILD, leftX + 10, y + 2, 780, 18,
            hDlg, (HMENU)ID_STEAM_STATUS, NULL, NULL);
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

        if (g_directConnectExpanded) {
            g_directConnectExpanded = false;
            SetDirectConnectExpanded(hDlg, true);
        }

        SetTimer(hDlg, ID_STEAM_POLL_TIMER, 50, NULL);
        return msg == WM_INITDIALOG ? TRUE : 0;
    }

    case WM_TIMER:
        if (wParam == ID_STEAM_POLL_TIMER) {
            if (!g_isHost && g_dialogConfig.enetPeer && !g_localReady)
                InvalidateRect(GetDlgItem(hDlg, ID_PEER_READY), NULL, FALSE);
            if (g_isHost && g_dialogConfig.enetPeer && g_peerReady)
                InvalidateRect(GetDlgItem(hDlg, ID_LAUNCH_TRAINING), NULL, FALSE);

            SteamLobby_Pump();
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
            if (hSteamStatus) SetWindowTextA(hSteamStatus, SteamLobby_GetStatus());
            UpdateSteamLobbyButton(hDlg);

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
                SetTimer(hDlg, ID_NET_POLL_TIMER, 10, NULL);

                HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);
                if (hStatus) {
                    char peerStatus[256];
                    snprintf(peerStatus, sizeof(peerStatus),
                             "Steam connected: %s (SteamID %llu). Make your selection, NOW",
                             SteamLobby_GetPeerUsername(),
                             static_cast<unsigned long long>(
                                 SteamLobby_GetPeerSteamId()));
                    SetWindowTextA(hStatus, peerStatus);
                    g_statusColor = steamIsHost ? RGB(0, 120, 215) : RGB(200, 180, 0);
                    InvalidateRect(hStatus, NULL, TRUE);
                }
                ConfigureConnectedControls(hDlg);
                if (steamIsHost) SendP1Selection(hDlg);
                else SendP2Selection(hDlg);
            }
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
                    }
                    else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                        uint8_t* ip = (uint8_t*)&event.peer->address.host;
                        HookLog("UMVC3Hook: DISCONNECT event from %u.%u.%u.%u:%u\n",
                            ip[0], ip[1], ip[2], ip[3],
                            event.peer->address.port);
                        g_dialogConfig.enetPeer = nullptr;
                        g_localReady = false;
                        g_peerReady = false;
                        CleanupNetwork(hDlg, true);
                    }
                    else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                        uint32_t packetType = event.packet->dataLength >= sizeof(uint32_t)
                            ? *(uint32_t*)event.packet->data : UINT32_MAX;
                        if (packetType == PACKET_LAUNCH_P1_SELECTION &&
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
                    CleanupNetwork(hDlg, true);
                }
            }
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
                LVITEMA item = {};
                item.mask = LVIF_PARAM;
                item.iItem = static_cast<int>(draw->nmcd.dwItemSpec);
                if (ListView_GetItem(header->hwndFrom, &item) &&
                    SteamLobby_IsHostedLobby(static_cast<uint64_t>(item.lParam))) {
                    draw->clrText = GetSysColor(COLOR_GRAYTEXT);
                }
            }
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_RANDOMIZE_CHARS:
            RandomizeCharacters(hDlg);
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

        // ---- Launch buttons ----
        case ID_LAUNCH_NORMAL:
        {
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
            TrainingBootConfig cfg;
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
            g_statusColor = RGB(0, 0, 0); // default color while listening

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
            g_statusColor = RGB(0, 0, 0); // default color while connecting
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
                DrawFrameControl(draw->hDC, &rect, DFC_BUTTON, state);
                SetTextColor(draw->hDC, (draw->itemState & ODS_DISABLED)
                    ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_BTNTEXT));
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
        if (hCtrl == GetDlgItem(hDlg, ID_NET_STATUS)) {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, g_statusColor);
            SetBkMode(hdc, TRANSPARENT);
            if (!g_hStatusBrush)
                g_hStatusBrush = CreateSolidBrush(RGB(255, 255, 255));
            return (INT_PTR)g_hStatusBrush;
        }
        break;
    }

    case WM_CLOSE:
        g_launchOption = LAUNCH_CANCELLED;
        DestroyWindow(hDlg);
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        KillTimer(hDlg, ID_STEAM_POLL_TIMER);
        if (!g_isLaunching) {
            CleanupNetwork(hDlg, true);  // user closed/cancelled — full cleanup
        } else {
            CleanupNetwork(hDlg, false); // launching — preserve host for gameplay
        }
        if (g_hStatusBrush) {
            DeleteObject(g_hStatusBrush);
            g_hStatusBrush = nullptr;
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
    for (int team = 0; team < 2; ++team) {
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
        g_statusColor = RGB(0, 0, 0);
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
