// Standalone Win32 integration test; run under Windows or an isolated Wine prefix.
// Build with MinGW C++20: tools/test_ini_safe.cpp + UMVC3Hook/IniSafe.cpp (-static).
#include "../UMVC3Hook/IniSafe.h"
#include "../UMVC3Hook/IniText.h"
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
        for (int i = 0; i < 20; ++i)
            if (!IniSafe_WriteString("Workers", argv[3], std::to_string(i).c_str(), argv[2]))
                return 1;
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
    for (int i = 0; i < 4; ++i)
        assert(GetPrivateProfileIntA("Workers", ("key" + std::to_string(i)).c_str(),
                                    -1, path.c_str()) == 19);

    assert(DeleteFileA(path.c_str()));
    assert(DeleteFileA(backup.c_str()));
    assert(RemoveDirectoryA(directory)); // Also checks that temporary files were cleaned up.
    puts("INI safe-write tests passed");
}
