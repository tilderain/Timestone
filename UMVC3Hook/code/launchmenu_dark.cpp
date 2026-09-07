#include "launchmenu_dark.h"
#include "LaunchMenu_settings.h"
#include "../kiero/minhook/include/MinHook.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <unordered_map>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr COLORREF DARK_BACKGROUND = RGB(32, 32, 32);
constexpr COLORREF DARK_CONTROL = RGB(51, 51, 51);
constexpr COLORREF DARK_FACE = RGB(56, 56, 56);
constexpr COLORREF DARK_TEXT = RGB(224, 224, 224);
constexpr COLORREF DARK_DISABLED = RGB(136, 136, 136);
constexpr COLORREF DARK_BORDER = RGB(96, 96, 96);
constexpr char DARK_ORIGINAL_PROC_PROP[] = "TimestoneDarkOriginalProc";
constexpr char DARK_SCROLLBAR_PROP[] = "TimestoneDarkScrollbar";

using SetWindowThemeFn = HRESULT (WINAPI*)(HWND, LPCWSTR, LPCWSTR);
using AllowDarkModeForWindowFn = BOOL (WINAPI*)(HWND, BOOL);
using SetPreferredAppModeFn = int (WINAPI*)(int);
using AllowDarkModeForAppFn = BOOL (WINAPI*)(BOOL);
using RefreshImmersiveColorPolicyStateFn = void (WINAPI*)();
using OpenNcThemeDataFn = HANDLE (WINAPI*)(HWND, LPCWSTR);
using OpenThemeDataForDpiFn = HTHEME (WINAPI*)(HWND, LPCWSTR, UINT);

HMODULE g_uxTheme = nullptr;
AllowDarkModeForWindowFn g_allowDarkModeForWindow = nullptr;
SetWindowThemeFn g_setWindowTheme = nullptr;
SetPreferredAppModeFn g_setPreferredAppMode = nullptr;
AllowDarkModeForAppFn g_allowDarkModeForApp = nullptr;
OpenNcThemeDataFn g_openNcThemeData = nullptr;
OpenNcThemeDataFn g_openThemeData = nullptr;
OpenThemeDataForDpiFn g_openThemeDataForDpi = nullptr;
decltype(&DrawThemeBackground) g_drawThemeBackground = nullptr;
decltype(&DrawThemeBackgroundEx) g_drawThemeBackgroundEx = nullptr;
decltype(&CloseThemeData) g_closeThemeData = nullptr;
SRWLOCK g_scrollbarThemesLock = SRWLOCK_INIT;
std::unordered_map<HANDLE, HWND> g_scrollbarThemes;

HANDLE TrackScrollbarTheme(HWND window, LPCWSTR classList, HANDLE theme)
{
    if (theme && classList &&
        _wcsicmp(classList, L"ScrollBar") == 0) {
        AcquireSRWLockExclusive(&g_scrollbarThemesLock);
        g_scrollbarThemes[theme] = window;
        ReleaseSRWLockExclusive(&g_scrollbarThemesLock);
    }
    return theme;
}

HANDLE WINAPI OpenLaunchNcThemeData(HWND window, LPCWSTR classList)
{
    return TrackScrollbarTheme(window, classList, g_openNcThemeData(window, classList));
}

HANDLE WINAPI OpenLaunchThemeData(HWND window, LPCWSTR classList)
{
    return TrackScrollbarTheme(window, classList, g_openThemeData(window, classList));
}

HTHEME WINAPI OpenLaunchThemeDataForDpi(HWND window, LPCWSTR classList, UINT dpi)
{
    // Wine's UXTHEME_ScrollBarDraw opens non-client scrollbars through this
    // entry point with a null HWND, not OpenThemeData/OpenNcThemeData:
    // https://github.com/wine-mirror/wine/blob/master/dlls/uxtheme/scrollbar.c
    return TrackScrollbarTheme(window, classList, g_openThemeDataForDpi(window, classList, dpi));
}

HRESULT WINAPI CloseLaunchThemeData(HTHEME theme)
{
    AcquireSRWLockExclusive(&g_scrollbarThemesLock);
    g_scrollbarThemes.erase(theme);
    ReleaseSRWLockExclusive(&g_scrollbarThemesLock);
    return g_closeThemeData(theme);
}

bool PaintScrollbarTheme(HTHEME theme, HDC dc, int part, int state,
                         const RECT* rect, const RECT* clip)
{
    if (!dc || !rect || !IsLaunchMenuDarkModeEnabled()) return false;
    AcquireSRWLockShared(&g_scrollbarThemesLock);
    const auto found = g_scrollbarThemes.find(theme);
    const bool scrollbar = found != g_scrollbarThemes.end();
    const HWND owner = scrollbar ? found->second : nullptr;
    ReleaseSRWLockShared(&g_scrollbarThemesLock);
    if (!scrollbar || part < SBP_ARROWBTN || part > SBP_SIZEBOX) return false;
    // Some implementations cache a scrollbar theme opened with a null HWND.
    // In that case scope the paint using the destination window instead.
    HWND target = WindowFromDC(dc);
    if (!target) target = owner;
    if (!target || !GetPropA(target, DARK_SCROLLBAR_PROP)) return false;
    const int saved = SaveDC(dc);
    if (!saved) return false;
    IntersectClipRect(dc, rect->left, rect->top, rect->right, rect->bottom);
    if (clip) IntersectClipRect(dc, clip->left, clip->top, clip->right, clip->bottom);
    const auto fill = [dc](const RECT& area, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &area, brush);
        DeleteObject(brush);
    };
    fill(*rect, DARK_CONTROL);
    if (part == SBP_ARROWBTN) {
        // Arrow states are four groups (up/down/left/right), each with
        // normal/hot/pressed/disabled states. Size from BOTH dimensions.
        const int direction = state >= ABS_UPHOVER ? state - ABS_UPHOVER : (state - 1) / 4;
        const int interaction = state >= ABS_UPHOVER ? 1 : (state - 1) % 4;
        fill(*rect, interaction == 2 ? DARK_BORDER : interaction == 1 ? DARK_FACE : DARK_CONTROL);
        const int x = (rect->left + rect->right) / 2;
        const int y = (rect->top + rect->bottom) / 2;
        const int radius = std::max(1L, std::min(rect->right - rect->left, rect->bottom - rect->top) / 5);
        POINT points[3];
        const int sign = (direction == 1 || direction == 3) ? 1 : -1;
        if (direction < 2) {
            points[0] = {x, y + sign * radius};
            points[1] = {x - radius, y - sign * radius};
            points[2] = {x + radius, y - sign * radius};
        } else {
            points[0] = {x + sign * radius, y};
            points[1] = {x - sign * radius, y - radius};
            points[2] = {x - sign * radius, y + radius};
        }
        HBRUSH brush = CreateSolidBrush(interaction == 3 ? DARK_DISABLED : DARK_TEXT);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, 3);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    } else if (part == SBP_THUMBBTNHORZ || part == SBP_THUMBBTNVERT ||
               part == SBP_GRIPPERHORZ || part == SBP_GRIPPERVERT) {
        RECT thumb = *rect;
        if (part == SBP_THUMBBTNHORZ || part == SBP_THUMBBTNVERT)
            InflateRect(&thumb, -1, -1);
        fill(thumb, state == SCRBS_PRESSED ? DARK_TEXT :
                    state == SCRBS_HOT || state == SCRBS_HOVER ? DARK_DISABLED : DARK_BORDER);
    }
    RestoreDC(dc, saved);
    return true;
}

HRESULT WINAPI DrawLaunchThemeBackground(HTHEME theme, HDC dc, int part, int state,
                                         const RECT* rect, const RECT* clip)
{
    if (PaintScrollbarTheme(theme, dc, part, state, rect, clip)) return S_OK;
    return g_drawThemeBackground(theme, dc, part, state, rect, clip);
}

HRESULT WINAPI DrawLaunchThemeBackgroundEx(HTHEME theme, HDC dc, int part, int state,
                                           const RECT* rect, const DTBGOPTS* options)
{
    const RECT* clip = options && (options->dwFlags & DTBG_CLIPRECT) ? &options->rcClip : nullptr;
    if (PaintScrollbarTheme(theme, dc, part, state, rect, clip)) return S_OK;
    return g_drawThemeBackgroundEx(theme, dc, part, state, rect, options);
}

void InstallScrollbarThemeHook(const char* name, void* replacement, void** original)
{
    const char* label = IS_INTRESOURCE(name) ? "OpenNcThemeData (ordinal 49)" : name;
    void* target = reinterpret_cast<void*>(GetProcAddress(g_uxTheme, name));
    if (!target) {
        HookLog("LaunchMenu: Scrollbar theme export unavailable: %s\n", label);
        return;
    }
    MH_STATUS status = MH_CreateHook(target, replacement, original);
    if (status == MH_OK) {
        status = MH_EnableHook(target);
        if (status != MH_OK) {
            MH_RemoveHook(target);
            *original = nullptr;
        }
    }
    if (status != MH_OK)
        HookLog("LaunchMenu: Scrollbar theme hook %s failed (%d)\n", label, status);
}

HBRUSH DarkControlBrush()
{
    static HBRUSH brush = CreateSolidBrush(DARK_CONTROL);
    return brush;
}

HBRUSH DarkBorderBrush()
{
    static HBRUSH brush = CreateSolidBrush(DARK_BORDER);
    return brush;
}

HBRUSH DarkFaceBrush()
{
    static HBRUSH brush = CreateSolidBrush(DARK_FACE);
    return brush;
}

void PaintDarkControlBorder(HWND control)
{
    HDC dc = GetWindowDC(control);
    if (!dc) return;

    RECT border = {};
    GetWindowRect(control, &border);
    OffsetRect(&border, -border.left, -border.top);
    FrameRect(dc, &border, DarkBorderBrush());
    ReleaseDC(control, dc);
}

WNDPROC GetOriginalControlProc(HWND control)
{
    return reinterpret_cast<WNDPROC>(GetPropA(control, DARK_ORIGINAL_PROC_PROP));
}

void PaintDarkComboBox(HWND control, HDC dc)
{
    RECT client = {};
    GetClientRect(control, &client);
    HFONT font = reinterpret_cast<HFONT>(SendMessageA(control, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    FillRect(dc, &client, DarkControlBrush());
    FrameRect(dc, &client, DarkBorderBrush());

    const int arrowWidth = std::max(20, GetSystemMetrics(SM_CXVSCROLL));
    RECT arrow = client;
    arrow.left = std::max(client.left, client.right - arrowWidth);
    InflateRect(&arrow, -1, -1);
    FillRect(dc, &arrow, DarkFaceBrush());

    const int centerX = (arrow.left + arrow.right) / 2;
    const int centerY = (arrow.top + arrow.bottom) / 2;
    POINT triangle[] = {
        {centerX - 4, centerY - 2},
        {centerX + 4, centerY - 2},
        {centerX, centerY + 3},
    };
    HBRUSH arrowBrush = CreateSolidBrush(DARK_TEXT);
    HGDIOBJ oldBrush = SelectObject(dc, arrowBrush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, triangle, 3);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(arrowBrush);

    char text[256] = {};
    GetWindowTextA(control, text, sizeof(text));
    RECT textRect = client;
    textRect.left += 6;
    textRect.right = arrow.left - 4;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, IsWindowEnabled(control) ? DARK_TEXT : DARK_DISABLED);
    DrawTextA(dc, text, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
}

void SelectControlFont(HWND control, HDC dc, HGDIOBJ* oldFont)
{
    HFONT font = reinterpret_cast<HFONT>(SendMessageA(control, WM_GETFONT, 0, 0));
    *oldFont = font ? SelectObject(dc, font) : nullptr;
}

void DrawButtonText(HWND control, HDC dc, RECT textRect, UINT format)
{
    char text[256] = {};
    GetWindowTextA(control, text, sizeof(text));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, IsWindowEnabled(control) ? DARK_TEXT : DARK_DISABLED);
    DrawTextA(dc, text, -1, &textRect, format);
}

void PaintDarkPushButton(HWND control, HDC dc, RECT client, DWORD type)
{
    const LRESULT state = SendMessageA(control, BM_GETSTATE, 0, 0);
    const bool pushed = (state & BST_PUSHED) != 0;
    POINT cursor = {};
    GetCursorPos(&cursor);
    const bool hovered = IsWindowEnabled(control) && WindowFromPoint(cursor) == control;
    HBRUSH face = CreateSolidBrush(pushed ? RGB(40, 40, 40)
        : hovered ? RGB(58, 58, 58) : RGB(48, 48, 48));
    FillRect(dc, &client, face);
    DeleteObject(face);
    FrameRect(dc, &client, DarkBorderBrush());

    if (type == BS_DEFPUSHBUTTON) {
        RECT inner = client;
        InflateRect(&inner, -2, -2);
        FrameRect(dc, &inner, DarkBorderBrush());
    }

    RECT textRect = client;
    if (pushed) OffsetRect(&textRect, 1, 1);
    DrawButtonText(control, dc, textRect,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if ((state & BST_FOCUS) != 0) {
        RECT focus = client;
        InflateRect(&focus, -4, -4);
        FrameRect(dc, &focus, DarkBorderBrush());
    }
}

void PaintDarkCheckButton(HWND control, HDC dc, RECT client, bool radio)
{
    FillRect(dc, &client, LaunchMenuDarkBackgroundBrush());

    const int height = static_cast<int>(client.bottom - client.top);
    const int boxSize = std::min(15, height - 2);
    RECT box = {client.left + 1,
                client.top + (height - boxSize) / 2,
                client.left + 1 + boxSize,
                client.top + (height - boxSize) / 2 + boxSize};
    HBRUSH face = CreateSolidBrush(DARK_CONTROL);
    if (radio) {
        HGDIOBJ oldBrush = SelectObject(dc, face);
        HPEN pen = CreatePen(PS_SOLID, 1, DARK_BORDER);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        Ellipse(dc, box.left, box.top, box.right, box.bottom);
        if (SendMessageA(control, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            RECT dot = box;
            InflateRect(&dot, -4, -4);
            SelectObject(dc, GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc, DARK_TEXT);
            Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
        }
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        SelectObject(dc, oldBrush);
    } else {
        FillRect(dc, &box, face);
        FrameRect(dc, &box, DarkBorderBrush());
        const LRESULT checked = SendMessageA(control, BM_GETCHECK, 0, 0);
        if (checked == BST_CHECKED) {
            HPEN pen = CreatePen(PS_SOLID, 2, DARK_TEXT);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, box.left + 3, box.top + boxSize / 2, nullptr);
            LineTo(dc, box.left + boxSize / 2 - 1, box.bottom - 3);
            LineTo(dc, box.right - 2, box.top + 3);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        } else if (checked == BST_INDETERMINATE) {
            RECT mark = box;
            InflateRect(&mark, -4, -4);
            FillRect(dc, &mark, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
        }
    }
    DeleteObject(face);

    RECT textRect = client;
    textRect.left = box.right + 5;
    DrawButtonText(control, dc, textRect,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void PaintDarkGroupBox(HWND control, HDC dc, RECT client)
{
    char text[256] = {};
    GetWindowTextA(control, text, sizeof(text));
    TEXTMETRICA metrics = {};
    GetTextMetricsA(dc, &metrics);

    RECT frame = client;
    frame.top += metrics.tmHeight / 2;
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, DARK_BORDER);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Rectangle(dc, frame.left, frame.top, frame.right, frame.bottom);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    SelectObject(dc, oldBrush);

    RECT label = client;
    label.left += 9;
    label.right -= 9;
    label.bottom = label.top + metrics.tmHeight + 2;
    SetBkColor(dc, DARK_BACKGROUND);
    SetBkMode(dc, OPAQUE);
    SetTextColor(dc, DARK_TEXT);
    DrawTextA(dc, text, -1, &label, DT_LEFT | DT_SINGLELINE);
}

void PaintDarkButton(HWND control, HDC dc)
{
    RECT client = {};
    GetClientRect(control, &client);
    HGDIOBJ oldFont = nullptr;
    SelectControlFont(control, dc, &oldFont);

    const DWORD type = static_cast<DWORD>(GetWindowLongPtrA(control, GWL_STYLE)) &
                       BS_TYPEMASK;
    if (type == BS_GROUPBOX)
        PaintDarkGroupBox(control, dc, client);
    else if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
             type == BS_3STATE || type == BS_AUTO3STATE)
        PaintDarkCheckButton(control, dc, client, false);
    else if (type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON)
        PaintDarkCheckButton(control, dc, client, true);
    else
        PaintDarkPushButton(control, dc, client, type);

    if (oldFont) SelectObject(dc, oldFont);
}

LRESULT CALLBACK DarkComboBoxProc(HWND control, UINT message, WPARAM wParam,
                                  LPARAM lParam)
{
    WNDPROC original = GetOriginalControlProc(control);
    if (!original)
        return DefWindowProcA(control, message, wParam, lParam);
    if (!IsLaunchMenuDarkModeEnabled() && message != WM_NCDESTROY)
        return CallWindowProcA(original, control, message, wParam, lParam);

    if (message == WM_PAINT) {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(control, &paint);
        PaintDarkComboBox(control, dc);
        EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        PaintDarkComboBox(control, reinterpret_cast<HDC>(wParam));
        return 0;
    }
    if (message == WM_ERASEBKGND)
        return TRUE;

    const LRESULT result = CallWindowProcA(original, control, message, wParam, lParam);
    if (message == WM_NCPAINT || message == WM_SETFOCUS ||
        message == WM_KILLFOCUS || message == WM_ENABLE)
        PaintDarkControlBorder(control);
    if (message == CB_SETCURSEL || message == CB_SHOWDROPDOWN ||
        message == WM_COMMAND || message == WM_SETFOCUS ||
        message == WM_KILLFOCUS || message == WM_ENABLE ||
        message == WM_KEYDOWN || message == WM_LBUTTONDOWN ||
        message == WM_LBUTTONUP || message == WM_SETTEXT)
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    if (message == WM_NCDESTROY)
        RemovePropA(control, DARK_ORIGINAL_PROC_PROP);
    return result;
}

LRESULT CALLBACK DarkHeaderProc(HWND control, UINT message, WPARAM wParam,
                                LPARAM lParam)
{
    WNDPROC original = GetOriginalControlProc(control);
    if (!original)
        return DefWindowProcA(control, message, wParam, lParam);
    if (!IsLaunchMenuDarkModeEnabled() && message != WM_NCDESTROY)
        return CallWindowProcA(original, control, message, wParam, lParam);

    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint = {};
        HDC dc = message == WM_PAINT ? BeginPaint(control, &paint)
                                    : reinterpret_cast<HDC>(wParam);
        const int savedDC = SaveDC(dc);
        RECT client = {};
        GetClientRect(control, &client);
        FillRect(dc, &client, DarkFaceBrush());
        HGDIOBJ oldFont = nullptr;
        SelectControlFont(control, dc, &oldFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, DARK_TEXT);

        for (int i = 0; i < Header_GetItemCount(control); ++i) {
            RECT itemRect = {};
            if (!Header_GetItemRect(control, i, &itemRect)) continue;
            wchar_t text[256] = {};
            HDITEMW item = {};
            item.mask = HDI_TEXT | HDI_FORMAT;
            item.pszText = text;
            item.cchTextMax = static_cast<int>(sizeof(text) / sizeof(text[0]));
            if (!SendMessageW(control, HDM_GETITEMW, i,
                              reinterpret_cast<LPARAM>(&item))) continue;

            FrameRect(dc, &itemRect, DarkBorderBrush());
            itemRect.left += 6;
            itemRect.right -= 6;
            const UINT alignment = (item.fmt & HDF_JUSTIFYMASK) == HDF_RIGHT
                ? DT_RIGHT : (item.fmt & HDF_JUSTIFYMASK) == HDF_CENTER
                ? DT_CENTER : DT_LEFT;
            DrawTextW(dc, text, -1, &itemRect,
                      alignment | DT_VCENTER | DT_SINGLELINE |
                      DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        if (oldFont) SelectObject(dc, oldFont);
        RestoreDC(dc, savedDC);
        if (message == WM_PAINT) EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND)
        return TRUE;

    const LRESULT result = CallWindowProcA(original, control, message, wParam, lParam);
    if (message == WM_NCDESTROY)
        RemovePropA(control, DARK_ORIGINAL_PROC_PROP);
    return result;
}

LRESULT CALLBACK DarkBorderControlProc(HWND control, UINT message, WPARAM wParam,
                                       LPARAM lParam)
{
    WNDPROC original = GetOriginalControlProc(control);
    if (!original)
        return DefWindowProcA(control, message, wParam, lParam);
    if (!IsLaunchMenuDarkModeEnabled() && message != WM_NCDESTROY)
        return CallWindowProcA(original, control, message, wParam, lParam);

    const LRESULT result = CallWindowProcA(original, control, message, wParam, lParam);
    if (message == WM_PAINT || message == WM_NCPAINT ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_ENABLE)
        PaintDarkControlBorder(control);
    if (message == WM_NCDESTROY)
        RemovePropA(control, DARK_ORIGINAL_PROC_PROP);
    return result;
}

LRESULT CALLBACK DarkListViewProc(HWND control, UINT message, WPARAM wParam,
                                  LPARAM lParam)
{
    const LRESULT result = DarkBorderControlProc(control, message, wParam, lParam);
    if (!IsLaunchMenuDarkModeEnabled()) return result;
    if (message != WM_PAINT && message != WM_PRINTCLIENT)
        return result;

    HDC dc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wParam)
                                      : GetDC(control);
    if (!dc) return result;
    const int savedDC = SaveDC(dc);
    RECT client = {};
    GetClientRect(control, &client);
    HWND header = ListView_GetHeader(control);
    RECT headerRect = {};
    GetWindowRect(header, &headerRect);
    MapWindowPoints(nullptr, control, reinterpret_cast<POINT*>(&headerRect), 2);
    client.top = std::max(client.top, headerRect.bottom);
    IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
    HPEN pen = CreatePen(PS_SOLID, 1, DARK_BORDER);
    HGDIOBJ oldPen = SelectObject(dc, pen);

    // Native gridlines have no configurable color. Draw separators after the rows.
    for (int column = 0; column < Header_GetItemCount(header); ++column) {
        RECT rect = {};
        if (!Header_GetItemRect(header, column, &rect)) continue;
        MapWindowPoints(header, control, reinterpret_cast<POINT*>(&rect), 2);
        MoveToEx(dc, rect.right - 1, client.top, nullptr);
        LineTo(dc, rect.right - 1, client.bottom);
    }
    const int count = ListView_GetItemCount(control);
    for (int row = ListView_GetTopIndex(control); row < count; ++row) {
        RECT rect = {};
        if (!ListView_GetItemRect(control, row, &rect, LVIR_BOUNDS)) break;
        if (rect.top >= client.bottom) break;
        MoveToEx(dc, client.left, rect.bottom - 1, nullptr);
        LineTo(dc, client.right, rect.bottom - 1);
    }

    SelectObject(dc, oldPen);
    DeleteObject(pen);
    RestoreDC(dc, savedDC);
    if (message != WM_PRINTCLIENT) ReleaseDC(control, dc);
    return result;
}

LRESULT CALLBACK DarkButtonProc(HWND control, UINT message, WPARAM wParam,
                                LPARAM lParam)
{
    WNDPROC original = GetOriginalControlProc(control);
    if (!original)
        return DefWindowProcA(control, message, wParam, lParam);
    if (!IsLaunchMenuDarkModeEnabled() && message != WM_NCDESTROY)
        return CallWindowProcA(original, control, message, wParam, lParam);

    if (message == WM_PAINT) {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(control, &paint);
        PaintDarkButton(control, dc);
        EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        PaintDarkButton(control, reinterpret_cast<HDC>(wParam));
        return 0;
    }
    if (message == WM_ERASEBKGND)
        return TRUE;

    const LRESULT result = CallWindowProcA(original, control, message, wParam, lParam);
    if (message == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, control, 0};
        TrackMouseEvent(&tracking);
    }
    if (message == BM_SETCHECK || message == BM_SETSTATE || message == BM_SETSTYLE ||
        message == WM_SETTEXT || message == WM_ENABLE ||
        message == WM_MOUSEMOVE || message == WM_MOUSELEAVE ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_KEYDOWN || message == WM_KEYUP || message == WM_UPDATEUISTATE)
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    if (message == WM_NCDESTROY)
        RemovePropA(control, DARK_ORIGINAL_PROC_PROP);
    return result;
}

void SubclassDarkControl(HWND control, WNDPROC darkProc)
{
    if (GetPropA(control, DARK_ORIGINAL_PROC_PROP)) return;

    WNDPROC original = reinterpret_cast<WNDPROC>(GetWindowLongPtrA(control, GWLP_WNDPROC));
    if (!original) return;
    SetPropA(control, DARK_ORIGINAL_PROC_PROP, reinterpret_cast<HANDLE>(original));
    if (!SetWindowLongPtrA(control, GWLP_WNDPROC,
                           reinterpret_cast<LONG_PTR>(darkProc))) {
        const DWORD error = GetLastError();
        if (error != 0) {
            RemovePropA(control, DARK_ORIGINAL_PROC_PROP);
            return;
        }
    }
}

BOOL CALLBACK ApplyDarkControlTheme(HWND control, LPARAM setThemeParam)
{
    const auto setWindowTheme = reinterpret_cast<SetWindowThemeFn>(setThemeParam);
    char className[32] = {};
    GetClassNameA(control, className, sizeof(className));
    const bool dark = IsLaunchMenuDarkModeEnabled();
    if (dark)
        SetPropA(control, DARK_SCROLLBAR_PROP, reinterpret_cast<HANDLE>(1));
    else
        RemovePropA(control, DARK_SCROLLBAR_PROP);
    // Dropdown lists are popup windows, so EnumChildWindows misses them.
    if (_stricmp(className, "COMBOBOX") == 0) {
        COMBOBOXINFO info = {};
        info.cbSize = sizeof(info);
        if (GetComboBoxInfo(control, &info) && info.hwndList)
            ApplyDarkControlTheme(info.hwndList, setThemeParam);
    }
    if (!dark) {
        if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(control, FALSE);
        if (setWindowTheme) setWindowTheme(control, nullptr, nullptr);
        SendMessageW(control, WM_THEMECHANGED, 0, 0);
        if (_stricmp(className, WC_LISTVIEWA) == 0) {
            ListView_SetBkColor(control, GetSysColor(COLOR_WINDOW));
            ListView_SetTextBkColor(control, GetSysColor(COLOR_WINDOW));
            ListView_SetTextColor(control, GetSysColor(COLOR_WINDOWTEXT));
            ListView_SetExtendedListViewStyleEx(control, LVS_EX_GRIDLINES, LVS_EX_GRIDLINES);
        }
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        return TRUE;
    }
    if (_stricmp(className, "EDIT") == 0) {
        char parentClass[32] = {};
        GetClassNameA(GetParent(control), parentClass, sizeof(parentClass));
        // The combo paints its outer border; its editable text field needs none.
        if (_stricmp(parentClass, "COMBOBOX") == 0) {
            SetWindowLongPtrA(control, GWL_STYLE,
                              GetWindowLongPtrA(control, GWL_STYLE) & ~WS_BORDER);
            SetWindowLongPtrA(control, GWL_EXSTYLE,
                              GetWindowLongPtrA(control, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
            SetWindowPos(control, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
            return TRUE;
        }
    }
    if (setWindowTheme)
        setWindowTheme(control, L"Explorer", nullptr);
    if (g_allowDarkModeForWindow)
        g_allowDarkModeForWindow(control, TRUE);
    SendMessageW(control, WM_THEMECHANGED, 0, 0);

    if (_stricmp(className, WC_LISTVIEWA) == 0) {
        // Remove the native sunken edge so its light inner rim cannot show through.
        SetWindowLongPtrA(control, GWL_EXSTYLE,
                          GetWindowLongPtrA(control, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
        SetWindowLongPtrA(control, GWL_STYLE,
                          GetWindowLongPtrA(control, GWL_STYLE) | WS_BORDER);
        SetWindowPos(control, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
        ListView_SetBkColor(control, DARK_CONTROL);
        ListView_SetTextBkColor(control, DARK_CONTROL);
        ListView_SetTextColor(control, DARK_TEXT);
        ListView_SetExtendedListViewStyleEx(control, LVS_EX_GRIDLINES, 0);
    }

    if (_stricmp(className, "COMBOBOX") == 0)
        SubclassDarkControl(control, DarkComboBoxProc);
    else if (_stricmp(className, "BUTTON") == 0) {
        const DWORD type = static_cast<DWORD>(GetWindowLongPtrA(control, GWL_STYLE)) &
                           BS_TYPEMASK;
        SubclassDarkControl(control, type == BS_OWNERDRAW
            ? DarkBorderControlProc : DarkButtonProc);
    } else if (_stricmp(className, WC_HEADERA) == 0)
        SubclassDarkControl(control, DarkHeaderProc);
    else if (_stricmp(className, WC_LISTVIEWA) == 0)
        SubclassDarkControl(control, DarkListViewProc);
    else if (_stricmp(className, "EDIT") == 0 ||
              _stricmp(className, "LISTBOX") == 0)
        SubclassDarkControl(control, DarkBorderControlProc);

    RedrawWindow(control, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    return TRUE;
}

} // namespace

COLORREF LaunchMenuDarkBackgroundColor()
{
    return IsLaunchMenuDarkModeEnabled() ? DARK_BACKGROUND : GetSysColor(COLOR_BTNFACE);
}

COLORREF LaunchMenuDarkControlColor()
{
    return IsLaunchMenuDarkModeEnabled() ? DARK_CONTROL : GetSysColor(COLOR_WINDOW);
}

COLORREF LaunchMenuDarkTextColor()
{
    return IsLaunchMenuDarkModeEnabled() ? DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT);
}

HBRUSH LaunchMenuDarkBackgroundBrush()
{
    if (!IsLaunchMenuDarkModeEnabled()) return GetSysColorBrush(COLOR_BTNFACE);
    static HBRUSH brush = CreateSolidBrush(DARK_BACKGROUND);
    return brush;
}

void InitializeLaunchMenuDarkMode()
{
    if (!IsLaunchMenuDarkModeEnabled()) return;
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    g_uxTheme = LoadLibraryA("uxtheme.dll");
    if (!g_uxTheme) return;
    g_setWindowTheme = reinterpret_cast<SetWindowThemeFn>(
        GetProcAddress(g_uxTheme, "SetWindowTheme"));
    // Intercept the actual themed paint operation, including native scrolling
    // loops, rather than painting over a light scrollbar afterward. Use the
    // documented entry points on Wine/older Windows too.
    const MH_STATUS status = MH_Initialize();
    if (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED) {
        InstallScrollbarThemeHook("DrawThemeBackground",
            reinterpret_cast<void*>(DrawLaunchThemeBackground),
            reinterpret_cast<void**>(&g_drawThemeBackground));
        InstallScrollbarThemeHook("DrawThemeBackgroundEx",
            reinterpret_cast<void*>(DrawLaunchThemeBackgroundEx),
            reinterpret_cast<void**>(&g_drawThemeBackgroundEx));
        InstallScrollbarThemeHook("CloseThemeData",
            reinterpret_cast<void*>(CloseLaunchThemeData),
            reinterpret_cast<void**>(&g_closeThemeData));
        // Do not retain handles if their lifetime cannot be tracked.
        if (g_closeThemeData) {
            InstallScrollbarThemeHook("OpenThemeData",
                reinterpret_cast<void*>(OpenLaunchThemeData),
                reinterpret_cast<void**>(&g_openThemeData));
            InstallScrollbarThemeHook("OpenThemeDataForDpi",
                reinterpret_cast<void*>(OpenLaunchThemeDataForDpi),
                reinterpret_cast<void**>(&g_openThemeDataForDpi));
        }
    } else {
        HookLog("LaunchMenu: Scrollbar hook initialization failed (%d)\n", status);
    }

    OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
    const auto rtlGetVersion = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;
    if (!rtlGetVersion || rtlGetVersion(&version) != 0 ||
        version.dwMajorVersion < 10 || version.dwBuildNumber < 17763)
        return;

    // These dark-mode exports are undocumented, so resolve them only on the
    // Windows builds where their ordinals and signatures are known.
    g_allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
        GetProcAddress(g_uxTheme, MAKEINTRESOURCEA(133)));

    // OpenNcThemeData is ordinal 49 on the Windows builds checked above.
    // Only marked launch-menu controls are redirected; other game windows
    // continue to use their original theme.
    if (g_closeThemeData)
        InstallScrollbarThemeHook(MAKEINTRESOURCEA(49),
            reinterpret_cast<void*>(OpenLaunchNcThemeData),
            reinterpret_cast<void**>(&g_openNcThemeData));

    if (version.dwBuildNumber >= 18362) {
        g_setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(g_uxTheme, MAKEINTRESOURCEA(135)));
        if (g_setPreferredAppMode)
            g_setPreferredAppMode(2); // ForceDark
    } else {
        g_allowDarkModeForApp = reinterpret_cast<AllowDarkModeForAppFn>(
            GetProcAddress(g_uxTheme, MAKEINTRESOURCEA(135)));
        if (g_allowDarkModeForApp)
            g_allowDarkModeForApp(TRUE);
    }

    const auto refreshPolicy =
        reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(
            GetProcAddress(g_uxTheme, MAKEINTRESOURCEA(104)));
    if (refreshPolicy)
        refreshPolicy();
}

void ApplyLaunchMenuDarkTheme(HWND window)
{
    if (!window) return;
    const bool dark = IsLaunchMenuDarkModeEnabled();
    InitializeLaunchMenuDarkMode();
    if (g_setPreferredAppMode) g_setPreferredAppMode(dark ? 2 : 3);
    if (g_allowDarkModeForApp) g_allowDarkModeForApp(dark);
    using DwmSetWindowAttributeFn = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

    HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
    if (dwmapi) {
        const auto setWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
            GetProcAddress(dwmapi, "DwmSetWindowAttribute"));
        if (setWindowAttribute) {
            const BOOL enabled = dark;
            if (FAILED(setWindowAttribute(window, 20, &enabled, sizeof(enabled))))
                setWindowAttribute(window, 19, &enabled, sizeof(enabled));
        }
        FreeLibrary(dwmapi);
    }

    InitializeLaunchMenuDarkMode();
    if (g_allowDarkModeForWindow)
        g_allowDarkModeForWindow(window, dark);
    SendMessageW(window, WM_THEMECHANGED, 0, 0);
    EnumChildWindows(window, ApplyDarkControlTheme,
                     reinterpret_cast<LPARAM>(g_setWindowTheme));
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED);

    RedrawWindow(window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void PaintLaunchMenuDarkBackground(HWND window, HDC dc)
{
    RECT client = {};
    GetClientRect(window, &client);
    FillRect(dc, &client, LaunchMenuDarkBackgroundBrush());
}

INT_PTR HandleLaunchMenuDarkControlColor(UINT message, WPARAM wParam)
{
    HDC dc = reinterpret_cast<HDC>(wParam);
    if (!IsLaunchMenuDarkModeEnabled()) {
        const bool input = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc, GetSysColor(input ? COLOR_WINDOW : COLOR_BTNFACE));
        SetBkMode(dc, OPAQUE);
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(input ? COLOR_WINDOW : COLOR_BTNFACE));
    }
    SetTextColor(dc, DARK_TEXT);

    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        SetBkColor(dc, DARK_CONTROL);
        SetBkMode(dc, OPAQUE);
        return reinterpret_cast<INT_PTR>(DarkControlBrush());
    }

    SetBkColor(dc, DARK_BACKGROUND);
    SetBkMode(dc, TRANSPARENT);
    return reinterpret_cast<INT_PTR>(LaunchMenuDarkBackgroundBrush());
}
