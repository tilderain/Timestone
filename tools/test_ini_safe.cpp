// Standalone Win32 integration test; run under Windows or an isolated Wine prefix.
// Build with MinGW C++20: this file + UMVC3Hook/IniSafe.cpp +
// UMVC3Hook/code/connect_roster.cpp (-static).
#include "../UMVC3Hook/IniSafe.h"
#include "../UMVC3Hook/IniText.h"
#include "../UMVC3Hook/code/connect_roster.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>

void HookLog(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static std::string Read(const std::string& path)
{
    FILE* file = fopen(path.c_str(), "rb");
    assert(file);
    std::string bytes;
    char buffer[1024];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file))) bytes.append(buffer, count);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    return bytes;
}

static void Write(const std::string& path, const std::string& bytes)
{
    FILE* file = fopen(path.c_str(), "wb");
    assert(file);
    assert(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    assert(fclose(file) == 0);
}

int main(int argc, char** argv)
{
    if (argc == 4) {
        for (int i = 0; i < 20; ++i) {
            const IniSafe_Entry updates[] = {
                {"Workers", argv[3], std::to_string(i)},
                {"Workers", std::string(argv[3]) + "_pair", std::to_string(i)}
            };
            if (!IniSafe_WriteBatch(updates, 2, argv[2]))
                return 1;
        }
        return 0;
    }

    using IniText::Update;
    assert(Update<char>("[A]\nx=1\n[B]\ny=2\n", "a", "new", "3") ==
           "[A]\nx=1\nnew=3\n[B]\ny=2\n");
    assert(Update<char>("[A]\nx=1", "A", "new", "3") ==
           "[A]\nx=1\nnew=3\n");
    assert(Update<char>("[A]\r\n  X =1\r\n", "a", "x", "2") ==
           "[A]\r\n  X =2\r\n");
    assert(Update<char>("[A] ; comment\rx=1\r[B]\ry=2\r", "A", "x", "2") ==
           "[A] ; comment\rx=2\r[B]\ry=2\r");
    assert(Update<char>("[A]\nx=1\n[A]\nx=9\n", "A", "x", "2") ==
           "[A]\nx=2\n[A]\nx=9\n");

    char root[MAX_PATH], directory[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, root));
    assert(GetTempFileNameA(root, "its", 0, directory));
    assert(DeleteFileA(directory));
    assert(CreateDirectoryA(directory, nullptr));
    const std::string path = std::string(directory) + "\\settings.ini";
    const std::string backup = path + ".bak";
    const std::string original = "; preserved\r\n[Network]\r\nHostPort=1\r\n"
        "this is malformed\r\n[Broken\r\nopaque data\r\n[Other]\r\nKeep=yes\r\n";
    Write(path, original);
    assert(IniSafe_WriteString("Network", "HostPort", "2", path.c_str()));
    std::string expected = original;
    expected.replace(expected.find("HostPort=1"), 10, "HostPort=2");
    assert(Read(path) == expected);
    assert(Read(backup) == original);
    assert(IniSafe_WriteString("Network", "HostPort", "2", path.c_str()));
    assert(Read(backup) == original); // No-op does not churn the backup.
    assert(GetPrivateProfileIntA("Network", "HostPort", 0, path.c_str()) == 2);
    assert(IniSafe_WriteString("Network", "HostPort", "3", path.c_str()));
    assert(GetPrivateProfileIntA("Network", "HostPort", 0, path.c_str()) == 3);

    // A real read/sharing failure must not be treated as an empty configuration.
    const std::string beforeFailure = Read(path);
    HANDLE blocked = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    assert(blocked != INVALID_HANDLE_VALUE);
    assert(!IniSafe_WriteString("Network", "HostPort", "4", path.c_str()));
    CloseHandle(blocked);
    assert(Read(path) == beforeFailure);

    // Read succeeds, but replacement is denied: still no damage to live bytes.
    blocked = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    assert(blocked != INVALID_HANDLE_VALUE);
    assert(!IniSafe_WriteString("Network", "HostPort", "4", path.c_str()));
    CloseHandle(blocked);
    assert(Read(path) == beforeFailure);
    assert(IniSafe_WriteString("Network", "HostPort", "4", path.c_str())); // Retry works.

    // Backup failure aborts before committing the new live file.
    assert(DeleteFileA(backup.c_str()));
    assert(CreateDirectoryA(backup.c_str(), nullptr));
    const std::string beforeBackupFailure = Read(path);
    assert(!IniSafe_WriteString("Network", "HostPort", "5", path.c_str()));
    assert(Read(path) == beforeBackupFailure);
    assert(RemoveDirectoryA(backup.c_str()));

    // Embedded NULs and junk are not reasons to throw away unrelated bytes.
    std::string damaged = "junk";
    damaged += '\0';
    damaged += "more\n[Network]\nHostPort=7\n";
    Write(path, damaged);
    assert(IniSafe_WriteString("Network", "HostPort", "8", path.c_str()));
    assert(Read(backup) == damaged);
    damaged[damaged.size() - 2] = '8';
    assert(Read(path) == damaged);

    for (bool little : {true, false}) {
        std::string utf16 = little ? "\xff\xfe" : "\xfe\xff";
        for (char c : std::string("[A]\r\nx=1\r\n")) {
            utf16 += little ? c : '\0';
            utf16 += little ? '\0' : c;
        }
        Write(path, utf16);
        assert(IniSafe_WriteString("A", "x", "2", path.c_str()));
        assert(Read(backup) == utf16);
        utf16[2 + 7 * 2 + (little ? 0 : 1)] = '2';
        assert(Read(path) == utf16);
    }
    Write(path, "\xef\xbb\xbf[A]\nx=1\n");
    assert(IniSafe_WriteString("A", "x", "2", path.c_str()));
    assert(Read(path) == "\xef\xbb\xbf[A]\nx=2\n");

    // Empty existing files can be updated, without replacing an existing backup.
    const auto keptBackup = Read(backup);
    Write(path, "");
    assert(IniSafe_WriteString("A", "x", "2", path.c_str()));
    assert(Read(backup) == keptBackup);
    assert(DeleteFileA(path.c_str()));
    assert(IniSafe_WriteString("A", "x", "2", path.c_str()));
    assert(!IniSafe_WriteString("A", "x", "bad\nvalue", path.c_str()));

    // Concurrent processes must merge, not clobber, each other's updates.
    char executable[MAX_PATH];
    assert(GetModuleFileNameA(nullptr, executable, MAX_PATH));
    std::vector<HANDLE> workers;
    for (int i = 0; i < 4; ++i) {
        std::string command = "\"" + std::string(executable) + "\" --worker \"" +
            path + "\" key" + std::to_string(i);
        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        assert(CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                              nullptr, nullptr, &startup, &process));
        CloseHandle(process.hThread);
        workers.push_back(process.hProcess);
    }
    for (HANDLE worker : workers) {
        assert(WaitForSingleObject(worker, 30000) == WAIT_OBJECT_0);
        DWORD status = 1;
        assert(GetExitCodeProcess(worker, &status) && status == 0);
        CloseHandle(worker);
    }
    for (int i = 0; i < 4; ++i) {
        assert(GetPrivateProfileIntA("Workers", ("key" + std::to_string(i)).c_str(),
                                    -1, path.c_str()) == 19);
        assert(GetPrivateProfileIntA("Workers", ("key" + std::to_string(i) + "_pair").c_str(),
                                    -1, path.c_str()) == 19);
    }

    // Batches preserve one pre-transaction backup and fail without partial changes.
    Write(path, "[A]\nx=1\ny=1\n");
    const IniSafe_Entry batch[] = {{"A", "x", "2"}, {"A", "y", "3"}};
    assert(IniSafe_WriteBatch(batch, 2, path.c_str()));
    assert(Read(path) == "[A]\nx=2\ny=3\n");
    assert(Read(backup) == "[A]\nx=1\ny=1\n");
    const IniSafe_Entry invalidBatch[] = {{"A", "x", "4"}, {"A", "y", "bad\nvalue"}};
    assert(!IniSafe_WriteBatch(invalidBatch, 2, path.c_str()));
    assert(Read(path) == "[A]\nx=2\ny=3\n");
    blocked = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    assert(blocked != INVALID_HANDLE_VALUE);
    const IniSafe_Entry changedBatch[] = {{"A", "x", "4"}, {"A", "y", "5"}};
    assert(!IniSafe_WriteBatch(changedBatch, 2, path.c_str()));
    CloseHandle(blocked);
    assert(Read(path) == "[A]\nx=2\ny=3\n");

    // Personal defaults never come from legacy or advanced-menu rosters.
    const std::string legacy = std::string(directory) + "\\rollbacksetting.ini";
    const std::string legacyBytes = "[P1]\nSlot1=49\n[P2]\nSlot1=48\n";
    Write(legacy, legacyBytes);
    const std::string destination = "; keep this\nmalformed line\n[Roster.P1]\nSlot1=42\n"
        "[Roster.P2]\nSlot1=43\nSimpleMode=1\n[Training]\nStageId=30\n"
        "[Network]\nHostIp=\nHostPort=5678\n[Other]\nKeep=yes\n";
    Write(path, destination);
    ConnectRoster defaults;
    assert(defaults.stageId == STAGE_BONNE_WONDERLAND);
    assert(ConnectRoster_Load(path.c_str(), defaults).stageId == STAGE_BONNE_WONDERLAND);
    // A partial personal roster also defaults to Bonne, but an explicitly saved
    // Training Room remains a valid preference rather than "uninitialized".
    Write(path, destination + "[ConnectRoster]\nSlot1=12\n");
    assert(ConnectRoster_Load(path.c_str(), defaults).stageId == STAGE_BONNE_WONDERLAND);
    Write(path, destination + "[ConnectRoster]\nStageId=0\n");
    assert(ConnectRoster_Load(path.c_str(), defaults).stageId == 0);
    Write(path, destination);
    defaults.stageId = 9;
    auto personal = ConnectRoster_Load(path.c_str(), defaults);
    assert(personal.chars[0] == 1 && !personal.simpleMode && personal.stageId == 9);
    assert(Read(path) == destination); // Reads don't write defaults or migrate anything.

    int32_t chars[2][3] = {{11, 12, 13}, {21, 22, 23}};
    int32_t colors[2][3] = {{1, 2, 3}, {4, 5, 6}};
    int32_t assists[2][3] = {{0, 1, 2}, {2, 1, 0}};
    bool simpleMode[2] = {true, false};
    ConnectRoster_UpdateLocalTeam(personal, true, chars, colors, assists, simpleMode);
    assert(ConnectRoster_Save(path.c_str(), personal));
    assert(Read(backup) == destination); // One pre-batch backup for all 11 preferences.
    auto saved = ConnectRoster_Load(path.c_str(), defaults);
    for (int i = 0; i < 3; ++i) {
        assert(saved.chars[i] == chars[0][i]);
        assert(saved.colors[i] == colors[0][i]);
        assert(saved.assists[i] == assists[0][i]);
    }
    assert(saved.simpleMode && saved.stageId == 9);
    assert(Read(path).find(destination) == 0); // Advanced/other sections untouched.

    // The same personal roster follows the player when joining as P2.
    for (int i = 0; i < 3; ++i) {
        chars[1][i] = saved.chars[i];
        colors[1][i] = saved.colors[i];
        assists[1][i] = saved.assists[i];
        chars[0][i] = 40 + i; // New host's unrelated team.
    }
    simpleMode[0] = false;
    simpleMode[1] = true;
    ConnectRoster_UpdateLocalTeam(personal, false, chars, colors, assists, simpleMode);
    assert(ConnectRoster_Save(path.c_str(), personal));
    assert(Read(backup) == destination); // Role switch with same team is a no-op.

    // Editing P2 changes only our preferences, never the host's or advanced P2.
    chars[1][0] = 31;
    colors[1][1] = 6;
    assists[1][2] = 1;
    simpleMode[1] = false;
    ConnectRoster_UpdateLocalTeam(personal, false, chars, colors, assists, simpleMode);
    assert(personal.stageId == 9); // No match/host stage is captured by this API.
    assert(ConnectRoster_Save(path.c_str(), personal));
    saved = ConnectRoster_Load(path.c_str(), defaults);
    assert(saved.chars[0] == 31 && saved.colors[1] == 6 && saved.assists[2] == 1);
    assert(!saved.simpleMode && saved.stageId == 9);
    assert(GetPrivateProfileIntA("Roster.P1", "Slot1", -1, path.c_str()) == 42);
    assert(GetPrivateProfileIntA("Roster.P2", "Slot1", -1, path.c_str()) == 43);
    assert(GetPrivateProfileIntA("Training", "StageId", -1, path.c_str()) == 30);
    assert(GetPrivateProfileIntA("Network", "HostPort", -1, path.c_str()) == 5678);
    assert(Read(legacy) == legacyBytes);

    // Preferred stage changes only explicitly, and survives later joining saves.
    personal.stageId = 17;
    assert(ConnectRoster_Save(path.c_str(), personal));
    ConnectRoster_UpdateLocalTeam(personal, false, chars, colors, assists, simpleMode);
    assert(ConnectRoster_Save(path.c_str(), personal));
    assert(ConnectRoster_Load(path.c_str(), defaults).stageId == 17);

    const auto beforePersonalFailure = Read(path);
    blocked = CreateFileA(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(blocked != INVALID_HANDLE_VALUE);
    personal.chars[0] = 32;
    assert(!ConnectRoster_Save(path.c_str(), personal));
    CloseHandle(blocked);
    assert(Read(path) == beforePersonalFailure);
    assert(ConnectRoster_Save(path.c_str(), personal));
    assert(ConnectRoster_Load(path.c_str(), defaults).chars[0] == 32);
    assert(DeleteFileA(legacy.c_str()));
    assert(DeleteFileA(path.c_str()));

    assert(DeleteFileA(backup.c_str()));
    assert(RemoveDirectoryA(directory)); // Also checks that temporary files were cleaned up.
    puts("INI safe-write tests passed");
}
