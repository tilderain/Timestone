#pragma once

// The game's own result screen menu (REMATCH / CHARACTER SELECT / MAIN MENU)
// in a room match. Reverse engineering and design: the result screen sections
// of doc/native_character_select.md.
//
// In versus the game gives each player a cursor and a choice of their own on
// that screen and goes on once both have chosen. Here each client lets the
// game run its own player's side from its own pad, with nothing in between,
// and shows the other player's side from what its owner sends
// (ResultMenuPacket). What the game would do with the two choices is not
// allowed to happen; the caller reads them and acts.
//
// Game thread only, except LeaveArmed.

#include "netplay.h"

#include <cstdint>

namespace resultmenu {

// The game's own values for the three entries.
enum Choice : uint8_t {
    kNone = 0,
    kRematch = 1,
    kCharSelect = 2,
    kMainMenu = 4,
};

// Installs the hooks. They pass straight through while no session is set, so
// the game's offline modes and a netplay match outside a room are untouched.
bool Install();

// Once per game tick. `active` while the local game is a room match's battle;
// `watching` for a spectator, whose pad then reaches nothing on the screen.
// A new `generation` (a new game) forgets the choices of the one before.
void Tick(bool active, int localTeam, bool watching, uint32_t generation);

// The menu is on screen and taking input.
bool MenuUp();
Choice LocalChoice();
Choice RemoteChoice();

// The local side as it should be shown to the other player. True when one
// should be sent now.
bool TakeOutbound(ResultMenuPacket* packet);
// Checked in full before any of it is used.
void OnPeerPacket(const ResultMenuPacket& packet);

// Retires the result screen, as the game's own handlers do when they leave
// it for another area.
void KillScreen();

// MAIN MENU was pressed once: a second press leaves the match. Any thread.
bool LeaveArmed();

} // namespace resultmenu
