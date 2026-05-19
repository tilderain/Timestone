#pragma once

enum LaunchOption {
    LAUNCH_NORMAL = 0,
    LAUNCH_TRAINING = 1,
    LAUNCH_DUAL_TRAINING = 2,
};

// Shows a Win32 dialog before the game initializes.
// Blocks until the user makes a selection.
// Returns the chosen LaunchOption.
LaunchOption ShowLaunchMenu();

// Returns the launch option chosen at startup.
LaunchOption GetLaunchOption();

// Set launch option (e.g. from command-line args for spawned instance)
void SetLaunchOption(LaunchOption opt);
