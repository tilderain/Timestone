#pragma once

// Optional Discord Rich Presence for Timestone (UMVC3).
//
// The Discord Social SDK (discord_partner_sdk.dll) is optional: when it is
// missing next to the game executable, or one of its required exports cannot
// be resolved, every function below is a safe no-op and the settings UI shows
// the feature as unavailable. When present, players pick one of three modes:
//   0 = Off (default), 1 = Basic (no-OAuth local presence),
//   2 = Full (Discord OAuth account link with Steam launch-to-join).
// Presence states: Main Menu / Hosting a Lobby / In a Match (elapsed timer).

void DiscordRichPresence_Init();            // once, at D3D9 overlay init
void DiscordRichPresence_Pump();            // every frame from HookProcessStuff
bool DiscordRichPresence_IsAvailable();     // dll loaded + required exports resolved
int  DiscordRichPresence_GetMode();         // 0/1/2, persisted in Timestone.ini
void DiscordRichPresence_SetMode(int mode); // persists; the pump reacts next frame
