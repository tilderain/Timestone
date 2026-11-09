#pragma once

#include <Windows.h>

void ShowAdvancedConfigModal(HWND owner);
bool IsLaunchMenuDarkModeEnabled();
bool IsLaunchVsyncEnabled();
bool SetLaunchVsyncEnabled(bool enabled);
bool SaveGameResolution(int width, int height);
bool SaveGameFullscreen(bool enabled);
bool PrepareAdvancedLaunchConfig();
bool ApplyAdvancedLaunchConfig();
// Call on the game thread after the launch menu has stopped editing settings.
void ApplyLaunchDisplayConfig();
bool IsGameSingleThreaded();
