#pragma once

#include "LaunchMenu.h"

// Internal window outcomes. ShowLaunchMenu still returns the public LaunchOption.
enum class MenuAction {
    Cancel,
    Launch,
    OpenCompact,
    OpenAdvanced,
    OpenBattleSetup,
};

struct MenuResult {
    MenuAction action;
    LaunchOption launchOption; // Used only for MenuAction::Launch.
};

MenuResult ShowRegularLaunchMenu(bool battleSetupOnly = false);
MenuResult ShowCompactLaunchMenu();

constexpr bool PreserveSteamForMenuTransition(bool compactMode, MenuAction action)
{
    return compactMode && (action == MenuAction::OpenAdvanced ||
                           action == MenuAction::OpenBattleSetup);
}
