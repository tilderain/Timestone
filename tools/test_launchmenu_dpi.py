"""Run the production DPI scaler against mock Win32 calls (no game/Wine needed)."""

from pathlib import Path
import subprocess
import tempfile
import unittest


MOCK_WIN32 = r'''
#include <cassert>
#include <climits>
#include <cmath>
#include <cstring>
#include <strings.h>
#include <cstdint>
using UINT = unsigned;
using DWORD = unsigned long;
using LRESULT = intptr_t;
struct RECT { long left, top, right, bottom; };
struct POINT { long x, y; };
struct Window {
    RECT rect;
    const char* name;
    int columns[3] = {100, 200, 50};
    int dropdown = 180;
    int moves = 0;
    bool rectOk = true, mapOk = true, moveOk = true;
    Window* next = nullptr;
};
using HWND = Window*;
constexpr int GW_CHILD = 1, GW_HWNDNEXT = 2;
constexpr UINT SWP_NOACTIVATE = 1, SWP_NOZORDER = 2;
constexpr UINT CB_GETDROPPEDWIDTH = 3, CB_SETDROPPEDWIDTH = 4;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr const char* WC_LISTVIEWA = "SysListView32";
HWND first = nullptr, mappingChild = nullptr;
long originX = 300, originY = 200;
DWORD lastError = 0;
void HookLog(const char*, ...) {}
void SetLastError(DWORD error) { lastError = error; }
DWORD GetLastError() { return lastError; }
HWND GetWindow(HWND w, int which) { return which == GW_CHILD ? first : w->next; }
int GetDlgCtrlID(HWND) { return 100; }
bool GetWindowRect(HWND w, RECT* rect) {
    mappingChild = w;
    if (!w->rectOk) { lastError = 5; return false; }
    *rect = w->rect;
    return true;
}
int MapWindowPoints(HWND from, HWND, POINT* points, UINT count) {
    assert(!from && count == 2);
    if (!mappingChild->mapOk) { lastError = 5; return 0; }
    for (UINT i = 0; i < count; ++i) {
        points[i].x -= originX;
        points[i].y -= originY;
    }
    return originX || originY ? 1 : 0;
}
int MulDiv(int value, int numerator, int denominator) {
    assert(denominator);
    return static_cast<int>(std::round(static_cast<double>(value) * numerator / denominator));
}
bool SetWindowPos(HWND w, HWND, int x, int y, int width, int height, UINT flags) {
    assert(flags == (SWP_NOACTIVATE | SWP_NOZORDER));
    if (!w->moveOk) { lastError = 5; return false; }
    ++w->moves;
    w->rect = {originX + x, originY + y, originX + x + width, originY + y + height};
    return true;
}
int GetClassNameA(HWND w, char* name, int) { std::strcpy(name, w->name); return 1; }
int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
HWND ListView_GetHeader(HWND w) { return w; }
int Header_GetItemCount(HWND) { return 3; }
int ListView_GetColumnWidth(HWND w, int i) { return w->columns[i]; }
void ListView_SetColumnWidth(HWND w, int i, int width) { w->columns[i] = width; }
LRESULT SendMessageA(HWND w, UINT msg, intptr_t value, intptr_t) {
    if (msg == CB_SETDROPPEDWIDTH) w->dropdown = value;
    return w->dropdown;
}
'''

CASES = r'''
int main() {
    Window parent = {{0, 0, 0, 0}, "parent"};
    for (UINT dpi : {120u, 144u, 192u}) {
        Window combo = {{320, 230, 520, 250}, "COMBOBOX"};
        Window list = {{280, 260, 580, 460}, WC_LISTVIEWA};
        combo.next = &list;
        first = &combo;
        LaunchMenuUtils::ScaleChildren(&parent, 96, dpi, true);
        assert(combo.rect.left == 300 + MulDiv(20, dpi, 96));
        assert(combo.rect.top == 200 + MulDiv(30, dpi, 96));
        assert(combo.rect.right - combo.rect.left == MulDiv(200, dpi, 96));
        assert(combo.rect.bottom - combo.rect.top == MulDiv(20, dpi, 96));
        assert(combo.dropdown == MulDiv(180, dpi, 96));
        assert(list.rect.left == 300 + MulDiv(-20, dpi, 96));
        assert(list.columns[1] == MulDiv(200, dpi, 96));
        LaunchMenuUtils::ScaleChildren(&parent, dpi, 96, true);
        assert(combo.rect.left == 320 && combo.rect.top == 230);
        assert(combo.rect.right == 520 && combo.rect.bottom == 250);
        assert(combo.dropdown == 180 && list.columns[1] == 200);
    }
    Window combo = {{320, 230, 520, 250}, "COMBOBOX"};
    first = &combo;
    LaunchMenuUtils::ScaleChildren(&parent, 96, 96, true);
    LaunchMenuUtils::ScaleChildren(&parent, 0, 144, true);
    LaunchMenuUtils::ScaleChildren(&parent, 96, 0, true);
    LaunchMenuUtils::ScaleChildren(&parent, 96, UINT_MAX, true);
    LaunchMenuUtils::ScaleChildren(nullptr, 96, 144, true);
    assert(combo.moves == 0);
    combo.rectOk = false;
    LaunchMenuUtils::ScaleChildren(&parent, 96, 144, true);
    assert(combo.moves == 0 && combo.dropdown == 180);
    combo.rectOk = true;
    combo.mapOk = false;
    LaunchMenuUtils::ScaleChildren(&parent, 96, 144, true);
    assert(combo.moves == 0 && combo.rect.left == 320);
    combo.mapOk = true;
    combo.moveOk = false;
    LaunchMenuUtils::ScaleChildren(&parent, 96, 144, true);
    assert(combo.moves == 0 && combo.dropdown == 180);
    combo.moveOk = true;
    LaunchMenuUtils::ScaleChildren(&parent, 96, 144, false);
    assert(combo.moves == 1 && combo.dropdown == 180); // Settings: bounds only.
    originX = originY = 0;
    combo.rect = {20, 30, 220, 50};
    lastError = 5; // Must clear stale errors before a successful zero-offset mapping.
    LaunchMenuUtils::ScaleChildren(&parent, 96, 144, true);
    assert(combo.moves == 2 && combo.rect.left == 30 && combo.rect.top == 45);
    combo.rect = {0, 0, 0, 0};
    LaunchMenuUtils::ScaleChildren(&parent, 96, 144, true);
    assert(combo.moves == 2); // Invalid bounds must not collapse a control.
}
'''


class DpiScalingTest(unittest.TestCase):
    def test_production_scaler_with_mock_windows(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/launchmenu_utils.cpp").read_text()
        # Substitute only platform headers, leaving the implementation unchanged.
        source = "\n".join(line for line in source.splitlines()
                           if not line.startswith("#include"))
        with tempfile.TemporaryDirectory(prefix="dpi-test-", dir="/tmp/opencode") as temp:
            executable = str(Path(temp) / "test_dpi")
            subprocess.run(
                ["g++", "-std=c++20", "-O3", "-flto=auto", "-fipa-pta",
                 "-include", "initializer_list", "-x", "c++", "-", "-o", executable],
                input=MOCK_WIN32 + source + CASES, text=True, check=True,
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
