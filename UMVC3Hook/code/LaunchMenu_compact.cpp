#include "LaunchMenu.h"
#include "LaunchMenu_compact.h"
#include "launchmenu_controls.h"
#include "launch_music.h"

#include <commctrl.h>
#include <cstdio>
#include <cstring>

extern void HookLog(const char* fmt, ...);

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
    namespace ui = LaunchMenuControls;
    ui::Button(window, "Steam Lobbies",
        WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
        x, 12, width, 292);
    ui::Button(window, "Mute Lobby Music",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        x + width - 158, 8, 148, 24, ids.musicMute);
    ui::Button(window, "Search",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        x + 10, 34, 84, 26, ids.search);
    ui::Button(window, "Create Lobby",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        x + 100, 34, 104, 26, ids.create);

    ui::LobbyType(window, ids.type, lobbyType, x + 210, 34, 108);

    ui::Label(window, "Name:", WS_VISIBLE | WS_CHILD,
        x + 326, 39, 40, 18);
    ui::LobbyName(window, ids.name, lobbyName, 127, x + 368, 36, 142);
    ui::Button(window, "Join",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
        x + 516, 34, 82, 26, ids.join);

    ui::Label(window, "Steam ready", WS_VISIBLE | WS_CHILD,
        x + 10, 68, 452, 18, ids.status);
    ui::Button(window, "Copy Lobby Link",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        x + 468, 64, 130, 24, ids.copyLink);
    const ui::LobbyColumn columns[] = {{"Lobby", 238}, {"Player", 220}, {"Ping", 100}};
    ui::LobbyList(window, ids.list, x + 10, 90, 588, 202, columns);

    ui::Button(window, "Change Controls In-Game / Normal Launch",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON,
        x + width - 300, 318, 300, 42, ids.normal);
    ui::Button(window, "Configure settings",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
        x, 318, 180, 42, ids.config);

    char version[64] = {};
    snprintf(version, sizeof(version), "Timestone    v%s", UMVC3HOOK_VERSION);
    ui::Label(window, version, WS_VISIBLE | WS_CHILD,
        x, 378, 300, 18);
    ui::Button(window, "Advanced launch menu",
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        x + 400, 374, 208, 22, ids.advanced);
}

LaunchOption ShowLaunchMenu()
{
    struct MusicStopGuard {
        ~MusicStopGuard() { LaunchMusic_BeginFadeOut(); }
    } musicStopGuard;

    HookLog("UMVC3Hook: Checking launch disclaimer\n");
    if (!ConfirmLaunchDisclaimer()) {
        HookLog("UMVC3Hook: Launch disclaimer declined or closed\n");
        return LAUNCH_CANCELLED;
    }

    char settingsPath[MAX_PATH] = {};
    GetSettingsPath(settingsPath, sizeof(settingsPath));
    if (GetPrivateProfileIntA("General", "UseRegularLaunchMenu", 0,
                              settingsPath) != 0) {
        HookLog("UMVC3Hook: Opening regular launch menu\n");
        const LaunchOption result = ShowRegularLaunchMenu(false);
        if (!TakeRegularCompactRequest()) return result;
    }

    for (;;) {
        HookLog("UMVC3Hook: Opening compact launch menu\n");
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
