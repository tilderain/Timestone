#pragma once

// The game's own character select screen, shared by the two players of a room
// match. See doc/native_character_select.md for the reverse engineering and
// the design.
//
// The screen runs in versus mode on both clients. Each client lets the game
// run its own team's side from its own pad, with nothing in between, and
// draws the other team from the state its owner sends (CharSelPacket). With
// both teams complete it goes on as it does offline: the stage select (a coin
// toss decides who picks; the other is shown it) and the versus sequence. Where the game
// would then enter the battle by itself, the caller boots it instead.
//
// Everything here is game thread only. The caller owns the connection and the
// room; this file owns the screen.

#include "lobby_model.h"
#include "netplay.h"

#include <cstdint>

namespace charsel {

struct Config {
    int localTeam;        // 0 = player 1 (left), 1 = player 2 (right)
    // The team in use, with no random members left: what the clock running
    // out falls back to. Nothing is picked in advance on the screen.
    lobby::Team team;
    // Members of `team` (bits 0..2) that stand for RANDOM ALL.
    uint8_t teamRandomMask;
    uint32_t seconds;      // time to pick a team
    uint32_t stageSeconds; // time the picking player has for the stage
    // A spectator's screen: neither team is the local player's. Both are
    // drawn from their owners' packets (OnTeamPacket), nothing is read from
    // the local pad and nothing is sent. `localTeam` and `team` are unused.
    bool watch;
};

enum class Status {
    Idle,       // no session
    Opening,    // the area was requested; the screen is not up yet
    Selecting,  // the screen is up
    Failed,     // the screen did not come up
    LocalLeft,  // the local player backed out to the title area
    RemoteLeft, // the other player backed out
};

// Installs the hooks. They pass straight through while no session is open, so
// the game's own offline modes are untouched. Returns false if any failed, in
// which case no session can be started.
bool Install();

// Sets versus mode and asks for the select screen's area. The lobby's native
// units must already be released. False if the area manager is not there or
// the hooks are missing.
bool Begin(const Config& config);
// Ends the session and puts back what Begin changed. `startingBattle` is true
// when the battle boot follows: the screen then keeps its character archives.
void End(bool startingBattle);
// Once per game tick, session or not (it also notices the screen going away).
void Tick();
// Both teams are complete and the battle boot is about to begin: puts back
// the control-type setting and the pad table while the screen stays up,
// frozen. The boot reads the first and writes the second. End does this too.
void ReleaseSettings();

bool Active();
Status GetStatus();
// The team is complete and will not change.
bool LocalLockedIn();
bool RemoteLockedIn();
// Valid once the respective side is locked in.
lobby::Team LocalTeam();
lobby::Team RemoteTeam();
// Members (bits 0..2) the respective player left to RANDOM ALL: on the
// screen's own RANDOM ALL cell, or in the preset or fallback team put in.
// The characters shown are this game's draw; the caller draws again for
// each later game of the set.
uint8_t LocalRandomMask();
uint8_t RemoteRandomMask();
// Both teams are complete, on a player's screen or a spectator's.
bool AllLockedIn();
// Seconds left to pick.
uint32_t SecondsLeft();
// Once both teams are final the screen goes on to its stage select, which
// one player drives and the other watches, and then to its "versus" sequence.
// True once the stage is known on this client.
bool StageDecided();
// The team picking the stage (0 = player 1) while the stage select is up, -1
// before it and once the stage is decided.
int StagePicker();
int32_t StageId();
// The versus sequence has played out and the game asked to enter the battle
// (refused: the caller boots it). Stays true.
bool EnterRequested();

// The local side as it should be shown to the other player. True when one
// should be sent now (something changed, or the regular repeat is due).
bool TakeOutbound(CharSelPacket* packet);
// What the other player sent. Checked in full before any of it is used; a
// packet that fails is dropped whole.
void OnPeerPacket(const CharSelPacket& packet);
// The same for a named team (0 = player 1): a spectator is told both. Ignored
// for the local player's own team.
void OnTeamPacket(int team, const CharSelPacket& packet);

// The preset list can be opened now (the local cursor is on the roster).
bool CanPickPreset();
// While set, the local side takes no input: the list is on top of it.
void SetPresetListOpen(bool open);
// Puts a whole team (no random members left in it) on the local side and
// completes it. `randomMask`: the members that were RANDOM ALL before the
// caller drew them.
bool ApplyLocalTeam(const lobby::Team& team, uint8_t randomMask);

// True for a character both the game and this build can put in a match.
bool IsSelectableCharacter(int32_t characterId);

} // namespace charsel
