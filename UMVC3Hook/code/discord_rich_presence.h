#pragma once

// Optional Discord Rich Presence for Timestone (UMVC3).
//
// The Discord Social SDK (discord_partner_sdk.dll) is optional: when it is
// missing next to the game executable, or one of its required exports cannot
// be resolved, every function below is a safe no-op and the settings UI shows
// only tier 4 (ranked) as usable. Modes are stored in
// [Settings]TelemetryMode:
//   0 = Off (no Discord)
//   1 = Basic Discord Integration (no account link)
//   2 = Advanced Discord Integration (Discord OAuth account link over the
//       127.0.0.1 loopback; publishes a lobby join secret while hosting)
//   3 = Connect to Ranked Matchmaking Coordinator (coming soon!; links
//       through the login service, which then hands back the Discord token
//       that authorizes presence - ranked_account owns that link flow)
// Presence states: Main Menu / Hosting a Lobby / In a Match (elapsed timer).
void DiscordRichPresence_Init();            // once, at D3D9 overlay init
void DiscordRichPresence_Pump();            // every frame from HookProcessStuff
bool DiscordRichPresence_IsAvailable();     // dll loaded + required exports resolved
int  DiscordRichPresence_GetMode();         // raw stored 0..3 from Timestone.ini
void DiscordRichPresence_SetMode(int mode); // persists; the pump reacts next frame

// Hosted-link presence: ranked_account passes an access token minted by the
// link service; the SDK authenticates with it so rich presence (and the lobby
// join secret) work without the loopback OAuth. Tokens are short-lived -
// NeedsHostedToken() reports when a refresh is required.
bool DiscordRichPresence_ProvideHostedToken(const char* accessToken, int expiresInSeconds);
bool DiscordRichPresence_NeedsHostedToken();
