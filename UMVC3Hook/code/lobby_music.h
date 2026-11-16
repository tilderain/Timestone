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
constexpr int32_t kMusicCueLobbyRoom = 16;
constexpr int32_t kMusicCueMainMenu = 13; // played as the two-layer menu theme

constexpr int32_t MusicId(int32_t cue, int32_t variant = 0) { return cue | (variant << 8); }
constexpr int32_t MusicCue(int32_t id) { return id == kMusicDefault ? kMusicCueLobbyRoom : (id & 0xFF); }
// Stage cues 71..80 carry a second mix; sGSound__playSystemCue selects it
// with its fourth argument.
constexpr int32_t MusicVariant(int32_t id) { return (id >> 8) & 0xFF; }

enum class MusicGroup : uint8_t { Menu, Stage, Character };

struct MusicTrack {
    int32_t id;
    MusicGroup group;
    const char* name;
};

inline constexpr MusicTrack kMusicTracks[] = {
    // Menus and modes. Where the game plays a cue is noted in the doc; the
    // "B" entries are the second of a pair the BGM-style option picks from.
    {kMusicDefault, MusicGroup::Menu, "Online Lobby"},
    {MusicId(13), MusicGroup::Menu, "Main Menu"},
    {MusicId(4), MusicGroup::Menu, "Character Select"},
    {MusicId(5), MusicGroup::Menu, "Character Select B"},
    {MusicId(106), MusicGroup::Menu, "Character Select (Alt 1)"},
    {MusicId(107), MusicGroup::Menu, "Character Select (Alt 2)"},
    {MusicId(108), MusicGroup::Menu, "Character Select (Alt 3)"},
    {MusicId(2), MusicGroup::Menu, "Victory"},
    {MusicId(3), MusicGroup::Menu, "Victory B"},
    {MusicId(9), MusicGroup::Menu, "Continue"},
    {MusicId(10), MusicGroup::Menu, "Continue B"},
    {MusicId(19), MusicGroup::Menu, "Final Results"},
    {MusicId(15), MusicGroup::Menu, "Gallery"},
    {MusicId(134), MusicGroup::Menu, "Gallery Track 1"},
    {MusicId(135), MusicGroup::Menu, "Gallery Track 2"},
    {MusicId(136), MusicGroup::Menu, "Gallery Track 3"},
    {MusicId(137), MusicGroup::Menu, "Gallery Track 4"},
    {MusicId(17), MusicGroup::Menu, "License"},
    {MusicId(84), MusicGroup::Menu, "Ending 1"},
    {MusicId(85), MusicGroup::Menu, "Ending 2"},
    {MusicId(86), MusicGroup::Menu, "Ending 3"},
    {MusicId(87), MusicGroup::Menu, "Ending 4"},
    {MusicId(88), MusicGroup::Menu, "Ending 5"},
    {MusicId(138), MusicGroup::Menu, "Staff Credits"},
    // Cues whose use in the game has not been identified yet.
    {MusicId(18), MusicGroup::Menu, "Track 18"},
    {MusicId(112), MusicGroup::Menu, "Track 112"},
    {MusicId(113), MusicGroup::Menu, "Track 113"},
    {MusicId(132), MusicGroup::Menu, "Track 132"},
    {MusicId(133), MusicGroup::Menu, "Track 133"},

    // Stages: uStageNNN's theme virtual (+0x238) returns cue 70 + stage.
    {MusicId(70), MusicGroup::Stage, "Training Room"},
    {MusicId(71), MusicGroup::Stage, "Danger Room"},
    {MusicId(71, 1), MusicGroup::Stage, "Danger Room (Alt Mix)"},
    {MusicId(72), MusicGroup::Stage, "Daily Bugle"},
    {MusicId(72, 1), MusicGroup::Stage, "Daily Bugle (Alt Mix)"},
    {MusicId(73), MusicGroup::Stage, "Metro City"},
    {MusicId(73, 1), MusicGroup::Stage, "Metro City (Alt Mix)"},
    {MusicId(74), MusicGroup::Stage, "Demon Village"},
    {MusicId(74, 1), MusicGroup::Stage, "Demon Village (Alt Mix)"},
    {MusicId(75), MusicGroup::Stage, "Kattelox Island"},
    {MusicId(75, 1), MusicGroup::Stage, "Kattelox Island (Alt Mix)"},
    {MusicId(76), MusicGroup::Stage, "Hand Hideout"},
    {MusicId(76, 1), MusicGroup::Stage, "Hand Hideout (Alt Mix)"},
    {MusicId(77), MusicGroup::Stage, "S.H.I.E.L.D. Helicarrier"},
    {MusicId(77, 1), MusicGroup::Stage, "S.H.I.E.L.D. Helicarrier (Alt Mix)"},
    {MusicId(78), MusicGroup::Stage, "Stage 8"},
    {MusicId(78, 1), MusicGroup::Stage, "Stage 8 (Alt Mix)"},
    {MusicId(79), MusicGroup::Stage, "Asgard"},
    {MusicId(79, 1), MusicGroup::Stage, "Asgard (Alt Mix)"},
    {MusicId(80), MusicGroup::Stage, "Tricell Laboratory"},
    {MusicId(80, 1), MusicGroup::Stage, "Tricell Laboratory (Alt Mix)"},
    {MusicId(81), MusicGroup::Stage, "Fate of the World 1"},
    {MusicId(82), MusicGroup::Stage, "Fate of the World 2"},
    {MusicId(83), MusicGroup::Stage, "Fate of the World 3"},

    // Characters: each character class's theme virtual (+0x2E0).
    {MusicId(20), MusicGroup::Character, "Ryu"},
    {MusicId(21), MusicGroup::Character, "Chun-Li"},
    {MusicId(36), MusicGroup::Character, "Akuma"},
    {MusicId(22), MusicGroup::Character, "Chris"},
    {MusicId(23), MusicGroup::Character, "Wesker"},
    {MusicId(39), MusicGroup::Character, "Jill"},
    {MusicId(24), MusicGroup::Character, "Viewtiful Joe"},
    {MusicId(25), MusicGroup::Character, "Dante"},
    {MusicId(26), MusicGroup::Character, "Trish"},
    {MusicId(27), MusicGroup::Character, "Frank West"},
    {MusicId(28), MusicGroup::Character, "Spencer"},
    {MusicId(29), MusicGroup::Character, "Arthur"},
    {MusicId(30), MusicGroup::Character, "Amaterasu"},
    {MusicId(31), MusicGroup::Character, "Zero"},
    {MusicId(32), MusicGroup::Character, "Tron"},
    {MusicId(33), MusicGroup::Character, "Morrigan"},
    {MusicId(34), MusicGroup::Character, "Felicia"},
    {MusicId(38), MusicGroup::Character, "Hsien-Ko"},
    {MusicId(35), MusicGroup::Character, "C. Viper"},
    {MusicId(37), MusicGroup::Character, "Haggar"},
    {MusicId(124), MusicGroup::Character, "Strider"},
    {MusicId(122), MusicGroup::Character, "Vergil"},
    {MusicId(120), MusicGroup::Character, "Phoenix Wright"},
    {MusicId(121), MusicGroup::Character, "Phoenix Wright (Turnabout)"},
    {MusicId(125), MusicGroup::Character, "Firebrand"},
    {MusicId(123), MusicGroup::Character, "Nemesis"},
    {MusicId(40), MusicGroup::Character, "Spider-Man"},
    {MusicId(41), MusicGroup::Character, "Captain America"},
    {MusicId(42), MusicGroup::Character, "Wolverine"},
    {MusicId(43), MusicGroup::Character, "Magneto"},
    {MusicId(44), MusicGroup::Character, "Hulk"},
    {MusicId(45), MusicGroup::Character, "She-Hulk"},
    {MusicId(46), MusicGroup::Character, "Taskmaster"},
    {MusicId(47), MusicGroup::Character, "Iron Man"},
    {MusicId(48), MusicGroup::Character, "Thor"},
    {MusicId(49), MusicGroup::Character, "Dr. Doom"},
    {MusicId(54), MusicGroup::Character, "Phoenix"},
    {MusicId(59), MusicGroup::Character, "Shuma-Gorath"},
    {MusicId(53), MusicGroup::Character, "MODOK"},
    {MusicId(55), MusicGroup::Character, "Dormammu"},
    {MusicId(50), MusicGroup::Character, "Deadpool"},
    {MusicId(57), MusicGroup::Character, "Storm"},
    {MusicId(51), MusicGroup::Character, "Super-Skrull"},
    {MusicId(58), MusicGroup::Character, "Sentinel"},
    {MusicId(52), MusicGroup::Character, "X-23"},
    {MusicId(126), MusicGroup::Character, "Nova"},
    {MusicId(129), MusicGroup::Character, "Rocket Raccoon"},
    {MusicId(128), MusicGroup::Character, "Ghost Rider"},
    {MusicId(127), MusicGroup::Character, "Iron Fist"},
    {MusicId(130), MusicGroup::Character, "Dr. Strange"},
    {MusicId(131), MusicGroup::Character, "Hawkeye"},
    {MusicId(60), MusicGroup::Character, "Galactus"},
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
