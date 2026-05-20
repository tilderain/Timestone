#pragma once

// ---------------------------------------------------------------------------
// Boot-to-training-mode for UMvC3 Rollback.
// After the title logo sequence, instead of showing the title screen,
// this configures a training match and transitions directly into gameplay.
// ---------------------------------------------------------------------------

#include <cstdint>

// UMvC3 character IDs (internal game values, 1-based).
// Verified via the game binary: the name-lookup table at 0x140c553b0 maps
// IDs 1-59 directly to class names (e.g., 1=Ryu, 2=Chunli, ...).
// CAPCOM side = IDs 1-25, MARVEL side = IDs 26-50, Galactus = 51.
// IDs 52-59 are shadow clones and other extras.
// Character 0 is invalid/a placeholder — IDs start at 1.
enum UMVC3CharId : int32_t {
    CHR_INVALID      = 0,

    // ---- CAPCOM (1-25) ----
    CHR_RYU          = 1,
    CHR_CHUNLI       = 2,
    CHR_AKUMA        = 3,
    CHR_CHRIS        = 4,
    CHR_WESKER       = 5,
    CHR_VIEWTIFUL_JOE = 6,
    CHR_DANTE        = 7,
    CHR_TRISH        = 8,
    CHR_FRANK_WEST   = 9,
    CHR_SPENCER      = 10,
    CHR_ARTHUR       = 11,
    CHR_AMATERASU    = 12,
    CHR_ZERO         = 13,
    CHR_TRON         = 14,
    CHR_MORRIGAN     = 15,
    CHR_HSIENKO      = 16,
    CHR_FELICIA      = 17,
    CHR_CVIPER       = 18,
    CHR_HAGGAR       = 19,
    CHR_JILL         = 20,
    CHR_STRIDER      = 21,
    CHR_VERGIL       = 22,
    CHR_PHOENIX_WRIGHT = 23,
    CHR_FIREBRAND    = 24,
    CHR_NEMESIS      = 25,

    // ---- MARVEL (26-50) ----
    CHR_SPIDERMAN    = 26,
    CHR_CAPTAIN_AMERICA = 27,
    CHR_WOLVERINE    = 28,
    CHR_MAGNETO      = 29,
    CHR_HULK         = 30,
    CHR_SHEHULK      = 31,
    CHR_TASKMASTER   = 32,
    CHR_IRONMAN      = 33,
    CHR_THOR         = 34,
    CHR_DOOM         = 35,
    CHR_PHOENIX      = 36,
    CHR_SHUMA_GORATH = 37,
    CHR_MODOK        = 38,
    CHR_DORMAMMU     = 39,
    CHR_DEADPOOL     = 40,
    CHR_STORM        = 41,
    CHR_SUPER_SKRULL = 42,
    CHR_SENTINEL     = 43,
    CHR_X23          = 44,
    CHR_NOVA         = 45,
    CHR_ROCKET_RACCOON = 46,
    CHR_GHOST_RIDER  = 47,
    CHR_IRON_FIST    = 48,
    CHR_DR_STRANGE   = 49,
    CHR_HAWKEYE      = 50,

    // ---- BOSS / EXTRA ----
    CHR_GALACTUS     = 51,
};

// Stage IDs used in sBattleSetting::setStageValue.
enum UMVC3StageId : int32_t {
    STAGE_TRAINING_ROOM       = 0,
    STAGE_DAILY_BUGLE         = 2,
    STAGE_METRO_CITY          = 3,
    STAGE_DEMON_VILLAGE       = 4,
    STAGE_KATTLELOX_ISLAND    = 5,
    STAGE_HAND_HIDEOUT        = 6,
    STAGE_SHIELD_HELICARRIER  = 7,
    STAGE_ASGARD              = 9,
    STAGE_TRICELL_LABS        = 10,
    STAGE_FATE_OF_THE_WORLD   = 11,
    STAGE_DANGER_ROOM         = 100,
    STAGE_CITY_THAT_NEVER_SLEEPS = 102,
    STAGE_DAYS_OF_FUTURE_PAST = 103,
    STAGE_DEMON_VILLAGE_REDUX = 104,
    STAGE_BONNE_WONDERLAND    = 105,
    STAGE_SHADOWLAND          = 106,
    STAGE_SHIELD_AIR_SHOW     = 107,
    STAGE_ASGARD_SEA_OF_SPACE = 109,
    STAGE_CHAOS_AT_TRICELL    = 110,
};

// Configuration for the training match to boot into.
struct TrainingBootConfig {
    int32_t p1Chars[3];   // Character IDs for P1 (point, assist1, assist2)
    int32_t p2Chars[3];   // Character IDs for P2 (point, assist1, assist2)
    int32_t stageId;      // Stage ID
    int32_t timerValue;   // Timer in seconds (99 = infinite in training)
};

// Returns the default training config (Wolverine vs Wolverine, Daily Bugle, 99s).
const TrainingBootConfig& TrainingBoot_GetDefaultConfig();

// Set a custom config (e.g. from command-line parsing).
void TrainingBoot_SetConfig(const TrainingBootConfig& cfg);

// Execute the boot sequence: configure sBattleSetting and transition to
// aNextBattle. Called once from the aTitle__update hook.
void BootIntoTraining();

// Init the hook on aTitle__update.
void TrainingBoot_InitHooks();
