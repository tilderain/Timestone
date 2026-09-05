#pragma once

#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace LaunchMenuUtils {

// Kept out of line so all launch windows use the same Win32 scaling implementation.
void ScaleChildren(HWND window, UINT oldDpi, UINT newDpi, bool scaleContents);

inline void GetSettingsPath(const char* filename, char* path, size_t pathSize)
{
    if (!pathSize) return;
    const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(pathSize));
    if (length == 0 || length >= pathSize) {
        path[0] = '\0';
        return;
    }
    char* slash = strrchr(path, '\\');
    char* forwardSlash = strrchr(path, '/');
    if (!slash || (forwardSlash && forwardSlash > slash)) slash = forwardSlash;
    char* filenameStart = slash ? slash + 1 : path;
    snprintf(filenameStart, pathSize - static_cast<size_t>(filenameStart - path),
             "%s", filename);
}

inline void GetSettingsPath(char* path, size_t pathSize)
{
    GetSettingsPath("Timestone.ini", path, pathSize);
}

} // namespace LaunchMenuUtils
