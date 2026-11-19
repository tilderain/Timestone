#pragma once

// Native online rewards for netplay matches, and Steam achievement delivery.
// See doc/online_rewards.md.
//
// Installs two hooks. Call once, from the launch path that applies the
// plugin's gameplay patches (never for Launch Normally: the game's own
// achievement delivery works there).
void OnlineRewards_InstallHooks();
