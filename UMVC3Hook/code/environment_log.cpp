// See environment_log.h.
#include "environment_log.h"

#include <cpuid.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>

extern void HookLog(const char* fmt, ...);

namespace {

void LogProcessor()
{
    char brand[49] = {};
    unsigned regs[4] = {};
    if (__get_cpuid(0x80000000u, &regs[0], &regs[1], &regs[2], &regs[3]) &&
        regs[0] >= 0x80000004u) {
        for (unsigned leaf = 0; leaf < 3; ++leaf) {
            __get_cpuid(0x80000002u + leaf, &regs[0], &regs[1], &regs[2], &regs[3]);
            memcpy(brand + leaf * 16, regs, 16);
        }
    }
    const char* name = brand;
    while (*name == ' ') ++name;
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);
    HookLog("Environment: processor \"%s\" logical cores=%lu memory=%llu MB\n",
            *name ? name : "unknown", GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),
            static_cast<unsigned long long>(memory.ullTotalPhys >> 20));
}

void LogWindows()
{
    // RtlGetVersion: GetVersionEx answers with the manifest's version.
    RTL_OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    const HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    using GetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    auto getVersion = reinterpret_cast<GetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (getVersion) getVersion(&version);
    using WineVersionFn = const char*(__cdecl*)();
    auto wineVersion = reinterpret_cast<WineVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
    HookLog("Environment: Windows %lu.%lu build %lu%s%s\n", version.dwMajorVersion,
            version.dwMinorVersion, version.dwBuildNumber, wineVersion ? " under Wine " : "",
            wineVersion ? wineVersion() : "");
}

void LogPower()
{
    SYSTEM_POWER_STATUS status = {};
    if (!GetSystemPowerStatus(&status)) return;
    const char* source = status.ACLineStatus == 1 ? "mains"
        : status.ACLineStatus == 0 ? "BATTERY" : "unknown";
    const bool hasBattery = !(status.BatteryFlag & 128) && status.BatteryFlag != 255;

    // The plan by its well-known GUID; a vendor's own plan is printed raw.
    char plan[64] = "unknown";
    if (const HMODULE powrprof = LoadLibraryA("powrprof.dll")) {
        using GetSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
        auto getScheme = reinterpret_cast<GetSchemeFn>(
            reinterpret_cast<void*>(GetProcAddress(powrprof, "PowerGetActiveScheme")));
        GUID* scheme = nullptr;
        if (getScheme && getScheme(nullptr, &scheme) == ERROR_SUCCESS && scheme) {
            switch (scheme->Data1) {
            case 0xA1841308u: strcpy(plan, "POWER SAVER"); break;
            case 0x381B4222u: strcpy(plan, "balanced"); break;
            case 0x8C5E7FDAu: strcpy(plan, "high performance"); break;
            case 0xE9A42B02u: strcpy(plan, "ultimate performance"); break;
            default:
                snprintf(plan, sizeof(plan), "%08lX-%04X-%04X", scheme->Data1, scheme->Data2,
                         scheme->Data3);
            }
            LocalFree(scheme);
        }
        FreeLibrary(powrprof);
    }
    // SystemStatusFlag, which older headers still call Reserved1.
    const bool saver = reinterpret_cast<const BYTE*>(&status)[3] == 1;
    char battery[32] = "none";
    if (hasBattery) {
        if (status.BatteryLifePercent <= 100)
            snprintf(battery, sizeof(battery), "%u%%", status.BatteryLifePercent);
        else
            strcpy(battery, "present");
    }
    HookLog("Environment: power source=%s battery=%s battery saver=%s plan=%s\n", source,
            battery, saver ? "ON" : "off", plan);
}

bool StartsWithNoCase(const char* text, const char* prefix, size_t length)
{
    return length && _strnicmp(text, prefix, length) == 0 &&
           (text[length] == '\\' || text[length] == '/');
}

} // namespace

void EnvironmentLog_Boot()
{
    LogProcessor();
    LogWindows();
    LogPower();
}

void EnvironmentLog_Modules()
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static char last[2048];
    struct Guard {
        SRWLOCK& held;
        explicit Guard(SRWLOCK& l) : held(l) { AcquireSRWLockExclusive(&held); }
        ~Guard() { ReleaseSRWLockExclusive(&held); }
    } guard(lock);

    char windows[MAX_PATH] = {}, game[MAX_PATH] = {};
    size_t windowsLength = GetWindowsDirectoryA(windows, sizeof(windows));
    if (windowsLength >= sizeof(windows)) windowsLength = 0;
    size_t gameLength = 0;
    if (GetModuleFileNameA(nullptr, game, sizeof(game))) {
        char* slash = strrchr(game, '\\');
        if (slash) { *slash = '\0'; gameLength = static_cast<size_t>(slash - game); }
    }

    static HMODULE modules[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return;
    DWORD count = bytes / sizeof(HMODULE);
    if (count > 1024) count = 1024;

    // Windows' own DLLs are left out, and so is what ships in the game's
    // folder root with the game. What remains is short: this mod, other
    // mods beside it, and whatever injected itself from elsewhere.
    static const char* const kGameOwn[] = {
        "steam_api64.dll",
    };
    char text[sizeof(last)] = {};
    size_t used = 0;
    unsigned listed = 0, dropped = 0;
    for (DWORD i = 1; i < count; ++i) { // 0 is the game's exe
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(modules[i], path, sizeof(path))) continue;
        if (StartsWithNoCase(path, windows, windowsLength)) continue;
        const char* slash = strrchr(path, '\\');
        const char* name = slash ? slash + 1 : path;
        const bool inGameFolder = StartsWithNoCase(path, game, gameLength);
        bool own = false;
        if (inGameFolder && name == path + gameLength + 1)
            for (const char* known : kGameOwn) own |= _stricmp(name, known) == 0;
        if (own) continue;
        // In the game's folder the name says enough; anything from
        // elsewhere keeps its folder, which is what identifies it.
        const char* shown = inGameFolder ? path + gameLength + 1 : path;
        const size_t need = strlen(shown) + 2;
        if (used + need >= sizeof(text)) { ++dropped; continue; }
        if (used) { text[used++] = ','; text[used++] = ' '; }
        memcpy(text + used, shown, need - 2);
        used += need - 2;
        ++listed;
    }
    text[used] = '\0';
    if (strcmp(text, last) == 0) return;
    memcpy(last, text, sizeof(last));
    HookLog("Environment: %u DLLs from outside Windows and the game (of %lu loaded)%s: %s\n",
            listed + dropped, count, dropped ? " [list cut short]" : "", used ? text : "none");
}

void EnvironmentLog_Display(HWND window, unsigned width, unsigned height, bool windowed,
                            bool vsync)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static char last[192];
    struct Guard {
        SRWLOCK& held;
        explicit Guard(SRWLOCK& l) : held(l) { AcquireSRWLockExclusive(&held); }
        ~Guard() { ReleaseSRWLockExclusive(&held); }
    } guard(lock);

    const char* mode = "fullscreen";
    if (windowed) {
        const LONG_PTR style = window ? GetWindowLongPtrA(window, GWL_STYLE) : 0;
        mode = window && !(style & WS_CAPTION) ? "borderless" : "windowed";
    }
    char monitorText[96] = "unknown";
    MONITORINFOEXA info = {};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
    DEVMODEA current = {};
    current.dmSize = sizeof(current);
    if (monitor && GetMonitorInfoA(monitor, &info) &&
        EnumDisplaySettingsA(info.szDevice, ENUM_CURRENT_SETTINGS, &current))
        snprintf(monitorText, sizeof(monitorText), "%lux%lu at %lu Hz%s", current.dmPelsWidth,
                 current.dmPelsHeight, current.dmDisplayFrequency,
                 info.dwFlags & MONITORINFOF_PRIMARY ? "" : " (not the primary)");

    char text[sizeof(last)];
    snprintf(text, sizeof(text), "back buffer %ux%u %s vsync=%d, monitor %s", width, height,
             mode, vsync ? 1 : 0, monitorText);
    if (strcmp(text, last) == 0) return;
    memcpy(last, text, sizeof(last));
    HookLog("Environment: display %s\n", text);
}

namespace {

DWORD WINAPI ControllersThread(void*)
{
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || !count) {
        HookLog("Input: no input devices could be listed\n");
        return 0;
    }
    static RAWINPUTDEVICELIST devices[128];
    if (count > 128) count = 128;
    count = GetRawInputDeviceList(devices, &count, sizeof(RAWINPUTDEVICELIST));
    if (count == static_cast<UINT>(-1)) return 0;

    using GetProductFn = BOOLEAN(__stdcall*)(HANDLE, void*, ULONG);
    const HMODULE hid = LoadLibraryA("hid.dll");
    auto getProduct = hid ? reinterpret_cast<GetProductFn>(reinterpret_cast<void*>(
        GetProcAddress(hid, "HidD_GetProductString"))) : nullptr;

    unsigned found = 0;
    for (UINT i = 0; i < count; ++i) {
        if (devices[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICEINFO, &info, &size) ==
            static_cast<UINT>(-1)) continue;
        // Generic desktop page: joystick (4), gamepad (5), multi-axis (8).
        if (info.hid.usUsagePage != 1 ||
            (info.hid.usUsage != 4 && info.hid.usUsage != 5 && info.hid.usUsage != 8)) continue;
        char path[512] = {};
        size = sizeof(path);
        if (GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, path, &size) ==
            static_cast<UINT>(-1)) path[0] = '\0';
        char name[128] = "unnamed";
        if (getProduct && path[0]) {
            const HANDLE file = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                wchar_t product[127] = {};
                if (getProduct(file, product, sizeof(product)) && product[0])
                    WideCharToMultiByte(CP_UTF8, 0, product, -1, name, sizeof(name), nullptr,
                                        nullptr);
                name[sizeof(name) - 1] = '\0';
                CloseHandle(file);
            }
        }
        // Windows marks the HID side of an XInput controller with "IG_".
        const bool xinput = strstr(path, "IG_") || strstr(path, "ig_");
        HookLog("Input: controller \"%s\" %04lX:%04lX %s\n", name, info.hid.dwVendorId,
                info.hid.dwProductId, xinput ? "XInput" : "DirectInput only");
        ++found;
    }
    if (!found) HookLog("Input: no game controller is plugged in (keyboard only)\n");
    if (hid) FreeLibrary(hid);
    return 0;
}

LONG g_controllersBusy = 0;

DWORD WINAPI ControllersThreadEntry(void* argument)
{
    ControllersThread(argument);
    InterlockedExchange(&g_controllersBusy, 0);
    return 0;
}

} // namespace

void EnvironmentLog_Controllers()
{
    if (InterlockedExchange(&g_controllersBusy, 1)) return; // the last one is still asking
    const HANDLE thread = CreateThread(nullptr, 0, ControllersThreadEntry, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else InterlockedExchange(&g_controllersBusy, 0);
}
