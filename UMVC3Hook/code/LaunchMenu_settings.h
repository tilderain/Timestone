#pragma once

#include <Windows.h>

void ShowAdvancedConfigModal(HWND owner);
bool IsLaunchMenuDarkModeEnabled();
bool PrepareAdvancedLaunchConfig();
bool ApplyAdvancedLaunchConfig();
bool IsGameSingleThreaded();
bool SetGameSingleThreaded(bool enabled);
