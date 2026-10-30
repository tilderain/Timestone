#pragma once

#include <Windows.h>

void ShowAdvancedConfigModal(HWND owner);
bool IsLaunchMenuDarkModeEnabled();
bool PrepareAdvancedLaunchConfig();
bool ApplyAdvancedLaunchConfig();
// Call on the game thread after the launch menu has stopped editing settings.
void ApplyLaunchDisplayConfig();
bool IsGameSingleThreaded();
bool SetGameSingleThreaded(bool enabled);
