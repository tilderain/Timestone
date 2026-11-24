// The Timestone installer for Linux and Steam Deck: the same window and the
// same steps as TimestoneInstall.exe (tools/timestone_install.cpp), as a
// native program. It is shipped inside TimestoneInstall.sh.
//
// It finds Ultimate Marvel vs. Capcom 3 in the Steam libraries, downloads
// the latest Timestone release, puts its files beside umvc3.exe and tells
// the game's Proton prefix to load the dinput8 in the game folder. Nothing
// is run through Wine or Proton.
//
// It is linked whole, so it needs nothing installed but an X server (every
// desktop has one, Wayland ones through XWayland), a font, and curl or wget
// for the download. With no display it does the install in the terminal.
//
//   TimestoneInstall [game folder]   the folder holding umvc3.exe, if not found
//   TimestoneInstall --text          no window
//   TimestoneInstall --uninstall     remove Timestone from the game folder, no window

#include "install_ui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h" // the copy that comes with Dear ImGui

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TIMESTONE_VERSION
#define TIMESTONE_VERSION "0.1.0"
#endif

// The star tile, straight from the plugin's assets (see the Makefile for the
// include path that finds it).
__asm__(".section .rodata\n"
        ".global timestone_stars_tile\n"
        "timestone_stars_tile:\n"
        ".incbin \"space_stars_512.dxt1\"\n"
        ".global timestone_stars_tile_end\n"
        "timestone_stars_tile_end:\n"
        ".previous\n");
extern "C" const unsigned char timestone_stars_tile[];
extern "C" const unsigned char timestone_stars_tile_end[];

namespace {

using namespace install;

constexpr char kGameExe[] = "umvc3.exe";
constexpr char kSteamAppId[] = "357190";
constexpr char kDefaultInstallDir[] = "ULTIMATE MARVEL VS. CAPCOM 3";
constexpr char kStaged[] = ".timestone-new";

// ---------------------------------------------------------------------------
// Files and programs
// ---------------------------------------------------------------------------
bool FileExists(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

template <typename Bytes>
bool ReadWholeFile(const std::string& path, Bytes& data, size_t limit = 64u << 20)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    data.clear();
    unsigned char buffer[65536];
    bool ok = true;
    for (size_t got; (got = fread(buffer, 1, sizeof(buffer), file)) > 0;) {
        data.insert(data.end(), buffer, buffer + got);
        if (data.size() > limit) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(file);
    fclose(file);
    return ok;
}

bool WriteWholeFile(const std::string& path, const void* data, size_t size)
{
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool wrote = fwrite(data, 1, size, file) == size;
    const bool closed = fclose(file) == 0;
    if (wrote && closed) return true;
    const int error = errno;
    unlink(path.c_str());
    errno = error;
    return false;
}

std::string ParentFolder(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string Environment(const char* name)
{
    const char* value = getenv(name);
    return value ? value : "";
}

// Starts a program found on PATH. Its output goes to `outputFd` (or nowhere).
// Returns its process id, or -1.
pid_t Spawn(const std::vector<std::string>& arguments, int outputFd)
{
    std::vector<char*> argv;
    for (const std::string& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child != 0) return child;
    const int nowhere = open("/dev/null", O_RDWR);
    dup2(nowhere, 0);
    dup2(outputFd >= 0 ? outputFd : nowhere, 1);
    dup2(nowhere, 2);
    execvp(argv[0], argv.data());
    _exit(127); // not installed
}

int Wait(pid_t child)
{
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// Runs a program and collects what it prints. Returns its exit code; 127
// when it is not installed.
int Run(const std::vector<std::string>& arguments, std::string* output = nullptr)
{
    int ends[2];
    if (pipe(ends) != 0) return 1;
    const pid_t child = Spawn(arguments, ends[1]);
    close(ends[1]);
    if (child < 0) {
        close(ends[0]);
        return 1;
    }
    char buffer[4096];
    for (ssize_t got; (got = read(ends[0], buffer, sizeof(buffer))) > 0;)
        if (output && output->size() < (8u << 20)) output->append(buffer, static_cast<size_t>(got));
    close(ends[0]);
    return Wait(child);
}

std::string TrimLine(std::string text)
{
    const size_t end = text.find_first_of("\r\n");
    if (end != std::string::npos) text.resize(end);
    return text;
}

// ---------------------------------------------------------------------------
// Finding the game
// ---------------------------------------------------------------------------
bool IsGameFolder(const std::string& folder)
{
    return !folder.empty() && FileExists(folder + "/" + kGameExe);
}

void AddUnique(std::vector<std::string>& list, std::string path)
{
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path.empty()) return;
    if (char* real = realpath(path.c_str(), nullptr)) { // the usual Steam paths are links to one folder
        path = real;
        free(real);
    }
    for (const std::string& known : list)
        if (known == path) return;
    list.push_back(path);
}

std::string GameInLibrary(const std::string& library)
{
    const std::string apps = library + "/steamapps/";
    std::string manifest;
    if (ReadWholeFile(apps + "appmanifest_" + kSteamAppId + ".acf", manifest, 1u << 20)) {
        for (const std::string& folder : VdfValues(manifest, "installdir")) {
            const std::string candidate = apps + "common/" + folder;
            if (IsGameFolder(candidate)) return candidate;
        }
    }
    const std::string usual = apps + "common/" + kDefaultInstallDir;
    return IsGameFolder(usual) ? usual : std::string();
}

std::string FindGame()
{
    const std::string home = Environment("HOME");
    std::vector<std::string> libraries;
    if (!home.empty()) {
        for (const char* place : {"/.steam/steam", "/.local/share/Steam",
                                  "/.var/app/com.valvesoftware.Steam/.local/share/Steam", // Flatpak
                                  "/snap/steam/common/.local/share/Steam",                // Snap
                                  "/.steam/root"}) {
            const std::string steam = home + place;
            AddUnique(libraries, steam);
            for (const char* list : {"/config/libraryfolders.vdf", "/steamapps/libraryfolders.vdf"}) {
                std::string text;
                if (!ReadWholeFile(steam + list, text, 4u << 20)) continue;
                for (const std::string& path : VdfValues(text, "path")) AddUnique(libraries, path);
            }
        }
    }
    for (const std::string& library : libraries) {
        const std::string game = GameInLibrary(library);
        if (!game.empty()) return game;
    }
    return {};
}

bool GameRunning()
{
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    bool running = false;
    while (const dirent* entry = readdir(proc)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        std::string name;
        if (!ReadWholeFile(std::string("/proc/") + entry->d_name + "/comm", name, 4096)) continue;
        if (strcasecmp(TrimLine(name).c_str(), kGameExe) == 0) {
            running = true;
            break;
        }
    }
    closedir(proc);
    return running;
}

// ---------------------------------------------------------------------------
// Proton. Wine loads its own dinput8 unless told otherwise; the game's
// prefix is told in its registry file, beside the library's "common" folder.
// ---------------------------------------------------------------------------
enum class ProtonLoader {
    Set,      // the prefix loads the dinput8 beside the game
    NoPrefix, // the game has not been started under Proton yet
    Failed
};

ProtonLoader EnableProtonLoader(const std::string& gameDir)
{
    const std::string registry = ParentFolder(ParentFolder(gameDir)) + "/compatdata/" +
                                 kSteamAppId + "/pfx/user.reg";
    std::string text;
    if (!ReadWholeFile(registry, text)) return ProtonLoader::NoPrefix;
    switch (AddWineDllOverride(text, "umvc3.exe", "dinput8", static_cast<unsigned>(time(nullptr)))) {
    case OverrideResult::Present: return ProtonLoader::Set;
    case OverrideResult::NotRegistry: return ProtonLoader::Failed;
    case OverrideResult::Added: break;
    }
    const std::string staged = registry + kStaged;
    if (WriteWholeFile(staged, text.data(), text.size()) && rename(staged.c_str(), registry.c_str()) == 0)
        return ProtonLoader::Set;
    unlink(staged.c_str());
    return ProtonLoader::Failed;
}

// ---------------------------------------------------------------------------
// What the window shows. The worker thread writes it, the window reads it.
// ---------------------------------------------------------------------------
enum class Stage { Searching, NotFound, Ready, WaitGame, Working, Done, Removed, Failed };
enum class Task { Detect, Install, Browse };

struct State {
    Stage stage = Stage::Searching;
    std::string gameDir;
    std::string installed; // version of the Timestone.asi already there, if any
    std::string latest;    // release tag, "v0.6.6"
    std::string url;
    std::string sha256;
    unsigned long long size = 0;
    std::string title;      // Failed
    std::string message;    // NotFound, Working, Done, Failed
    std::string status;     // Working
    float fraction = -1.0f; // Working
    bool committing = false;    // files are being swapped: the window stays open
    bool protonPending = false; // installed, but the game's Proton prefix is not there yet
    Task retry = Task::Detect;  // what RETRY runs again
};

std::mutex g_lock;
State g_state;           // under g_lock
std::string g_chosenDir; // under g_lock; a folder the player gave
std::atomic<bool> g_busy{false};

void SetWorking(const std::string& message, float fraction = -1.0f)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_state.stage = Stage::Working;
    g_state.message = message;
    g_state.fraction = fraction;
    g_state.status.clear();
}

void Fail(Task retry, const char* title, const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_state.stage = Stage::Failed;
    g_state.committing = false;
    g_state.title = title;
    g_state.message = message;
    g_state.retry = retry;
}

// ---------------------------------------------------------------------------
// GitHub, through curl or wget
// ---------------------------------------------------------------------------
bool FetchText(const std::string& url, std::string& text)
{
    text.clear();
    int code = Run({"curl", "-fsSL", "--max-time", "30", "-A", "TimestoneInstall/1.0", url}, &text);
    if (code == 127) {
        text.clear();
        code = Run({"wget", "-q", "-T", "30", "-U", "TimestoneInstall/1.0", "-O", "-", url}, &text);
    }
    return code == 0 && !text.empty();
}

bool Download(const std::string& url, const std::string& destination, unsigned long long total)
{
    for (const bool curl : {true, false}) {
        unlink(destination.c_str());
        const pid_t child = curl
            ? Spawn({"curl", "-fsSL", "--retry", "2", "-A", "TimestoneInstall/1.0", "-o", destination, url}, -1)
            : Spawn({"wget", "-q", "-U", "TimestoneInstall/1.0", "-O", destination, url}, -1);
        if (child < 0) return false;
        int status = 0;
        for (;;) {
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child || (done < 0 && errno != EINTR)) break;
            struct stat info;
            const unsigned long long got = stat(destination.c_str(), &info) == 0
                ? static_cast<unsigned long long>(info.st_size) : 0;
            char text[96];
            if (total)
                snprintf(text, sizeof(text), "%.1f MB of %.1f MB", got / 1048576.0, total / 1048576.0);
            else
                snprintf(text, sizeof(text), "%.1f MB", got / 1048576.0);
            {
                std::lock_guard<std::mutex> lock(g_lock);
                g_state.fraction = total ? static_cast<float>(static_cast<double>(got) / total) : -1.0f;
                if (g_state.fraction > 1.0f) g_state.fraction = 1.0f;
                g_state.status = text;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (code == 0) return true;
        unlink(destination.c_str());
        if (code != 127) return false; // it ran and failed; the other would too
    }
    return false;
}

// ---------------------------------------------------------------------------
// Looking: where the game is, what is installed there, what the latest is.
// ---------------------------------------------------------------------------
void Detect()
{
    std::string chosen;
    bool haveRelease;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_state.stage = Stage::Searching;
        chosen = g_chosenDir;
        haveRelease = !g_state.url.empty();
    }

    std::string gameDir = chosen;
    if (gameDir.empty()) {
        // TimestoneInstall.sh says where it sits: in the game folder, that is the game.
        const std::string beside = Environment("TIMESTONE_SCRIPT_DIR");
        gameDir = IsGameFolder(beside) ? beside : FindGame();
    }
    std::string installed;
    std::vector<unsigned char> plugin;
    if (!gameDir.empty() && ReadWholeFile(gameDir + "/Timestone.asi", plugin)) {
        installed = PeProductVersion(plugin);
        if (installed.empty()) installed = "(unknown version)"; // still installed: it can be updated or removed
    }
    // An install made before the game had a Proton prefix is finished here,
    // on the run after the game was first started.
    const bool protonPending = !plugin.empty() && !GameRunning() &&
                               EnableProtonLoader(gameDir) == ProtonLoader::NoPrefix;

    std::string json, tag, url, sha256;
    unsigned long long size = 0;
    if (!haveRelease) {
        if (!FetchText("https://api.github.com/repos/tilderain/Timestone/releases/latest", json) ||
            !ExtractJsonString(json, "tag_name", 0, tag) ||
            !ParseReleaseAsset(json, "Timestone.zip", url, sha256, &size)) {
            Fail(Task::Detect, "NO CONNECTION",
                 "The latest Timestone release could not be read from GitHub.\n"
                 "Check your connection (and that curl or wget is installed), then try again.");
            return;
        }
    }

    std::lock_guard<std::mutex> lock(g_lock);
    if (!haveRelease) {
        g_state.latest = tag;
        g_state.url = url;
        g_state.sha256 = sha256;
        g_state.size = size;
    }
    g_state.gameDir = gameDir;
    g_state.installed = installed;
    g_state.protonPending = protonPending;
    if (gameDir.empty()) {
        g_state.stage = Stage::NotFound;
        g_state.message = "Ultimate Marvel vs. Capcom 3 was not found in your Steam libraries.\n"
                          "Choose the folder that holds umvc3.exe.";
    } else {
        g_state.stage = Stage::Ready;
    }
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------
std::string WithFileError(const char* message, const std::string& file, int error)
{
    return std::string(message) + "\n\n" + strerror(error) + ": " + file;
}

void Install()
{
    std::string gameDir, tag, url, sha256;
    unsigned long long size;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        gameDir = g_state.gameDir;
        tag = g_state.latest;
        url = g_state.url;
        sha256 = g_state.sha256;
        size = g_state.size;
    }

    if (GameRunning()) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_state.stage = Stage::WaitGame;
        }
        while (GameRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // Wine writes the prefix's registry back as it shuts down.
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    // The download goes into the game folder, where the files are headed.
    SetWorking("Downloading Timestone " + tag);
    const std::string folder = gameDir + "/";
    const std::string archive = folder + "Timestone.zip" + kStaged;
    if (!Download(url, archive, size)) {
        Fail(Task::Install, "DOWNLOAD FAILED",
             "Timestone " + tag + " could not be downloaded from GitHub into the game folder.\n"
             "Check your connection and try again.");
        return;
    }

    SetWorking("Checking the download", 1.0f);
    std::vector<unsigned char> zip;
    const bool read = ReadWholeFile(archive, zip);
    unlink(archive.c_str());
    if (!read || Sha256Hex(zip.data(), zip.size()) != sha256) {
        Fail(Task::Install, "DOWNLOAD FAILED",
             "The download did not match the SHA-256 digest GitHub lists for it.\n"
             "Nothing was installed. Try again.");
        return;
    }
    std::vector<ZipFile> files;
    const ZipFile* plugin = nullptr;
    if (ListStoredZip(zip, files)) {
        for (const ZipFile& file : files)
            if (file.name == "Timestone.asi") plugin = &file;
    }
    if (!plugin || plugin->size < 64 * 1024 || zip[plugin->offset] != 'M' ||
        zip[plugin->offset + 1] != 'Z') {
        Fail(Task::Install, "INSTALL FAILED",
             "The Timestone " + tag + " package is not one this installer can read.\n"
             "Nothing was installed. Download the newest TimestoneInstall.sh.");
        return;
    }

    // Everything is written beside its destination first, so a full disk or
    // a folder that cannot be written leaves the game as it was.
    SetWorking("Installing Timestone " + tag, 1.0f);
    for (size_t i = 0; i < files.size(); ++i) {
        if (WriteWholeFile(folder + files[i].name + kStaged, zip.data() + files[i].offset, files[i].size))
            continue;
        const int error = errno;
        for (size_t written = 0; written <= i; ++written)
            unlink((folder + files[written].name + kStaged).c_str());
        Fail(Task::Install, "INSTALL FAILED",
             WithFileError("The new files could not be written into the game folder.\n"
                           "Nothing was changed.", files[i].name, error));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_state.committing = true;
    }
    const std::string pluginPath = folder + "Timestone.asi";
    std::vector<unsigned char> previous;
    if (ReadWholeFile(pluginPath, previous))
        WriteWholeFile(pluginPath + ".old", previous.data(), previous.size());
    // The plugin first: if it cannot be replaced, nothing has been yet.
    std::vector<const ZipFile*> order{plugin};
    for (const ZipFile& file : files)
        if (&file != plugin) order.push_back(&file);
    for (const ZipFile* file : order) {
        if (rename((folder + file->name + kStaged).c_str(), (folder + file->name).c_str()) == 0)
            continue;
        const int error = errno;
        for (const ZipFile& staged : files) unlink((folder + staged.name + kStaged).c_str());
        Fail(Task::Install, "INSTALL FAILED",
             WithFileError("The new files could not be put into the game folder.", file->name, error));
        return;
    }
    // The updater's name before it became the installer, unless this
    // release still ships it.
    bool shipsUpdater = false;
    for (const ZipFile& file : files) shipsUpdater |= file.name == "TimestoneUpdater.exe";
    if (!shipsUpdater) unlink((folder + "TimestoneUpdater.exe").c_str());
    unlink((folder + "TimestoneUpdater.exe.new").c_str());

    std::string message = "Timestone " + tag + " is installed.";
    switch (EnableProtonLoader(gameDir)) {
    case ProtonLoader::Set:
        message += "\nProton is set to load it.";
        break;
    case ProtonLoader::NoPrefix:
        message += "\n\nOne more step: start the game once from Steam, close it, then run "
                   "this installer again so it can tell Proton to load Timestone.";
        break;
    case ProtonLoader::Failed:
        message += "\n\nThe game's Proton settings could not be changed. Put this in the "
                   "game's Steam launch options instead:\n"
                   "WINEDLLOVERRIDES=\"dinput8=n,b\" %command%";
        break;
    }

    std::lock_guard<std::mutex> lock(g_lock);
    g_state.stage = Stage::Done;
    g_state.committing = false;
    g_state.protonPending = false;
    g_state.message = message;
}

// Uninstalling takes out what the installer put in: the plugin and its
// backup, the notices and the old updater. The loader (dinput8.dll, Ultimate
// ASI Loader) goes too unless another .asi mod is there to need it. Settings
// (Timestone.ini), logs and dumps stay.
bool OtherPluginsPresent(const std::string& gameDir)
{
    // Where the loader looks for plugins.
    for (const char* folder : {"", "/plugins", "/scripts", "/update"}) {
        DIR* dir = opendir((gameDir + folder).c_str());
        if (!dir) continue;
        bool found = false;
        while (const dirent* entry = readdir(dir)) {
            const size_t length = strlen(entry->d_name);
            if (length > 4 && strcasecmp(entry->d_name + length - 4, ".asi") == 0) {
                found = true;
                break;
            }
        }
        closedir(dir);
        if (found) return true;
    }
    return false;
}

void Uninstall()
{
    std::string gameDir;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        gameDir = g_state.gameDir;
    }
    if (GameRunning()) {
        Fail(Task::Detect, "UNINSTALL FAILED",
             "Ultimate Marvel vs. Capcom 3 is running.\nClose it, then uninstall.");
        return;
    }
    const auto remove = [&gameDir](const char* name) {
        const std::string path = gameDir + "/" + name;
        if (unlink(path.c_str()) == 0 || errno == ENOENT) return true;
        Fail(Task::Detect, "UNINSTALL FAILED",
             WithFileError("Timestone could not be removed from the game folder.", name, errno));
        return false;
    };
    // The plugin first: once it is gone the game starts without Timestone,
    // whatever else is left.
    for (const char* name : {"Timestone.asi", "Timestone.asi.old", "Timestone-THIRDPARTYNOTICES",
                             "TimestoneUpdater.exe", "TimestoneUpdater.exe.new"})
        if (!remove(name)) return;
    const bool keepLoader = OtherPluginsPresent(gameDir);
    if (!keepLoader && !remove("dinput8.dll")) return;

    std::lock_guard<std::mutex> lock(g_lock);
    g_state.stage = Stage::Removed;
    g_state.installed.clear();
    g_state.protonPending = false;
    g_state.message = keepLoader
        ? "dinput8.dll was kept: another .asi mod in the folder needs it.\n"
          "Timestone was removed from:"
        : "Timestone was removed from:";
}

// A folder picker from whichever desktop is there.
void Browse()
{
    const char* const title = "Choose the Ultimate Marvel vs. Capcom 3 folder (the one with umvc3.exe)";
    std::string picked;
    int code = Run({"zenity", "--file-selection", "--directory", "--title", title}, &picked);
    if (code == 127) {
        picked.clear();
        code = Run({"kdialog", "--title", title, "--getexistingdirectory", Environment("HOME")}, &picked);
    }
    if (code == 127) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_state.stage = Stage::NotFound;
        g_state.message = "There is no folder picker on this system (zenity or kdialog).\n"
                          "Give the folder when starting the installer instead:\n"
                          "sh TimestoneInstall.sh \"/path/to/ULTIMATE MARVEL VS. CAPCOM 3\"";
        return;
    }
    picked = TrimLine(picked);
    if (code != 0 || picked.empty()) return; // cancelled
    if (!IsGameFolder(picked)) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_state.stage = Stage::NotFound;
        g_state.message = "umvc3.exe is not in that folder.\nChoose the folder that holds umvc3.exe.";
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_chosenDir = picked;
    }
    Detect();
}

void StartTask(Task task)
{
    if (g_busy.exchange(true)) return;
    if (task != Task::Browse) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_state.stage = task == Task::Install ? Stage::Working : Stage::Searching;
        g_state.message = task == Task::Install ? "Getting ready" : "";
        g_state.fraction = -1.0f;
        g_state.status.clear();
    }
    std::thread([task] {
        if (task == Task::Install) Install();
        else if (task == Task::Browse) Browse();
        else Detect();
        g_busy = false;
    }).detach();
}

// ---------------------------------------------------------------------------
// Text, from a font of the system's drawn into the picture.
// ---------------------------------------------------------------------------
std::wstring Wide(const std::string& utf8)
{
    std::wstring wide;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char lead = static_cast<unsigned char>(utf8[i]);
        const int extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
        uint32_t codepoint = extra ? lead & (0x3F >> extra) : lead;
        size_t next = i + 1;
        for (int k = 0; k < extra && next < utf8.size() &&
                        (static_cast<unsigned char>(utf8[next]) & 0xC0) == 0x80; ++k, ++next)
            codepoint = (codepoint << 6) | (static_cast<unsigned char>(utf8[next]) & 0x3F);
        wide.push_back(static_cast<wchar_t>(codepoint));
        i = next;
    }
    return wide;
}

class FontText final : public TextPainter {
public:
    FontText(uint32_t* pixels, int width, int height)
        : pixels_(pixels), width_(width), height_(height), scale_(width / kScreenW)
    {
    }

    // False if the system has no font this can read.
    bool Load()
    {
        static const char* const kBold[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
            "/usr/share/fonts/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
            "/usr/share/fonts/TTF/LiberationSans-Bold.ttf"};
        static const char* const kRegular[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/TTF/LiberationSans-Regular.ttf"};
        // What the desktop itself would use, then the places fonts usually are.
        const auto load = [](Face& face, const char* pattern, const char* const* places, size_t count) {
            std::string file;
            if (Run({"fc-match", "-f", "%{file}", pattern}, &file) == 0 && face.Open(TrimLine(file)))
                return true;
            for (size_t i = 0; i < count; ++i)
                if (face.Open(places[i])) return true;
            return false;
        };
        const bool regular = load(regular_, "sans", kRegular, sizeof(kRegular) / sizeof(kRegular[0]));
        const bool bold = load(bold_, "sans:bold", kBold, sizeof(kBold) / sizeof(kBold[0]));
        if (regular && !bold) bold_ = regular_, bold_.Rebind();
        if (bold && !regular) regular_ = bold_, regular_.Rebind();
        return regular || bold;
    }

    float BlockHeight(FontId font, const wchar_t* text, float width, unsigned flags) override
    {
        return Wrap(font, text, width * scale_, (flags & kTextPath) != 0).size() * LineHeight(font) /
                   scale_ + 2.0f;
    }

    void Draw(FontId font, const wchar_t* text, Rect rect, Color color, unsigned flags) override
    {
        if (!text[0]) return;
        const float left = rect.x * scale_, top = rect.y * scale_;
        const float boxW = rect.w * scale_, boxH = rect.h * scale_;
        const float lineH = LineHeight(font);
        std::vector<std::wstring> lines;
        float y = top;
        if (flags & (kTextMiddle | kTextRight)) {
            lines.push_back(Fit(font, text, boxW));
            y = top + (boxH - lineH) * 0.5f;
        } else {
            lines = Wrap(font, text, boxW, (flags & kTextPath) != 0);
        }
        for (const std::wstring& line : lines) {
            const float lineW = Measure(font, line);
            const float x = flags & kTextRight ? left + boxW - lineW : left + (boxW - lineW) * 0.5f;
            const float baseline = y + Ascent(font);
            if (flags & kTextOutline) {
                const int edge = scale_ >= 1.25f ? 2 : 1;
                for (int dy = -edge; dy <= edge; ++dy)
                    for (int dx = -edge; dx <= edge; ++dx)
                        if (dx || dy) DrawLine(font, line, x + dx, baseline + dy, lobby::Rgb(0x080A12));
            }
            DrawLine(font, line, x, baseline, color);
            y += lineH;
        }
    }

private:
    struct Face {
        std::vector<unsigned char> data;
        stbtt_fontinfo info{};

        bool Open(const std::string& path)
        {
            // Plain TrueType and OpenType files; a collection's first face.
            if (path.empty() || !ReadWholeFile(path, data, 48u << 20) || data.size() < 12) return false;
            return Rebind();
        }
        bool Rebind()
        {
            const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
            return offset >= 0 && stbtt_InitFont(&info, data.data(), offset) != 0;
        }
    };
    struct Glyph {
        int w = 0, h = 0, left = 0, top = 0;
        float advance = 0.0f;
        std::vector<unsigned char> cover;
    };

    static int Index(FontId font) { return static_cast<int>(font); }
    Face& FaceOf(FontId font) { return font == FontId::Title || font == FontId::Button ? bold_ : regular_; }
    float Pixels(FontId font) const { return kFontHeight[Index(font)] * 0.80f * scale_; }
    float Scale(FontId font) { return stbtt_ScaleForMappingEmToPixels(&FaceOf(font).info, Pixels(font)); }
    float Ascent(FontId font)
    {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&FaceOf(font).info, &ascent, &descent, &gap);
        return ascent * Scale(font);
    }
    float LineHeight(FontId font)
    {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&FaceOf(font).info, &ascent, &descent, &gap);
        return (ascent - descent + gap) * Scale(font);
    }

    const Glyph& GlyphOf(FontId font, wchar_t character)
    {
        const uint64_t key = (static_cast<uint64_t>(Index(font)) << 32) | static_cast<uint32_t>(character);
        const auto found = glyphs_.find(key);
        if (found != glyphs_.end()) return found->second;
        Glyph glyph;
        const stbtt_fontinfo* info = &FaceOf(font).info;
        const float scale = Scale(font);
        const int index = stbtt_FindGlyphIndex(info, static_cast<int>(character));
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(info, index, &advance, &bearing);
        glyph.advance = advance * scale;
        if (unsigned char* bitmap = stbtt_GetGlyphBitmap(info, scale, scale, index, &glyph.w, &glyph.h,
                                                         &glyph.left, &glyph.top)) {
            glyph.cover.assign(bitmap, bitmap + static_cast<size_t>(glyph.w) * glyph.h);
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        return glyphs_.emplace(key, std::move(glyph)).first->second;
    }

    float Measure(FontId font, const std::wstring& text)
    {
        float width = 0.0f;
        for (const wchar_t c : text) width += GlyphOf(font, c).advance;
        return width;
    }

    // One line no wider than `width`: what does not fit is replaced by dots.
    std::wstring Fit(FontId font, const std::wstring& text, float width)
    {
        if (Measure(font, text) <= width) return text;
        std::wstring head = text;
        while (head.size() > 1 && Measure(font, head + L"...") > width) head.pop_back();
        return head + L"...";
    }

    // Lines broken to fit `width`: at spaces, or for a path after a slash
    // (a folder name is kept whole where it can be). A run longer than a
    // line is cut where the line ends.
    std::vector<std::wstring> Wrap(FontId font, const std::wstring& text, float width, bool path = false)
    {
        std::vector<std::wstring> lines;
        std::wstring line;
        size_t keep = 0, resume = 0; // the last place to break: the line's length, and where the next starts
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == L'\n') {
                lines.push_back(line);
                line.clear();
                keep = resume = 0;
                continue;
            }
            const size_t before = line.size();
            line.push_back(text[i]);
            if (Measure(font, line) > width && before > 0) {
                const bool broken = keep > 0;
                lines.push_back(line.substr(0, broken ? keep : before));
                line = line.substr(broken ? resume : before);
                keep = resume = 0;
            }
            if (!path && text[i] == L' ') {
                keep = line.size() - 1; // the space itself is dropped
                resume = line.size();
            } else if (path && (text[i] == L'/' || text[i] == L'\\')) {
                keep = resume = line.size(); // the slash stays at the line's end
            }
        }
        return lines;
    }

    void DrawLine(FontId font, const std::wstring& text, float x, float baseline, Color color)
    {
        const int r = static_cast<int>(color.r * 255.0f), g = static_cast<int>(color.g * 255.0f),
                  b = static_cast<int>(color.b * 255.0f);
        const int base = static_cast<int>(std::floor(baseline + 0.5f));
        for (const wchar_t c : text) {
            const Glyph& glyph = GlyphOf(font, c);
            const int originX = static_cast<int>(std::floor(x + 0.5f)) + glyph.left;
            const int originY = base + glyph.top;
            for (int row = 0; row < glyph.h; ++row) {
                const int py = originY + row;
                if (py < 0 || py >= height_) continue;
                for (int column = 0; column < glyph.w; ++column) {
                    const int px = originX + column;
                    const int cover = glyph.cover[static_cast<size_t>(row) * glyph.w + column];
                    if (!cover || px < 0 || px >= width_) continue;
                    uint32_t& pixel = pixels_[static_cast<size_t>(py) * width_ + px];
                    const int alpha = cover + (cover >> 7), keep = 256 - alpha;
                    const int dr = (pixel >> 16) & 255, dg = (pixel >> 8) & 255, db = pixel & 255;
                    pixel = static_cast<uint32_t>(((r * alpha + dr * keep) >> 8) << 16 |
                                                  ((g * alpha + dg * keep) >> 8) << 8 |
                                                  ((b * alpha + db * keep) >> 8));
                }
            }
            x += glyph.advance;
        }
    }

    uint32_t* pixels_;
    int width_, height_;
    float scale_;
    Face regular_, bold_;
    std::map<uint64_t, Glyph> glyphs_;
};

// ---------------------------------------------------------------------------
// One screen from the state: shared by the window and the screenshots.
// ---------------------------------------------------------------------------
enum class Action { Nothing, Install, Uninstall, Browse, Close, Retry, Launch };

struct Panel {
    std::wstring title, message, detail, status;
    const wchar_t* labels[kMaxButtons] = {};
    Action actions[kMaxButtons] = {};
    int buttonCount = 0;
    bool busy = false, progress = false;
    float fraction = -1.0f;

    void Button(const wchar_t* label, Action action)
    {
        labels[buttonCount] = label;
        actions[buttonCount] = action;
        ++buttonCount;
    }
};

Panel PanelFor(const State& state)
{
    Panel screen;
    switch (state.stage) {
    case Stage::Searching:
        screen.title = L"TIMESTONE";
        screen.message = L"Looking for Ultimate Marvel vs. Capcom 3...";
        screen.busy = true;
        break;
    case Stage::NotFound:
        screen.title = L"GAME NOT FOUND";
        screen.message = Wide(state.message);
        screen.Button(L"BROWSE", Action::Browse);
        screen.Button(L"CLOSE", Action::Close);
        break;
    case Stage::Ready: {
        const wchar_t* go = L"INSTALL";
        if (state.installed.empty()) {
            screen.title = L"INSTALL TIMESTONE";
            screen.message = Wide("Timestone " + state.latest + " will be installed into:");
        } else if (IsNewerVersion(state.latest.c_str(), state.installed.c_str())) {
            screen.title = L"UPDATE TIMESTONE";
            screen.message = Wide("Timestone " + state.installed + " will be updated to " +
                                  state.latest + " in:");
            go = L"UPDATE";
        } else {
            screen.title = L"TIMESTONE IS UP TO DATE";
            screen.message = Wide("Timestone " + state.installed + " is already installed in:");
            go = L"REINSTALL";
        }
        if (state.protonPending)
            screen.message = L"Start the game once from Steam, close it, then run this installer "
                             L"again so it can tell Proton to load Timestone.\n\n" + screen.message;
        screen.detail = Wide(state.gameDir);
        screen.Button(go, Action::Install);
        if (!state.installed.empty()) screen.Button(L"UNINSTALL", Action::Uninstall);
        screen.Button(L"BROWSE", Action::Browse);
        screen.Button(L"CLOSE", Action::Close);
        break;
    }
    case Stage::WaitGame:
        screen.title = L"CLOSE THE GAME";
        screen.message = L"Ultimate Marvel vs. Capcom 3 is running.\n"
                         L"The install continues when it closes.";
        screen.busy = true;
        screen.Button(L"CLOSE", Action::Close);
        break;
    case Stage::Working:
        screen.title = L"INSTALLING TIMESTONE";
        screen.message = Wide(state.message);
        screen.progress = true;
        screen.fraction = state.fraction;
        screen.status = Wide(state.status);
        break;
    case Stage::Done:
        screen.title = L"TIMESTONE INSTALLED";
        screen.message = Wide(state.message);
        screen.Button(L"LAUNCH GAME", Action::Launch);
        screen.Button(L"CLOSE", Action::Close);
        break;
    case Stage::Removed:
        screen.title = L"TIMESTONE REMOVED";
        screen.message = Wide(state.message);
        screen.detail = Wide(state.gameDir);
        screen.Button(L"CLOSE", Action::Close);
        break;
    case Stage::Failed:
        screen.title = Wide(state.title);
        screen.message = Wide(state.message);
        screen.Button(L"RETRY", Action::Retry);
        screen.Button(L"CLOSE", Action::Close);
        break;
    }
    return screen;
}

Layout DrawState(SoftCanvas& canvas, TextPainter& text, const Panel& screen, int cursor, uint32_t frame)
{
    View view;
    view.title = screen.title.c_str();
    view.message = screen.message.c_str();
    view.detail = screen.detail.c_str();
    view.status = screen.status.c_str();
    view.footer = L"Timestone Install " TIMESTONE_VERSION;
    for (int i = 0; i < screen.buttonCount; ++i) view.buttons[i] = screen.labels[i];
    view.buttonCount = screen.buttonCount;
    view.cursor = cursor;
    view.busy = screen.busy;
    view.progress = screen.progress;
    view.fraction = screen.fraction;
    return DrawScreen(canvas, text, view, frame);
}

bool LoadStars(std::vector<uint8_t>& rgba)
{
    rgba.resize(lobby::kSpaceStarsRgbaBytes);
    if (lobby::BuildSpaceStarsRgba(timestone_stars_tile,
            static_cast<size_t>(timestone_stars_tile_end - timestone_stars_tile), rgba.data()))
        return true;
    rgba.clear();
    return false;
}

// ---------------------------------------------------------------------------
// Without a window: the same steps, said in the terminal.
// ---------------------------------------------------------------------------
int RunInTerminal()
{
    printf("Timestone Install %s\nLooking for Ultimate Marvel vs. Capcom 3...\n", TIMESTONE_VERSION);
    Detect();
    State state = g_state;
    if (state.stage == Stage::Ready) {
        printf("Game folder: %s\n", state.gameDir.c_str());
        if (!state.installed.empty() && !IsNewerVersion(state.latest.c_str(), state.installed.c_str())) {
            printf("Timestone %s is already installed and up to date.\n", state.installed.c_str());
            if (state.protonPending)
                printf("Start the game once from Steam, close it, then run this again so it can "
                       "tell Proton to load Timestone.\n");
            return 0;
        }
        if (GameRunning()) printf("The game is running; waiting for it to close...\n");
        printf("Downloading and installing Timestone %s...\n", state.latest.c_str());
        Install();
        state = g_state;
    }
    if (state.stage == Stage::NotFound)
        state.message += "\nGive it when starting the installer: sh TimestoneInstall.sh \"/path/to/game\"";
    printf("%s\n", state.message.c_str());
    return state.stage == Stage::Done ? 0 : 1;
}

// Pictures of the screens as the window draws them, for looking at the
// layout without a display: TimestoneInstall --screens <folder>.
int WriteScreens(const char* folder)
{
    const int width = 1120, height = 630;
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    std::vector<uint8_t> stars;
    LoadStars(stars);
    FontText text(pixels.data(), width, height);
    if (!text.Load()) {
        fprintf(stderr, "no usable font found\n");
        return 1;
    }
    State base;
    base.gameDir = "/home/deck/.local/share/Steam/steamapps/common/ULTIMATE MARVEL VS. CAPCOM 3";
    base.latest = "v0.6.6";
    struct Shot {
        const char* name;
        State state;
        int cursor;
        uint32_t frame;
    };
    std::vector<Shot> shots;
    const auto add = [&](const char* name, uint32_t frame, int cursor, auto&& change) {
        Shot shot{name, base, cursor, frame};
        change(shot.state);
        shots.push_back(shot);
    };
    add("searching", 40, 0, [](State& s) { s.stage = Stage::Searching; });
    add("ready", 1500, 0, [](State& s) { s.stage = Stage::Ready; });
    add("update", 2400, 0, [](State& s) { s.stage = Stage::Ready; s.installed = "0.6.5"; });
    add("pending", 2800, 2, [](State& s) {
        s.stage = Stage::Ready; s.installed = "0.6.6"; s.protonPending = true;
        s.gameDir = "/run/media/deck/a-very-long-sd-card-name/SteamLibrary/steamapps/common/ULTIMATE MARVEL VS. CAPCOM 3";
    });
    add("progress", 3200, 0, [](State& s) {
        s.stage = Stage::Working; s.message = "Downloading Timestone v0.6.6";
        s.fraction = 0.62f; s.status = "4.9 MB of 7.9 MB";
    });
    add("done", 4000, 0, [](State& s) {
        s.stage = Stage::Done; s.message = "Timestone v0.6.6 is installed.\nProton is set to load it.";
    });
    add("installed", 4300, 1, [](State& s) { s.stage = Stage::Ready; s.installed = "0.6.6"; });
    add("removed", 4500, 0, [](State& s) {
        s.stage = Stage::Removed;
        s.message = "dinput8.dll was kept: another .asi mod in the folder needs it.\n"
                    "Timestone was removed from:";
    });
    add("failed", 4700, 1, [](State& s) {
        s.stage = Stage::Failed; s.title = "INSTALL FAILED";
        s.message = "The new files could not be put into the game folder.\n\nPermission denied: dinput8.dll";
    });
    add("notfound", 5200, 0, [](State& s) {
        s.stage = Stage::NotFound;
        s.message = "Ultimate Marvel vs. Capcom 3 was not found in your Steam libraries.\n"
                    "Choose the folder that holds umvc3.exe.";
    });
    for (const Shot& shot : shots) {
        SoftCanvas canvas(pixels.data(), width, height, stars.empty() ? nullptr : stars.data());
        DrawState(canvas, text, PanelFor(shot.state), shot.cursor, shot.frame);
        const std::string path = std::string(folder) + "/install_" + shot.name + ".ppm";
        FILE* out = fopen(path.c_str(), "wb");
        if (!out) return 1;
        fprintf(out, "P6\n%d %d\n255\n", width, height);
        for (const uint32_t pixel : pixels) {
            const unsigned char rgb[3] = {static_cast<unsigned char>(pixel >> 16),
                                          static_cast<unsigned char>(pixel >> 8),
                                          static_cast<unsigned char>(pixel)};
            fwrite(rgb, 1, 3, out);
        }
        fclose(out);
        printf("wrote %s\n", path.c_str());
    }
    return 0;
}

} // namespace

// X11 comes last: its headers turn ordinary words (None, Status, Bool) into
// macros.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

namespace {

int RunWindow(Display* display)
{
    const int screen = DefaultScreen(display);
    Visual* visual = DefaultVisual(display, screen);
    const int depth = DefaultDepth(display, screen);
    // The picture is 0x00RRGGBB words; anything else is a display from another age.
    if (depth < 24 || visual->red_mask != 0xff0000 || visual->blue_mask != 0xff) return -1;

    // 1120 x 630, larger on a very dense screen, never more than most of it.
    // The picture is drawn by this program, so it is kept to a size a
    // processor fills comfortably.
    const int screenW = DisplayWidth(display, screen), screenH = DisplayHeight(display, screen);
    int width = screenH >= 1800 ? 1600 : 1120;
    if (width > screenW * 9 / 10) width = screenW * 9 / 10;
    if (width * 9 / 16 > screenH * 85 / 100) width = screenH * 85 / 100 * 16 / 9;
    if (width < 640) width = 640;
    width &= ~1;
    const int height = width * 9 / 16;

    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    std::vector<uint8_t> stars;
    LoadStars(stars); // without them the backdrop is its grid on black
    FontText text(pixels.data(), width, height);
    if (!text.Load()) return -1;

    const Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
        (screenW - width) / 2, (screenH - height) / 2, width, height, 0, 0, 0);
    XStoreName(display, window, "Timestone Install");
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_NAME", False), utf8, 8,
        PropModeReplace, reinterpret_cast<const unsigned char*>("Timestone Install"), 17);
    XClassHint classHint;
    char className[] = "TimestoneInstall";
    classHint.res_name = className;
    classHint.res_class = className;
    XSetClassHint(display, window, &classHint);
    XSizeHints sizeHints = {};
    sizeHints.flags = PPosition | PSize | PMinSize | PMaxSize;
    sizeHints.x = (screenW - width) / 2;
    sizeHints.y = (screenH - height) / 2;
    sizeHints.width = sizeHints.min_width = sizeHints.max_width = width;
    sizeHints.height = sizeHints.min_height = sizeHints.max_height = height;
    XSetWMNormalHints(display, window, &sizeHints);
    Atom closeRequest = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &closeRequest, 1);
    XSelectInput(display, window,
        ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
            StructureNotifyMask);
    XMapWindow(display, window);

    const GC gc = XCreateGC(display, window, 0, nullptr);
    XImage* image = XCreateImage(display, visual, static_cast<unsigned>(depth), ZPixmap, 0,
        reinterpret_cast<char*>(pixels.data()), static_cast<unsigned>(width),
        static_cast<unsigned>(height), 32, 0);
    if (!image) return -1;

    const auto startedAt = std::chrono::steady_clock::now();
    Stage shownStage = Stage::Searching;
    int cursor = 0;
    Panel shown;
    Layout layout{};
    const float scale = width / kScreenW;
    const auto buttonAt = [&](int x, int y) {
        for (int i = 0; i < shown.buttonCount; ++i)
            if (Contains(layout.buttons[i], x / scale, y / scale)) return i;
        return -1;
    };
    int pressed = -1; // the button the mouse went down on
    bool open = true;
    const auto perform = [&](Action action) {
        switch (action) {
        case Action::Install: StartTask(Task::Install); break;
        case Action::Uninstall:
            if (!g_busy) Uninstall();
            break;
        case Action::Browse: StartTask(Task::Browse); break;
        case Action::Retry: {
            Task task;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                task = g_state.retry;
            }
            StartTask(task);
            break;
        }
        case Action::Launch:
            // Steam's own link, handed to the desktop.
            if (Wait(Spawn({"xdg-open", std::string("steam://rungameid/") + kSteamAppId}, -1)) != 0)
                Spawn({"steam", std::string("steam://rungameid/") + kSteamAppId}, -1);
            open = false;
            break;
        case Action::Close: {
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_state.committing) open = false;
            break;
        }
        case Action::Nothing: break;
        }
    };

    StartTask(Task::Detect);
    while (open) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            switch (event.type) {
            case MotionNotify: {
                const int over = buttonAt(event.xmotion.x, event.xmotion.y);
                if (over >= 0) cursor = over;
                break;
            }
            // A click is a press and a release on the same button. The
            // press has to be asked for: X sends a release only to the
            // window that took the press.
            case ButtonPress:
                if (event.xbutton.button == Button1) {
                    pressed = buttonAt(event.xbutton.x, event.xbutton.y);
                    if (pressed >= 0) cursor = pressed;
                }
                break;
            case ButtonRelease: {
                if (event.xbutton.button != Button1) break;
                const int over = buttonAt(event.xbutton.x, event.xbutton.y);
                if (over >= 0 && over == pressed) perform(shown.actions[over]);
                pressed = -1;
                break;
            }
            case KeyPress: {
                const KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape) {
                    perform(Action::Close);
                } else if (shown.buttonCount > 0) {
                    if (key == XK_Left || key == XK_Up)
                        cursor = (cursor + shown.buttonCount - 1) % shown.buttonCount;
                    else if (key == XK_Right || key == XK_Down || key == XK_Tab)
                        cursor = (cursor + 1) % shown.buttonCount;
                    else if (key == XK_Return || key == XK_KP_Enter || key == XK_space)
                        perform(shown.actions[cursor]);
                }
                break;
            }
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == closeRequest) perform(Action::Close);
                break;
            }
        }
        if (!open) break;

        State state;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            state = g_state;
        }
        if (state.stage != shownStage) {
            shownStage = state.stage;
            cursor = 0;
        }
        shown = PanelFor(state);
        if (cursor >= shown.buttonCount) cursor = shown.buttonCount > 0 ? shown.buttonCount - 1 : 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        SoftCanvas canvas(pixels.data(), width, height, stars.empty() ? nullptr : stars.data());
        layout = DrawState(canvas, text, shown, cursor, static_cast<uint32_t>(elapsed * 60 / 1000));
        XPutImage(display, window, gc, image, 0, 0, 0, 0, static_cast<unsigned>(width),
            static_cast<unsigned>(height));
        XFlush(display);

        // About thirty pictures a second, sooner when there is input.
        pollfd waiting{ConnectionNumber(display), POLLIN, 0};
        poll(&waiting, 1, 33);
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    bool textOnly = false, uninstall = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--screens" && i + 1 < argc) return WriteScreens(argv[i + 1]);
        if (argument == "--text") {
            textOnly = true;
        } else if (argument == "--uninstall") {
            uninstall = true;
        } else if (IsGameFolder(argument)) {
            g_chosenDir = argument;
        } else {
            fprintf(stderr, "TimestoneInstall: umvc3.exe is not in \"%s\".\n", argument.c_str());
            return 2;
        }
    }
    if (uninstall) {
        const std::string gameDir = !g_chosenDir.empty() ? g_chosenDir : FindGame();
        if (gameDir.empty()) {
            fprintf(stderr, "TimestoneInstall: the game was not found; give its folder.\n");
            return 1;
        }
        g_state.gameDir = gameDir;
        Uninstall();
        const bool removed = g_state.stage == Stage::Removed;
        printf("%s%s\n", g_state.message.c_str(), removed ? (" " + gameDir).c_str() : "");
        return removed ? 0 : 1;
    }
    if (!textOnly) {
        if (Display* display = XOpenDisplay(nullptr)) {
            if (RunWindow(display) == 0) {
                fflush(nullptr);
                _exit(0); // a download still running ends with the program
            }
        }
        fprintf(stderr, "TimestoneInstall: no window could be opened; carrying on in the terminal.\n");
    }
    return RunInTerminal();
}
