#pragma once

#include <Windows.h>

COLORREF LaunchMenuDarkBackgroundColor();
COLORREF LaunchMenuDarkControlColor();
COLORREF LaunchMenuDarkTextColor();
HBRUSH LaunchMenuDarkBackgroundBrush();
void InitializeLaunchMenuDarkMode();
void ApplyLaunchMenuDarkTheme(HWND window);
void PaintLaunchMenuDarkBackground(HWND window, HDC dc);
INT_PTR HandleLaunchMenuDarkControlColor(UINT message, WPARAM wParam);
