#pragma once

// Tracks a lobby host can choose as room music: the game's own BGM cues from
// `sound\bgm\bgm` (sound\cmn.arc). Shared by the lobby screens (names), the
// room service (validation of what a host published) and the game glue
// (playback). See "Music" in doc/native_lobby_ui.md for how each was found.

#include <cstdint>

namespace lobby {

// A track id travels in the room's lobby data, so it is built from the cue
// number rather than a table position: `cue | variant << 8`. Id 0 is the
// default, the native lobby's own room track.
constexpr int32_t kMusicDefault = 0;
// Silence: no room music at all. Not a cue the game has.
constexpr int32_t kMusicNone = 0xFF;
constexpr int32_t kMusicCueLobbyRoom = 16;
constexpr int32_t kMusicCueMainMenu = 13; // played as the two-layer menu theme

constexpr int32_t MusicId(int32_t cue, int32_t variant = 0) { return cue | (variant << 8); }
constexpr int32_t MusicCue(int32_t id) { return id == kMusicDefault ? kMusicCueLobbyRoom : (id & 0xFF); }
// Stage cues 71..80 carry three mixes, the sound test's "stage theme 1/2/3";
// sGSound__playSystemCue selects one with its fourth argument (0, 1, 2).
constexpr int32_t MusicVariant(int32_t id) { return (id >> 8) & 0xFF; }

enum class MusicGroup : uint8_t { Menu, Stage, Character };

struct MusicTrack {
    int32_t id;
    MusicGroup group;
    const char* name;
};

inline constexpr MusicTrack kMusicTracks[] = {
    // Names are the soundtrack's track titles; the doc has the evidence for
    // which cue is which. "Normal"/"Dynamic" are the two BGM STYLE mixes.
    {kMusicDefault, MusicGroup::Menu, "Online Lobby"},
    {kMusicNone, MusicGroup::Menu, "None"},
    {MusicId(132), MusicGroup::Menu, "Comic Book Intro"},
    {MusicId(13), MusicGroup::Menu, "Main Menu ~Ultimate~"},
    {MusicId(112), MusicGroup::Menu, "Marvel Vs. Capcom 3 ~ Fate of Two Worlds"},
    {MusicId(113), MusicGroup::Menu, "License Card"},
    {MusicId(4), MusicGroup::Menu, "Character Select ~Ultimate - Normal~"},
    {MusicId(5), MusicGroup::Menu, "Character Select ~Ultimate - Dynamic~"},
    {MusicId(106), MusicGroup::Menu, "Take You For A Ride ~Traditional Mix~"},
    {MusicId(107), MusicGroup::Menu, "Take You For A Ride ~Techno Mix~"},
    {MusicId(108), MusicGroup::Menu, "Take You For A Ride ~Rock Mix~"},
    {MusicId(2), MusicGroup::Menu, "Victory ~Ultimate - Normal~"},
    {MusicId(3), MusicGroup::Menu, "Victory ~Ultimate - Dynamic~"},
    {MusicId(9), MusicGroup::Menu, "Continue ~Normal~"},
    {MusicId(10), MusicGroup::Menu, "Continue ~Dynamic~"},
    {MusicId(19), MusicGroup::Menu, "Arcade Clear"},
    {MusicId(18), MusicGroup::Menu, "Mission Mode"},
    {MusicId(17), MusicGroup::Menu, "Ranking"},
    {MusicId(15), MusicGroup::Menu, "Gallery"},
    {MusicId(133), MusicGroup::Menu, "Heroes & Heralds"},
    {MusicId(134), MusicGroup::Menu, "Heroes -Ver. 1-"},
    {MusicId(135), MusicGroup::Menu, "Heroes -Ver. 2-"},
    {MusicId(136), MusicGroup::Menu, "Heralds -Ver. 1-"},
    {MusicId(137), MusicGroup::Menu, "Heralds -Ver. 2-"},
    {MusicId(84), MusicGroup::Menu, "Arcade Ending -Type A-"},
    {MusicId(85), MusicGroup::Menu, "Arcade Ending -Type B-"},
    {MusicId(86), MusicGroup::Menu, "Arcade Ending -Type C-"},
    {MusicId(87), MusicGroup::Menu, "Arcade Ending -Type D-"},
    {MusicId(88), MusicGroup::Menu, "Arcade Ending -Type E-"},
    {MusicId(138), MusicGroup::Menu, "Staff Roll"},

    // Stages: uStageNNN's theme virtual (+0x238) returns cue 70 + stage. The
    // layered ones carry three mixes, as the game's sound test lists them.
    {MusicId(70), MusicGroup::Stage, "Training Room"},
    {MusicId(71), MusicGroup::Stage, "Danger Room 1"},
    {MusicId(71, 1), MusicGroup::Stage, "Danger Room 2"},
    {MusicId(71, 2), MusicGroup::Stage, "Danger Room 3"},
    {MusicId(72), MusicGroup::Stage, "Daily Bugle 1"},
    {MusicId(72, 1), MusicGroup::Stage, "Daily Bugle 2"},
    {MusicId(72, 2), MusicGroup::Stage, "Daily Bugle 3"},
    {MusicId(73), MusicGroup::Stage, "Metro City 1"},
    {MusicId(73, 1), MusicGroup::Stage, "Metro City 2"},
    {MusicId(73, 2), MusicGroup::Stage, "Metro City 3"},
    {MusicId(74), MusicGroup::Stage, "Demon Village 1"},
    {MusicId(74, 1), MusicGroup::Stage, "Demon Village 2"},
    {MusicId(74, 2), MusicGroup::Stage, "Demon Village 3"},
    {MusicId(75), MusicGroup::Stage, "Kattelox Island 1"},
    {MusicId(75, 1), MusicGroup::Stage, "Kattelox Island 2"},
    {MusicId(75, 2), MusicGroup::Stage, "Kattelox Island 3"},
    {MusicId(76), MusicGroup::Stage, "Hand Hideout 1"},
    {MusicId(76, 1), MusicGroup::Stage, "Hand Hideout 2"},
    {MusicId(76, 2), MusicGroup::Stage, "Hand Hideout 3"},
    {MusicId(77), MusicGroup::Stage, "S.H.I.E.L.D. Helicarrier 1"},
    {MusicId(77, 1), MusicGroup::Stage, "S.H.I.E.L.D. Helicarrier 2"},
    {MusicId(77, 2), MusicGroup::Stage, "S.H.I.E.L.D. Helicarrier 3"},
    {MusicId(78), MusicGroup::Stage, "Okami Stage ~Unused~ 1"},
    {MusicId(78, 1), MusicGroup::Stage, "Okami Stage ~Unused~ 2"},
    {MusicId(78, 2), MusicGroup::Stage, "Okami Stage ~Unused~ 3"},
    {MusicId(79), MusicGroup::Stage, "Asgard 1"},
    {MusicId(79, 1), MusicGroup::Stage, "Asgard 2"},
    {MusicId(79, 2), MusicGroup::Stage, "Asgard 3"},
    {MusicId(80), MusicGroup::Stage, "TRICELL Laboratory 1"},
    {MusicId(80, 1), MusicGroup::Stage, "TRICELL Laboratory 2"},
    {MusicId(80, 2), MusicGroup::Stage, "TRICELL Laboratory 3"},
    {MusicId(81), MusicGroup::Stage, "The Battle for Earth -Round 1-"},
    {MusicId(82), MusicGroup::Stage, "The Battle for Earth -Round 2-"},
    {MusicId(83), MusicGroup::Stage, "The Battle for Earth -Round 3-"},

    // Characters: each character class's theme virtual (+0x2E0).
    {MusicId(20), MusicGroup::Character, "Ryu ~Become the Storm~"},
    {MusicId(21), MusicGroup::Character, "Chun-Li ~Dream Position~"},
    {MusicId(36), MusicGroup::Character, "Akuma ~Gouki - MvC3 Arrange~"},
    {MusicId(22), MusicGroup::Character, "Chris Redfield ~Burning with Anger~"},
    {MusicId(23), MusicGroup::Character, "Albert Wesker ~Wind of Madness~"},
    {MusicId(39), MusicGroup::Character, "Jill Valentine ~Sad but True~"},
    {MusicId(24), MusicGroup::Character, "Viewtiful Joe ~Joe the Hero~"},
    {MusicId(25), MusicGroup::Character, "Dante ~Devil's Never Cry~"},
    {MusicId(26), MusicGroup::Character, "Trish ~Lock & Load~"},
    {MusicId(27), MusicGroup::Character, "Frank West ~Adam~"},
    {MusicId(28), MusicGroup::Character, "Nathan Spencer ~Bionic Commando~"},
    {MusicId(29), MusicGroup::Character, "Arthur ~Haunted Graveyard~"},
    {MusicId(30), MusicGroup::Character, "Amaterasu ~The Sun Rises~"},
    {MusicId(31), MusicGroup::Character, "Zero ~Zero - Megaman X2~"},
    {MusicId(32), MusicGroup::Character, "Tron Bonne ~The Flutter Vs. The Gesellschaft~"},
    {MusicId(33), MusicGroup::Character, "Morrigan Aensland ~Scotland~"},
    {MusicId(34), MusicGroup::Character, "Felicia ~U.S.A.~"},
    {MusicId(38), MusicGroup::Character, "Hsien-Ko ~Lei-Lei - China~"},
    {MusicId(35), MusicGroup::Character, "Crimson Viper ~MvC3 Arrange~"},
    {MusicId(37), MusicGroup::Character, "Mike Haggar ~Slum Alleyway~"},
    {MusicId(124), MusicGroup::Character, "Strider Hiryu ~Moon Over St. Petersburg~"},
    {MusicId(122), MusicGroup::Character, "Vergil ~One More Game~"},
    {MusicId(120), MusicGroup::Character, "Phoenix Wright ~Ryuichi Naruhodo - Objection! 2001~"},
    {MusicId(121), MusicGroup::Character, "Phoenix Wright -Turnabout Mode ~Pressing Pursuit~"},
    {MusicId(125), MusicGroup::Character, "Firebrand ~Gargoyle's Quest~"},
    {MusicId(123), MusicGroup::Character, "Nemesis T-Type ~Unstoppable Nemesis~"},
    {MusicId(40), MusicGroup::Character, "Spiderman"},
    {MusicId(41), MusicGroup::Character, "Captain America"},
    {MusicId(42), MusicGroup::Character, "Wolverine"},
    {MusicId(43), MusicGroup::Character, "Magneto"},
    {MusicId(44), MusicGroup::Character, "Hulk"},
    {MusicId(45), MusicGroup::Character, "She-Hulk"},
    {MusicId(46), MusicGroup::Character, "Taskmaster"},
    {MusicId(47), MusicGroup::Character, "Iron Man"},
    {MusicId(48), MusicGroup::Character, "Thor"},
    {MusicId(49), MusicGroup::Character, "Doctor Doom"},
    {MusicId(54), MusicGroup::Character, "Phoenix"},
    {MusicId(59), MusicGroup::Character, "Shuma-Gorath"},
    {MusicId(53), MusicGroup::Character, "M.O.D.O.K."},
    {MusicId(55), MusicGroup::Character, "Dormammu"},
    {MusicId(50), MusicGroup::Character, "Deadpool ~Never do it now!~"},
    {MusicId(57), MusicGroup::Character, "Storm"},
    {MusicId(51), MusicGroup::Character, "Super Skrull"},
    {MusicId(58), MusicGroup::Character, "Sentinel"},
    {MusicId(52), MusicGroup::Character, "X-23 ~Destiny~"},
    {MusicId(126), MusicGroup::Character, "Nova"},
    {MusicId(129), MusicGroup::Character, "Rocket Raccoon"},
    {MusicId(128), MusicGroup::Character, "Ghost Rider"},
    {MusicId(127), MusicGroup::Character, "Iron Fist"},
    {MusicId(130), MusicGroup::Character, "Doctor Strange"},
    {MusicId(131), MusicGroup::Character, "Hawkeye"},
    {MusicId(60), MusicGroup::Character, "Galactus ~The Fate of Two Worlds~"},
};
constexpr int kMusicTrackCount = static_cast<int>(sizeof(kMusicTracks) / sizeof(kMusicTracks[0]));

// Table position of a track id, or -1 if it is not one the lobby offers.
// Lobby data is untrusted: anything unknown falls back to the default.
constexpr int FindMusicTrack(int32_t id)
{
    for (int i = 0; i < kMusicTrackCount; ++i)
        if (kMusicTracks[i].id == id) return i;
    return -1;
}

inline const char* MusicGroupName(MusicGroup group)
{
    switch (group) {
    case MusicGroup::Stage: return "STAGE";
    case MusicGroup::Character: return "CHARACTER";
    default: return "MENU";
    }
}

} // namespace lobby
