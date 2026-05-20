#pragma once

// ---------------------------------------------------------------------------
// Boot-to-training-mode for UMvC3 Rollback.
// After the title logo sequence, instead of showing the title screen,
// this configures a training match and transitions directly into gameplay.
// ---------------------------------------------------------------------------

#include <cstdint>

// UMvC3 character IDs (internal game values, 1-based).
// Verified via SomethingThatWriteCharacterIDInCharacterSelectScreen (0x140146560):
//   if (charId - 1U < 0x3b) => valid IDs range from 1 to 59.
// The actual class names are found in reflection.txt (e.g., uWolverine, uIronMan...).
// NOTE: These IDs are approximate and may vary by game version.
// Character 0 is invalid/a placeholder — IDs start at 1.
enum UMVC3CharId : int32_t {
    CHR_INVALID      = 0,
    CHR_WOLVERINE    = 1,
    CHR_IRONMAN      = 2,
    CHR_STORM        = 3,
    CHR_MAGNETO      = 4,
    CHR_RYU          = 5,
    CHR_CHUNLI       = 6,
    CHR_DOOM         = 7,
    CHR_SENTINEL     = 8,
    CHR_HULK         = 9,
    CHR_SPIDERMAN    = 10,
    CHR_CAPTAIN_AMERICA = 11,
    CHR_DEADPOOL     = 12,
    CHR_DANTE        = 13,
    CHR_AMATERASU    = 14,
    CHR_MORRIGAN     = 15,
    CHR_FELICIA      = 16,
    CHR_TRON         = 17,
    CHR_SHEHULK      = 18,
    CHR_WESKER       = 19,
    CHR_AKUMA        = 20,
    CHR_X23          = 21,
    CHR_ZERO         = 22,
    CHR_VIEWTIFUL_JOE = 23,
    CHR_HAWKEYE      = 24,
    CHR_STRIDER      = 25,
    CHR_FRANK_WEST   = 26,
    CHR_NEMESIS      = 27,
    CHR_CHRIS        = 28,
    CHR_FIREBRAND    = 29,
    CHR_PHOENIX      = 30,
    CHR_NOVA         = 31,
    CHR_GHOST_RIDER  = 32,
    CHR_DORMAMMU     = 33,
    CHR_DR_STRANGE   = 34,
    CHR_TASKMASTER   = 35,
    CHR_IRON_FIST    = 36,
    CHR_HAWKEYE_AOA  = 37,
    CHR_DR_DOOM      = 38,
    CHR_SKRILL       = 39,
    CHR_MODOK        = 40,
    CHR_SUPER_SKRULL = 41,
    CHR_PALADIN      = 42,
    CHR_GALACTUS     = 43,
    CHR_SIGMA        = 44,
    CHR_ASURA        = 45,
    CHR_VIRGIL       = 46,
    CHR_MEGAMAN      = 47,
    CHR_JIN          = 48,
    CHR_TORNADO      = 49,
    CHR_SHAZAM       = 50,
    CHR_ULTRON       = 51,
    CHR_THANOS       = 52,
    CHR_PHOENIX_WRIGHT = 53,
    CHR_SHUMA_GORATH = 54,
    CHR_RACHNERA     = 55,
    CHR_ROLENTO      = 56,
    CHR_KARMA        = 57,
};

// Stage IDs used in sBattleSetting::setStageValue.
enum UMVC3StageId : int32_t {
    STAGE_DAILY_BUGLE    = 0,
    STAGE_SHIELD_HELI    = 1,
    STAGE_AVENGERS_TOWER = 2,
    STAGE_UNDERGROUND    = 3,
    STAGE_ASGARD         = 4,
    STAGE_COMPUTER_ROOM  = 5,
    STAGE_LEAVING_EARTH  = 6,
    STAGE_MURDERWORLD    = 7,
    STAGE_THANOS_SHRINE  = 8,
    STAGE_SKRILL_SHIP    = 9,
    STAGE_MEPHISTO       = 10,
    STAGE_KNOCKOFF       = 11,
    STAGE_BONUS          = 12,
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
