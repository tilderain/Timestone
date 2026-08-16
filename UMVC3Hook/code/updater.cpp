#include "updater.h"
#include "umvc3.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

static void LogUpdater(FILE* log, const char* format, ...)
{
    if (!log)
        return;
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    std::fprintf(log, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfprintf(log, format, args);
    va_end(args);
    std::fputc('\n', log);
    std::fflush(log);
}

void Updater_Init()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&Updater_Init), &module))
        return;

    char asiPath[MAX_PATH] = {};
    if (!GetModuleFileNameA(module, asiPath, MAX_PATH))
        return;
    char updaterPath[MAX_PATH] = {};
    std::snprintf(updaterPath, MAX_PATH, "%s", asiPath);
    char* slash = strrchr(updaterPath, '\\');
    if (!slash)
        slash = strrchr(updaterPath, '/');
    if (slash)
        std::snprintf(slash + 1, MAX_PATH - (slash + 1 - updaterPath),
            "HeroNetUpdater.exe");
    else
        std::snprintf(updaterPath, MAX_PATH, "HeroNetUpdater.exe");

    char logPath[MAX_PATH] = {};
    std::snprintf(logPath, MAX_PATH, "%s", updaterPath);
    char* logName = strrchr(logPath, '\\');
    if (!logName)
        logName = strrchr(logPath, '/');
    std::snprintf(logName ? logName + 1 : logPath,
        logName ? MAX_PATH - (logName + 1 - logPath) : MAX_PATH,
        "HeroNetUpdater.log");
    FILE* log = std::fopen(logPath, "a");

    char pendingUpdaterPath[MAX_PATH + 8] = {};
    char oldUpdaterPath[MAX_PATH + 8] = {};
    std::snprintf(pendingUpdaterPath, sizeof(pendingUpdaterPath),
        "%s.new", updaterPath);
    std::snprintf(oldUpdaterPath, sizeof(oldUpdaterPath),
        "%s.old", updaterPath);
    DWORD pendingAttributes = GetFileAttributesA(pendingUpdaterPath);
    DWORD pendingError = pendingAttributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError() : ERROR_SUCCESS;
    LogUpdater(log, "Updater_Init: asi=\"%s\" updater=\"%s\"", asiPath,
        updaterPath);
    LogUpdater(log, "Pending: path=\"%s\" attributes=0x%08lx error=%lu",
        pendingUpdaterPath, pendingAttributes, pendingError);
    if (pendingAttributes != INVALID_FILE_ATTRIBUTES) {
        bool installed = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            DWORD updaterAttributes = GetFileAttributesA(updaterPath);
            if (updaterAttributes != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileA(updaterPath, oldUpdaterPath, FALSE)) {
                    DWORD error = GetLastError();
                    LogUpdater(log,
                        "Attempt %d: backing up updater failed, error=%lu",
                        attempt + 1, error);
                    Sleep(100);
                    continue;
                }
                LogUpdater(log, "Attempt %d: backed up updater to .old",
                    attempt + 1);
            }
            if (CopyFileA(pendingUpdaterPath, updaterPath, FALSE)) {
                installed = true;
                BOOL deleted = DeleteFileA(pendingUpdaterPath);
                DWORD deleteError = deleted ? ERROR_SUCCESS : GetLastError();
                LogUpdater(log,
                    "Attempt %d: installed pending updater; .new deletion=%d error=%lu",
                    attempt + 1, deleted, deleteError);
                break;
            }
            DWORD installError = GetLastError();
            LogUpdater(log,
                "Attempt %d: copying .new over updater failed, error=%lu",
                attempt + 1, installError);
            if (GetFileAttributesA(oldUpdaterPath) != INVALID_FILE_ATTRIBUTES) {
                BOOL restored = CopyFileA(oldUpdaterPath, updaterPath, FALSE);
                DWORD restoreError = restored ? ERROR_SUCCESS : GetLastError();
                LogUpdater(log,
                    "Attempt %d: rollback result=%d error=%lu",
                    attempt + 1, restored, restoreError);
            }
            Sleep(100);
        }
        LogUpdater(log, "Pending updater installation result=%s",
            installed ? "success" : "failure");
    } else if (GetFileAttributesA(oldUpdaterPath) != INVALID_FILE_ATTRIBUTES) {
        BOOL deleted = DeleteFileA(oldUpdaterPath);
        DWORD error = deleted ? ERROR_SUCCESS : GetLastError();
        LogUpdater(log, "Old updater cleanup result=%d error=%lu", deleted,
            error);
    }
    if (GetFileAttributesA(updaterPath) == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        LogUpdater(log, "Updater executable is unavailable, error=%lu", error);
        MessageBoxA(nullptr,
            "HeroNetUpdater.exe is missing. Automatic update checks are disabled.\n\n"
            "Place HeroNetUpdater.exe beside HeroNet.asi to restore updates.",
            "HeroNet Updater Missing", MB_OK | MB_ICONWARNING);
        if (log)
            std::fclose(log);
        return;
    }

    char gamePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, gamePath, MAX_PATH)) {
        LogUpdater(log, "Could not resolve game path, error=%lu",
            GetLastError());
        if (log)
            std::fclose(log);
        return;
    }

    char command[MAX_PATH * 3 + 160] = {};
    std::snprintf(command, sizeof(command),
        "\"%s\" --startup --pid %lu --game-exe \"%s\" "
        "--target \"%s\" --current-version %s",
        updaterPath, GetCurrentProcessId(), gamePath, asiPath,
        UMVC3HOOK_VERSION);
    std::vector<char> mutableCommand(command, command + strlen(command) + 1);
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (CreateProcessA(updaterPath, mutableCommand.data(), nullptr, nullptr,
            FALSE, 0, nullptr, nullptr, &startup, &process)) {
        LogUpdater(log, "Launched updater process pid=%lu", process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } else
        LogUpdater(log, "Launching updater failed, error=%lu", GetLastError());
    if (log)
        std::fclose(log);
}
