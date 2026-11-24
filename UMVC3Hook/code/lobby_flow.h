#pragma once

// Glue between the game and the in-game lobby.
//
// The lobby lives in the title area (aTitle): the plugin already owns that
// area's move callback for its boot-to-battle path, and it is also where the
// game naturally returns after a battle. While held there, the lobby screens
// are drawn with native UI units; a match is started through the same
// aChrSelectBlank transition the launcher flow uses, and leaving a match
// requests aTitle again so the player lands back in their room.

#include <cstddef>
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
// "Main Menu" in the lobby hands the title area back to the game: the native
// main menu is spawned and aTitle::move runs untouched until the lobby is
// reopened (the plugin's Esc menu offers that while the title area is up).
bool LobbyFlow_IsReleasedToMainMenu();
// True while released and the title area is the one running.
bool LobbyFlow_CanReopen();
// Safe from the render thread; takes effect on the next title-area frame.
void LobbyFlow_RequestReopen();
// Released-state replacement for LobbyFlow_TitleTick. Returns true when the
// lobby took the title area back this frame (skip the native move).
bool LobbyFlow_ReleasedTitleTick(uint64_t titleArea);
// BootIntoTraining committed and queued the battle transition: the lobby's
// units must be gone before the area changes under them.
void LobbyFlow_OnBattleBootCommitted();

// Called from the hooked sMvc3Main::move every frame.
void LobbyFlow_GameTick();
// The synchronized restart that goes through the character select has come
// due (framestepper.cpp, RESTART_CHARSEL): asks for the select screen's area
// in place of the battle's. False if it could not; the match is then left.
bool LobbyFlow_BeginMatchCharSelect();
// A packet from the match session's extension channel. True if it was one of
// the lobby's (the result menu, the select screen reached from a match).
bool LobbyFlow_HandleExtension(const void* data, size_t size);

// Spectating: who is watching, the setup's queue in order, and who the feed
// comes from with the room's ping estimate to them (-1 when unknown).
// `valid` is false when not watching.
struct SpectatorInfo {
    static constexpr int kMax = 8;
    struct Person {
        char name[64];
        bool local;
    };
    bool valid;
    char feed[64]; // empty until someone feeds us
    int feedPing;
    int watcherCount;
    Person watchers[kMax];
    int queueCount;
    Person queue[kMax];
};
SpectatorInfo LobbyFlow_GetSpectatorInfo();
// Shows or hides the spectator's overlay (who is playing, feed state) and
// saves the choice. Game thread.
void LobbyFlow_ToggleSpectatorOverlay();

// True from match launch until the player is back in the lobby.
bool LobbyFlow_InRoomMatch();
// Called from the result-phase hooks every frame the winner outro or result
// screen runs, with sAction's winning team (negative for a draw).
void LobbyFlow_OnResultPhase(int32_t winTeam);
// False while a rematch must not be started from the result phase: the game
// just finished has not been counted yet, or it decided the set.
bool LobbyFlow_AllowResultRestart();
// A game of a room set is restarting with this configuration: draws the team
// members that are random for every game again, the same on both players.
struct TrainingBootConfig;
void LobbyFlow_RerollRandomMembers(TrainingBootConfig& config);
// Ask to leave the current match and return to the room. Safe from the
// render thread; the transition happens on the next game tick.
void LobbyFlow_RequestReturnToLobby(const char* reason);
// The player chose "Return to Lobby": as above, and they also give up their
// seat at the match slot (and with it any place in line).
void LobbyFlow_RequestLeaveMatch();

// True while the local game is watching someone's room match.
bool LobbyFlow_IsSpectating();
// The shared character select is up and the local player is one of the two
// picking on it (not a spectator). Any thread.
bool LobbyFlow_PlayerInCharSelect();
// The two players of the match being watched, player 1 first.
bool LobbyFlow_GetWatchedNames(const char** p1, const char** p2);

// ImGui fallback drawn from the overlay pass: tells the player the lobby is
// still starting, or that the native UI could not be brought up.
void LobbyFlow_DrawOverlay();

// True when a request for this system cue should be dropped: the menu theme
// is already playing and is being carried across a title-area reload.
bool LobbyFlow_ShouldSkipSystemCue(uint32_t cue);

// A text field (room name, preset name, chat) is open: typed characters and
// the input method go to it (lobby_ime.h).
bool LobbyFlow_WantsText();
