// TimestoneInstall.exe: finds Ultimate Marvel vs. Capcom 3 in the player's
// Steam libraries, downloads the latest Timestone release and puts its files
// beside umvc3.exe. Run from the game folder it does the same for that
// folder, which makes it the updater too.
//
// It also runs under Wine/Proton, where it looks for the Linux Steam install
// and, since Wine prefers its own dinput8, tells the game's Proton prefix to
// load the one in the game folder.
//
// The window is drawn by tools/install_ui.h: the lobby's space backdrop and
// a modal in the style of the lobby's. This file is the Windows side.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// windows.h makes these words macros; the lobby's backdrop uses them as names.
#undef near
#undef far

#include "install_ui.h"

#ifndef TIMESTONE_VERSION
#define TIMESTONE_VERSION "0.1.0"
#endif

namespace {

constexpr wchar_t kWindowTitle[] = L"Timestone Install";
constexpr wchar_t kGameExe[] = L"umvc3.exe";
constexpr wchar_t kSteamAppId[] = L"357190";
constexpr wchar_t kDefaultInstallDir[] = L"ULTIMATE MARVEL VS. CAPCOM 3";
constexpr wchar_t kStaged[] = L".timestone-new";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
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

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The whole file, or false. Reads to the end rather than trusting the size,
// which Linux reports as zero for the files under /proc.
template <typename Bytes>
bool ReadWholeFile(const std::wstring& path, Bytes& data, size_t limit = 64u << 20)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    data.clear();
    bool ok = true;
    for (;;) {
        const size_t oldSize = data.size();
        data.resize(oldSize + 256 * 1024);
        DWORD read = 0;
        ok = ReadFile(file, &data[oldSize], 256 * 1024, &read, nullptr) != FALSE;
        data.resize(oldSize + (ok ? read : 0));
        if (!ok || !read)
            break;
        if (data.size() > limit) {
            ok = false;
            break;
        }
    }
    CloseHandle(file);
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const void* data, size_t size)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) &&
        written == size;
    const DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        SetLastError(error);
    }
    return ok;
}

std::wstring Environment(const wchar_t* name)
{
    wchar_t value[1024] = {};
    const DWORD length = GetEnvironmentVariableW(name, value, 1024);
    return length && length < 1024 ? std::wstring(value, length) : std::wstring();
}

std::wstring OwnPath()
{
    wchar_t executable[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    return length && length < MAX_PATH ? std::wstring(executable, length) : std::wstring();
}

std::wstring ParentFolder(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring Backslashes(std::wstring path)
{
    for (wchar_t& c : path)
        if (c == L'/') c = L'\\';
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    return path;
}

// ---------------------------------------------------------------------------
// Wine. Its processes see the Linux files through a drive letter, so the
// Linux Steam install can be read with the ordinary file calls.
// ---------------------------------------------------------------------------
bool RunningUnderWine()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

// The Windows name of a Linux path.
std::wstring HostPath(const std::string& unixPath)
{
    using GetDosFileName = wchar_t* (__cdecl*)(const char*);
    const auto getDosName = reinterpret_cast<GetDosFileName>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name")));
    if (getDosName) {
        if (wchar_t* name = getDosName(unixPath.c_str())) {
            std::wstring result = name;
            HeapFree(GetProcessHeap(), 0, name);
            if (!result.empty())
                return Backslashes(result);
        }
    }
    return Backslashes(L"Z:" + Utf8ToWide(unixPath)); // where Wine puts / by default
}

std::string WideToUtf8(const std::wstring& text)
{
    int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    if (length)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            result.data(), length, nullptr, nullptr);
    return result;
}

// Whether the game is running on the Linux side. This process is in another
// Wine prefix than the game's, so its own process list does not show it.
bool HostGameRunning()
{
    const std::wstring proc = HostPath("/proc");
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((proc + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
        return false;
    bool running = false;
    do {
        if (entry.cFileName[0] < L'0' || entry.cFileName[0] > L'9')
            continue;
        std::string name;
        if (!ReadWholeFile(proc + L"\\" + entry.cFileName + L"\\comm", name, 4096))
            continue;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
            name.pop_back();
        running = _stricmp(name.c_str(), "umvc3.exe") == 0;
    } while (!running && FindNextFileW(search, &entry));
    FindClose(search);
    return running;
}

bool GameRunning()
{
    if (RunningUnderWine())
        return HostGameRunning();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            running = _wcsicmp(entry.szExeFile, kGameExe) == 0;
        } while (!running && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

// ---------------------------------------------------------------------------
// Finding the game: Steam's folder, the libraries it lists, and the game's
// manifest in one of them.
// ---------------------------------------------------------------------------
std::wstring RegistryString(HKEY root, const wchar_t* key, const wchar_t* value, REGSAM view)
{
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &handle) != ERROR_SUCCESS)
        return {};
    wchar_t text[1024] = {};
    DWORD size = sizeof(text) - sizeof(wchar_t), type = 0;
    const bool ok = RegQueryValueExW(handle, value, nullptr, &type,
        reinterpret_cast<BYTE*>(text), &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(handle);
    return ok ? std::wstring(text) : std::wstring();
}

void AddUnique(std::vector<std::wstring>& list, const std::wstring& path)
{
    if (path.empty())
        return;
    const std::wstring clean = Backslashes(path);
    for (const std::wstring& known : list)
        if (_wcsicmp(known.c_str(), clean.c_str()) == 0)
            return;
    list.push_back(clean);
}

std::vector<std::wstring> SteamFolders()
{
    std::vector<std::wstring> folders;
    if (RunningUnderWine()) {
        // Set by Steam for whatever it launches, the installer included when
        // it is run as a non-Steam game.
        const std::wstring client = Environment(L"STEAM_COMPAT_CLIENT_INSTALL_PATH");
        if (!client.empty())
            AddUnique(folders, HostPath(WideToUtf8(client)));
        const std::string home = WideToUtf8(Environment(L"HOME"));
        if (!home.empty()) {
            for (const char* place : {
                    "/.steam/steam",
                    "/.local/share/Steam",
                    "/.var/app/com.valvesoftware.Steam/.local/share/Steam", // Flatpak
                    "/snap/steam/common/.local/share/Steam",                // Snap
                    "/.steam/root"})
                AddUnique(folders, HostPath(home + place));
        }
        return folders;
    }
    AddUnique(folders, RegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam",
        L"SteamPath", 0));
    AddUnique(folders, RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",
        L"InstallPath", KEY_WOW64_32KEY));
    AddUnique(folders, RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam",
        L"InstallPath", KEY_WOW64_64KEY));
    return folders;
}

bool IsGameFolder(const std::wstring& folder)
{
    return !folder.empty() && FileExists(folder + L"\\" + kGameExe);
}

std::wstring GameInLibrary(const std::wstring& library)
{
    const std::wstring apps = library + L"\\steamapps\\";
    std::string manifest;
    if (ReadWholeFile(apps + L"appmanifest_" + kSteamAppId + L".acf", manifest, 1u << 20)) {
        for (const std::string& folder : install::VdfValues(manifest, "installdir")) {
            const std::wstring candidate = apps + L"common\\" + Utf8ToWide(folder);
            if (IsGameFolder(candidate))
                return candidate;
        }
    }
    const std::wstring usual = apps + L"common\\" + kDefaultInstallDir;
    return IsGameFolder(usual) ? usual : std::wstring();
}

std::wstring FindGame()
{
    const bool wine = RunningUnderWine();
    std::vector<std::wstring> libraries;
    for (const std::wstring& steam : SteamFolders()) {
        AddUnique(libraries, steam);
        for (const wchar_t* list : {L"\\config\\libraryfolders.vdf",
                                    L"\\steamapps\\libraryfolders.vdf"}) {
            std::string text;
            if (!ReadWholeFile(steam + list, text, 4u << 20))
                continue;
            for (const std::string& path : install::VdfValues(text, "path"))
                AddUnique(libraries, wine ? HostPath(path) : Utf8ToWide(path));
        }
    }
    for (const std::wstring& library : libraries) {
        const std::wstring game = GameInLibrary(library);
        if (!game.empty())
            return game;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------
// The version of an installed Timestone.asi; false if there is no such file.
// Windows' own version lookup is not used: releases up to 0.6.6 carry their
// version resource under a name instead of the number 1 it looks for, so it
// finds nothing in them. The text is found in the file itself instead.
bool GetInstalledVersion(const std::wstring& path, std::wstring& version)
{
    std::vector<unsigned char> file;
    if (!ReadWholeFile(path, file))
        return false;
    version = Utf8ToWide(install::PeProductVersion(file));
    if (version.empty())
        version = L"(unknown version)"; // still installed: it can be updated or removed
    return true;
}

using install::IsNewerVersion;

// ---------------------------------------------------------------------------
// What the window shows. The worker thread writes it, the window reads it.
// ---------------------------------------------------------------------------
enum class Stage {
    Searching,
    NotFound,
    Ready,
    WaitGame,
    Working,
    Done,
    Removed,
    Failed
};

enum class Task { None, Detect, Install };

struct State {
    Stage stage = Stage::Searching;
    std::wstring gameDir;
    std::wstring installed; // version of the Timestone.asi already there, if any
    std::wstring latest;    // release tag, "v0.6.6"
    std::wstring url;
    std::wstring sha256;
    std::wstring title;     // Failed
    std::wstring message;   // NotFound, Working, Done, Failed
    std::wstring status;    // Working
    float fraction = -1.0f; // Working
    bool committing = false; // files are being swapped: the window stays open
    bool protonPending = false; // installed, but the game's Proton prefix is not there yet
    Task retry = Task::None; // what RETRY runs again
};

CRITICAL_SECTION g_lock;
State g_state;          // under g_lock
std::wstring g_chosenDir; // under g_lock; a folder the player picked
HANDLE g_worker = nullptr;

struct Locked {
    Locked() { EnterCriticalSection(&g_lock); }
    ~Locked() { LeaveCriticalSection(&g_lock); }
};

void SetWorking(const std::wstring& message, float fraction = -1.0f,
    const std::wstring& status = L"")
{
    Locked lock;
    g_state.stage = Stage::Working;
    g_state.message = message;
    g_state.fraction = fraction;
    g_state.status = status;
}

void SetDownloadProgress(ULONGLONG downloaded, ULONGLONG total)
{
    wchar_t status[96] = {};
    if (total)
        swprintf(status, 96, L"%.1f MB of %.1f MB", downloaded / 1048576.0, total / 1048576.0);
    else
        swprintf(status, 96, L"%.1f MB", downloaded / 1048576.0);
    Locked lock;
    g_state.fraction = total ? static_cast<float>(static_cast<double>(downloaded) / total) : -1.0f;
    g_state.status = status;
}

void Fail(Task retry, const wchar_t* title, const std::wstring& message)
{
    Locked lock;
    g_state.stage = Stage::Failed;
    g_state.committing = false;
    g_state.title = title;
    g_state.message = message;
    g_state.retry = retry;
}

std::wstring WithFileError(const wchar_t* message, const std::wstring& file, DWORD error)
{
    wchar_t detail[160] = {};
    swprintf(detail, 160, L"\n\nWindows error %lu on ", error);
    return std::wstring(message) + detail + file;
}

// ---------------------------------------------------------------------------
// GitHub
// ---------------------------------------------------------------------------
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

HINTERNET OpenSession()
{
    HINTERNET session = WinHttpOpen(L"TimestoneInstall/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (session) {
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
            &secureProtocols, sizeof(secureProtocols));
        WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    }
    return session;
}

bool SendGet(HINTERNET request)
{
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
        return false;
    DWORD status = 0;
    DWORD size = sizeof(status);
    return WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
        WINHTTP_NO_HEADER_INDEX) && status == 200;
}

bool FetchLatestRelease(std::string& response)
{
    HINTERNET session = OpenSession();
    HINTERNET connection = session ? WinHttpConnect(session, L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET",
        L"/repos/tilderain/Timestone/releases/latest", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    const bool ok = SendGet(request) && ReadResponse(request, response);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

using install::ExtractJsonString;
using install::ParseReleaseAsset;

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

bool Download(const std::wstring& url, const std::wstring& destination)
{
    URL_COMPONENTS parts;
    std::vector<wchar_t> host;
    std::vector<wchar_t> path;
    if (!CrackUrl(url, parts, host, path))
        return false;

    HINTERNET session = OpenSession();
    HINTERNET connection = session ? WinHttpConnect(
        session, host.data(), parts.nPort, 0) : nullptr;
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = connection ? WinHttpOpenRequest(
        connection, L"GET", path.data(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags) : nullptr;
    bool ok = SendGet(request);

    ULONGLONG total = 0;
    if (ok) {
        wchar_t contentLength[64] = {};
        DWORD contentLengthSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX, contentLength,
                &contentLengthSize, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(contentLength, nullptr, 10);
        SetDownloadProgress(0, total);
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
            SetDownloadProgress(downloaded, total);
        }
    }
    if (ok && total && downloaded != total)
        ok = false;
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

bool Sha256Matches(const std::vector<unsigned char>& data, const std::wstring& expected)
{
    if (expected.size() != 64)
        return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    const bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) == 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, const_cast<PUCHAR>(data.data()),
            static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        return false;
    static const wchar_t kHex[] = L"0123456789abcdef";
    wchar_t actual[65] = {};
    for (size_t i = 0; i < sizeof(digest); ++i) {
        actual[i * 2] = kHex[digest[i] >> 4];
        actual[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return _wcsicmp(actual, expected.c_str()) == 0;
}

enum class ProtonLoader {
    Set,      // the game's prefix loads the dinput8 beside the game
    NoPrefix, // the game has not been started under Proton yet
    Failed
};

// The prefix Steam keeps for the game sits beside the library's "common"
// folder: steamapps/compatdata/<appid>/pfx.
ProtonLoader EnableProtonLoader(const std::wstring& gameDir)
{
    const std::wstring registry = ParentFolder(ParentFolder(gameDir)) + L"\\compatdata\\" +
        kSteamAppId + L"\\pfx\\user.reg";
    std::string text;
    if (!ReadWholeFile(registry, text))
        return ProtonLoader::NoPrefix;
    switch (install::AddWineDllOverride(text, "umvc3.exe", "dinput8",
            static_cast<unsigned>(time(nullptr)))) {
    case install::OverrideResult::Present:
        return ProtonLoader::Set;
    case install::OverrideResult::NotRegistry:
        return ProtonLoader::Failed;
    case install::OverrideResult::Added:
        break;
    }
    const std::wstring staged = registry + kStaged;
    if (WriteWholeFile(staged, text.data(), text.size()) &&
        MoveFileExW(staged.c_str(), registry.c_str(), MOVEFILE_REPLACE_EXISTING))
        return ProtonLoader::Set;
    DeleteFileW(staged.c_str());
    return ProtonLoader::Failed;
}

// ---------------------------------------------------------------------------
// Looking: where the game is, what is installed there, what the latest is.
// ---------------------------------------------------------------------------
DWORD WINAPI DetectThread(void*)
{
    std::wstring chosen;
    bool haveRelease;
    {
        Locked lock;
        g_state.stage = Stage::Searching;
        chosen = g_chosenDir;
        haveRelease = !g_state.url.empty();
    }

    std::wstring gameDir = chosen;
    if (gameDir.empty()) {
        // Beside the game already: this is the copy the release puts there.
        const std::wstring own = ParentFolder(OwnPath());
        gameDir = IsGameFolder(own) ? own : FindGame();
    }
    std::wstring installed;
    if (!gameDir.empty())
        GetInstalledVersion(gameDir + L"\\Timestone.asi", installed);
    // An install made before the game had a Proton prefix is finished here,
    // on the run after the game was first started.
    const bool protonPending = !installed.empty() && RunningUnderWine() &&
        !GameRunning() && EnableProtonLoader(gameDir) == ProtonLoader::NoPrefix;

    std::string json, tag, url, sha256;
    if (!haveRelease) {
        if (!FetchLatestRelease(json) || !ExtractJsonString(json, "tag_name", 0, tag) ||
            !ParseReleaseAsset(json, "Timestone.zip", url, sha256)) {
            Fail(Task::Detect, L"NO CONNECTION",
                L"The latest Timestone release could not be read from GitHub.\n"
                L"Check your connection and try again.");
            return 0;
        }
    }

    Locked lock;
    if (!haveRelease) {
        g_state.latest = Utf8ToWide(tag);
        g_state.url = Utf8ToWide(url);
        g_state.sha256 = Utf8ToWide(sha256);
    }
    g_state.gameDir = gameDir;
    g_state.installed = installed;
    g_state.protonPending = protonPending;
    if (gameDir.empty()) {
        g_state.stage = Stage::NotFound;
        g_state.message = L"Ultimate Marvel vs. Capcom 3 was not found in your Steam libraries.\n"
                          L"Choose the folder that holds umvc3.exe.";
    } else {
        g_state.stage = Stage::Ready;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

// Puts `staged` in place of `destination`. A file that cannot be replaced
// while it is open (this program, when it runs from the game folder) can
// still be renamed out of the way.
bool PutInPlace(const std::wstring& staged, const std::wstring& destination)
{
    if (MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    const std::wstring old = destination + L".old";
    if (!MoveFileExW(destination.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
        return false;
    if (MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    const DWORD error = GetLastError();
    MoveFileExW(old.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING);
    SetLastError(error);
    return false;
}

DWORD WINAPI InstallThread(void*)
{
    std::wstring gameDir, tag, url, sha256;
    {
        Locked lock;
        gameDir = g_state.gameDir;
        tag = g_state.latest;
        url = g_state.url;
        sha256 = g_state.sha256;
    }

    if (GameRunning()) {
        {
            Locked lock;
            g_state.stage = Stage::WaitGame;
        }
        while (GameRunning())
            Sleep(500);
        Sleep(500); // let it let go of its files
    }

    SetWorking(L"Downloading Timestone " + tag);
    wchar_t tempFolder[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempFolder))
        tempFolder[0] = 0;
    const std::wstring archive = std::wstring(tempFolder) + L"Timestone-install.zip";
    if (!Download(url, archive)) {
        Fail(Task::Install, L"DOWNLOAD FAILED",
            L"Timestone " + tag + L" could not be downloaded from GitHub.\n"
            L"Check your connection and try again.");
        return 0;
    }

    SetWorking(L"Checking the download", 1.0f);
    std::vector<unsigned char> zip;
    const bool read = ReadWholeFile(archive, zip);
    DeleteFileW(archive.c_str());
    if (!read || !Sha256Matches(zip, sha256)) {
        Fail(Task::Install, L"DOWNLOAD FAILED",
            L"The download did not match the SHA-256 digest GitHub lists for it.\n"
            L"Nothing was installed. Try again.");
        return 0;
    }
    std::vector<install::ZipFile> files;
    const install::ZipFile* plugin = nullptr;
    if (install::ListStoredZip(zip, files)) {
        for (const install::ZipFile& file : files)
            if (file.name == "Timestone.asi")
                plugin = &file;
    }
    if (!plugin || plugin->size < 64 * 1024 || zip[plugin->offset] != 'M' ||
        zip[plugin->offset + 1] != 'Z') {
        Fail(Task::Install, L"INSTALL FAILED",
            L"The Timestone " + tag + L" package is not one this installer can read.\n"
            L"Nothing was installed. Download the newest TimestoneInstall.exe.");
        return 0;
    }

    // Everything is written beside its destination first, so a full disk or
    // a folder that cannot be written leaves the game as it was.
    SetWorking(L"Installing Timestone " + tag, 1.0f);
    const std::wstring folder = gameDir + L"\\";
    std::vector<std::wstring> names;
    for (const install::ZipFile& file : files)
        names.push_back(Utf8ToWide(file.name));
    for (size_t i = 0; i < files.size(); ++i) {
        if (WriteWholeFile(folder + names[i] + kStaged, zip.data() + files[i].offset,
                files[i].size))
            continue;
        const DWORD error = GetLastError();
        for (size_t written = 0; written <= i; ++written)
            DeleteFileW((folder + names[written] + kStaged).c_str());
        Fail(Task::Install, L"INSTALL FAILED", WithFileError(
            L"The new files could not be written into the game folder.\n"
            L"Nothing was changed.", names[i], error));
        return 0;
    }

    {
        Locked lock;
        g_state.committing = true;
    }
    // The plugin first: if it cannot be replaced, nothing has been yet.
    std::vector<size_t> order;
    for (size_t i = 0; i < files.size(); ++i)
        if (&files[i] == plugin) order.push_back(i);
    for (size_t i = 0; i < files.size(); ++i)
        if (&files[i] != plugin) order.push_back(i);
    const std::wstring pluginPath = folder + L"Timestone.asi";
    if (FileExists(pluginPath))
        CopyFileW(pluginPath.c_str(), (pluginPath + L".old").c_str(), FALSE);
    for (const size_t index : order) {
        if (PutInPlace(folder + names[index] + kStaged, folder + names[index]))
            continue;
        const DWORD error = GetLastError();
        for (const std::wstring& name : names)
            DeleteFileW((folder + name + kStaged).c_str());
        Fail(Task::Install, L"INSTALL FAILED", WithFileError(
            L"The new files could not be put into the game folder.\n"
            L"Close the game and anything else using the folder, then try again.",
            names[index], error));
        return 0;
    }
    // The updater's name before it became this installer, unless this
    // release still ships it.
    bool shipsUpdater = false;
    for (const install::ZipFile& file : files)
        shipsUpdater |= file.name == "TimestoneUpdater.exe";
    if (!shipsUpdater)
        DeleteFileW((folder + L"TimestoneUpdater.exe").c_str());
    DeleteFileW((folder + L"TimestoneUpdater.exe.new").c_str());

    std::wstring message = L"Timestone " + tag + L" is installed.";
    if (RunningUnderWine()) {
        switch (EnableProtonLoader(gameDir)) {
        case ProtonLoader::Set:
            message += L"\nProton is set to load it. Start the game from Steam.";
            break;
        case ProtonLoader::NoPrefix:
            message += L"\n\nOne more step: start the game once from Steam, close it, then run "
                       L"this installer again so it can tell Proton to load Timestone.";
            break;
        case ProtonLoader::Failed:
            message += L"\n\nThe game's Proton settings could not be changed. Put this in the "
                       L"game's Steam launch options instead:\n"
                       L"WINEDLLOVERRIDES=\"dinput8=n,b\" %command%";
            break;
        }
    }

    Locked lock;
    g_state.stage = Stage::Done;
    g_state.committing = false;
    g_state.message = message;
    return 0;
}

// Uninstalling takes out what the installer put in: the plugin and its
// backup, the notices and the old updater. The loader (dinput8.dll, Ultimate
// ASI Loader) goes too unless another .asi mod is there to need it. Settings
// (Timestone.ini), logs and dumps stay.
bool OtherPluginsPresent(const std::wstring& gameDir)
{
    // Where the loader looks for plugins.
    for (const wchar_t* folder : {L"", L"\\plugins", L"\\scripts", L"\\update"}) {
        WIN32_FIND_DATAW entry = {};
        HANDLE search = FindFirstFileW((gameDir + folder + L"\\*.asi").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE)
            continue;
        // "*.asi" also matches longer extensions that begin with it.
        bool found = false;
        do {
            const size_t length = wcslen(entry.cFileName);
            found = length > 4 && _wcsicmp(entry.cFileName + length - 4, L".asi") == 0;
        } while (!found && FindNextFileW(search, &entry));
        FindClose(search);
        if (found)
            return true;
    }
    return false;
}

void Uninstall()
{
    std::wstring gameDir;
    {
        Locked lock;
        gameDir = g_state.gameDir;
    }
    if (GameRunning()) {
        Fail(Task::Detect, L"UNINSTALL FAILED",
            L"Ultimate Marvel vs. Capcom 3 is running.\nClose it, then uninstall.");
        return;
    }
    const auto remove = [&gameDir](const wchar_t* name) {
        const std::wstring path = gameDir + L"\\" + name;
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); // a read-only file is still ours
        if (DeleteFileW(path.c_str()))
            return true;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return true;
        Fail(Task::Detect, L"UNINSTALL FAILED", WithFileError(
            L"Timestone could not be removed from the game folder.", name, error));
        return false;
    };
    // The plugin first: once it is gone the game starts without Timestone,
    // whatever else is left.
    for (const wchar_t* name : {L"Timestone.asi", L"Timestone.asi.old",
            L"Timestone-THIRDPARTYNOTICES", L"TimestoneUpdater.exe",
            L"TimestoneUpdater.exe.new"})
        if (!remove(name))
            return;
    const bool keepLoader = OtherPluginsPresent(gameDir);
    if (!keepLoader && !remove(L"dinput8.dll"))
        return;

    Locked lock;
    g_state.stage = Stage::Removed;
    g_state.installed.clear();
    g_state.protonPending = false;
    g_state.message = keepLoader
        ? L"dinput8.dll was kept: another .asi mod in the folder needs it.\n"
          L"Timestone was removed from:"
        : L"Timestone was removed from:";
}

void StartTask(Task task)
{
    if (g_worker) {
        if (WaitForSingleObject(g_worker, 0) != WAIT_OBJECT_0)
            return; // still busy
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    {
        Locked lock;
        g_state.stage = task == Task::Install ? Stage::Working : Stage::Searching;
        g_state.message = task == Task::Install ? L"Getting ready" : L"";
        g_state.fraction = -1.0f;
        g_state.status.clear();
    }
    g_worker = CreateThread(nullptr, 0, task == Task::Install ? InstallThread : DetectThread,
        nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// Text, drawn by GDI straight into the picture.
// ---------------------------------------------------------------------------
class GdiText final : public install::TextPainter {
public:
    GdiText(HDC dc, float scale) : dc_(dc), scale_(scale)
    {
        static const int kWeight[4] = {FW_BOLD, FW_BOLD, FW_NORMAL, FW_NORMAL};
        for (int i = 0; i < 4; ++i) {
            LOGFONTW font = {};
            // The heights in install_ui.h are a line's; the letters are a little less.
            font.lfHeight = -static_cast<int>(install::kFontHeight[i] * 0.80f * scale + 0.5f);
            font.lfWeight = kWeight[i];
            font.lfCharSet = DEFAULT_CHARSET;
            font.lfQuality = CLEARTYPE_QUALITY;
            font.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
            // Wine has no Segoe UI and would pick whatever it likes; Tahoma
            // comes with every Wine and Proton.
            wcscpy(font.lfFaceName, RunningUnderWine() ? L"Tahoma" : L"Segoe UI");
            fonts_[i] = CreateFontIndirectW(&font);
        }
        SetBkMode(dc_, TRANSPARENT);
    }

    ~GdiText() override
    {
        for (HFONT font : fonts_)
            if (font) DeleteObject(font);
    }

    float BlockHeight(install::FontId font, const wchar_t* text, float width,
        unsigned flags) override
    {
        RECT box = {0, 0, static_cast<LONG>(width * scale_), 0};
        HGDIOBJ previous = SelectObject(dc_, fonts_[static_cast<int>(font)]);
        DrawTextW(dc_, text, -1, &box, DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX |
            (flags & install::kTextPath ? kPathBreaks : 0));
        SelectObject(dc_, previous);
        return box.bottom / scale_ + 2.0f;
    }

    void Draw(install::FontId font, const wchar_t* text, install::Rect rect,
        install::Color color, unsigned flags) override
    {
        if (!text[0])
            return;
        RECT box = {static_cast<LONG>(rect.x * scale_), static_cast<LONG>(rect.y * scale_),
            static_cast<LONG>((rect.x + rect.w) * scale_),
            static_cast<LONG>((rect.y + rect.h) * scale_)};
        UINT format = DT_NOPREFIX;
        if (flags & install::kTextRight)
            format |= DT_RIGHT | DT_SINGLELINE | DT_VCENTER;
        else if (flags & install::kTextPath)
            format |= DT_CENTER | DT_WORDBREAK | kPathBreaks;
        else if (flags & install::kTextMiddle)
            format |= DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
        else
            format |= DT_CENTER | DT_WORDBREAK;

        HGDIOBJ previous = SelectObject(dc_, fonts_[static_cast<int>(font)]);
        if (flags & install::kTextOutline) {
            const LONG edge = scale_ >= 1.25f ? 2 : 1;
            SetTextColor(dc_, RGB(8, 10, 18));
            for (LONG dy = -edge; dy <= edge; ++dy) {
                for (LONG dx = -edge; dx <= edge; ++dx) {
                    if (!dx && !dy)
                        continue;
                    RECT shifted = box;
                    OffsetRect(&shifted, dx, dy);
                    DrawTextW(dc_, text, -1, &shifted, format);
                }
            }
        }
        const auto channel = [](float value) {
            return static_cast<int>((value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value) * 255.0f);
        };
        SetTextColor(dc_, RGB(channel(color.r), channel(color.g), channel(color.b)));
        DrawTextW(dc_, text, -1, &box, format);
        SelectObject(dc_, previous);
        GdiFlush(); // the shapes around the text are written to the pixels directly
    }

private:
    // A path is shown whole: broken at its spaces, and inside a name that is
    // longer than a line, as an edit box breaks one.
    static constexpr UINT kPathBreaks = DT_EDITCONTROL;

    HDC dc_;
    float scale_;
    HFONT fonts_[4] = {};
};

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------
enum class Action { None, Install, Uninstall, Browse, Close, Retry, Launch };

struct Window {
    HWND handle = nullptr;
    HDC dc = nullptr;          // memory DC holding the picture
    HBITMAP bitmap = nullptr;
    uint32_t* pixels = nullptr;
    int width = 0, height = 0;
    std::vector<uint8_t> stars;
    GdiText* text = nullptr;
    ULONGLONG startedAt = 0;
    Stage shownStage = Stage::Searching;
    int cursor = 0;
    int buttonCount = 0;
    Action actions[install::kMaxButtons] = {};
    install::Layout layout = {};
};

Window g_window;

bool LoadStars(std::vector<uint8_t>& rgba)
{
    HRSRC resource = FindResourceW(nullptr, L"SPACE_STARS", RT_RCDATA);
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const void* tile = loaded ? LockResource(loaded) : nullptr;
    if (!tile)
        return false;
    rgba.resize(lobby::kSpaceStarsRgbaBytes);
    if (lobby::BuildSpaceStarsRgba(static_cast<const uint8_t*>(tile),
            SizeofResource(nullptr, resource), rgba.data()))
        return true;
    rgba.clear();
    return false;
}

bool CloseAllowed()
{
    Locked lock;
    return !g_state.committing;
}

void BrowseForGame(HWND owner)
{
    BROWSEINFOW browse = {};
    browse.hwndOwner = owner;
    browse.lpszTitle = L"Choose the Ultimate Marvel vs. Capcom 3 folder (the one with umvc3.exe)";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    PIDLIST_ABSOLUTE picked = SHBrowseForFolderW(&browse);
    if (!picked)
        return;
    wchar_t path[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListW(picked, path) != FALSE;
    CoTaskMemFree(picked);
    if (!ok)
        return;
    const std::wstring folder = Backslashes(path);
    if (!IsGameFolder(folder)) {
        Locked lock;
        g_state.stage = Stage::NotFound;
        g_state.message = L"umvc3.exe is not in that folder.\n"
                          L"Choose the folder that holds umvc3.exe.";
        return;
    }
    {
        Locked lock;
        g_chosenDir = folder;
    }
    StartTask(Task::Detect);
}

// Starts the game through Steam's own link, or failing that the game's
// program (which hands itself to Steam).
void LaunchGame()
{
    const std::wstring link = std::wstring(L"steam://rungameid/") + kSteamAppId;
    SHELLEXECUTEINFOW launch = {};
    launch.cbSize = sizeof(launch);
    // This program exits as soon as the link is handed over. Without
    // SEE_MASK_NOASYNC the hand-over may still be running on a background
    // thread at that point, and is lost with the process.
    launch.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    launch.lpFile = link.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&launch))
        return;

    std::wstring gameDir;
    {
        Locked lock;
        gameDir = g_state.gameDir;
    }
    const std::wstring program = gameDir + L"\\" + kGameExe;
    launch.lpFile = program.c_str();
    launch.lpDirectory = gameDir.c_str();
    ShellExecuteExW(&launch);
}

void Perform(Action action)
{
    switch (action) {
    case Action::Install:
        StartTask(Task::Install);
        break;
    case Action::Uninstall:
        if (!g_worker || WaitForSingleObject(g_worker, 0) == WAIT_OBJECT_0)
            Uninstall();
        break;
    case Action::Retry: {
        Task task;
        {
            Locked lock;
            task = g_state.retry;
        }
        StartTask(task == Task::Install ? Task::Install : Task::Detect);
        break;
    }
    case Action::Browse:
        BrowseForGame(g_window.handle);
        break;
    case Action::Launch:
        LaunchGame();
        DestroyWindow(g_window.handle);
        break;
    case Action::Close:
        if (CloseAllowed())
            DestroyWindow(g_window.handle);
        break;
    case Action::None:
        break;
    }
}

void Render()
{
    Window& window = g_window;
    State state;
    {
        Locked lock;
        state = g_state;
    }
    if (state.stage != window.shownStage) {
        window.shownStage = state.stage;
        window.cursor = 0;
    }

    install::View view;
    std::wstring message;
    const auto button = [&](const wchar_t* label, Action action) {
        view.buttons[view.buttonCount] = label;
        window.actions[view.buttonCount] = action;
        ++view.buttonCount;
    };
    view.footer = L"Timestone Install " TIMESTONE_VERSION;
    switch (state.stage) {
    case Stage::Searching:
        view.title = L"TIMESTONE";
        view.message = L"Looking for Ultimate Marvel vs. Capcom 3...";
        view.busy = true;
        break;
    case Stage::NotFound:
        view.title = L"GAME NOT FOUND";
        view.message = state.message.c_str();
        button(L"BROWSE", Action::Browse);
        button(L"CLOSE", Action::Close);
        break;
    case Stage::Ready: {
        const wchar_t* go = L"INSTALL";
        if (state.installed.empty()) {
            view.title = L"INSTALL TIMESTONE";
            message = L"Timestone " + state.latest + L" will be installed into:";
        } else if (IsNewerVersion(state.latest.c_str(), state.installed.c_str())) {
            view.title = L"UPDATE TIMESTONE";
            message = L"Timestone " + state.installed + L" will be updated to " + state.latest +
                L" in:";
            go = L"UPDATE";
        } else {
            view.title = L"TIMESTONE IS UP TO DATE";
            message = L"Timestone " + state.installed + L" is already installed in:";
            go = L"REINSTALL";
        }
        if (state.protonPending)
            message = L"Start the game once from Steam, close it, then run this installer "
                      L"again so it can tell Proton to load Timestone.\n\n" + message;
        view.message = message.c_str();
        view.detail = state.gameDir.c_str();
        button(go, Action::Install);
        if (!state.installed.empty())
            button(L"UNINSTALL", Action::Uninstall);
        button(L"BROWSE", Action::Browse);
        button(L"CLOSE", Action::Close);
        break;
    }
    case Stage::WaitGame:
        view.title = L"CLOSE THE GAME";
        view.message = L"Ultimate Marvel vs. Capcom 3 is running.\n"
                       L"The install continues when it closes.";
        view.busy = true;
        button(L"CLOSE", Action::Close);
        break;
    case Stage::Working:
        view.title = L"INSTALLING TIMESTONE";
        view.message = state.message.c_str();
        view.progress = true;
        view.fraction = state.fraction;
        view.status = state.status.c_str();
        break;
    case Stage::Done:
        view.title = L"TIMESTONE INSTALLED";
        view.message = state.message.c_str();
        // Steam's own link; under Wine there is no Steam on this side to take it.
        if (!RunningUnderWine())
            button(L"LAUNCH GAME", Action::Launch);
        button(L"CLOSE", Action::Close);
        break;
    case Stage::Removed:
        view.title = L"TIMESTONE REMOVED";
        view.message = state.message.c_str();
        view.detail = state.gameDir.c_str();
        button(L"CLOSE", Action::Close);
        break;
    case Stage::Failed:
        view.title = state.title.c_str();
        view.message = state.message.c_str();
        button(L"RETRY", Action::Retry);
        button(L"CLOSE", Action::Close);
        break;
    }
    if (window.cursor >= view.buttonCount)
        window.cursor = view.buttonCount > 0 ? view.buttonCount - 1 : 0;
    view.cursor = window.cursor;
    window.buttonCount = view.buttonCount;

    GdiFlush();
    install::SoftCanvas canvas(window.pixels, window.width, window.height,
        window.stars.empty() ? nullptr : window.stars.data());
    const uint32_t frame = static_cast<uint32_t>(
        (GetTickCount64() - window.startedAt) * 60 / 1000);
    window.layout = install::DrawScreen(canvas, *window.text, view, frame);
    GdiFlush();

    HDC screen = GetDC(window.handle);
    BitBlt(screen, 0, 0, window.width, window.height, window.dc, 0, 0, SRCCOPY);
    ReleaseDC(window.handle, screen);
}

int ButtonAt(LPARAM position)
{
    const float scale = g_window.width / install::kScreenW;
    const float x = static_cast<short>(LOWORD(position)) / scale;
    const float y = static_cast<short>(HIWORD(position)) / scale;
    for (int i = 0; i < g_window.buttonCount; ++i)
        if (install::Contains(g_window.layout.buttons[i], x, y))
            return i;
    return -1;
}

LRESULT CALLBACK WindowProc(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
{
    Window& window = g_window;
    switch (message) {
    case WM_TIMER:
        Render();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC screen = BeginPaint(handle, &paint);
        BitBlt(screen, 0, 0, window.width, window.height, window.dc, 0, 0, SRCCOPY);
        EndPaint(handle, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int over = ButtonAt(lParam);
        if (over >= 0)
            window.cursor = over;
        return 0;
    }
    case WM_LBUTTONUP: {
        const int over = ButtonAt(lParam);
        if (over >= 0) {
            window.cursor = over;
            Perform(window.actions[over]);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            Perform(Action::Close);
        } else if (window.buttonCount > 0) {
            if (wParam == VK_LEFT || wParam == VK_UP)
                window.cursor = (window.cursor + window.buttonCount - 1) % window.buttonCount;
            else if (wParam == VK_RIGHT || wParam == VK_DOWN || wParam == VK_TAB)
                window.cursor = (window.cursor + 1) % window.buttonCount;
            else if (wParam == VK_RETURN || wParam == VK_SPACE)
                Perform(window.actions[window.cursor]);
        }
        return 0;
    case WM_CLOSE:
        if (CloseAllowed())
            DestroyWindow(handle);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(handle, message, wParam, lParam);
}

bool CreateInstallerWindow(HINSTANCE instance)
{
    Window& window = g_window;
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = L"TimestoneInstall";
    if (!RegisterClassW(&windowClass))
        return false;

    // 1120 x 630 at ordinary scaling, larger with the display's, and never
    // more than most of the screen. The picture is drawn by this program, so
    // it is kept to a size a processor fills comfortably.
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    RECT work = {0, 0, 1280, 720};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int width = MulDiv(1120, dpi, 96);
    const int roomW = (work.right - work.left) * 9 / 10;
    const int roomH = (work.bottom - work.top) * 85 / 100;
    if (width > 1600) width = 1600;
    if (width > roomW) width = roomW;
    if (width * 9 / 16 > roomH) width = roomH * 16 / 9;
    if (width < 640) width = 640;
    window.width = width;
    window.height = width * 9 / 16;

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT frame = {0, 0, window.width, window.height};
    AdjustWindowRect(&frame, style, FALSE);
    const int outerW = frame.right - frame.left, outerH = frame.bottom - frame.top;
    window.handle = CreateWindowExW(0, windowClass.lpszClassName, kWindowTitle, style,
        work.left + (work.right - work.left - outerW) / 2,
        work.top + (work.bottom - work.top - outerH) / 2, outerW, outerH,
        nullptr, nullptr, instance, nullptr);
    if (!window.handle)
        return false;

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = window.width;
    info.bmiHeader.biHeight = -window.height; // top row first
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    window.dc = CreateCompatibleDC(nullptr);
    window.bitmap = window.dc ? CreateDIBSection(window.dc, &info, DIB_RGB_COLORS, &bits,
        nullptr, 0) : nullptr;
    if (!window.bitmap || !bits)
        return false;
    SelectObject(window.dc, window.bitmap);
    window.pixels = static_cast<uint32_t*>(bits);

    LoadStars(window.stars); // without them the backdrop is its grid on black
    window.text = new GdiText(window.dc, window.width / install::kScreenW);
    window.startedAt = GetTickCount64();
    Render();
    ShowWindow(window.handle, SW_SHOWNORMAL);
    SetForegroundWindow(window.handle);
    SetTimer(window.handle, 1, 33, nullptr);
    return true;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    InitializeCriticalSection(&g_lock);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // the folder picker

    // What a copy of this program left behind when it replaced itself.
    const std::wstring own = OwnPath();
    if (!own.empty())
        DeleteFileW((own + L".old").c_str());

    if (!CreateInstallerWindow(instance)) {
        MessageBoxW(nullptr, L"The Timestone Install window could not be opened.",
            kWindowTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    StartTask(Task::Detect);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    // A worker still downloading ends with the process.
    return 0;
}
