#include "updater.h"
#include "LaunchMenu.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern void HookLog(const char* fmt, ...);
extern void HookLogFlush();

namespace {

constexpr const wchar_t* kUserAgent = L"Timestone/" UMVC3HOOK_VERSION;
constexpr const wchar_t* kReleaseHost = L"api.github.com";
constexpr const wchar_t* kReleasePath = L"/repos/tilderain/Timestone/releases/latest";
constexpr const char* kWaitPidArgument = "-timestone-wait-pid";
constexpr DWORD kWaitForOldProcessMs = 30000;

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
struct Paths {
    std::string asi;      // the loaded Timestone.asi
    std::string download; // what GitHub sent (ZIP or ASI)
    std::string staged;   // the verified ASI, ready to swap in
    std::string old;      // the replaced ASI, deleted at next start-up
};

bool ResolvePaths(Paths& paths)
{
    HMODULE module = nullptr;
    char asi[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&ResolvePaths), &module))
        return false;
    const DWORD length = GetModuleFileNameA(module, asi, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return false;
    paths.asi = asi;
    paths.download = paths.asi + ".download";
    paths.staged = paths.asi + ".new";
    paths.old = paths.asi + ".old";
    return true;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
struct Http {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    ~Http()
    {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

// Opens a GET and waits for a 200 response. `total` receives Content-Length
// (0 if absent).
bool OpenGet(Http& http, const wchar_t* host, INTERNET_PORT port, const wchar_t* path, bool secure,
             int receiveTimeoutMs, unsigned long long* total)
{
    http.session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
    if (!http.session) return false;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(http.session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(http.session, 8000, 8000, 8000, receiveTimeoutMs);
    http.connection = WinHttpConnect(http.session, host, port, 0);
    if (!http.connection) return false;
    http.request = WinHttpOpenRequest(http.connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!http.request ||
        !WinHttpSendRequest(http.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(http.request, nullptr))
        return false;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(http.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200)
        return false;
    if (total) {
        wchar_t length[64] = {};
        DWORD lengthSize = sizeof(length);
        *total = WinHttpQueryHeaders(http.request, WINHTTP_QUERY_CONTENT_LENGTH,
                                     WINHTTP_HEADER_NAME_BY_INDEX, length, &lengthSize,
                                     WINHTTP_NO_HEADER_INDEX)
                     ? _wcstoui64(length, nullptr, 10)
                     : 0;
    }
    return true;
}

bool FetchLatestRelease(std::string& json)
{
    Http http;
    if (!OpenGet(http, kReleaseHost, INTERNET_DEFAULT_HTTPS_PORT, kReleasePath, true, 8000, nullptr))
        return false;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http.request, &available)) return false;
        if (!available) return true;
        const size_t oldSize = json.size();
        json.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(http.request, json.data() + oldSize, available, &read)) return false;
        json.resize(oldSize + read);
        if (json.size() > 4 * 1024 * 1024) return false; // not a release description
    }
}

// ---------------------------------------------------------------------------
// Release description (the few fields needed, without a JSON library)
// ---------------------------------------------------------------------------
bool ExtractJsonString(const std::string& json, const char* key, size_t start, std::string& value)
{
    const std::string marker = std::string("\"") + key + "\"";
    const size_t keyPos = json.find(marker, start);
    const size_t colon = keyPos == std::string::npos ? keyPos : json.find(':', keyPos + marker.size());
    const size_t quote = colon == std::string::npos ? colon : json.find('"', colon + 1);
    if (quote == std::string::npos) return false;
    value.clear();
    for (size_t i = quote + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') return true;
        if (c == '\\' && i + 1 < json.size()) {
            const char escaped = json[++i];
            if (escaped != '/' && escaped != '\\' && escaped != '"') return false;
            value.push_back(escaped);
        } else {
            value.push_back(c);
        }
    }
    return false;
}

bool ParseReleaseAsset(const std::string& json, const char* assetName, std::string& url,
                       std::string& sha256)
{
    size_t asset = json.find("\"assets\"");
    while (asset != std::string::npos) {
        asset = json.find("\"name\"", asset + 1);
        if (asset == std::string::npos) break;
        std::string name;
        if (ExtractJsonString(json, "name", asset, name) && name == assetName) {
            std::string digest;
            if (!ExtractJsonString(json, "digest", asset, digest) || digest.size() != 71 ||
                digest.compare(0, 7, "sha256:") != 0 ||
                !ExtractJsonString(json, "browser_download_url", asset, url))
                return false;
            sha256 = digest.substr(7);
            return true;
        }
    }
    return false;
}

const char* SkipVersionPrefix(const char* version)
{
    return (*version == 'v' || *version == 'V') ? version + 1 : version;
}

bool IsNewerVersion(const char* latest, const char* current)
{
    latest = SkipVersionPrefix(latest);
    current = SkipVersionPrefix(current);
    for (int part = 0; part < 4; ++part) {
        char* latestEnd = nullptr;
        char* currentEnd = nullptr;
        const unsigned long latestPart = strtoul(latest, &latestEnd, 10);
        const unsigned long currentPart = strtoul(current, &currentEnd, 10);
        if (latestEnd == latest || currentEnd == current) return _stricmp(latest, current) != 0;
        if (latestPart != currentPart) return latestPart > currentPart;
        latest = (*latestEnd == '.') ? latestEnd + 1 : latestEnd;
        current = (*currentEnd == '.') ? currentEnd + 1 : currentEnd;
        if (!*latest && !*current) return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& data)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= 256 * 1024 * 1024;
    if (ok) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = data.empty() ||
             (ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) &&
              read == data.size());
    }
    CloseHandle(file);
    return ok;
}

bool WriteWholeFile(const std::string& path, const unsigned char* data, size_t size)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) &&
                    written == size;
    CloseHandle(file);
    if (!ok) DeleteFileA(path.c_str());
    return ok;
}

bool IsPlausibleAsi(const std::vector<unsigned char>& data)
{
    return data.size() >= 64 * 1024 && data[0] == 'M' && data[1] == 'Z';
}

bool Sha256Matches(const std::vector<unsigned char>& data, const std::string& expected)
{
    if (expected.size() != 64) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                             0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return false;
    static const char kHex[] = "0123456789abcdef";
    char actual[65] = {};
    for (int i = 0; i < 32; ++i) {
        actual[i * 2] = kHex[digest[i] >> 4];
        actual[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return _stricmp(actual, expected.c_str()) == 0;
}

bool IsZip(const std::vector<unsigned char>& data)
{
    return data.size() >= 4 && data[0] == 'P' && data[1] == 'K' &&
           ((data[2] == 3 && data[3] == 4) || (data[2] == 5 && data[3] == 6));
}

uint16_t ZipU16(const std::vector<unsigned char>& data, size_t offset)
{
    return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

uint32_t ZipU32(const std::vector<unsigned char>& data, size_t offset)
{
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

// Finds one entry of a ZIP whose files are stored uncompressed, as release
// packages are (CI and `make release` both pack them with no compression).
// Returns nullptr and sets `error` otherwise.
const unsigned char* FindStoredZipEntry(const std::vector<unsigned char>& zip, const char* wantedName,
                                        size_t& size, const char*& error)
{
    error = "The downloaded update package is damaged.";
    if (zip.size() < 22) return nullptr;
    const size_t searchStart = zip.size() > 22 + 0xffff ? zip.size() - (22 + 0xffff) : 0;
    size_t eocd = std::string::npos;
    for (size_t pos = zip.size() - 22;; --pos) {
        if (ZipU32(zip, pos) == 0x06054b50) {
            eocd = pos;
            break;
        }
        if (pos == searchStart) break;
    }
    if (eocd == std::string::npos || ZipU16(zip, eocd + 4) != 0 || ZipU16(zip, eocd + 6) != 0 ||
        ZipU16(zip, eocd + 8) != ZipU16(zip, eocd + 10))
        return nullptr;

    const size_t entries = ZipU16(zip, eocd + 10);
    const size_t centralSize = ZipU32(zip, eocd + 12);
    size_t cursor = ZipU32(zip, eocd + 16);
    if (cursor > zip.size() || centralSize > zip.size() - cursor) return nullptr;

    bool found = false;
    uint16_t method = 0;
    size_t compressedSize = 0, uncompressedSize = 0, localOffset = 0;
    for (size_t i = 0; i < entries; ++i) {
        if (zip.size() - cursor < 46 || ZipU32(zip, cursor) != 0x02014b50) return nullptr;
        const size_t nameLength = ZipU16(zip, cursor + 28);
        const size_t recordSize =
            46 + nameLength + ZipU16(zip, cursor + 30) + ZipU16(zip, cursor + 32);
        if (recordSize > zip.size() - cursor) return nullptr;
        const std::string name(reinterpret_cast<const char*>(zip.data() + cursor + 46), nameLength);
        const size_t slash = name.find_last_of("/\\");
        const char* baseName = name.c_str() + (slash == std::string::npos ? 0 : slash + 1);
        if (_stricmp(baseName, wantedName) == 0) {
            if (found || (ZipU16(zip, cursor + 8) & 1) != 0) return nullptr; // duplicate / encrypted
            found = true;
            method = ZipU16(zip, cursor + 10);
            compressedSize = ZipU32(zip, cursor + 20);
            uncompressedSize = ZipU32(zip, cursor + 24);
            localOffset = ZipU32(zip, cursor + 42);
        }
        cursor += recordSize;
    }
    if (!found) {
        error = "The update package does not contain Timestone.asi.";
        return nullptr;
    }
    if (method != 0) {
        error = "The update package is compressed in a way Timestone cannot unpack.";
        return nullptr;
    }
    if (compressedSize != uncompressedSize || localOffset > zip.size() ||
        zip.size() - localOffset < 30 || ZipU32(zip, localOffset) != 0x04034b50)
        return nullptr;
    const size_t dataOffset =
        localOffset + 30 + ZipU16(zip, localOffset + 26) + ZipU16(zip, localOffset + 28);
    if (dataOffset > zip.size() || uncompressedSize > zip.size() - dataOffset) return nullptr;
    size = uncompressedSize;
    error = nullptr;
    return zip.data() + dataOffset;
}

std::wstring Widen(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(length > 0 ? length - 1 : 0, L'\0');
    if (length > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    return result;
}

// ---------------------------------------------------------------------------
// Wine. Its file server names every open file by realpath(), so Windows
// APIs always act on the file behind a symlinked Timestone.asi, and they
// refuse to replace a file the game has mapped. The install is done on the
// Unix side instead, on that file: the update is copied next to it (same
// filesystem) and renamed over it. A rename never touches the old file's
// contents, so the running game keeps its mapping, and a symlink in the game
// folder stays as it is, now pointing at the new build.
// ---------------------------------------------------------------------------
using WineGetUnixFileName = char*(__cdecl*)(const wchar_t*);
using WineUnixSpawnvp = LONG(WINAPI*)(char* const* argv, int wait);

bool RunningUnderWine()
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

// The Unix path of an existing file, with every link resolved.
bool WineRealPath(const std::string& path, std::string& unixPath)
{
    const auto getUnixName = reinterpret_cast<WineGetUnixFileName>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name"));
    if (!getUnixName) return false;
    char* name = getUnixName(Widen(path).c_str());
    if (!name) return false;
    unixPath = name;
    HeapFree(GetProcessHeap(), 0, name);
    return !unixPath.empty();
}

// Puts `update` in place of the file `installed` is or links to, with the
// host's sh, cp and mv. Leaves everything as it was on failure.
bool WineInstall(const std::string& update, const std::string& installed, char* error,
                 size_t capacity)
{
    const auto spawn = reinterpret_cast<WineUnixSpawnvp>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "__wine_unix_spawnvp"));
    std::string unixUpdate, target;
    if (!spawn || !WineRealPath(update, unixUpdate) || !WineRealPath(installed, target)) {
        snprintf(error, capacity, "This version of Wine cannot run the update's install step.");
        return false;
    }
    // Paths go in as arguments, never into the script text.
    char sh[] = "sh", dashC[] = "-c", name[] = "timestone-update";
    char script[] =
        "staged=\"$2.timestone-update\"; "
        "if cp -f -- \"$1\" \"$staged\" && mv -f -- \"$staged\" \"$2\"; then exit 0; fi; "
        "rm -f -- \"$staged\"; exit 1";
    char* const argv[] = {sh, dashC, script, name, unixUpdate.data(), target.data(), nullptr};
    const LONG status = spawn(argv, 1);
    HookLog("Updater: Wine install \"%s\" -> \"%s\" status=%ld\n", unixUpdate.c_str(),
            target.c_str(), status);
    if (status != 0) {
        snprintf(error, capacity, "The new Timestone.asi could not be put in place (status %ld).",
                 status);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shared state. The worker thread writes, the game thread polls.
// ---------------------------------------------------------------------------
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<UpdateCheck> g_check{UpdateCheck::Idle};
std::atomic<bool> g_cancel{false};
HANDLE g_checkWorker = nullptr;
HANDLE g_downloadWorker = nullptr;
char g_latestTag[64] = {};
std::string g_assetUrl;    // set once by the check, read by the download
std::string g_assetSha256;
UpdateProgress g_progress = {};
ULONGLONG g_downloadStartedAt = 0;

struct Locked {
    Locked() { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
};

void SetStage(const char* stage)
{
    Locked lock;
    snprintf(g_progress.stage, sizeof(g_progress.stage), "%s", stage);
}

void SetFailed(const char* error)
{
    {
        Locked lock;
        snprintf(g_progress.error, sizeof(g_progress.error), "%s", error);
        g_progress.state = UpdateProgress::Failed;
    }
    HookLog("Updater: %s\n", error);
}

void JoinWorker(HANDLE& worker)
{
    if (!worker) return;
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    worker = nullptr;
}

DWORD WINAPI CheckThread(void*)
{
    std::string json, tag, url, sha256;
    UpdateCheck result = UpdateCheck::Failed;
    if (!FetchLatestRelease(json) || !ExtractJsonString(json, "tag_name", 0, tag)) {
        HookLog("Updater: could not read the latest release from GitHub\n");
    } else if (!ParseReleaseAsset(json, "Timestone.zip", url, sha256) &&
               !ParseReleaseAsset(json, "Timestone.asi", url, sha256)) {
        HookLog("Updater: release %s has no usable Timestone asset\n", tag.c_str());
    } else {
        result = IsNewerVersion(tag.c_str(), UMVC3HOOK_VERSION) ? UpdateCheck::Available
                                                                : UpdateCheck::UpToDate;
        Locked lock;
        snprintf(g_latestTag, sizeof(g_latestTag), "%s", tag.c_str());
        g_assetUrl = url;
        g_assetSha256 = sha256;
    }
    HookLog("Updater: installed %s, latest %s -> %s\n", UMVC3HOOK_VERSION,
            tag.empty() ? "?" : tag.c_str(),
            result == UpdateCheck::Available  ? "update available"
            : result == UpdateCheck::UpToDate ? "up to date"
                                              : "check failed");
    g_check.store(result, std::memory_order_release);
    return 0;
}

// Downloads `url` to `path`, reporting progress. False on failure or cancel.
bool Download(const std::string& url, const std::string& path)
{
    const std::wstring wideUrl = Widen(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts))
        return false;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring requestPath(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) requestPath.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    Http http;
    unsigned long long total = 0;
    if (!OpenGet(http, host.c_str(), parts.nPort, requestPath.c_str(),
                 parts.nScheme == INTERNET_SCHEME_HTTPS, 30000, &total))
        return false;
    {
        Locked lock;
        g_progress.total = total;
        g_progress.downloaded = 0;
    }

    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> buffer(64 * 1024);
    unsigned long long downloaded = 0;
    bool ok = true;
    while (ok && !g_cancel.load(std::memory_order_relaxed)) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http.request, &available)) {
            ok = false;
            break;
        }
        if (!available) break;
        DWORD read = 0, written = 0;
        ok = WinHttpReadData(http.request, buffer.data(),
                             available < buffer.size() ? available : static_cast<DWORD>(buffer.size()),
                             &read) &&
             WriteFile(file, buffer.data(), read, &written, nullptr) && written == read;
        downloaded += written;
        const ULONGLONG elapsed = GetTickCount64() - g_downloadStartedAt;
        Locked lock;
        g_progress.downloaded = downloaded;
        g_progress.bytesPerSecond = elapsed ? downloaded * 1000.0 / elapsed : 0.0;
    }
    CloseHandle(file);
    if (!ok || g_cancel.load(std::memory_order_relaxed) || (total && downloaded != total)) {
        DeleteFileA(path.c_str());
        return false;
    }
    return true;
}

DWORD WINAPI DownloadThread(void*)
{
    Paths paths;
    if (!ResolvePaths(paths)) {
        SetFailed("Timestone could not find its own file.");
        return 0;
    }
    std::string url, sha256, tag;
    {
        Locked lock;
        url = g_assetUrl;
        sha256 = g_assetSha256;
        tag = g_latestTag;
    }
    DeleteFileA(paths.download.c_str());
    DeleteFileA(paths.staged.c_str());

    char stage[160];
    snprintf(stage, sizeof(stage), "Downloading Timestone %s from Github", tag.c_str());
    SetStage(stage);
    g_downloadStartedAt = GetTickCount64();
    if (!Download(url, paths.download)) {
        if (!g_cancel.load()) SetFailed("The update could not be downloaded from GitHub.");
        return 0;
    }

    SetStage("Verifying the SHA-256 digest...");
    std::vector<unsigned char> data;
    const bool read = ReadWholeFile(paths.download, data);
    DeleteFileA(paths.download.c_str());
    if (!read || !Sha256Matches(data, sha256)) {
        SetFailed("The download did not match its GitHub SHA-256 digest.");
        return 0;
    }

    const unsigned char* asi = data.data();
    size_t asiSize = data.size();
    if (IsZip(data)) {
        SetStage("Unpacking the update...");
        const char* error = nullptr;
        asi = FindStoredZipEntry(data, "Timestone.asi", asiSize, error);
        if (!asi) {
            SetFailed(error);
            return 0;
        }
    }
    if (asiSize < 64 * 1024 || asi[0] != 'M' || asi[1] != 'Z') {
        SetFailed("The update does not contain a valid Timestone.asi.");
        return 0;
    }
    if (!WriteWholeFile(paths.staged, asi, asiSize)) {
        SetFailed("The update could not be saved beside Timestone.asi.");
        return 0;
    }
    {
        Locked lock;
        snprintf(g_progress.stage, sizeof(g_progress.stage), "Update downloaded and verified.");
        g_progress.state = UpdateProgress::Ready;
    }
    HookLog("Updater: %s staged at %s\n", tag.c_str(), paths.staged.c_str());
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
void Updater_OnProcessStart()
{
    // Started by an update (or a launch-menu return) while the old process
    // was still on its way out: let it go first.
    if (const char* wait = strstr(GetCommandLineA(), kWaitPidArgument)) {
        const DWORD pid = strtoul(wait + strlen(kWaitPidArgument), nullptr, 10);
        if (pid && pid != GetCurrentProcessId()) {
            if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
                const DWORD result = WaitForSingleObject(process, kWaitForOldProcessMs);
                CloseHandle(process);
                HookLog("Updater: waited for previous process %lu (%s)\n", pid,
                        result == WAIT_OBJECT_0 ? "exited" : "still running, continuing");
            }
        }
    }

    Paths paths;
    if (!ResolvePaths(paths)) return;
    // The ASI an update replaced is no longer loaded by anyone now.
    if (GetFileAttributesA(paths.old.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const BOOL deleted = DeleteFileA(paths.old.c_str());
        HookLog("Updater: removed the previous Timestone.asi (%s)\n",
                deleted ? "ok" : "failed, kept for next time");
    }
    DeleteFileA(paths.download.c_str());
    DeleteFileA(paths.staged.c_str());
}

void Updater_BeginOnlineCheck()
{
    UpdateCheck expected = UpdateCheck::Idle;
    if (!g_check.compare_exchange_strong(expected, UpdateCheck::Checking)) return;
    g_checkWorker = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    if (!g_checkWorker) {
        HookLog("Updater: could not start the update check\n");
        g_check.store(UpdateCheck::Failed);
        return;
    }
    HookLog("Updater: checking GitHub for a newer release\n");
}

UpdateCheck Updater_PollOnlineCheck()
{
    const UpdateCheck state = g_check.load(std::memory_order_acquire);
    if (state != UpdateCheck::Checking && state != UpdateCheck::Idle) JoinWorker(g_checkWorker);
    return state;
}

const char* Updater_LatestVersion()
{
    return g_latestTag;
}

bool Updater_BeginDownload()
{
    if (g_check.load() != UpdateCheck::Available) return false;
    {
        Locked lock;
        if (g_progress.state == UpdateProgress::Downloading) return true;
    }
    JoinWorker(g_downloadWorker); // a cancelled download still winding down
    g_cancel.store(false);
    {
        Locked lock;
        g_progress = {};
        g_progress.state = UpdateProgress::Downloading;
        snprintf(g_progress.stage, sizeof(g_progress.stage),
                 "Connecting to github.com/tilderain/Timestone...");
    }
    g_downloadWorker = CreateThread(nullptr, 0, DownloadThread, nullptr, 0, nullptr);
    if (!g_downloadWorker) {
        SetFailed("The download could not be started.");
        return false;
    }
    HookLog("Updater: downloading %s\n", g_latestTag);
    return true;
}

UpdateProgress Updater_PollDownload()
{
    Locked lock;
    return g_progress;
}

void Updater_CancelDownload()
{
    g_cancel.store(true);
    // The worker notices within one read and deletes its partial file; it
    // is joined before the next download starts.
    Locked lock;
    g_progress.state = UpdateProgress::Idle;
    HookLog("Updater: download cancelled\n");
}

bool Updater_InstallAndRestart(char* error, size_t capacity)
{
    const auto fail = [&](const char* text) {
        snprintf(error, capacity, "%s", text);
        HookLog("Updater: install failed: %s (error %lu)\n", text, GetLastError());
        return false;
    };
    {
        Locked lock;
        if (g_progress.state != UpdateProgress::Ready) return fail("The update is not ready yet.");
    }
    JoinWorker(g_downloadWorker);

    Paths paths;
    char game[MAX_PATH] = {};
    const DWORD gameLength = GetModuleFileNameA(nullptr, game, MAX_PATH);
    if (!ResolvePaths(paths) || !gameLength || gameLength >= MAX_PATH)
        return fail("Timestone could not find the game's files.");

    if (RunningUnderWine()) {
        // See WineInstall: the file behind the ASI is replaced, any link kept.
        char wineError[160];
        if (!WineInstall(paths.staged, paths.asi, wineError, sizeof(wineError)))
            return fail(wineError);
        DeleteFileA(paths.staged.c_str());
    } else {
        // A loaded DLL cannot be overwritten or deleted, but it can be
        // renamed: this process keeps running the old image from the renamed
        // file. (A symlinked ASI is renamed as the link, not its target.)
        DeleteFileA(paths.old.c_str());
        if (!MoveFileExA(paths.asi.c_str(), paths.old.c_str(), MOVEFILE_REPLACE_EXISTING))
            return fail("The installed Timestone.asi could not be moved aside.");
        if (!MoveFileExA(paths.staged.c_str(), paths.asi.c_str(), 0)) {
            MoveFileExA(paths.old.c_str(), paths.asi.c_str(), 0);
            return fail("The new Timestone.asi could not be put in place. Nothing was changed.");
        }
    }
    HookLog("Updater: installed %s, restarting the game\n", g_latestTag);

    // The new process waits for this one to be gone (Updater_OnProcessStart).
    std::string directory = game;
    const size_t slash = directory.find_last_of("\\/");
    directory.resize(slash == std::string::npos ? 0 : slash);
    char command[MAX_PATH + 64] = {};
    snprintf(command, sizeof(command), "\"%s\" %s %lu", game, kWaitPidArgument,
             GetCurrentProcessId());
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(game, command, nullptr, nullptr, FALSE, 0, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &startup, &process)) {
        // The update is in place either way; it loads on the next start.
        return fail("Timestone was updated, but the game could not be restarted.\n"
                    "Please start it again yourself.");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    DisableReturnToLaunchMenuOnExit();
    CleanupNetworkOnExit();
    HookLogFlush();
    TerminateProcess(GetCurrentProcess(), 0);
    return true;
}
