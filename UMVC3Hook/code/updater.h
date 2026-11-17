#pragma once

#include <cstddef>

// Timestone's self-update, entirely in the plugin: the online menu checks
// GitHub for a newer release, downloads and verifies it in the background,
// swaps it in beside the running game (a loaded DLL can be renamed, just not
// overwritten) and restarts the game.

// Very first thing at plugin start-up. If this process was started by an
// update or a launch-menu return, waits for the old process to exit, then
// clears what an earlier update left behind.
void Updater_OnProcessStart();

enum class UpdateCheck {
    Idle,      // not started
    Checking,
    UpToDate,
    Available, // Updater_LatestVersion() names the release
    Failed,    // no network, GitHub unavailable, or an unexpected answer
};

// Starts the check once per game session; later calls do nothing.
void Updater_BeginOnlineCheck();
UpdateCheck Updater_PollOnlineCheck();
const char* Updater_LatestVersion(); // the release tag, e.g. "v0.5.2"

struct UpdateProgress {
    enum State { Idle, Downloading, Ready, Failed } state;
    char stage[160];
    char error[256];
    unsigned long long downloaded;
    unsigned long long total; // 0 while unknown
    double bytesPerSecond;
};

// Downloads, verifies and unpacks the release found by the check, beside
// the installed ASI. Nothing is installed until Updater_InstallAndRestart.
bool Updater_BeginDownload();
UpdateProgress Updater_PollDownload();
void Updater_CancelDownload();
// Once Ready: puts the new ASI in place and restarts the game (does not
// return on success). On failure the installed ASI is left as it was and
// `error` says why.
bool Updater_InstallAndRestart(char* error, size_t capacity);
