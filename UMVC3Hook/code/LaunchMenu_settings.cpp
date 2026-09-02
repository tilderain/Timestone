#include "LaunchMenu_settings.h"

#include "launchmenu_dark.h"
#include "mk10utils.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr int ID_SINGLE_THREADED = 100;
constexpr int ID_SAVE = 101;
constexpr int ID_CANCEL = 102;
constexpr char SETTINGS_CLASS[] = "TimestoneAdvancedConfig";

bool g_settingsLoaded = false;
bool g_runSingleThreaded = false;

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
    if (oldDpi != newDpi) {
        for (HWND child = GetWindow(window, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            RECT rect = {};
            GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
            SetWindowPos(child, nullptr,
                         MulDiv(rect.left, newDpi, oldDpi),
                         MulDiv(rect.top, newDpi, oldDpi),
                         MulDiv(rect.right - rect.left, newDpi, oldDpi),
                         MulDiv(rect.bottom - rect.top, newDpi, oldDpi),
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }

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
        }
        if (oldFont) DeleteObject(oldFont);
    }
    SetPropA(window, "TimestoneSettingsDpi",
             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(newDpi)));
}

template <size_t N>
bool PatchBytes(uintptr_t address, const uint8_t (&expected)[N],
                const uint8_t (&replacement)[N])
{
    uint8_t* target = reinterpret_cast<uint8_t*>(_addr(address));
    if (memcmp(target, replacement, N) == 0) return true;
    if (memcmp(target, expected, N) != 0) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, N, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    memcpy(target, replacement, N);
    FlushInstructionCache(GetCurrentProcess(), target, N);
    DWORD ignored = 0;
    VirtualProtect(target, N, oldProtect, &ignored);
    return true;
}

void GetSettingsPath(char* path, size_t pathSize)
{
    const DWORD length = GetModuleFileNameA(
        nullptr, path, static_cast<DWORD>(pathSize));
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

void LoadSettings()
{
    if (g_settingsLoaded) return;

    char path[MAX_PATH] = {};
    GetSettingsPath(path, sizeof(path));
    g_runSingleThreaded = GetPrivateProfileIntA(
        "Advanced", "EntireGameSingleThreaded", 0, path) != 0;
    g_settingsLoaded = true;
}

void SaveSettings(HWND window)
{
    g_runSingleThreaded = SendMessageA(
        GetDlgItem(window, ID_SINGLE_THREADED), BM_GETCHECK, 0, 0) == BST_CHECKED;

    char path[MAX_PATH] = {};
    GetSettingsPath(path, sizeof(path));
    if (!WritePrivateProfileStringA(
            "Advanced", "EntireGameSingleThreaded",
            g_runSingleThreaded ? "1" : "0", path)) {
        HookLog("LaunchMenu: failed to save advanced config to %s\n", path);
    } else {
        HookLog("LaunchMenu: saved entire-game single-thread setting: %s\n",
                g_runSingleThreaded ? "enabled" : "disabled");
    }
}

LRESULT CALLBACK SettingsWndProc(HWND window, UINT message,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
    {
        LoadSettings();
        HWND description = CreateWindowA(
            "STATIC",
            "Sets game worker count to 1. May massively improve performance by 100% "
            "on pcs with bad thread work, but may decrease performance by about 10% on higher end pcs.",
            WS_VISIBLE | WS_CHILD, 18, 16, 374, 50,
            window, nullptr, nullptr, nullptr);
        HWND checkbox = CreateWindowA(
            "BUTTON", "Run entire game single-threaded",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
            18, 72, 300, 24, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SINGLE_THREADED)),
            nullptr, nullptr);
        HWND save = CreateWindowA(
            "BUTTON", "Save", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON,
            222, 104, 82, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)), nullptr, nullptr);
        HWND cancel = CreateWindowA(
            "BUTTON", "Cancel", WS_VISIBLE | WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
            310, 104, 82, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL)), nullptr, nullptr);
        ApplyDpi(window, 96, GetWindowDpi(window));
        ApplyLaunchMenuDarkTheme(window);
        SendMessageA(checkbox, BM_SETCHECK,
                     g_runSingleThreaded ? BST_CHECKED : BST_UNCHECKED, 0);
        SetFocus(checkbox);
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
        if (LOWORD(wParam) == ID_SAVE) {
            SaveSettings(window);
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
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return HandleLaunchMenuDarkControlColor(message, wParam);
    case WM_ERASEBKGND:
        PaintLaunchMenuDarkBackground(window, reinterpret_cast<HDC>(wParam));
        return TRUE;
    case WM_DESTROY:
    {
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
    const int width = MulDiv(428, dpi, 96);
    const int height = MulDiv(178, dpi, 96);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    EnableWindow(owner, FALSE);
    HWND modal = CreateWindowExA(
        WS_EX_DLGMODALFRAME, SETTINGS_CLASS, "Advanced Config",
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
    while (IsWindow(modal) && GetMessageA(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(modal, &message)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    SetFocus(owner);
}

bool PrepareAdvancedLaunchConfig()
{
    LoadSettings();
    if (!g_runSingleThreaded) return true;

    // sMain__dispatchAndWaitJobs normally queues work to the delay-job ring and
    // waits for workers. Use its native inline branch before suppressing worker
    // creation, otherwise an asynchronous batch would wait forever.
    static constexpr uint8_t DELAY_DISPATCH[] = {
        0x0F, 0x84, 0xBB, 0x00, 0x00, 0x00,
    };
    static constexpr uint8_t DELAY_INLINE[] = {
        0xE9, 0xBC, 0x00, 0x00, 0x00, 0x90,
    };
    if (!PatchBytes(0x14095ECD9, DELAY_DISPATCH, DELAY_INLINE)) {
        HookLog("Advanced Config: failed to force delay jobs inline\n");
        return false;
    }

    // sMain's constructor passes the detected logical CPU count in EDX. Zero
    // is a supported input and makes createDelayJobThread skip every worker.
    static constexpr uint8_t LOAD_DELAY_THREAD_COUNT[] = {
        0x8B, 0x97, 0x88, 0x00, 0x04, 0x00,
    };
    static constexpr uint8_t ZERO_DELAY_THREAD_COUNT[] = {
        0x33, 0xD2, 0x90, 0x90, 0x90, 0x90,
    };
    if (!PatchBytes(0x14051FC6D, LOAD_DELAY_THREAD_COUNT,
                    ZERO_DELAY_THREAD_COUNT)) {
        HookLog("Advanced Config: failed to disable delay-job workers\n");
        return false;
    }

    HookLog("Advanced Config: delay jobs forced inline; workers disabled\n");
    return true;
}

bool ApplyAdvancedLaunchConfig()
{
    LoadSettings();
    if (!g_runSingleThreaded) return true;

    uint64_t sMain = *(uint64_t*)_addr(0x140e177e8);
    if (!sMain) return false;

    using SetJobThreadNumFn = void(__fastcall*)(uint64_t, uint32_t);
    ((SetJobThreadNumFn)_addr(0x140522450))(sMain, 1);
    HookLog("Advanced Config: regular jobs forced inline\n");
    return true;
}
