#pragma once

// Optional Discord Rich Presence for Timestone (UMVC3).
//
// The Discord Social SDK (discord_partner_sdk.dll) is optional: when it is
// missing next to the game executable, or one of its required exports cannot
// be resolved, every function below is a safe no-op and the settings UI shows
// only tier 4 (ranked) as usable. Online modes are stored in
// [Settings]OnlineMode:
//   0 = Off (default)
//   1 = Basic rich presence (no account link)
//   2 = Advanced (Discord OAuth account link over the 127.0.0.1 loopback;
//       publishes a lobby join secret while hosting)
//   3 = Ranked (link for matchmaking through auth.c.hero.rehab; the Discord
//       SDK is off under this mode and ranked_account owns the link flow)
// Presence states: Main Menu / Hosting a Lobby / In a Match (elapsed timer).
void DiscordRichPresence_Init();            // once, at D3D9 overlay init
void DiscordRichPresence_Pump();            // every frame from HookProcessStuff
bool DiscordRichPresence_IsAvailable();     // dll loaded + required exports resolved
int  DiscordRichPresence_GetMode();         // raw stored 0..3 from Timestone.ini
void DiscordRichPresence_SetMode(int mode); // persists; the pump reacts next frame
