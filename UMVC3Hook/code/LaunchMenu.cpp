// LaunchMenu.cpp
// Pre-game launch dialog: character/stage selection, network configuration.
//
// Displays a Win32 dialog before the game initialises, allowing the user to
// configure characters, stage, and GGPO netplay parameters.  Stores the
// selection in global state for the game thread to consume at startup.
#include "LaunchMenu.h"
#include "trainingboot.h"
#include "netplay.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>

// HookLog from dllmain.cpp
extern void HookLog(const char* fmt, ...);
extern void HookLogSetFile(const char* filename);

// ---------------------------------------------------------------------------
// Character and stage name tables
// ---------------------------------------------------------------------------
const char* kCharNames[] = {
    "Random",
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
    0,           // stageId (Training Room)
    "",          // remoteIP
    7777,       // remotePort
    7777,       // localPort
    false,       // hostEnabled
    false        // isHost
};
static bool      g_isHost = false;          // true if we clicked Host, false if Connect
static COLORREF  g_statusColor = RGB(0, 0, 0); // default black for status text
static HBRUSH    g_hStatusBrush = nullptr;
static bool g_isLaunching = false;

// Declare global cleanup for DLL detach
void CleanupNetworkOnExit()
{
    if (Netplay_IsActive()) {
        Netplay_Shutdown();
    }
    g_dialogConfig.hostEnabled = false;
}

// ---------------------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------------------
enum CtrlId {
    // Launch buttons
    ID_LAUNCH_NORMAL      = 1001,
    ID_LAUNCH_TRAINING    = 1002,
    ID_LAUNCH_DUAL        = 1003,
    // Character combos
    ID_P1_SLOT1           = 2001,
    ID_P1_SLOT2           = 2002,
    ID_P1_SLOT3           = 2003,
    ID_P2_SLOT1           = 2004,
    ID_P2_SLOT2           = 2005,
    ID_P2_SLOT3           = 2006,
    // Stage combo
    ID_STAGE              = 2101,
    // Network controls
    ID_NET_IP             = 2201,
    ID_NET_PORT           = 2202,
    ID_NET_HOST_BTN       = 2203,
    ID_NET_STATUS         = 2204,
    ID_NET_CONNECT_BTN    = 2205,
    // Steam lobby controls
    ID_STEAM_SEARCH_BTN   = 2301,
    ID_STEAM_CREATE_BTN   = 2302,
    ID_STEAM_LIST         = 2303,
    ID_STEAM_STATUS       = 2304,
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static INT_PTR CALLBACK LaunchDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
static void PopulateCharCombo(HWND hCombo, int defaultCharId);
static void PopulateStageCombo(HWND hCombo, int defaultStageId);
static int FindStageIndex(int stageId);
static void UpdateCharFromCombo(HWND hDlg, int comboId, int slotIndex, int team);
static void CleanupNetwork(HWND hDlg, bool forceCleanup = false);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
LaunchOption ShowLaunchMenu()
{
    HINSTANCE hInst = GetModuleHandle(NULL);

    // Default config from training boot
    const TrainingBootConfig& def = TrainingBoot_GetDefaultConfig();
    for (int i = 0; i < 3; i++) {
        g_dialogConfig.p1Chars[i] = def.p1Chars[i];
        g_dialogConfig.p2Chars[i] = def.p2Chars[i];
    }
    g_dialogConfig.stageId = def.stageId;

    // Default IP
    strcpy(g_dialogConfig.remoteIP, "127.0.0.1");
    g_dialogConfig.remotePort = 7777;
    g_dialogConfig.localPort = 7777;
    g_dialogConfig.hostEnabled = false;
    g_dialogConfig.isHost = false;

    // Create the launch dialog window
    const char CLASS_NAME[] = "UMVC3LaunchMenu";

    WNDCLASS wc = {};
    wc.lpfnWndProc = LaunchDlgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClassA(&wc);

    int w = 540, h = 660;
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

    if (g_isLaunching && g_dialogConfig.hostEnabled) {
        HookLog("UMVC3Hook: Launch menu closing, netplay will start from game thread\n");
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

const LaunchDialogConfig& GetLaunchDialogConfig()
{
    return g_dialogConfig;
}

void SetNetworkActive()
{
    g_dialogConfig.hostEnabled = true;
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
        const int labelW = 80;
        const int comboW = 180;
        const int leftX = 20;
        const int left2X = 280; // second column start
        const int margin = 8;

        // ---- Title ----
        CreateWindowA("STATIC", "Configure launch mode and match settings:",
            WS_VISIBLE | WS_CHILD, leftX, y, 500, 18, hDlg, NULL, NULL, NULL);
        y += 28;

        // ---- Character Select Section ----
        CreateWindowA("BUTTON", "Character Select",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, 500, 175,
            hDlg, NULL, NULL, NULL);
        y += 18;

        struct CharSlot { const char* label; int ctrlId; int team; int slot; };
        CharSlot slots[6] = {
            {"P1 Slot 1:", ID_P1_SLOT1, 0, 0},
            {"P1 Slot 2:", ID_P1_SLOT2, 0, 1},
            {"P1 Slot 3:", ID_P1_SLOT3, 0, 2},
            {"P2 Slot 1:", ID_P2_SLOT1, 1, 0},
            {"P2 Slot 2:", ID_P2_SLOT2, 1, 1},
            {"P2 Slot 3:", ID_P2_SLOT3, 1, 2},
        };

        for (int i = 0; i < 6; i++) {
            int col = i < 3 ? 0 : 1;
            int row = i % 3;
            int cx = col == 0 ? leftX + 10 : left2X;
            int cy = y + row * 35;

            CreateWindowA("STATIC", slots[i].label,
                WS_VISIBLE | WS_CHILD, cx, cy + 4, labelW, 18,
                hDlg, NULL, NULL, NULL);

            HWND hCombo = CreateWindowA("COMBOBOX", "",
                WS_VISIBLE | WS_CHILD | WS_TABSTOP |
                CBS_DROPDOWN | CBS_HASSTRINGS | CBS_AUTOHSCROLL,
                cx + labelW, cy, comboW, 200,
                hDlg, (HMENU)slots[i].ctrlId, NULL, NULL);

            int defaultCharId = (slots[i].team == 0)
                ? g_dialogConfig.p1Chars[slots[i].slot]
                : g_dialogConfig.p2Chars[slots[i].slot];
            PopulateCharCombo(hCombo, defaultCharId);
        }
        y += 105;

        // ---- Stage Select ----
        y += 5;
        CreateWindowA("BUTTON", "Stage Select",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, 500, 52,
            hDlg, NULL, NULL, NULL);
        y += 18;

        CreateWindowA("STATIC", "Stage:",
            WS_VISIBLE | WS_CHILD, leftX + 10, y + 4, 50, 18,
            hDlg, NULL, NULL, NULL);

        HWND hStage = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP |
            CBS_DROPDOWN | CBS_HASSTRINGS | CBS_AUTOHSCROLL,
            leftX + 65, y, 300, 200,
            hDlg, (HMENU)ID_STAGE, NULL, NULL);
        PopulateStageCombo(hStage, g_dialogConfig.stageId);
        y += 35;

        // ---- Network Section ----
        y += 5;
        CreateWindowA("BUTTON", "Network (Direct Connect)",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, 500, 75,
            hDlg, NULL, NULL, NULL);
        y += 18;

        CreateWindowA("STATIC", "IP:",
            WS_VISIBLE | WS_CHILD, leftX + 10, y + 4, 25, 18,
            hDlg, NULL, NULL, NULL);

        HWND hIpEdit = CreateWindowA("EDIT", g_dialogConfig.remoteIP,
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
            leftX + 35, y, 140, 22,
            hDlg, (HMENU)ID_NET_IP, NULL, NULL);

        CreateWindowA("STATIC", "Port:",
            WS_VISIBLE | WS_CHILD, leftX + 185, y + 4, 35, 18,
            hDlg, NULL, NULL, NULL);

        char portStr[16];
        snprintf(portStr, sizeof(portStr), "%d", g_dialogConfig.remotePort);
        HWND hPortEdit = CreateWindowA("EDIT", portStr,
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
            leftX + 220, y, 60, 22,
            hDlg, (HMENU)ID_NET_PORT, NULL, NULL);

        CreateWindowA("BUTTON", "Host Game",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX + 300, y - 2, 90, 26,
            hDlg, (HMENU)ID_NET_HOST_BTN, NULL, NULL);

        CreateWindowA("BUTTON", "Connect",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX + 395, y - 2, 90, 26,
            hDlg, (HMENU)ID_NET_CONNECT_BTN, NULL, NULL);

        // Status row
        y += 28;
        CreateWindowA("STATIC", "Ready",
            WS_VISIBLE | WS_CHILD, leftX + 10, y + 4, 200, 18,
            hDlg, (HMENU)ID_NET_STATUS, NULL, NULL);
        y += 30;

        // ---- Steam Lobby Section ----
        y += 5;
        CreateWindowA("BUTTON", "Steam Lobby",
            WS_VISIBLE | WS_CHILD | BS_GROUPBOX, leftX, y, 500, 160,
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

        CreateWindowA("STATIC", "",
            WS_VISIBLE | WS_CHILD, leftX + 270, y + 4, 220, 18,
            hDlg, (HMENU)ID_STEAM_STATUS, NULL, NULL);
        y += 30;

        // Lobby listbox
        CreateWindowA("LISTBOX", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_BORDER |
            LBS_STANDARD | LBS_NOINTEGRALHEIGHT,
            leftX + 10, y, 480, 95,
            hDlg, (HMENU)ID_STEAM_LIST, NULL, NULL);
        y += 100;

        // ---- Launch Buttons ----
        y += 10;
        CreateWindowA("STATIC", "Choose how to launch the game:",
            WS_VISIBLE | WS_CHILD, leftX, y, 300, 18, hDlg, NULL, NULL, NULL);
        y += 25;

        CreateWindowA("BUTTON", "Launch Normally",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX, y, 240, 32, hDlg, (HMENU)ID_LAUNCH_NORMAL, NULL, NULL);

        CreateWindowA("BUTTON", "Launch into Training Mode",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX + 250, y, 240, 32, hDlg, (HMENU)ID_LAUNCH_TRAINING, NULL, NULL);
        y += 38;

        CreateWindowA("BUTTON", "Training Mode + Spawn 2nd Instance",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            leftX, y, 240, 32, hDlg, (HMENU)ID_LAUNCH_DUAL, NULL, NULL);

        CreateWindowA("STATIC", "Spawned instance will auto-enter training.",
            WS_VISIBLE | WS_CHILD, leftX + 250, y + 8, 240, 18,
            hDlg, NULL, NULL, NULL);

        return msg == WM_INITDIALOG ? TRUE : 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        // ---- Launch buttons ----
        case ID_LAUNCH_NORMAL:
        {
            // Apply network config before launching
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            if (hIp) GetWindowTextA(hIp, g_dialogConfig.remoteIP, sizeof(g_dialogConfig.remoteIP));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.remotePort = atoi(buf);
                g_dialogConfig.localPort = g_dialogConfig.remotePort;
            }

            g_isLaunching = true;
            g_launchOption = LAUNCH_NORMAL;
            DestroyWindow(hDlg);
            PostQuitMessage(0);
            return 0;
        }
        case ID_LAUNCH_TRAINING:
        case ID_LAUNCH_DUAL:
        {
            // Read character selections from combo boxes
            for (int i = 0; i < 3; i++) {
                UpdateCharFromCombo(hDlg, ID_P1_SLOT1 + i, i, 0);
                UpdateCharFromCombo(hDlg, ID_P2_SLOT1 + i, i, 1);
            }

            // Read stage selection
            HWND hStage = GetDlgItem(hDlg, ID_STAGE);
            if (hStage) {
                int sel = (int)SendMessageA(hStage, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR && sel >= 0 && sel < kStageCount) {
                    g_dialogConfig.stageId = kStages[sel].id;
                }
            }

            // Read network config
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            if (hIp) GetWindowTextA(hIp, g_dialogConfig.remoteIP, sizeof(g_dialogConfig.remoteIP));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.remotePort = atoi(buf);
                g_dialogConfig.localPort = g_dialogConfig.remotePort;
            }

            // Apply config to training boot
            TrainingBootConfig cfg;
            for (int i = 0; i < 3; i++) {
                cfg.p1Chars[i] = g_dialogConfig.p1Chars[i];
                cfg.p2Chars[i] = g_dialogConfig.p2Chars[i];
            }
            cfg.stageId = g_dialogConfig.stageId;
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
                        break;
                    }
                }
            }
            return 0;

        // ---- Stage combo box ----
        case ID_STAGE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                HWND hCombo = (HWND)lParam;
                int sel = (int)SendMessageA(hCombo, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR && sel >= 0 && sel < kStageCount) {
                    g_dialogConfig.stageId = kStages[sel].id;
                }
            }
            return 0;

        // ---- Network Host button ----
        case ID_NET_HOST_BTN:
        {
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);

            if (hIp) GetWindowTextA(hIp, g_dialogConfig.remoteIP, sizeof(g_dialogConfig.remoteIP));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.remotePort = atoi(buf);
            }
            g_dialogConfig.localPort = g_dialogConfig.remotePort;

            HookLogSetFile("UMVC3Hook_host.log");
            HookLog("UMVC3Hook: Host button clicked, port %d\n", g_dialogConfig.localPort);

            g_isHost = true;
            g_dialogConfig.isHost = true;
            g_dialogConfig.hostEnabled = true;
            g_statusColor = RGB(0, 120, 215);

            if (hStatus) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Host ready on port %d", g_dialogConfig.localPort);
                SetWindowTextA(hStatus, msg);
            }
            EnableWindow(GetDlgItem(hDlg, ID_NET_HOST_BTN), FALSE);
            EnableWindow(GetDlgItem(hDlg, ID_NET_CONNECT_BTN), FALSE);
            return 0;
        }

        // ---- Network Connect button ----
        case ID_NET_CONNECT_BTN:
        {
            HWND hIp = GetDlgItem(hDlg, ID_NET_IP);
            HWND hPort = GetDlgItem(hDlg, ID_NET_PORT);
            HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);

            if (hIp) GetWindowTextA(hIp, g_dialogConfig.remoteIP, sizeof(g_dialogConfig.remoteIP));
            if (hPort) {
                char buf[16];
                GetWindowTextA(hPort, buf, sizeof(buf));
                g_dialogConfig.remotePort = atoi(buf);
            }
            g_dialogConfig.localPort = g_dialogConfig.remotePort;

            HookLogSetFile("UMVC3Hook_client.log");
            HookLog("UMVC3Hook: Connect button clicked, target %s:%d\n",
                g_dialogConfig.remoteIP, g_dialogConfig.remotePort);

            g_isHost = false;
            g_dialogConfig.isHost = false;
            g_dialogConfig.hostEnabled = true;
            g_statusColor = RGB(200, 180, 0);

            if (hStatus) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Will connect to %s:%d", g_dialogConfig.remoteIP, g_dialogConfig.remotePort);
                SetWindowTextA(hStatus, msg);
            }
            EnableWindow(GetDlgItem(hDlg, ID_NET_HOST_BTN), FALSE);
            EnableWindow(GetDlgItem(hDlg, ID_NET_CONNECT_BTN), FALSE);
            return 0;
        }

        // ---- Steam Lobby buttons ----
        case ID_STEAM_SEARCH_BTN:
        {
            HWND hList = GetDlgItem(hDlg, ID_STEAM_LIST);
            HWND hStatus = GetDlgItem(hDlg, ID_STEAM_STATUS);
            if (hList) SendMessageA(hList, LB_RESETCONTENT, 0, 0);
            if (hStatus) SetWindowTextA(hStatus, "Searching... (Steam SDK required)");

            // Try to load steam_api and search for lobbies
            HMODULE hSteam = LoadLibraryA("steam_api64.dll");
            if (hSteam) {
                // Steam API is available - try to init and use it
                // This is a simplified approach; full lobby support needs Steamworks SDK headers
                if (hList) {
                    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)"Steam loaded - lobby search needs SDK integration");
                }
                FreeLibrary(hSteam);
            } else {
                if (hList) {
                    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)"steam_api.dll not found in path");
                    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)"Place this DLL in the game directory");
                }
            }
            return 0;
        }

        case ID_STEAM_CREATE_BTN:
        {
            HWND hStatus = GetDlgItem(hDlg, ID_STEAM_STATUS);
            if (hStatus) SetWindowTextA(hStatus, "Creating lobby... (Steam SDK required)");

            HMODULE hSteam = LoadLibraryA("steam_api64.dll");
            if (hSteam) {
                HWND hList = GetDlgItem(hDlg, ID_STEAM_LIST);
                if (hList) {
                    SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)"Steam loaded - lobby creation needs SDK integration");
                    SendMessageA(hList, LB_RESETCONTENT, 0, 0);
                }
                FreeLibrary(hSteam);
            }
            return 0;
        }
        }
        break;

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
        g_launchOption = LAUNCH_NORMAL;
        DestroyWindow(hDlg);
        PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        if (!g_isLaunching) {
            CleanupNetwork(hDlg, true);  // user closed/cancelled -- full cleanup
        } else {
            CleanupNetwork(hDlg, false); // launching -- preserve host for gameplay
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

static void PopulateStageCombo(HWND hCombo, int defaultStageId)
{
    if (!hCombo) return;

    int selectIdx = 0;
    for (int i = 0; i < kStageCount; i++) {
        int idx = (int)SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)kStages[i].name);
        SendMessageA(hCombo, CB_SETITEMDATA, idx, kStages[i].id);
        if (kStages[i].id == defaultStageId) {
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

    // No match found -- keep default
    if (team == 0)
        g_dialogConfig.p1Chars[slotIndex] = 1;
    else
        g_dialogConfig.p2Chars[slotIndex] = 1;
}

// ---------------------------------------------------------------------------
// Network helpers
// ---------------------------------------------------------------------------

static void CleanupNetwork(HWND hDlg, bool forceCleanup) {
    if (g_dialogConfig.hostEnabled && !forceCleanup) {
        HookLog("UMVC3Hook: CleanupNetwork - skipping (netplay persists for gameplay)\n");
        return;
    }

    g_dialogConfig.hostEnabled = false;

    if (hDlg) {
        HWND hStatus = GetDlgItem(hDlg, ID_NET_STATUS);
        g_statusColor = RGB(0, 0, 0);
        if (hStatus) {
            SetWindowTextA(hStatus, "Ready");
            InvalidateRect(hStatus, NULL, TRUE);
        }
        EnableWindow(GetDlgItem(hDlg, ID_NET_HOST_BTN), TRUE);
        EnableWindow(GetDlgItem(hDlg, ID_NET_CONNECT_BTN), TRUE);
    }
}
