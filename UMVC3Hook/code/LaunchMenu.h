#pragma once

#include <cstdint>
#include <enet/enet.h>

struct LaunchTrainingPacket;

enum LaunchOption {
    LAUNCH_CANCELLED = -1,
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
    int32_t p1Colors[3];
    int32_t p2Colors[3];
    int32_t p1Assists[3];
    int32_t p2Assists[3];
    int32_t stageId;
    // Network host config
    char    hostIp[64];
    int     hostPort;
    bool    hostEnabled;
    // enet handles (set by host/connect threads)
    ENetHost* enetHost;
    ENetPeer* enetPeer;
};

// Shows a Win32 dialog before the game initializes.
// Blocks until the user makes a selection.
// Returns LAUNCH_CANCELLED if the user closes the window.
LaunchOption ShowLaunchMenu();

// Returns the launch option chosen at startup.
LaunchOption GetLaunchOption();

// Set launch option (e.g. from command-line args for spawned instance)
void SetLaunchOption(LaunchOption opt);

// Configure a dialog-free dual-training autotest launch. The selected
// character is placed in every slot on both teams. A non-zero hostPort
// overrides the saved network port.
bool ConfigureAutoTestDualTraining(int32_t characterId, int32_t hostPort = 0);
bool IsAutoTestLaunchEnabled();

// Get the dialog config (characters, stage, network settings)
const LaunchDialogConfig& GetLaunchDialogConfig();

// Set enet handles from outside (used by auto-host/connect in dual training mode)
void SetNetworkHandles(ENetHost* host, ENetPeer* peer);

// Send the host's selected training configuration after the peer connects.
void SendTrainingLaunchPacket();

// Apply a host-selected training configuration received by an auto-connected
// dual-training client before its training boot sequence begins.
bool ApplyTrainingLaunchPacket(const LaunchTrainingPacket& packet);

// Clean up any active network session (call on DLL detach / game exit)
void CleanupNetworkOnExit();
