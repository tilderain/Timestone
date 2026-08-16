#include "updater.h"
#include "umvc3.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

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

    char pendingUpdaterPath[MAX_PATH + 8] = {};
    std::snprintf(pendingUpdaterPath, sizeof(pendingUpdaterPath),
        "%s.new", updaterPath);
    if (GetFileAttributesA(pendingUpdaterPath) != INVALID_FILE_ATTRIBUTES) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (CopyFileA(pendingUpdaterPath, updaterPath, FALSE)) {
                DeleteFileA(pendingUpdaterPath);
                break;
            }
            Sleep(100);
        }
    }
    if (GetFileAttributesA(updaterPath) == INVALID_FILE_ATTRIBUTES)
        return;

    char gamePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, gamePath, MAX_PATH))
        return;

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
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}
