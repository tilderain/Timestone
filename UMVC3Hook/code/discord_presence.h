#pragma once

// Discord rich presence for Timestone: the plugin's own activity on the
// player's Discord profile, published over the desktop client's local pipe
// (no SDK, no sign-in, no backend).
//
// Off publishes nothing and opens no pipe. Basic publishes the lobby, the
// room, the opponent, both teams, the set score and the match timer, and
// offers a Join button for public rooms with a free seat.

enum class DiscordPresenceLevel : int { Off = 0, Basic = 1 };

DiscordPresenceLevel DiscordPresence_GetLevel();
bool DiscordPresence_SetLevel(DiscordPresenceLevel level);
void DiscordPresence_GameTick();
void DiscordPresence_Shutdown();
