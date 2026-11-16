#pragma once

// Glue between the game and the in-game lobby.
//
// The lobby lives in the title area (aTitle): the plugin already owns that
// area's move callback for its boot-to-battle path, and it is also where the
// game naturally returns after a battle. While held there, the lobby screens
// are drawn with native UI units; a match is started through the same
// aChrSelectBlank transition the launcher flow uses, and leaving a match
// requests aTitle again so the player lands back in their room.

#include <cstdint>

// On by default; -launcher or [Launch] NativeLobby=0 opts out. Must be called
// before the title area is reached.
void LobbyFlow_Enable();
bool LobbyFlow_IsEnabled();

// True while lobby screens are on screen and own menu input.
bool LobbyFlow_IsLobbyVisible();

// aTitleLogo hold: once boot archives are resident, skip the logos and the
// opening movie and request the title area directly.
void LobbyFlow_EnterTitleFromLogo();

// Replaces aTitle::move while the plugin has not booted a battle. Returns
// true when a match handshake has finished and BootIntoTraining should run.
bool LobbyFlow_TitleTick(uint64_t titleArea);
// BootIntoTraining committed and queued the battle transition: the lobby's
// units must be gone before the area changes under them.
void LobbyFlow_OnBattleBootCommitted();

// Called from the hooked sMvc3Main::move every frame.
void LobbyFlow_GameTick();

// True from match launch until the player is back in the lobby.
bool LobbyFlow_InRoomMatch();
// Ask to leave the current match and return to the room. Safe from the
// render thread; the transition happens on the next game tick.
void LobbyFlow_RequestReturnToLobby(const char* reason);

// ImGui fallback drawn from the overlay pass: tells the player the lobby is
// still starting, or that the native UI could not be brought up.
void LobbyFlow_DrawOverlay();

// Keyboard text entry (room name, chat).
bool LobbyFlow_WantsText();
void LobbyFlow_OnChar(uint32_t utf16);
