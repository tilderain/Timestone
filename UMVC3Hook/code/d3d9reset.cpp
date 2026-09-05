#include "d3d9reset.h"

#include "mk10utils.h"

#include <Windows.h>

extern void HookLog(const char* fmt, ...);

namespace {

volatile LONG g_graphicsSyncCalls = 0;
volatile LONG g_appResetCalls = 0;
volatile LONG g_forwardedAppResetCalls = 0;
volatile LONG g_deviceResetCalls = 0;
volatile LONG g_watchdogStarted = 0;
volatile LONG g_startupComplete = 0;
ULONGLONG g_watchdogStartTick = 0;

LONG ReadCounter(volatile LONG* counter)
{
    return InterlockedCompareExchange(counter, 0, 0);
}

DWORD WINAPI StartupWatchdogThread(LPVOID)
{
    unsigned elapsedSeconds = 0;
    while (InterlockedCompareExchange(&g_startupComplete, 0, 0) == 0) {
        Sleep(1000);
        ++elapsedSeconds;
        if (elapsedSeconds % 5 != 0 ||
            InterlockedCompareExchange(&g_startupComplete, 0, 0) != 0) {
            continue;
        }

        const uintptr_t app = *(uintptr_t*)_addr(0x140E17758);
        const LONG jobType = app ? *(volatile LONG*)(app + 0x2AC4) : -1;
        const LONG resetResult = app ? *(volatile LONG*)(app + 0x2AF0) : -1;
        const LONG retryAck = app ? *(volatile LONG*)(app + 0x2AF4) : -1;
        HookLog("D3D9: startup watchdog elapsed=%us app=%p "
            "jobType=%ld resetResult=0x%08lX retryAck=%ld "
            "syncCalls=%ld appResetCalls=%ld forwardedAppResets=%ld "
            "deviceResetCalls=%ld\n",
            elapsedSeconds, (void*)app, jobType,
            (unsigned long)resetResult, retryAck,
            ReadCounter(&g_graphicsSyncCalls), ReadCounter(&g_appResetCalls),
            ReadCounter(&g_forwardedAppResetCalls),
            ReadCounter(&g_deviceResetCalls));
    }
    return 0;
}

} // namespace

void D3D9ResetDiagnostics_RecordGraphicsSync()
{
    InterlockedIncrement(&g_graphicsSyncCalls);
}

void D3D9ResetDiagnostics_BeginAppReset(bool forwarded)
{
    InterlockedIncrement(&g_appResetCalls);
    if (forwarded)
        InterlockedIncrement(&g_forwardedAppResetCalls);
}

void D3D9ResetDiagnostics_EndAppReset(uint64_t result, uint64_t elapsedMs)
{
    HookLog("D3D9: app Reset submission returned result=%llu elapsed=%llums\n",
        (unsigned long long)result, (unsigned long long)elapsedMs);
}

void D3D9ResetDiagnostics_RecordDeviceReset()
{
    InterlockedIncrement(&g_deviceResetCalls);
}

void D3D9ResetDiagnostics_StartStartupWatchdog()
{
    if (InterlockedCompareExchange(&g_watchdogStarted, 1, 0) != 0)
        return;

    g_watchdogStartTick = GetTickCount64();
    HANDLE thread = CreateThread(nullptr, 0, StartupWatchdogThread,
        nullptr, 0, nullptr);
    if (!thread) {
        HookLog("D3D9: failed to start startup watchdog error=%lu\n",
            GetLastError());
        return;
    }
    CloseHandle(thread);
}

void D3D9ResetDiagnostics_CompleteStartup()
{
    InterlockedExchange(&g_startupComplete, 1);
    HookLog("D3D9: startup watchdog complete elapsed=%llums "
        "syncCalls=%ld appResetCalls=%ld forwardedAppResets=%ld "
        "deviceResetCalls=%ld\n",
        (unsigned long long)(GetTickCount64() - g_watchdogStartTick),
        ReadCounter(&g_graphicsSyncCalls), ReadCounter(&g_appResetCalls),
        ReadCounter(&g_forwardedAppResetCalls),
        ReadCounter(&g_deviceResetCalls));
}
