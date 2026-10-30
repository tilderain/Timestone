#include "steam_lobby.h"
#include "LaunchMenu_settings.h"
#include "../IniSafe.h"

#include "launchmenu_dark.h"
#include "launchmenu_utils.h"
#include "mk10utils.h"
#include "d3d9reset.h"
#include "../kiero/minhook/include/MinHook.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <shlobj.h>
#include <cwchar>
#include <d3d9.h>

extern void HookLog(const char* fmt, ...);

namespace {

using LaunchMenuUtils::GetSettingsPath;

constexpr int ID_SINGLE_THREADED = 100;
constexpr int ID_SAVE = 101;
constexpr int ID_CANCEL = 102;
constexpr int ID_DARK_MODE = 103;
constexpr int ID_SINGLE_THREAD_DESCRIPTION = 104;
constexpr int ID_RESOLUTION = 105;
constexpr int ID_FULLSCREEN = 106;
constexpr int ID_DISPLAY_HEADING = 107;
constexpr int ID_ADVANCED_HEADING = 108;
constexpr int ID_RENDERING_THREAD = 109;
constexpr int ID_STEAM_ROUTING = 110;
constexpr char SETTINGS_CLASS[] = "TimestoneAdvancedConfig";

bool g_settingsLoaded = false;
std::atomic<bool> g_runSingleThreaded{false};
using GeometryGroupMoveFn = void(__fastcall*)(void*);
GeometryGroupMoveFn g_geometryGroupMove = nullptr;
bool g_threadingHooksReady = false;
bool g_darkMode = true;
bool g_savedDarkMode = true;
char g_gameResolution[64] = {};
bool g_gameFullscreen = false;
bool g_displayChanged = false;

bool GetGameConfigPath(wchar_t (&path)[MAX_PATH])
{
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                SHGFP_TYPE_CURRENT, path))) return false;
    constexpr wchar_t suffix[] = L"\\CAPCOM\\ULTIMATE MARVEL VS. CAPCOM 3\\config.ini";
    if (wcslen(path) + wcslen(suffix) >= MAX_PATH) return false;
    wcscat(path, suffix);
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

UINT GetWindowDpi(HWND window)
{
    using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
    const auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
    if (getDpiForWindow) {
        const UINT dpi = getDpiForWindow(window);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(window);
    const UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) ReleaseDC(window, dc);
    return dpi ? dpi : 96;
}

void ApplyDpi(HWND window, UINT oldDpi, UINT newDpi)
{
    LaunchMenuUtils::ScaleChildren(window, oldDpi, newDpi, false);

    HFONT oldFont = reinterpret_cast<HFONT>(GetPropA(window, "TimestoneSettingsFont"));
    LOGFONTA description = {};
    description.lfHeight = -MulDiv(10, newDpi, 72);
    description.lfCharSet = DEFAULT_CHARSET;
    description.lfQuality = CLEARTYPE_QUALITY;
    snprintf(description.lfFaceName, sizeof(description.lfFaceName), "MS Shell Dlg 2");
    HFONT font = CreateFontIndirectA(&description);
    if (font) {
        SetPropA(window, "TimestoneSettingsFont", font);
        for (HWND child = GetWindow(window, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            SendMessageA(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            const int id = GetDlgCtrlID(child);
            if (id == ID_DISPLAY_HEADING || id == ID_ADVANCED_HEADING) {
                char text[64] = {};
                const int length = GetWindowTextA(child, text, sizeof(text));
                HDC dc = GetDC(child);
                HGDIOBJ oldFont = SelectObject(dc, font);
                SIZE size = {};
                GetTextExtentPoint32A(dc, text, length, &size);
                SelectObject(dc, oldFont);
                ReleaseDC(child, dc);
                RECT rect = {};
                GetWindowRect(child, &rect);
                SetWindowPos(child, nullptr, 0, 0, size.cx, rect.bottom - rect.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        if (oldFont) DeleteObject(oldFont);
    }
    SetPropA(window, "TimestoneSettingsDpi",
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(newDpi)));
}

void __fastcall HookGeometryGroupMove(void* group)
{
    if (!g_runSingleThreaded.load()) {
        g_geometryGroupMove(group);
        return;
    }
    // uGeometry2Group::mFlgMultiThread selects its native synchronous inline path.
    // Preserve each group's preference so disabling the override restores it.
    auto& multiThread = *(reinterpret_cast<uint8_t*>(group) + 0xB0);
    const uint8_t saved = multiThread;
    multiThread = 0;
    g_geometryGroupMove(group);
    multiThread = saved;
}

void LoadSettings()
{
    if (g_settingsLoaded) return;

    char path[MAX_PATH] = {};
    GetSettingsPath(path, sizeof(path));
    g_runSingleThreaded = GetPrivateProfileIntA(
        "Advanced", "EntireGameSingleThreaded", 0, path) != 0;
    g_darkMode = GetPrivateProfileIntA("General", "LaunchMenuDarkMode", 1, path) != 0;
    g_savedDarkMode = g_darkMode;
    g_settingsLoaded = true;
}

bool SaveSettings(HWND window)
{
    const int renderMode = static_cast<int>(SendMessageA(
        GetDlgItem(window, ID_RENDERING_THREAD), CB_GETCURSEL, 0, 0)) - 1;
    if (D3D9Reset_RenderingThreadOverrideAvailable &&
        !D3D9Reset_SetRenderingThreadOverride(renderMode)) {
        MessageBoxA(window, "Could not save the rendering thread setting.",
                    "Settings", MB_OK | MB_ICONERROR);
        return false;
    }
    if (IsWindowEnabled(GetDlgItem(window, ID_RESOLUTION))) {
        char resolution[64] = {};
        GetDlgItemTextA(window, ID_RESOLUTION, resolution, sizeof(resolution));
        int width = 0, height = 0;
        char extra = 0;
        if (sscanf(resolution, " %dx%d %c", &width, &height, &extra) != 2 ||
            width < 640 || height < 480 || width > 8192 || height > 8192) {
            MessageBoxA(window, "Enter a resolution such as 1920x1080.\n"
                        "Width must be 640-8192 and height 480-8192.",
                        "Invalid resolution", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(window, ID_RESOLUTION));
            return false;
        }
        snprintf(resolution, sizeof(resolution), "%dx%d", width, height);
        const bool fullscreen = SendMessageA(GetDlgItem(window, ID_FULLSCREEN),
                                             BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (strcmp(resolution, g_gameResolution) != 0 || fullscreen != g_gameFullscreen) {
            wchar_t path[MAX_PATH] = {}, value[64] = {};
            swprintf(value, 64, L"%dx%d", width, height);
            if (!GetGameConfigPath(path) ||
                (strcmp(resolution, g_gameResolution) != 0 &&
                 !WritePrivateProfileStringW(L"DISPLAY", L"Resolution", value, path)) ||
                (fullscreen != g_gameFullscreen &&
                 !WritePrivateProfileStringW(L"DISPLAY", L"FullScreen",
                                              fullscreen ? L"ON" : L"OFF", path))) {
                MessageBoxA(window, "Could not save MVC3's config.ini. Check that it exists "
                            "and is writable. Some changes may already have been saved.",
                            "Game display settings", MB_OK | MB_ICONERROR);
                return false;
            }
            snprintf(g_gameResolution, sizeof(g_gameResolution), "%s", resolution);
            g_gameFullscreen = fullscreen;
            g_displayChanged = true;
        }
    }
    const bool singleThreaded = SendMessageA(
        GetDlgItem(window, ID_SINGLE_THREADED), BM_GETCHECK, 0, 0) == BST_CHECKED;

    char path[MAX_PATH] = {};
    GetSettingsPath(path, sizeof(path));
    const bool darkMode = SendMessageA(GetDlgItem(window, ID_DARK_MODE),
                                       BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (IsWindowEnabled(GetDlgItem(window, ID_STEAM_ROUTING))) {
        const bool relayOnly = SendMessageA(GetDlgItem(window, ID_STEAM_ROUTING),
                                            CB_GETCURSEL, 0, 0) != 0;
        if (!IniSafe_WriteString("Network", "SteamRelayOnly",
                                 relayOnly ? "1" : "0", path)) {
            MessageBoxA(window, "Could not save the Steam routing setting.",
                        "Settings", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    if (IniSafe_WriteString("General", "LaunchMenuDarkMode",
                                   darkMode ? "1" : "0", path))
        g_savedDarkMode = darkMode;
    else
        HookLog("LaunchMenu: failed to save dark mode to %s\n", path);
    if (!SetGameSingleThreaded(singleThreaded)) {
        MessageBoxA(window, "Could not save the single-thread setting.",
                    "Settings", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

LRESULT CALLBACK SettingsWndProc(HWND window, UINT message,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
    {
        LoadSettings();
        HWND darkMode = CreateWindowA(
            "BUTTON", "Dark mode",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            18, 16, 374, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DARK_MODE)),
            nullptr, nullptr);
        SendMessageA(darkMode, BM_SETCHECK,
                     g_savedDarkMode ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateWindowA("STATIC", "Display settings",
            WS_VISIBLE | WS_CHILD, 34, 48, 110, 20, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DISPLAY_HEADING)), nullptr, nullptr);
        CreateWindowA("STATIC", "Resolution:", WS_VISIBLE | WS_CHILD,
            18, 78, 80, 20, window, nullptr, nullptr, nullptr);
        HWND resolution = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
            100, 74, 170, 180, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RESOLUTION)), nullptr, nullptr);
        // Match sRender__enumerateAdapterModes, including its resolution deduplication.
        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (d3d) {
            const UINT count = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
            for (UINT i = 0; i < count; ++i) {
                D3DDISPLAYMODE mode = {};
                if (FAILED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8,
                                                 i, &mode)) ||
                    mode.Width < 640 || mode.Height < 480 || (mode.Width & 7) != 0)
                    continue;
                char value[32] = {};
                snprintf(value, sizeof(value), "%ux%u", mode.Width, mode.Height);
                if (SendMessageA(resolution, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                 reinterpret_cast<LPARAM>(value)) == CB_ERR)
                    SendMessageA(resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
            }
            d3d->Release();
        }
        if (SendMessageA(resolution, CB_GETCOUNT, 0, 0) == 0)
            HookLog("LaunchMenu: no D3D9 resolution choices available; custom entry remains enabled\n");
        SendMessageA(resolution, CB_LIMITTEXT, 63, 0);
        HWND fullscreen = CreateWindowA("BUTTON", "Fullscreen",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            282, 74, 110, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_FULLSCREEN)), nullptr, nullptr);
        wchar_t gamePath[MAX_PATH] = {}, value[64] = {};
        const bool hasConfig = GetGameConfigPath(gamePath);
        if (hasConfig) {
            GetPrivateProfileStringW(L"DISPLAY", L"Resolution", L"1280x720",
                                     value, 64, gamePath);
            WideCharToMultiByte(CP_ACP, 0, value, -1, g_gameResolution,
                                sizeof(g_gameResolution), nullptr, nullptr);
            SetWindowTextA(resolution, g_gameResolution);
            GetPrivateProfileStringW(L"DISPLAY", L"FullScreen", L"OFF", value, 64, gamePath);
            g_gameFullscreen = _wcsicmp(value, L"ON") == 0 ||
                               _wcsicmp(value, L"true") == 0 || _wtoi(value) != 0;
            SendMessageA(fullscreen, BM_SETCHECK,
                         g_gameFullscreen ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        EnableWindow(resolution, hasConfig);
        EnableWindow(fullscreen, hasConfig);
        if (!hasConfig)
            CreateWindowA("STATIC", "config.ini not found. Run the game once to create it.",
                WS_VISIBLE | WS_CHILD, 18, 106, 374, 34, window, nullptr, nullptr, nullptr);
        const int headingY = hasConfig ? 118 : 150;
        CreateWindowA("STATIC", "Advanced",
            WS_VISIBLE | WS_CHILD, 34, headingY, 80, 20, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ADVANCED_HEADING)), nullptr, nullptr);
        const int sectionY = headingY + 28;
        HWND description = CreateWindowA(
            "STATIC",
            "Sets game worker count to 1. May massively improve performance by 100% "
            "on pcs with bad thread work, but may decrease performance by about 10% on higher end pcs.",
            WS_VISIBLE | WS_CHILD, 18, sectionY, 374, 50,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SINGLE_THREAD_DESCRIPTION)),
            nullptr, nullptr);
        HWND checkbox = CreateWindowA(
            "BUTTON", "Run entire game single-threaded",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            18, sectionY + 56, 374, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SINGLE_THREADED)),
            nullptr, nullptr);
        CreateWindowA("STATIC", "Rendering thread:", WS_VISIBLE | WS_CHILD,
            18, sectionY + 92, 130, 20, window, nullptr, nullptr, nullptr);
        HWND renderingThread = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST,
            154, sectionY + 88, 238, 120, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RENDERING_THREAD)), nullptr, nullptr);
        for (const char* option : {"Game default", "Force OFF", "Force ON"})
            SendMessageA(renderingThread, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
        SendMessageA(renderingThread, CB_SETCURSEL,
                     D3D9Reset_GetRenderingThreadOverride() + 1, 0);
        EnableWindow(renderingThread, D3D9Reset_RenderingThreadOverrideAvailable);
        CreateWindowA("STATIC", "ON adds one frame of latency. OFF may decrease performance or other various issues.",
            WS_VISIBLE | WS_CHILD, 18, sectionY + 120, 374, 34,
            window, nullptr, nullptr, nullptr);
        CreateWindowA("STATIC", "Steam routing:", WS_VISIBLE | WS_CHILD,
            18, sectionY + 164, 130, 20, window, nullptr, nullptr, nullptr);
        HWND steamRouting = CreateWindowA("COMBOBOX", "",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST,
            154, sectionY + 160, 238, 100, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STEAM_ROUTING)), nullptr, nullptr);
        for (const char* option : {"Automatic", "Relay Only"})
            SendMessageA(steamRouting, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
        SendMessageA(steamRouting, CB_SETCURSEL, SteamLobby_IsRelayOnly() ? 1 : 0, 0);
        EnableWindow(steamRouting, !SteamLobby_HasLobby() && !SteamLobby_IsCreating());
        CreateWindowA("STATIC",
            "Automatic may share your IP with your opponent. "
            "When in doubt, leave it to automatic, as it can provide "
            "fastest routing regardless of steam server or direct p2p connection.",
            WS_VISIBLE | WS_CHILD, 18, sectionY + 194, 374, 84,
            window, nullptr, nullptr, nullptr);
        HWND save = CreateWindowA(
            "BUTTON", "Save", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON,
            222, sectionY + 284, 82, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)), nullptr, nullptr);
        HWND cancel = CreateWindowA(
            "BUTTON", "Cancel", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            310, sectionY + 284, 82, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL)), nullptr, nullptr);
        ApplyDpi(window, 96, GetWindowDpi(window));
        ApplyLaunchMenuDarkTheme(window);
        SendMessageA(checkbox, BM_SETCHECK,
                     g_runSingleThreaded ? BST_CHECKED : BST_UNCHECKED, 0);
        SetFocus(darkMode);
        return 0;
    }
    case WM_DPICHANGED:
    {
        const UINT oldDpi = static_cast<UINT>(reinterpret_cast<INT_PTR>(
            GetPropA(window, "TimestoneSettingsDpi")));
        const UINT newDpi = HIWORD(wParam);
        ApplyDpi(window, oldDpi ? oldDpi : 96, newDpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_DARK_MODE && HIWORD(wParam) == BN_CLICKED) {
            g_darkMode = SendMessageA(GetDlgItem(window, ID_DARK_MODE),
                                      BM_GETCHECK, 0, 0) == BST_CHECKED;
            ApplyLaunchMenuDarkTheme(GetWindow(window, GW_OWNER));
            ApplyLaunchMenuDarkTheme(window);
            return 0;
        }
        if (LOWORD(wParam) == ID_SAVE) {
            if (!SaveSettings(window)) return 0;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == ID_CANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == ID_SINGLE_THREAD_DESCRIPTION) {
            const bool dark = IsLaunchMenuDarkModeEnabled();
            const COLORREF background = dark ? RGB(62, 57, 35) : RGB(255, 249, 219);
            static HBRUSH darkBrush = CreateSolidBrush(RGB(62, 57, 35));
            static HBRUSH lightBrush = CreateSolidBrush(RGB(255, 249, 219));
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, LaunchMenuDarkTextColor());
            SetBkColor(dc, background);
            SetBkMode(dc, OPAQUE);
            return reinterpret_cast<LRESULT>(dark ? darkBrush : lightBrush);
        }
        return HandleLaunchMenuDarkControlColor(message, wParam);
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleLaunchMenuDarkControlColor(message, wParam);
    case WM_ERASEBKGND:
        PaintLaunchMenuDarkBackground(window, reinterpret_cast<HDC>(wParam));
        return TRUE;
    case WM_PAINT:
    {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        RECT description = {}, checkbox = {};
        GetWindowRect(GetDlgItem(window, ID_SINGLE_THREAD_DESCRIPTION), &description);
        GetWindowRect(GetDlgItem(window, ID_SINGLE_THREADED), &checkbox);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&description), 2);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&checkbox), 2);
        const int inset = MulDiv(8, GetWindowDpi(window), 96);
        const int x = description.left - inset;
        const int top = (description.top + description.bottom) / 2;
        const int bottom = (checkbox.top + checkbox.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, IsLaunchMenuDarkModeEnabled()
                            ? RGB(96, 96, 96) : RGB(160, 160, 160));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        POINT line[] = {{description.left, top}, {x, top},
                        {x, bottom}, {checkbox.left, bottom}};
        Polyline(dc, line, 4);
        const int headings[] = {ID_DISPLAY_HEADING, ID_ADVANCED_HEADING};
        for (int id : headings) {
            HWND label = GetDlgItem(window, id);
            RECT heading = {};
            GetWindowRect(label, &heading);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&heading), 2);
            const int y = (heading.top + heading.bottom) / 2;
            MoveToEx(dc, description.left, y, nullptr);
            LineTo(dc, heading.left - inset, y);
            MoveToEx(dc, heading.right + inset, y, nullptr);
            LineTo(dc, description.right, y);
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
    {
        if (g_darkMode != g_savedDarkMode) {
            g_darkMode = g_savedDarkMode;
            ApplyLaunchMenuDarkTheme(GetWindow(window, GW_OWNER));
        }
        HFONT font = reinterpret_cast<HFONT>(
            GetPropA(window, "TimestoneSettingsFont"));
        if (font) DeleteObject(font);
        RemovePropA(window, "TimestoneSettingsFont");
        RemovePropA(window, "TimestoneSettingsDpi");
        return 0;
    }
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

} // namespace

bool IsLaunchMenuDarkModeEnabled()
{
    LoadSettings();
    return g_darkMode;
}

void ShowAdvancedConfigModal(HWND owner)
{
    HINSTANCE instance = GetModuleHandleA(nullptr);
    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = SettingsWndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = SETTINGS_CLASS;
    windowClass.hbrBackground = nullptr;
    windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    if (!RegisterClassA(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        HookLog("LaunchMenu: advanced config class registration failed error=%lu\n",
                GetLastError());
        return;
    }

    RECT ownerRect = {};
    GetWindowRect(owner, &ownerRect);
    const UINT dpi = GetWindowDpi(owner);
    wchar_t gamePath[MAX_PATH] = {};
    RECT bounds = {0, 0, MulDiv(410, dpi, 96),
                   MulDiv(GetGameConfigPath(gamePath) ? 476 : 508, dpi, 96)};
    using AdjustWindowRectExForDpiFn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    const auto adjustForDpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
        GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi"));
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    if (adjustForDpi)
        adjustForDpi(&bounds, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    else
        AdjustWindowRectEx(&bounds, style, FALSE, WS_EX_DLGMODALFRAME);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    EnableWindow(owner, FALSE);
    HWND modal = CreateWindowExA(
        WS_EX_DLGMODALFRAME, SETTINGS_CLASS, "Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, width, height, owner, nullptr, instance, nullptr);
    if (!modal) {
        EnableWindow(owner, TRUE);
        HookLog("LaunchMenu: advanced config window creation failed error=%lu\n",
                GetLastError());
        return;
    }

    ShowWindow(modal, SW_SHOW);
    UpdateWindow(modal);
    MSG message = {};
    while (IsWindow(modal) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(modal, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    SetFocus(owner);
}

bool PrepareAdvancedLaunchConfig()
{
    LoadSettings();
    if (g_threadingHooksReady) return true;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        HookLog("Advanced Config: MinHook initialization failed status=%d\n", (int)init);
        return false;
    }
    void* target = reinterpret_cast<void*>(_addr(0x14095EC90));
    MH_STATUS status = MH_CreateHook(target,
        reinterpret_cast<void*>(&HookGeometryGroupMove),
        reinterpret_cast<void**>(&g_geometryGroupMove));
    if (status == MH_OK) {
        status = MH_EnableHook(target);
        if (status != MH_OK) MH_RemoveHook(target);
    }
    g_threadingHooksReady = status == MH_OK;
    HookLog("Advanced Config: live threading hook status=%d\n", (int)status);
    return g_threadingHooksReady;
}

bool ApplyAdvancedLaunchConfig()
{
    LoadSettings();
    uint64_t sMain = *(uint64_t*)_addr(0x140e177e8);
    if (!sMain || !g_threadingHooksReady) return false;

    static bool initialized = false;
    static bool applied = false;
    static uint32_t savedThreadCount = 1;
    const bool enabled = g_runSingleThreaded.load();
    if (initialized && enabled == applied) return true;

    auto& requested = *reinterpret_cast<uint32_t*>(sMain + 0xA8);
    if (enabled) savedThreadCount = requested;
    using SetJobThreadNumFn = void(__fastcall*)(uint64_t, uint32_t);
    if (enabled || initialized) {
        ((SetJobThreadNumFn)_addr(0x140522450))(sMain, enabled ? 1 : savedThreadCount);
        // sMain::move has already copied the count this tick. No batch is active
        // at this game-thread boundary, so update the current count as well.
        *reinterpret_cast<uint32_t*>(sMain + 0xA4) = requested;
    }
    applied = enabled;
    initialized = true;
    HookLog("Advanced Config: single-thread override %s\n", enabled ? "enabled" : "disabled");
    return true;
}

void ApplyLaunchDisplayConfig()
{
    if (!g_displayChanged)
        return;
    g_displayChanged = false;
    void* render = *reinterpret_cast<void**>(_addr(0x140E179A8));
    if (!render)
        return; // Blocking launches have not created their renderer yet.
    uint32_t dimensions[2] = {};
    int width = 0, height = 0;
    if (sscanf(g_gameResolution, "%dx%d", &width, &height) != 2)
        return;
    dimensions[0] = static_cast<uint32_t>(width);
    dimensions[1] = static_cast<uint32_t>(height);
    // The existing recovery hook copies this request and applies it at the next
    // native render boundary, rather than resetting the device from the menu.
    using ResizeFn = void(__fastcall*)(void*, bool, const uint64_t*, uint64_t);
    uint64_t size = 0;
    memcpy(&size, dimensions, sizeof(size));
    reinterpret_cast<ResizeFn>(_addr(0x14053EDE0))(
        render, g_gameFullscreen, &size, 0);
    HookLog("LaunchMenu: queued display change for preloaded renderer (%s fullscreen=%d)\n",
            g_gameResolution, g_gameFullscreen);
}

bool IsGameSingleThreaded()
{
    LoadSettings();
    return g_runSingleThreaded.load();
}

bool SetGameSingleThreaded(bool enabled)
{
    LoadSettings();
    char path[MAX_PATH] = {};
    GetSettingsPath(path, sizeof(path));
    if (!IniSafe_WriteString("Advanced", "EntireGameSingleThreaded",
                             enabled ? "1" : "0", path)) return false;
    g_runSingleThreaded.store(enabled);
    return true;
}
