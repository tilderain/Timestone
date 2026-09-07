#include "IniSafe.h"
#include "IniText.h"

#include <windows.h>
#include <cstring>
#include <stdexcept>
#include <string>

extern void HookLog(const char* fmt, ...);

namespace {
constexpr DWORD kMaxIniBytes = 4 * 1024 * 1024;

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    bool Close() {
        if (value == INVALID_HANDLE_VALUE || !value) return true;
        const HANDLE old = value;
        value = INVALID_HANDLE_VALUE;
        return CloseHandle(old) != FALSE;
    }
    ~Handle() { const DWORD error = GetLastError(); Close(); SetLastError(error); }
};

struct Lock {
    // Shared by all cooperating writers, including separate game processes.
    Handle mutex;
    bool owned = false;
    Lock() {
        mutex.value = CreateMutexA(nullptr, FALSE, "Local\\Timestone.IniWriter.v1");
        if (mutex.value) {
            const DWORD result = WaitForSingleObject(mutex.value, 5000);
            owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
            if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
        }
    }
    ~Lock() {
        const DWORD error = GetLastError();
        if (owned) ReleaseMutex(mutex.value);
        SetLastError(error);
    }
};

bool Read(HANDLE file, std::string& bytes)
{
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size)) return false;
    if (size.QuadPart < 0 || size.QuadPart > kMaxIniBytes) {
        SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD count = 0;
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
        return false;
    if (count != bytes.size()) { SetLastError(ERROR_READ_FAULT); return false; }
    return true;
}

struct Temporary {
    char path[MAX_PATH] = {};
    ~Temporary() {
        const DWORD error = GetLastError();
        if (path[0]) DeleteFileA(path);
        SetLastError(error);
    }
    bool Write(const std::string& directory, const std::string& bytes) {
        if (!GetTempFileNameA(directory.c_str(), "tsi", 0, path)) return false;
        Handle file;
        file.value = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) return false;
        DWORD count = 0;
        if (!WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
            return false;
        if (count != bytes.size()) { SetLastError(ERROR_WRITE_FAULT); return false; }
        if (!FlushFileBuffers(file.value)) return false;
        LARGE_INTEGER zero = {};
        std::string verified;
        if (!SetFilePointerEx(file.value, zero, nullptr, FILE_BEGIN) || !Read(file.value, verified))
            return false;
        if (verified != bytes) { SetLastError(ERROR_CRC); return false; }
        return file.Close();
    }
};

std::wstring Wide(const char* text)
{
    const int count = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (!count) throw std::runtime_error("INI text conversion failed");
    std::wstring result(count, 0);
    if (!MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), count))
        throw std::runtime_error("INI text conversion failed");
    result.pop_back();
    return result;
}

std::string Utf8(const char* text)
{
    const auto wide = Wide(text);
    if (wide.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("INI UTF-8 conversion failed");
    std::string result(count, 0);
    if (!WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                             result.data(), count, nullptr, nullptr))
        throw std::runtime_error("INI UTF-8 conversion failed");
    return result;
}

std::string Update(const std::string& bytes, const char* section,
                   const char* key, const char* value)
{
    if (bytes.size() >= 2 &&
        (bytes.compare(0, 2, "\xff\xfe") == 0 || bytes.compare(0, 2, "\xfe\xff") == 0)) {
        const bool little = static_cast<unsigned char>(bytes[0]) == 0xff;
        std::wstring text;
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const unsigned a = static_cast<unsigned char>(bytes[i]);
            const unsigned b = static_cast<unsigned char>(bytes[i + 1]);
            text += static_cast<wchar_t>(little ? a | (b << 8) : (a << 8) | b);
        }
        const auto s = Wide(section), k = Wide(key), v = Wide(value);
        text = IniText::Update<wchar_t>(text, s, k, v);
        std::string result = bytes.substr(0, 2);
        for (wchar_t c : text) {
            result += static_cast<char>(little ? c & 255 : c >> 8);
            result += static_cast<char>(little ? c >> 8 : c & 255);
        }
        // Preserve even an incomplete final code unit in an already damaged INI.
        if (bytes.size() % 2) result += bytes.back();
        return result;
    }
    const size_t bom = bytes.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    if (bom) {
        const auto s = Utf8(section), k = Utf8(key), v = Utf8(value);
        return IniText::Update<char>(bytes, s, k, v, bom);
    }
    return IniText::Update<char>(bytes, section, key, value, bom);
}

bool Save(const IniSafe_Entry* entries, size_t count, const char* filename)
{
    if ((!entries && count) || !filename || !*filename) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        if (entry.section.empty() || entry.key.empty() ||
            entry.section.find_first_of("\r\n[]") != std::string::npos ||
            entry.key.find_first_of("\r\n=[]") != std::string::npos ||
            entry.value.find_first_of("\r\n") != std::string::npos ||
            entry.section.find('\0') != std::string::npos ||
            entry.key.find('\0') != std::string::npos ||
            entry.value.find('\0') != std::string::npos) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
    }
    if (!count) return true;
    char path[MAX_PATH] = {};
    char* leaf = nullptr;
    const DWORD length = GetFullPathNameA(filename, MAX_PATH, path, &leaf);
    if (!length) return false;
    if (length >= MAX_PATH - 4 || !leaf || !*leaf) {
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    const std::string directory(path, leaf - path);
    Lock lock;
    if (!lock.owned) return false;

    Handle original;
    // Exclude in-place external writers while reading/staging, but allow rename.
    original.value = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool exists = original.value != INVALID_HANDLE_VALUE;
    if (!exists && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    std::string before;
    if (exists && !Read(original.value, before)) return false;
    std::string after = before;
    for (size_t i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        after = Update(after, entry.section.c_str(), entry.key.c_str(),
                        entry.value.c_str());
        if (after.size() > kMaxIniBytes) {
            SetLastError(ERROR_FILE_TOO_LARGE);
            return false;
        }
    }
    if (exists && before == after) return true;
    if (after.empty() || after.size() > kMaxIniBytes) {
        SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }

    Temporary next;
    if (!next.Write(directory, after)) return false;
    // Never replace a useful backup with an empty file. Malformed nonempty
    // inputs are preserved too; they can still contain recoverable settings.
    if (exists && !before.empty()) {
        Temporary backup;
        const std::string backupPath = std::string(path) + ".bak";
        if (!backup.Write(directory, before) ||
            !MoveFileExA(backup.path, backupPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    }
    // Some Wine versions refuse replacement while the destination is open,
    // even with FILE_SHARE_DELETE. Keep the interprocess mutex through commit.
    if (!original.Close()) return false;
    // No delete-then-copy fallback: failure must leave the live INI untouched.
    return MoveFileExA(next.path, path, MOVEFILE_WRITE_THROUGH |
        (exists ? MOVEFILE_REPLACE_EXISTING : 0)) != FALSE;
}
} // namespace

bool IniSafe_WriteBatch(const IniSafe_Entry* entries, size_t count, const char* filename)
{
    try {
        if (Save(entries, count, filename)) return true;
    } catch (...) {
        // Allocation failures must not escape through a UI callback or hook.
    }
    HookLog("IniSafe: batch save failed; live file left untouched: %s (entries=%zu error=%lu)\n",
            filename ? filename : "(null)", count, static_cast<unsigned long>(GetLastError()));
    return false;
}

bool IniSafe_WriteString(const char* section, const char* key,
                         const char* value, const char* filename)
{
    if (!section || !key || !value) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
    try {
        const IniSafe_Entry entry{section, key, value};
        return IniSafe_WriteBatch(&entry, 1, filename);
    } catch (...) {
        HookLog("IniSafe: could not allocate setting update\n");
        return false;
    }
}
