#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Launch option enum
// ---------------------------------------------------------------------------
enum LaunchOption {
    LAUNCH_NORMAL = 0,
    LAUNCH_TRAINING = 1,
    LAUNCH_DUAL_TRAINING = 2,
};

// Character display names (index = game character ID)
extern const char* kCharNames[];
extern const int kCharNameCount;

// Stage entries: display name -> game stage ID
struct StageEntry {
    const char* name;
    int32_t id;
};

extern const StageEntry kStages[];
extern const int kStageCount;

// Configuration from the launch dialog
struct LaunchDialogConfig {
    // Character IDs for P1 and P2 (point, assist1, assist2)
    int32_t p1Chars[3];
    int32_t p2Chars[3];
    int32_t stageId;
    // Network config (GGPO handles the actual connection)
    char    remoteIP[64];
    int     remotePort;
    int     localPort;
    bool    hostEnabled;
    bool    isHost;
};

// Shows a Win32 dialog before the game initializes.
// Blocks until the user makes a selection.
// Returns the chosen LaunchOption.
LaunchOption ShowLaunchMenu();

// Returns the launch option chosen at startup.
LaunchOption GetLaunchOption();

// Set launch option (e.g. from command-line args for spawned instance)
void SetLaunchOption(LaunchOption opt);

// Get the dialog config (characters, stage, network settings)
const LaunchDialogConfig& GetLaunchDialogConfig();

// Mark networking as active (GGPO session will be started on game init)
void SetNetworkActive();

// Clean up any active network session (call on DLL detach / game exit)
void CleanupNetworkOnExit();
