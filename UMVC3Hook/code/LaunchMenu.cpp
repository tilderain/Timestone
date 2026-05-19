#include "LaunchMenu.h"
#include <Windows.h>

static LaunchOption g_launchOption = LAUNCH_NORMAL;

LaunchOption ShowLaunchMenu()
{
    HINSTANCE hInst = GetModuleHandle(NULL);

    const char CLASS_NAME[] = "UMVC3LaunchMenu";

    WNDCLASS wc = {};
    wc.lpfnWndProc = [](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        switch (msg) {
        case WM_CREATE:
        {
            CreateWindowA("STATIC", "UMVC3Hook - Launch Mode",
                WS_VISIBLE | WS_CHILD, 20, 15, 260, 20, hWnd, NULL, NULL, NULL);

            CreateWindowA("BUTTON", "Launch Normally",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                20, 55, 260, 35, hWnd, (HMENU)1001, NULL, NULL);

            CreateWindowA("BUTTON", "Launch into Training Mode",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                20, 100, 260, 35, hWnd, (HMENU)1002, NULL, NULL);

            CreateWindowA("BUTTON", "Training Mode + Spawn 2nd Instance",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                20, 145, 260, 35, hWnd, (HMENU)1003, NULL, NULL);

            CreateWindowA("STATIC", "Choose how to launch the game:",
                WS_VISIBLE | WS_CHILD, 20, 225, 260, 15, hWnd, NULL, NULL, NULL);
            CreateWindowA("STATIC", "Spawned instance will auto-enter training.",
                WS_VISIBLE | WS_CHILD, 20, 240, 260, 15, hWnd, NULL, NULL, NULL);

            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case 1001:
                g_launchOption = LAUNCH_NORMAL;
                DestroyWindow(hWnd);
                PostQuitMessage(0);
                return 0;
            case 1002:
                g_launchOption = LAUNCH_TRAINING;
                DestroyWindow(hWnd);
                PostQuitMessage(0);
                return 0;
            case 1003:
                g_launchOption = LAUNCH_DUAL_TRAINING;
                DestroyWindow(hWnd);
                PostQuitMessage(0);
                return 0;
            }
            break;
        case WM_CLOSE:
            g_launchOption = LAUNCH_NORMAL;
            DestroyWindow(hWnd);
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    };
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    int w = 310, h = 280;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hWnd = CreateWindowExA(
        0, CLASS_NAME, "UMVC3Hook - Launch Mode",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, w, h,
        NULL, NULL, hInst, NULL
    );

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterClassA(CLASS_NAME, hInst);

    return g_launchOption;
}

LaunchOption GetLaunchOption()
{
    return g_launchOption;
}

void SetLaunchOption(LaunchOption opt)
{
    g_launchOption = opt;
}
