#pragma once

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace LaunchMenuUtils {

inline std::wstring Utf8ToWide(const char* text)
{
    if (!text || !*text) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (!length) return {};
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    result.resize(length - 1);
    return result;
}

inline void SetText(HWND window, const char* text)
{
    SetWindowTextW(window, Utf8ToWide(text).c_str());
}

// Never truncate a UTF-8 sequence to fit a fixed-size network/settings buffer.
inline int GetText(HWND window, char* text, size_t capacity)
{
    if (!capacity) return -1;
    std::wstring wide(GetWindowTextLengthW(window) + 1, L'\0');
    GetWindowTextW(window, wide.data(), static_cast<int>(wide.size()));
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (!bytes || static_cast<size_t>(bytes) > capacity) return -1;
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), -1,
                        text, bytes, nullptr, nullptr);
    return bytes - 1;
}

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
