#include "launchmenu_utils.h"

#include <commctrl.h>
#include <climits>

extern void HookLog(const char* fmt, ...);

namespace LaunchMenuUtils {

void ScaleChildren(HWND window, UINT oldDpi, UINT newDpi, bool scaleContents)
{
    if (!window || !oldDpi || !newDpi || oldDpi > INT_MAX || newDpi > INT_MAX) {
        HookLog("UMVC3Hook: DPI scaling skipped window=%p dpi=%u->%u\n",
                window, oldDpi, newDpi);
        return;
    }
    if (oldDpi == newDpi) return;

    HookLog("UMVC3Hook: DPI scaling window=%p dpi=%u->%u contents=%d\n",
            window, oldDpi, newDpi, scaleContents);
    bool loggedBounds = false;
    for (HWND child = GetWindow(window, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        RECT screen = {};
        if (!GetWindowRect(child, &screen)) {
            HookLog("UMVC3Hook: DPI GetWindowRect failed control=%d error=%lu\n",
                    GetDlgCtrlID(child), GetLastError());
            continue;
        }

        // Use actual POINT objects, not a RECT accessed through a POINT pointer.
        // Map both corners together to retain Win32's mirrored-window handling.
        POINT corners[2] = {{screen.left, screen.top}, {screen.right, screen.bottom}};
        SetLastError(ERROR_SUCCESS);
        const int mapped = MapWindowPoints(nullptr, window, corners, 2);
        const DWORD mapError = GetLastError();
        // A zero return also means a successful mapping with no translation.
        if (!mapped && mapError != ERROR_SUCCESS) {
            HookLog("UMVC3Hook: DPI MapWindowPoints failed control=%d error=%lu\n",
                    GetDlgCtrlID(child), mapError);
            continue;
        }

        const int x = MulDiv(corners[0].x, newDpi, oldDpi);
        const int y = MulDiv(corners[0].y, newDpi, oldDpi);
        const int width = MulDiv(corners[1].x - corners[0].x, newDpi, oldDpi);
        const int height = MulDiv(corners[1].y - corners[0].y, newDpi, oldDpi);
        if (width <= 0 || height <= 0) {
            HookLog("UMVC3Hook: DPI invalid bounds control=%d size=%dx%d; unchanged\n",
                    GetDlgCtrlID(child), width, height);
            continue;
        }
        if (!loggedBounds) {
            HookLog("UMVC3Hook: DPI control=%d position=(%ld,%ld)->(%d,%d) size=%dx%d\n",
                    GetDlgCtrlID(child), corners[0].x, corners[0].y, x, y, width, height);
            loggedBounds = true;
        }
        if (!SetWindowPos(child, nullptr, x, y, width, height,
                          SWP_NOACTIVATE | SWP_NOZORDER)) {
            HookLog("UMVC3Hook: DPI SetWindowPos failed control=%d error=%lu\n",
                    GetDlgCtrlID(child), GetLastError());
            continue;
        }

        if (!scaleContents) continue;
        char className[32] = {};
        GetClassNameA(child, className, sizeof(className));
        if (_stricmp(className, WC_LISTVIEWA) == 0) {
            HWND header = ListView_GetHeader(child);
            const int columnCount = header ? Header_GetItemCount(header) : 0;
            for (int column = 0; column < columnCount; ++column) {
                ListView_SetColumnWidth(child, column, MulDiv(
                    ListView_GetColumnWidth(child, column), newDpi, oldDpi));
            }
        } else if (_stricmp(className, "COMBOBOX") == 0) {
            const LRESULT width = SendMessageA(child, CB_GETDROPPEDWIDTH, 0, 0);
            if (width > 0) {
                SendMessageA(child, CB_SETDROPPEDWIDTH,
                             MulDiv(static_cast<int>(width), newDpi, oldDpi), 0);
            }
        }
    }
}

} // namespace LaunchMenuUtils
