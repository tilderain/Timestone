#pragma once

// Discord rich presence for Timestone: the plugin's own activity on the
// player's Discord profile, published over the desktop client's local pipe
// (no SDK, no sign-in, no backend).
//
// Off publishes nothing and opens no pipe. On publishes the lobby, the room,
// the opponent, both teams, the set score and the match timer, and offers a
// Join button for public rooms with a free seat.

// Nothing here looks for Discord (its pipe, the registry, processes) until the
// player has said yes: ShowDiscordStatus=1, from the lobby's question or a
// settings menu.
enum class DiscordPresenceStatus : int { Off, Connecting, AppNotRunning, Connected, Refused };

bool DiscordPresence_IsEnabled();                // ShowDiscordStatus=1
bool DiscordPresence_NeedsSetup();               // the lobby's question is due
bool DiscordPresence_SetEnabled(bool enabled);   // false when Timestone.ini could not be written
DiscordPresenceStatus DiscordPresence_GetStatus();
void DiscordPresence_GameTick();
void DiscordPresence_Shutdown();
