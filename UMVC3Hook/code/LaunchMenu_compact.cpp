#include "LaunchMenu.h"
#include "LaunchMenu_compact.h"

#include <commctrl.h>
#include <cstdio>
#include <cstring>

namespace {

void GetSettingsPath(char* path, size_t pathSize)
{
    const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(pathSize));
    if (length == 0 || length >= pathSize) {
        path[0] = '\0';
        return;
    }
    char* slash = strrchr(path, '\\');
    char* forwardSlash = strrchr(path, '/');
    if (!slash || (forwardSlash && forwardSlash > slash)) slash = forwardSlash;
    char* filename = slash ? slash + 1 : path;
    snprintf(filename, pathSize - static_cast<size_t>(filename - path),
             "Timestone.ini");
}

} // namespace

void CreateCompactLaunchMenuControls(HWND window,
                                     const CompactLaunchMenuControlIds& ids,
                                     int lobbyType,
                                     const char* lobbyName)
{
    constexpr int x = 16;
    constexpr int width = 608;
    CreateWindowA("BUTTON", "Steam Lobbies",
        WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
        x, 12, width, 292, window, NULL, NULL, NULL);
    CreateWindowA("BUTTON", "Search",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        x + 10, 34, 84, 26, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.search)), NULL, NULL);
    CreateWindowA("BUTTON", "Create Lobby",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        x + 100, 34, 104, 26, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.create)), NULL, NULL);

    HWND type = CreateWindowA("COMBOBOX", "",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
        x + 210, 34, 108, 100, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.type)), NULL, NULL);
    SendMessageA(type, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Public"));
    SendMessageA(type, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Friends Only"));
    SendMessageA(type, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Private"));
    SendMessageA(type, CB_SETCURSEL, lobbyType, 0);

    CreateWindowA("STATIC", "Name:", WS_VISIBLE | WS_CHILD,
        x + 326, 39, 40, 18, window, NULL, NULL, NULL);
    HWND name = CreateWindowA("EDIT", lobbyName,
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        x + 368, 36, 142, 22, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.name)), NULL, NULL);
    SendMessageA(name, EM_SETLIMITTEXT, 127, 0);
    CreateWindowA("BUTTON", "Join",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
        x + 516, 34, 82, 26, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.join)), NULL, NULL);

    CreateWindowA("STATIC", "Steam ready", WS_VISIBLE | WS_CHILD,
        x + 10, 68, 588, 18, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.status)), NULL, NULL);
    HWND list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | WS_VSCROLL |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        x + 10, 90, 588, 202, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.list)), NULL, NULL);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const char* headings[] = {"Lobby", "Player", "Ping"};
    const int widths[] = {238, 220, 100};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNA column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<char*>(headings[i]);
        column.cx = widths[i];
        column.iSubItem = i;
        ListView_InsertColumn(list, i, &column);
    }

    CreateWindowA("BUTTON", "Configure Settings In-Game / Normal Launch",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON,
        x + width - 300, 318, 300, 42, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.normal)), NULL, NULL);

    char version[64] = {};
    snprintf(version, sizeof(version), "Timestone    v%s", UMVC3HOOK_VERSION);
    CreateWindowA("STATIC", version, WS_VISIBLE | WS_CHILD,
        x, 378, 300, 18, window, NULL, NULL, NULL);
    CreateWindowA("BUTTON", "Advanced launch menu",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        x + 400, 374, 208, 22, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids.advanced)), NULL, NULL);
}

LaunchOption ShowLaunchMenu()
{
    if (!ConfirmLaunchDisclaimer()) return LAUNCH_CANCELLED;

    char settingsPath[MAX_PATH] = {};
    GetSettingsPath(settingsPath, sizeof(settingsPath));
    if (GetPrivateProfileIntA("General", "UseRegularLaunchMenu", 0,
                              settingsPath) != 0) {
        const LaunchOption result = ShowRegularLaunchMenu(false);
        if (!TakeRegularCompactRequest()) return result;
    }

    for (;;) {
        LaunchOption result = ShowCompactLaunchMenu();
        if (TakeCompactAdvancedRequest()) {
            result = ShowRegularLaunchMenu(false);
            if (TakeRegularCompactRequest()) continue;
            return result;
        }
        if (!TakeCompactBattleSetupRequest()) return result;

        result = ShowRegularLaunchMenu(true);
        if (!TakeBattleLobbyReturnRequest()) return result;
    }
}
