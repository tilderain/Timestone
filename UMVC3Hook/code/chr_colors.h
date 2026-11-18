#pragma once

#include <cstdint>

// Character colors, as the select screen numbers them (zero-based):
//   0..5  the six standard colors, shown as "1".."6"
//   6     the Herald color, shown as "H"; the game unlocks it per character
//         through Heroes & Heralds (cSecret bit characterId + 0x33)
//   7     the alternate costume, shown as "ALT"; the game ties it to DLC
//         ownership (cSecret bit characterId + 0x352)
// Variant 8 exists as an archive for Akuma, Wesker, Doctor Doom and Dormammu
// (their Herald boss forms) but the game never offers it, and neither do we.
//
// The mod treats every color whose archive is installed as unlocked, for both
// players of a match, whatever either save file says. A color without an
// archive (Magneto's alternate costume is not shipped on PC) stays out.

constexpr int kChrColorSlots = 8;

// True when the character can be played in this color on this install.
bool ChrColors_Available(int32_t characterId, int32_t color);

// Number of colors to offer for a character: the colors 0..count-1 are all
// available. 0 for an id that is not a playable character.
int ChrColors_Count(int32_t characterId);

// The select screen's label for a color: "1".."6", "H", "ALT".
const char* ChrColors_Label(int32_t color);

// Makes the game's own color pickers (character select, random color, model
// viewer) offer every installed color. Call once, after MinHook is ready.
void ChrColors_InstallUnlock();
