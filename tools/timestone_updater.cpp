#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shldisp.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#ifndef TIMESTONE_VERSION
#define TIMESTONE_VERSION "0.1.0"
#endif

namespace {

void ShowError(const wchar_t* message)
{
    MessageBoxW(nullptr, message, L"Timestone Updater", MB_OK | MB_ICONERROR);
}

void ShowFileError(const wchar_t* message, DWORD error)
{
    wchar_t detail[512] = {};
    swprintf(detail, 512, L"%ls\n\nWindows error: %lu", message, error);
    ShowError(detail);
}

struct DownloadProgress {
    HWND window = nullptr;
    HWND source = nullptr;
    HWND status = nullptr;
    HWND bar = nullptr;
    HFONT font = nullptr;
    ULONGLONG startedAt = 0;
};

BOOL CALLBACK SetProgressControlFont(HWND control, LPARAM font)
{
    SendMessageW(control, WM_SETFONT, font, TRUE);
    return TRUE;
}

void ApplyProgressWindowFont(DownloadProgress& progress)
{
    HDC dc = GetDC(progress.window);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(progress.window, dc);

    LOGFONTW font = {};
    font.lfHeight = -MulDiv(10, dpi, 72);
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    swprintf(font.lfFaceName, LF_FACESIZE, L"MS Shell Dlg 2");
    progress.font = CreateFontIndirectW(&font);
    if (!progress.font)
        return;

    SendMessageW(progress.window, WM_SETFONT,
        reinterpret_cast<WPARAM>(progress.font), TRUE);
    EnumChildWindows(progress.window, SetProgressControlFont,
        reinterpret_cast<LPARAM>(progress.font));
}

void PumpWindowMessages()
{
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool CreateProgressWindow(DownloadProgress& progress)
{
    INITCOMMONCONTROLSEX controls = {
        sizeof(controls), ICC_PROGRESS_CLASS
    };
    InitCommonControlsEx(&controls);

    static const wchar_t className[] = L"TimestoneUpdaterProgress";
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        registered = true;
    }

    const int width = 500;
    const int height = 160;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    progress.window = CreateWindowExW(WS_EX_TOPMOST, className,
        L"Timestone Updater", WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE,
        x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!progress.window)
        return false;

    progress.source = CreateWindowExW(0, L"STATIC",
        L"Connecting to github.com/tilderain/Timestone...",
        WS_CHILD | WS_VISIBLE, 20, 18, 450, 20,
        progress.window, nullptr, instance, nullptr);
    progress.bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 20, 50, 450, 22,
        progress.window, nullptr, instance, nullptr);
    progress.status = CreateWindowExW(0, L"STATIC", L"Preparing download...",
        WS_CHILD | WS_VISIBLE, 20, 84, 450, 20,
        progress.window, nullptr, instance, nullptr);
    ApplyProgressWindowFont(progress);
    SendMessageW(progress.bar, PBM_SETRANGE32, 0, 1000);
    progress.startedAt = GetTickCount64();
    UpdateWindow(progress.window);
    PumpWindowMessages();
    return progress.source && progress.bar && progress.status;
}

void SetProgressStage(DownloadProgress* progress, const wchar_t* stage)
{
    if (!progress || !progress->window)
        return;
    SetWindowTextW(progress->source, stage);
    PumpWindowMessages();
}

void UpdateDownloadProgress(DownloadProgress* progress, ULONGLONG downloaded,
    ULONGLONG total)
{
    if (!progress || !progress->window)
        return;
    ULONGLONG elapsed = GetTickCount64() - progress->startedAt;
    double kibPerSecond = elapsed
        ? static_cast<double>(downloaded) * 1000.0 / elapsed / 1024.0 : 0.0;
    double downloadedMiB = static_cast<double>(downloaded) / (1024.0 * 1024.0);
    wchar_t text[256] = {};
    if (total) {
        double totalMiB = static_cast<double>(total) / (1024.0 * 1024.0);
        unsigned int percent = static_cast<unsigned int>(
            (downloaded * 100) / total);
        swprintf(text, 256, L"%u%%  |  %.2f / %.2f MiB  |  %.0f KiB/s",
            percent, downloadedMiB, totalMiB, kibPerSecond);
        SendMessageW(progress->bar, PBM_SETPOS,
            static_cast<WPARAM>((downloaded * 1000) / total), 0);
    } else {
        swprintf(text, 256, L"%.2f MiB downloaded  |  %.0f KiB/s",
            downloadedMiB, kibPerSecond);
    }
    SetWindowTextW(progress->status, text);
    PumpWindowMessages();
}

void CloseProgressWindow(DownloadProgress& progress)
{
    if (progress.window)
        DestroyWindow(progress.window);
    progress.window = nullptr;
    if (progress.font)
        DeleteObject(progress.font);
    progress.font = nullptr;
    PumpWindowMessages();
}

bool ReadResponse(HINTERNET request, std::string& response)
{
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
            return false;
        if (!available)
            return true;
        size_t oldSize = response.size();
        response.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, response.data() + oldSize, available, &read))
            return false;
        response.resize(oldSize + read);
    }
}

bool FetchLatestRelease(std::string& response)
{
    HINTERNET session = WinHttpOpen(L"TimestoneUpdater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (session)
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
            &secureProtocols, sizeof(secureProtocols));
    HINTERNET connection = session ? WinHttpConnect(session, L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET",
        L"/repos/tilderain/Timestone/releases/latest", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = request && WinHttpSendRequest(request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ok = WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
            WINHTTP_NO_HEADER_INDEX) && status == 200;
    }
    if (ok)
        ok = ReadResponse(request, response);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

bool ExtractJsonString(const std::string& json, const char* key, size_t start,
    std::string& value)
{
    std::string marker = std::string("\"") + key + "\"";
    size_t keyPos = json.find(marker, start);
    size_t colon = keyPos == std::string::npos
        ? keyPos : json.find(':', keyPos + marker.size());
    size_t quote = colon == std::string::npos
        ? colon : json.find('"', colon + 1);
    if (quote == std::string::npos)
        return false;
    value.clear();
    for (size_t i = quote + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"')
            return true;
        if (c == '\\' && i + 1 < json.size()) {
            char escaped = json[++i];
            if (escaped != '/' && escaped != '\\' && escaped != '"')
                return false;
            value.push_back(escaped);
        } else {
            value.push_back(c);
        }
    }
    return false;
}

bool ParseReleaseAsset(const std::string& json, const char* assetName,
    std::string& url, std::string& sha256)
{
    size_t asset = json.find("\"assets\"");
    while (asset != std::string::npos) {
        asset = json.find("\"name\"", asset + 1);
        if (asset == std::string::npos)
            break;
        std::string name;
        if (ExtractJsonString(json, "name", asset, name) && name == assetName) {
            std::string digest;
            if (!ExtractJsonString(json, "digest", asset, digest) ||
                digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 ||
                !ExtractJsonString(json, "browser_download_url", asset, url))
                return false;
            sha256 = digest.substr(7);
            return true;
        }
    }
    return false;
}

bool ParseLatestRelease(const std::string& json, std::string& tag,
    std::string& url, std::string& sha256)
{
    if (!ExtractJsonString(json, "tag_name", 0, tag))
        return false;
    if (ParseReleaseAsset(json, "Timestone.zip", url, sha256))
        return true;
    return ParseReleaseAsset(json, "Timestone.asi", url, sha256);
}

std::wstring Utf8ToWide(const std::string& text)
{
    int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    if (length)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), result.data(), length);
    return result;
}

bool GetSiblingAsiPath(std::wstring& path)
{
    wchar_t executable[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length == MAX_PATH)
        return false;
    path.assign(executable, length);
    size_t slash = path.find_last_of(L"\\/");
    path.resize(slash == std::wstring::npos ? 0 : slash + 1);
    path += L"Timestone.asi";
    return true;
}

bool GetInstalledVersion(const std::wstring& path, std::wstring& version)
{
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size)
        return false;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
        return false;
    wchar_t* value = nullptr;
    UINT valueLength = 0;
    if (VerQueryValueW(data.data(),
            L"\\StringFileInfo\\040904E4\\ProductVersion",
            reinterpret_cast<void**>(&value), &valueLength) && valueLength) {
        version.assign(value, valueLength - 1);
        return true;
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoLength = 0;
    if (!VerQueryValueW(data.data(), L"\\",
            reinterpret_cast<void**>(&fixedInfo), &fixedInfoLength) ||
        fixedInfoLength < sizeof(VS_FIXEDFILEINFO) ||
        fixedInfo->dwSignature != 0xFEEF04BD)
        return false;
    wchar_t buffer[64] = {};
    swprintf(buffer, 64, L"%u.%u.%u",
        HIWORD(fixedInfo->dwProductVersionMS),
        LOWORD(fixedInfo->dwProductVersionMS),
        HIWORD(fixedInfo->dwProductVersionLS));
    version = buffer;
    return true;
}

const wchar_t* SkipVersionPrefix(const wchar_t* version)
{
    return (*version == L'v' || *version == L'V') ? version + 1 : version;
}

bool IsNewerVersion(const wchar_t* latest, const wchar_t* current)
{
    latest = SkipVersionPrefix(latest);
    current = SkipVersionPrefix(current);
    for (int part = 0; part < 4; ++part) {
        wchar_t* latestEnd = nullptr;
        wchar_t* currentEnd = nullptr;
        unsigned long latestPart = wcstoul(latest, &latestEnd, 10);
        unsigned long currentPart = wcstoul(current, &currentEnd, 10);
        if (latestEnd == latest || currentEnd == current)
            return _wcsicmp(latest, current) != 0;
        if (latestPart != currentPart)
            return latestPart > currentPart;
        latest = (*latestEnd == L'.') ? latestEnd + 1 : latestEnd;
        current = (*currentEnd == L'.') ? currentEnd + 1 : currentEnd;
        if (!*latest && !*current)
            return false;
    }
    return false;
}

DWORD FindGameProcess()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"umvc3.exe") == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

bool CrackUrl(const std::wstring& url, URL_COMPONENTS& parts,
    std::vector<wchar_t>& host, std::vector<wchar_t>& path)
{
    ZeroMemory(&parts, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts))
        return false;
    host.assign(parts.lpszHostName, parts.lpszHostName + parts.dwHostNameLength);
    host.push_back(L'\0');
    path.assign(parts.lpszUrlPath, parts.lpszUrlPath + parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.insert(path.end(), parts.lpszExtraInfo,
            parts.lpszExtraInfo + parts.dwExtraInfoLength);
    path.push_back(L'\0');
    return true;
}

bool Download(const std::wstring& url, const std::wstring& destination,
    DownloadProgress* progress, const wchar_t* stage)
{
    URL_COMPONENTS parts;
    std::vector<wchar_t> host;
    std::vector<wchar_t> path;
    if (!CrackUrl(url, parts, host, path))
        return false;

    HINTERNET session = WinHttpOpen(L"TimestoneUpdater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (session)
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
            &secureProtocols, sizeof(secureProtocols));
    HINTERNET connection = session ? WinHttpConnect(
        session, host.data(), parts.nPort, 0) : nullptr;
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connection ? WinHttpOpenRequest(
        connection, L"GET", path.data(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
    bool ok = request && WinHttpSendRequest(request,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ok = WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
            WINHTTP_NO_HEADER_INDEX) && status == 200;
    }

    ULONGLONG total = 0;
    if (ok) {
        wchar_t contentLength[64] = {};
        DWORD contentLengthSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX, contentLength,
                &contentLengthSize, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(contentLength, nullptr, 10);
        SetProgressStage(progress, stage);
        UpdateDownloadProgress(progress, 0, total);
    }

    HANDLE file = ok ? CreateFileW(destination.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    ULONGLONG downloaded = 0;
    while (ok && file != INVALID_HANDLE_VALUE) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            ok = false;
            break;
        }
        if (!available)
            break;
        std::vector<unsigned char> buffer(available);
        DWORD read = 0;
        DWORD written = 0;
        ok = WinHttpReadData(request, buffer.data(), available, &read) &&
            WriteFile(file, buffer.data(), read, &written, nullptr) && written == read;
        if (ok) {
            downloaded += written;
            UpdateDownloadProgress(progress, downloaded, total);
        }
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    else
        ok = false;
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (!ok)
        DeleteFileW(destination.c_str());
    return ok;
}

bool IsPlausibleAsi(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    unsigned char header[2] = {};
    DWORD read = 0;
    bool valid = GetFileSizeEx(file, &size) && size.QuadPart >= 64 * 1024 &&
        ReadFile(file, header, sizeof(header), &read, nullptr) && read == 2 &&
        header[0] == 'M' && header[1] == 'Z';
    CloseHandle(file);
    return valid;
}

bool IsZipArchive(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    unsigned char signature[4] = {};
    DWORD read = 0;
    bool zip = ReadFile(file, signature, sizeof(signature), &read, nullptr) &&
        read == sizeof(signature) && signature[0] == 'P' &&
        signature[1] == 'K' &&
        ((signature[2] == 3 && signature[3] == 4) ||
         (signature[2] == 5 && signature[3] == 6));
    CloseHandle(file);
    return zip;
}

uint16_t ZipU16(const std::vector<unsigned char>& data, size_t offset)
{
    return static_cast<uint16_t>(data[offset]) |
        (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t ZipU32(const std::vector<unsigned char>& data, size_t offset)
{
    return static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
}

enum class StoredZipResult {
    Extracted,
    NotFound,
    Unsupported,
    Invalid
};

StoredZipResult ExtractStoredZipEntry(const std::wstring& archive,
    const char* wantedName, const std::wstring& destination)
{
    HANDLE file = CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return StoredZipResult::Invalid;
    LARGE_INTEGER fileSize = {};
    bool valid = GetFileSizeEx(file, &fileSize) && fileSize.QuadPart >= 22 &&
        fileSize.QuadPart <= 0xffffffffULL;
    std::vector<unsigned char> zip(valid
        ? static_cast<size_t>(fileSize.QuadPart) : 0);
    DWORD read = 0;
    if (valid)
        valid = ReadFile(file, zip.data(), static_cast<DWORD>(zip.size()),
            &read, nullptr) && read == zip.size();
    CloseHandle(file);
    if (!valid)
        return StoredZipResult::Invalid;

    size_t searchStart = zip.size() > 22 + 0xffff
        ? zip.size() - (22 + 0xffff) : 0;
    size_t eocd = std::string::npos;
    for (size_t pos = zip.size() - 22;; --pos) {
        if (ZipU32(zip, pos) == 0x06054b50) {
            eocd = pos;
            break;
        }
        if (pos == searchStart)
            break;
    }
    if (eocd == std::string::npos || ZipU16(zip, eocd + 4) != 0 ||
        ZipU16(zip, eocd + 6) != 0 ||
        ZipU16(zip, eocd + 8) != ZipU16(zip, eocd + 10))
        return StoredZipResult::Invalid;

    size_t entries = ZipU16(zip, eocd + 10);
    size_t centralSize = ZipU32(zip, eocd + 12);
    size_t cursor = ZipU32(zip, eocd + 16);
    if (cursor > zip.size() || centralSize > zip.size() - cursor)
        return StoredZipResult::Invalid;

    bool found = false;
    uint16_t method = 0;
    size_t compressedSize = 0;
    size_t uncompressedSize = 0;
    size_t localOffset = 0;
    for (size_t i = 0; i < entries; ++i) {
        if (cursor > zip.size() || zip.size() - cursor < 46 ||
            ZipU32(zip, cursor) != 0x02014b50)
            return StoredZipResult::Invalid;
        size_t nameLength = ZipU16(zip, cursor + 28);
        size_t extraLength = ZipU16(zip, cursor + 30);
        size_t commentLength = ZipU16(zip, cursor + 32);
        size_t recordSize = 46 + nameLength + extraLength + commentLength;
        if (recordSize > zip.size() - cursor)
            return StoredZipResult::Invalid;
        std::string name(reinterpret_cast<const char*>(zip.data() + cursor + 46),
            nameLength);
        size_t slash = name.find_last_of("/\\");
        const char* baseName = name.c_str() +
            (slash == std::string::npos ? 0 : slash + 1);
        if (_stricmp(baseName, wantedName) == 0) {
            if (found || (ZipU16(zip, cursor + 8) & 1) != 0)
                return StoredZipResult::Invalid;
            found = true;
            method = ZipU16(zip, cursor + 10);
            compressedSize = ZipU32(zip, cursor + 20);
            uncompressedSize = ZipU32(zip, cursor + 24);
            localOffset = ZipU32(zip, cursor + 42);
        }
        cursor += recordSize;
    }
    if (!found)
        return StoredZipResult::NotFound;
    if (method != 0)
        return StoredZipResult::Unsupported;
    if (compressedSize != uncompressedSize || localOffset > zip.size() ||
        zip.size() - localOffset < 30 ||
        ZipU32(zip, localOffset) != 0x04034b50)
        return StoredZipResult::Invalid;
    size_t dataOffset = localOffset + 30 + ZipU16(zip, localOffset + 26) +
        ZipU16(zip, localOffset + 28);
    if (dataOffset > zip.size() || uncompressedSize > zip.size() - dataOffset)
        return StoredZipResult::Invalid;

    HANDLE output = CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        return StoredZipResult::Invalid;
    DWORD written = 0;
    bool wrote = WriteFile(output, zip.data() + dataOffset,
        static_cast<DWORD>(uncompressedSize), &written, nullptr) &&
        written == uncompressedSize;
    CloseHandle(output);
    if (!wrote) {
        DeleteFileW(destination.c_str());
        return StoredZipResult::Invalid;
    }
    return StoredZipResult::Extracted;
}

void DeleteDirectoryTree(const std::wstring& directory)
{
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(entry.cFileName, L".") == 0 ||
                wcscmp(entry.cFileName, L"..") == 0)
                continue;
            std::wstring path = directory + L"\\" + entry.cFileName;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteDirectoryTree(path);
            else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    RemoveDirectoryW(directory.c_str());
}

void ScanExtractedFiles(const std::wstring& directory,
    const wchar_t* wantedName, std::wstring& found, unsigned int& matches,
    ULONGLONG& totalSize, unsigned int& fileCount)
{
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((directory + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        if (wcscmp(entry.cFileName, L".") == 0 ||
            wcscmp(entry.cFileName, L"..") == 0)
            continue;
        std::wstring path = directory + L"\\" + entry.cFileName;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanExtractedFiles(path, wantedName, found, matches, totalSize,
                fileCount);
        } else {
            ++fileCount;
            totalSize += (static_cast<ULONGLONG>(entry.nFileSizeHigh) << 32) |
                entry.nFileSizeLow;
            if (_wcsicmp(entry.cFileName, wantedName) == 0) {
                found = path;
                ++matches;
            }
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

bool ExtractZipPackage(const std::wstring& archive,
    const std::wstring& asiDestination, const std::wstring& updaterDestination,
    bool& updaterExtracted)
{
    StoredZipResult asiResult = ExtractStoredZipEntry(
        archive, "Timestone.asi", asiDestination);
    if (asiResult == StoredZipResult::Extracted) {
        StoredZipResult updaterResult = ExtractStoredZipEntry(
            archive, "TimestoneUpdater.exe", updaterDestination);
        updaterExtracted = updaterResult == StoredZipResult::Extracted;
        return updaterResult == StoredZipResult::Extracted ||
            updaterResult == StoredZipResult::NotFound;
    }
    if (asiResult != StoredZipResult::Unsupported)
        return false;

    std::wstring staging = archive + L".stage";
    DeleteDirectoryTree(staging);
    if (!CreateDirectoryW(staging.c_str(), nullptr))
        return false;

    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninitialize = SUCCEEDED(initialized);
    IShellDispatch* shell = nullptr;
    Folder* archiveFolder = nullptr;
    Folder* stagingFolder = nullptr;
    FolderItems* items = nullptr;
    HRESULT result = SUCCEEDED(initialized) ? CoCreateInstance(CLSID_Shell,
        nullptr, CLSCTX_INPROC_SERVER, IID_IShellDispatch,
        reinterpret_cast<void**>(&shell)) : initialized;

    VARIANT archivePath;
    VARIANT stagingPath;
    VariantInit(&archivePath);
    VariantInit(&stagingPath);
    archivePath.vt = VT_BSTR;
    archivePath.bstrVal = SysAllocString(archive.c_str());
    stagingPath.vt = VT_BSTR;
    stagingPath.bstrVal = SysAllocString(staging.c_str());
    if (SUCCEEDED(result) &&
        (!shell || !archivePath.bstrVal || !stagingPath.bstrVal))
        result = E_OUTOFMEMORY;
    if (SUCCEEDED(result))
        result = shell->NameSpace(archivePath, &archiveFolder);
    if (SUCCEEDED(result) && !archiveFolder)
        result = E_NOINTERFACE;
    if (SUCCEEDED(result))
        result = shell->NameSpace(stagingPath, &stagingFolder);
    if (SUCCEEDED(result) && !stagingFolder)
        result = E_NOINTERFACE;
    if (SUCCEEDED(result))
        result = archiveFolder->Items(&items);
    if (SUCCEEDED(result) && !items)
        result = E_NOINTERFACE;
    if (SUCCEEDED(result)) {
        VARIANT source;
        VARIANT options;
        VariantInit(&source);
        VariantInit(&options);
        source.vt = VT_DISPATCH;
        source.pdispVal = items;
        options.vt = VT_I4;
        options.lVal = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI;
        result = stagingFolder->CopyHere(source, options);
    }

    std::wstring asiPath;
    std::wstring updaterPath;
    unsigned int stableTicks = 0;
    ULONGLONG previousSize = ~0ULL;
    unsigned int previousCount = ~0U;
    for (int attempt = 0; SUCCEEDED(result) && attempt < 600; ++attempt) {
        unsigned int asiMatches = 0;
        unsigned int updaterMatches = 0;
        unsigned int fileCount = 0;
        ULONGLONG totalSize = 0;
        asiPath.clear();
        updaterPath.clear();
        ScanExtractedFiles(staging, L"Timestone.asi", asiPath, asiMatches,
            totalSize, fileCount);
        ScanExtractedFiles(staging, L"TimestoneUpdater.exe", updaterPath,
            updaterMatches, totalSize, fileCount);
        if (asiMatches > 1 || updaterMatches > 1) {
            result = E_FAIL;
            break;
        }
        if (asiMatches == 1 && totalSize == previousSize &&
            fileCount == previousCount)
            ++stableTicks;
        else
            stableTicks = 0;
        if (stableTicks >= 10)
            break;
        previousSize = totalSize;
        previousCount = fileCount;
        Sleep(100);
        PumpWindowMessages();
    }

    if (items) items->Release();
    if (stagingFolder) stagingFolder->Release();
    if (archiveFolder) archiveFolder->Release();
    if (shell) shell->Release();
    VariantClear(&stagingPath);
    VariantClear(&archivePath);
    if (uninitialize)
        CoUninitialize();

    updaterExtracted = false;
    bool ok = SUCCEEDED(result) && !asiPath.empty() && stableTicks >= 10 &&
        CopyFileW(asiPath.c_str(), asiDestination.c_str(), FALSE);
    if (ok && !updaterPath.empty()) {
        updaterExtracted = CopyFileW(updaterPath.c_str(),
            updaterDestination.c_str(), FALSE) != FALSE;
        ok = updaterExtracted;
    }
    DeleteDirectoryTree(staging);
    return ok;
}

bool VerifySha256(const std::wstring& path, const wchar_t* expected)
{
    if (!expected || wcslen(expected) != 64)
        return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD hashSize = 0;
    DWORD resultSize = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;
    bool ok = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &resultSize, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
            &resultSize, 0) == 0;
    if (ok) {
        object.resize(objectSize);
        digest.resize(hashSize);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
            nullptr, 0, 0) == 0;
    }

    HANDLE file = ok ? CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    std::vector<unsigned char> buffer(64 * 1024);
    while (ok && file != INVALID_HANDLE_VALUE) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                &read, nullptr)) {
            ok = false;
            break;
        }
        if (!read)
            break;
        ok = BCryptHashData(hash, buffer.data(), read, 0) == 0;
    }
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    else
        ok = false;
    if (ok)
        ok = BCryptFinishHash(hash, digest.data(), hashSize, 0) == 0;

    static const wchar_t kHex[] = L"0123456789abcdef";
    wchar_t actual[65] = {};
    if (ok && digest.size() == 32) {
        for (size_t i = 0; i < digest.size(); ++i) {
            actual[i * 2] = kHex[digest[i] >> 4];
            actual[i * 2 + 1] = kHex[digest[i] & 0x0f];
        }
        ok = _wcsicmp(actual, expected) == 0;
    } else {
        ok = false;
    }

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

const wchar_t* ArgumentValue(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0)
            return argv[i + 1];
    }
    return nullptr;
}

bool HasArgument(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0)
            return true;
    }
    return false;
}

bool RelaunchGame(const std::wstring& executable)
{
    if (executable.empty())
        return false;
    std::wstring command = L"\"" + executable + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    std::wstring workingDirectory = executable;
    size_t slash = workingDirectory.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        workingDirectory.clear();
    else
        workingDirectory.resize(slash);

    STARTUPINFOW startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo = {};
    BOOL created = CreateProcessW(executable.c_str(), mutableCommand.data(),
        nullptr, nullptr, FALSE, 0, nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startupInfo, &processInfo);
    if (!created)
        return false;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        ShowError(L"Could not parse updater arguments.");
        return 2;
    }
    if (HasArgument(argc, argv, L"--return-menu-watch")) {
        const wchar_t* pidText = ArgumentValue(argc, argv, L"--pid");
        const wchar_t* gameText = ArgumentValue(argc, argv, L"--game-exe");
        const wchar_t* eventText = ArgumentValue(argc, argv, L"--cancel-event");
        if (!pidText || !gameText || !eventText) {
            LocalFree(argv);
            return 2;
        }

        const DWORD pid = wcstoul(pidText, nullptr, 10);
        const std::wstring gameExecutable = gameText;
        const std::wstring eventName = eventText;
        LocalFree(argv);

        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        HANDLE cancelEvent = OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
        if (!process) {
            if (cancelEvent) CloseHandle(cancelEvent);
            return RelaunchGame(gameExecutable) ? 0 : 4;
        }

        HANDLE waits[2] = {process, cancelEvent};
        const DWORD result = cancelEvent
            ? WaitForMultipleObjects(2, waits, FALSE, INFINITE)
            : WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        if (cancelEvent) CloseHandle(cancelEvent);
        if (result != WAIT_OBJECT_0)
            return 0;

        Sleep(100);
        return RelaunchGame(gameExecutable) ? 0 : 4;
    }
    bool manual = argc == 1;
    bool startup = HasArgument(argc, argv, L"--startup");
    bool localCheck = manual || startup;
    DWORD pid = 0;
    std::wstring target;
    std::wstring url;
    std::wstring sha256;
    std::wstring updaterUrl;
    std::wstring updaterSha256;
    std::wstring gameExecutable;
    if (localCheck) {
        const wchar_t* suppliedTarget = ArgumentValue(argc, argv, L"--target");
        if (suppliedTarget)
            target = suppliedTarget;
        else if (!GetSiblingAsiPath(target))
            target.clear();
        if (target.empty() ||
            GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LocalFree(argv);
            if (manual)
                ShowError(L"Timestone.asi was not found beside TimestoneUpdater.exe.");
            return 2;
        }
        std::wstring currentVersion;
        const wchar_t* suppliedVersion = ArgumentValue(
            argc, argv, L"--current-version");
        if (suppliedVersion)
            currentVersion = suppliedVersion;
        else if (!GetInstalledVersion(target, currentVersion))
            currentVersion = Utf8ToWide(TIMESTONE_VERSION);
        std::string json;
        std::string latestTag;
        std::string downloadUrl;
        std::string digest;
        std::string updaterDownloadUrl;
        std::string updaterDigest;
        if (!FetchLatestRelease(json) ||
            !ParseLatestRelease(json, latestTag, downloadUrl, digest)) {
            LocalFree(argv);
            if (manual)
                ShowError(L"Could not read the latest Timestone release from GitHub.");
            return 3;
        }
        ParseReleaseAsset(json, "TimestoneUpdater.exe",
            updaterDownloadUrl, updaterDigest);
        std::wstring latestVersion = Utf8ToWide(latestTag);
        if (!IsNewerVersion(latestVersion.c_str(), currentVersion.c_str())) {
            LocalFree(argv);
            if (manual) {
                wchar_t message[256] = {};
                swprintf(message, 256, L"Timestone.asi %ls is already up to date.",
                    currentVersion.c_str());
                MessageBoxW(nullptr, message, L"Timestone Updater",
                    MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        wchar_t prompt[384] = {};
        swprintf(prompt, 384,
            L"Timestone update is required.\n\nInstalled: %ls\nAvailable: %ls\n\n"
            L"Update now? UMVC3 will close and restart automatically.",
            currentVersion.c_str(), latestVersion.c_str());
        if (MessageBoxW(nullptr, prompt, L"Timestone Updater",
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES) {
            LocalFree(argv);
            return 0;
        }
        url = Utf8ToWide(downloadUrl);
        sha256 = Utf8ToWide(digest);
        updaterUrl = Utf8ToWide(updaterDownloadUrl);
        updaterSha256 = Utf8ToWide(updaterDigest);
        const wchar_t* suppliedPid = ArgumentValue(argc, argv, L"--pid");
        const wchar_t* suppliedGame = ArgumentValue(argc, argv, L"--game-exe");
        pid = suppliedPid ? wcstoul(suppliedPid, nullptr, 10) : FindGameProcess();
        if (suppliedGame)
            gameExecutable = suppliedGame;
    } else {
        const wchar_t* pidText = ArgumentValue(argc, argv, L"--pid");
        const wchar_t* targetText = ArgumentValue(argc, argv, L"--target");
        const wchar_t* urlText = ArgumentValue(argc, argv, L"--url");
        const wchar_t* sha256Text = ArgumentValue(argc, argv, L"--sha256");
        if (!pidText || !targetText || !urlText || !sha256Text) {
            LocalFree(argv);
            ShowError(L"Missing --pid, --target, --url, or --sha256 argument.");
            return 2;
        }
        pid = wcstoul(pidText, nullptr, 10);
        target = targetText;
        url = urlText;
        sha256 = sha256Text;
    }
    std::wstring download = target + L".download";
    std::wstring zipDownload = download + L".zip";
    std::wstring extractedAsi = target + L".zip.asi";
    std::wstring backup = target + L".old";
    std::wstring pendingUpdater = target;
    size_t targetSlash = pendingUpdater.find_last_of(L"\\/");
    pendingUpdater.resize(targetSlash == std::wstring::npos ? 0 : targetSlash + 1);
    pendingUpdater += L"TimestoneUpdater.exe.new";
    LocalFree(argv);

    if (startup) {
        HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (!process) {
            ShowError(L"UMVC3 could not be opened for the update.");
            return 4;
        }
        if (!TerminateProcess(process, 0)) {
            CloseHandle(process);
            ShowError(L"UMVC3 could not be closed for the update.");
            return 4;
        }
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
    }

    DownloadProgress progress;
    CreateProgressWindow(progress);
    DeleteFileW(download.c_str());
    DeleteFileW(zipDownload.c_str());
    DeleteFileW(extractedAsi.c_str());
    const wchar_t* downloadError = nullptr;
    bool downloadValid = Download(url, download, &progress,
        L"Downloading Timestone update from tilderain/Timestone");
    if (!downloadValid)
        downloadError = L"The Timestone update could not be downloaded from GitHub.";
    bool updaterFromArchive = false;
    if (downloadValid && IsZipArchive(download)) {
        SetProgressStage(&progress, L"Verifying Timestone ZIP SHA-256 digest...");
        downloadValid = VerifySha256(download, sha256.c_str());
        if (!downloadValid) {
            downloadError = L"The downloaded Timestone ZIP did not match its GitHub SHA-256 digest.";
        } else {
            SetProgressStage(&progress, L"Extracting Timestone update...");
            downloadValid = MoveFileW(download.c_str(), zipDownload.c_str()) != FALSE;
            if (!downloadValid) {
                downloadError = L"The downloaded Timestone ZIP could not be prepared for extraction.";
            } else if (!ExtractZipPackage(zipDownload, extractedAsi,
                    pendingUpdater, updaterFromArchive)) {
                downloadValid = false;
                downloadError = L"The downloaded Timestone ZIP could not be extracted.";
            } else if (!IsPlausibleAsi(extractedAsi)) {
                downloadValid = false;
                downloadError = L"The downloaded Timestone ZIP did not contain a valid Timestone.asi.";
            } else if (updaterFromArchive &&
                !IsPlausibleAsi(pendingUpdater)) {
                downloadValid = false;
                downloadError = L"The downloaded Timestone ZIP did not contain a valid TimestoneUpdater.exe.";
            } else {
                DeleteFileW(zipDownload.c_str());
                downloadValid = MoveFileW(extractedAsi.c_str(),
                    download.c_str()) != FALSE;
                if (!downloadValid)
                    downloadError = L"The extracted Timestone.asi could not be staged for installation.";
            }
        }
    } else if (downloadValid) {
        SetProgressStage(&progress, L"Verifying GitHub SHA-256 digest...");
        if (!IsPlausibleAsi(download)) {
            downloadValid = false;
            downloadError = L"The downloaded update is not a valid Timestone.asi.";
        } else if (!VerifySha256(download, sha256.c_str())) {
            downloadValid = false;
            downloadError = L"The downloaded Timestone.asi did not match its GitHub SHA-256 digest.";
        }
    }
    if (!downloadValid) {
        DeleteFileW(download.c_str());
        DeleteFileW(zipDownload.c_str());
        DeleteFileW(extractedAsi.c_str());
        DeleteFileW(pendingUpdater.c_str());
        CloseProgressWindow(progress);
        ShowError(downloadError ? downloadError :
            L"The Timestone update failed for an unknown reason.");
        if (startup)
            RelaunchGame(gameExecutable);
        return 3;
    }

    if (!updaterFromArchive && !updaterUrl.empty()) {
        progress.startedAt = GetTickCount64();
        DeleteFileW(pendingUpdater.c_str());
        bool updaterValid = Download(
            updaterUrl, pendingUpdater, &progress,
            L"Downloading TimestoneUpdater.exe from tilderain/Timestone");
        if (updaterValid) {
            SetProgressStage(&progress,
                L"Verifying updater SHA-256 digest...");
            updaterValid = IsPlausibleAsi(pendingUpdater) &&
                VerifySha256(pendingUpdater, updaterSha256.c_str());
        }
        if (!updaterValid) {
            DeleteFileW(download.c_str());
            DeleteFileW(pendingUpdater.c_str());
            CloseProgressWindow(progress);
            ShowError(L"TimestoneUpdater.exe failed to download or did not match its GitHub SHA-256 digest.");
            if (startup)
                RelaunchGame(gameExecutable);
            return 3;
        }
    }

    if (!startup) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process) {
            if (localCheck) {
                SetProgressStage(&progress, L"Waiting for UMVC3 to close...");
                MessageBoxW(nullptr,
                    L"The update is downloaded. Close UMVC3 to finish installing it.",
                    L"Timestone Updater", MB_OK | MB_ICONINFORMATION);
            }
            WaitForSingleObject(process, INFINITE);
            CloseHandle(process);
        }
    }

    SetProgressStage(&progress, L"Installing Timestone update...");
    if (!CopyFileW(target.c_str(), backup.c_str(), FALSE)) {
        DWORD error = GetLastError();
        DeleteFileW(download.c_str());
        DeleteFileW(pendingUpdater.c_str());
        CloseProgressWindow(progress);
        ShowFileError(L"Could not create a backup of the installed Timestone.asi.", error);
        if (startup)
            RelaunchGame(gameExecutable);
        return 4;
    }
    if (!CopyFileW(download.c_str(), target.c_str(), FALSE)) {
        DWORD error = GetLastError();
        CopyFileW(backup.c_str(), target.c_str(), FALSE);
        DeleteFileW(download.c_str());
        DeleteFileW(pendingUpdater.c_str());
        CloseProgressWindow(progress);
        ShowFileError(L"Could not install the update. The previous ASI was restored.", error);
        if (startup)
            RelaunchGame(gameExecutable);
        return 5;
    }
    DeleteFileW(download.c_str());
    if (startup) {
        SetProgressStage(&progress, L"Update complete. Relaunching UMVC3...");
        for (int i = 0; i < 10; ++i) {
            Sleep(100);
            PumpWindowMessages();
        }
        if (!RelaunchGame(gameExecutable)) {
            CloseProgressWindow(progress);
            ShowError(L"Timestone was updated, but UMVC3 could not be relaunched.");
            return 6;
        }
        CloseProgressWindow(progress);
    } else if (localCheck) {
        CloseProgressWindow(progress);
        MessageBoxW(nullptr, L"Timestone was updated successfully.",
            L"Timestone Updater", MB_OK | MB_ICONINFORMATION);
    } else {
        CloseProgressWindow(progress);
    }
    return 0;
}
