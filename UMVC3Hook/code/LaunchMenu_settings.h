#pragma once

#include <Windows.h>

void ShowAdvancedConfigModal(HWND owner);
bool IsLaunchMenuDarkModeEnabled();
bool PrepareAdvancedLaunchConfig();
bool ApplyAdvancedLaunchConfig();
