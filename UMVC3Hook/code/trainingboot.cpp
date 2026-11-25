// Boot directly into a configured battle, bypassing title/menu/character select.
#include "../pch.h"
#include "trainingboot.h"
#include "chr_colors.h"
#include "mk10utils.h"
#include "netplay.h"
#include "LaunchMenu.h"
#include "lobby_flow.h"
#include "framestepper.h"
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include <Windows.h>
#include "kiero/minhook/include/MinHook.h"

using namespace Memory::VP;

// ---------------------------------------------------------------------------
// Preferred executable addresses relocated with _addr; field offsets stay relative.
// ---------------------------------------------------------------------------

// sBattleSetting API
static constexpr uintptr_t BATTLE_SETTING_GET_INSTANCE      = 0x140004700;
static constexpr uintptr_t BATTLE_SETTING_SET_BATTLE_TYPE    = 0x14024e050;
static constexpr uintptr_t BATTLE_SETTING_SET_STAGE_VALUE    = 0x1401cf640;
static constexpr uintptr_t BATTLE_SETTING_SET_TIMER_VALUE    = 0x1401cf4f0;
static constexpr uintptr_t BATTLE_SETTING_COMMIT_ONE_PLAY    = 0x140214a40;
static constexpr uintptr_t BATTLE_SETTING_SET_CHAR_ID        = 0x140146560;
static constexpr uintptr_t BATTLE_SETTING_SET_DRAW_MODE      = 0x1401461e0;
static constexpr uintptr_t BATTLE_SETTING_SET_ASSIST_TYPE    = 0x140024d80;
static constexpr uintptr_t ACTION_EXECUTE_MATCH_PHASE         = 0x140004b80;
static constexpr uintptr_t SCHARACTER_GET_INSTANCE            = 0x140001aa0;
static constexpr uintptr_t SCHARACTER_GET_TEAM                = 0x140001b60;
static constexpr uintptr_t SCHARACTER_SET_TEAM_METER          = 0x14005a320;
static constexpr uintptr_t SCHARACTER_COUNT_CHARACTERS        = 0x140058EE0;
static constexpr uintptr_t SCHARACTER_GET_CHARACTER_BY_INDEX  = 0x140059020;
static constexpr uintptr_t VTABLE_UFRANK                      = 0x140A75840;
static constexpr uintptr_t UFRANK_LEVEL_OFFSET                = 0x69C4;
static constexpr uintptr_t CCHRTEAM_SET_EASY_MODE             = 0x1400203d0;
static constexpr uintptr_t BATTLE_SETTING_SET_TEAM_AUTO_JUMP  = 0x1402074d0;
static constexpr uintptr_t ACTION_AUTO_JUMP_P1_RETURN         = 0x140203e44;
static constexpr uintptr_t ACTION_AUTO_JUMP_P2_RETURN         = 0x140203e6a;

// sArea scene-chain manager
static constexpr uintptr_t SAREA_INSTANCE_PTR                = 0x140e17a80;

// aChrSelectBlank transition-area DTI
static constexpr uintptr_t ACHRSELBLANK_DTI                  = 0x140d500f0;

// sGameUI
static constexpr uintptr_t SGAMEUI_GET_INSTANCE              = 0x140001ac0;
static constexpr uintptr_t SGAMEUI_SET_BOOT_STATE            = 0x14000b440;
static constexpr uintptr_t ACHRSELECT_UTIL_4AC0              = 0x140214ac0;  // writes to ui+0x5b74
static constexpr uintptr_t SGAMEUI_CREATE_LOADING_SCREEN       = 0x1402c0540;

// sAgentSession
static constexpr uintptr_t AGENTSESSION_GET_INSTANCE         = 0x1400046e0;
static constexpr uintptr_t UUIBASE_METHOD_5560               = 0x140275560;  // uUiBase::method_5560(agent, val)

// sBattleSetting additional
static constexpr uintptr_t BATTLE_SETTING_RESTORE_BACKUP     = 0x14024de40;

// Character ID storage offset in sBattleSetting
// Access pattern: bs + 0x3bc0c + ((slot % 3) + (team & 0xf) * 3) * 4
static constexpr uintptr_t CHAR_ID_ARRAY_OFFSET              = 0x3bc0c;

// Battle descriptor, meter, and CPU-control offsets.
static constexpr uintptr_t BS_BACKUP_STAGE_ID_OFFSET             = 0x588;
static constexpr uintptr_t BS_P2_METER_DEFAULT_OFFSET         = 0x29c;
static constexpr uintptr_t BS_P1_METER_DEFAULT_OFFSET         = 0x2d4;
static constexpr uintptr_t BS_HP_DRAIN_MODE_OFFSET            = 0x2d8;
static constexpr uintptr_t SLOT_CPU_CONTROL_OFFSET             = 0x18;

// Input routing uses manager+0x140 for pad assignments and the keyboard player.
// sPad+0xC20/+0xC30 gate keyboard input independently of that assignment.
static constexpr uintptr_t SPAD_INSTANCE_PTR                 = 0x140E174B8;
static constexpr uintptr_t SMVC3MANAGER_INSTANCE_PTR         = 0x140D510A0;

// Copy the client's local P1 bindings to P2 for both gameplay mapping variants.
static constexpr uintptr_t SGAMECONFIG_GET_INSTANCE           = 0x140001AB0;
static constexpr uintptr_t SGAMECONFIG_START_NATIVE_LOAD      = 0x140253620;
static constexpr uintptr_t SGAMECONFIG_GET_BUTTON_MAPPING     = 0x1402522A0;
static constexpr uintptr_t SGAMECONFIG_SET_BINDING_TABLE      = 0x140254A20;
static constexpr uintptr_t SGAMECONFIG_SELECT_BINDING_TABLE   = 0x140254C40;
static constexpr uintptr_t SGAMECONFIG_GET_CHARACTER_VOICE    = 0x140251BD0;
static constexpr uintptr_t SGAMECONFIG_STORAGE_BUSY_OFFSET    = 0x4D350;
static constexpr uint32_t GAMEPLAY_BINDING_COUNT              = 11;
// Set by the title pre-menu once savedata.bin has been applied.
static constexpr uintptr_t SGAMECONFIG_IS_SAVEDATA_LOADED     = 0x140216F30;
// Packs the live settings into the payload written to savedata.bin; its only
// caller is sGameConfig__startSavedataSave (0x1402536E0).
static constexpr uintptr_t SGAMECONFIG_PACK_SAVE_PAYLOAD      = 0x14024F210;
static constexpr uintptr_t CSYSTEM_ENTER_CS                   = 0x140006D00;
static constexpr uintptr_t CSYSTEM_LEAVE_CS                   = 0x14000B860;
// Per-player binding state (player p: 0 = P1, 1 = P2), as read by
// sGameConfig__getButtonMappingValue (0x1402522A0).
static constexpr uintptr_t SGAMECONFIG_BINDINGS_MAIN          = 0xC0;   // + p * 0x2C, 11 x u32
static constexpr uintptr_t SGAMECONFIG_BINDINGS_ALTERNATE     = 0x118;  // + p * 0x2C, 11 x u32
static constexpr uintptr_t SGAMECONFIG_BINDING_LAYOUT         = 0x178;  // + p * 4
static constexpr uintptr_t SGAMECONFIG_BINDING_TABLE_CACHE    = 0x320;  // + p * 8
static constexpr uintptr_t BINDING_PLAYER_STRIDE              = GAMEPLAY_BINDING_COUNT * 4;
// Controller settings page (uMenuOptionFantastic__processOptionInput): a
// fixed control type (+0x191, with +0x190 set once the row has been used)
// makes character select apply CtrlType (+0x188: 0 Normal, 1 Simple) instead
// of asking (uMenuChrSel__slotState1_loadTeam). AutoJump is +0x192.
static constexpr uintptr_t SGAMECONFIG_CTRL_TYPE              = 0x188;
static constexpr uintptr_t SGAMECONFIG_CTRL_TYPE_SET          = 0x190;
static constexpr uintptr_t SGAMECONFIG_CTRL_TYPE_FIXED        = 0x191;
static constexpr uintptr_t SGAMECONFIG_AUTO_JUMP              = 0x192;
// sBattleSetting__restoreBackupAndInit copies this into sGameConfig CtrlType.
static constexpr uintptr_t BS_BACKUP_CTRL_MODE_OFFSET         = 0x58C;

static constexpr uintptr_t SUNIT_GET_INSTANCE                 = 0x140003160;
static constexpr uintptr_t SUNIT_ADD_UNIT                      = 0x14051A6B0;
static constexpr uintptr_t MTOBJECT_SET_FLAG_2000              = 0x1402545A0;
static constexpr uint32_t STORAGE_UNIT_CATEGORY                = 0x19;
static constexpr uintptr_t STORAGE_RESULT_OFFSET               = 0x4C;

// Offsets within sMvc3Manager:
static constexpr uintptr_t MANAGER_PAD_ASSIGN_OFFSET         = 0x140;   // int32_t[4]: -1 unassigned, 0 P1, 1 P2

// Offsets within sPad:
static constexpr uintptr_t PAD_KEYBOARD_BLOCKED_1            = 0xc20;   // byte: non-zero blocks keyboard
static constexpr uintptr_t PAD_KEYBOARD_BLOCKED_2            = 0xc30;   // byte: non-zero blocks keyboard

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static TrainingBootConfig g_bootConfig = {
    { CHR_MORRIGAN, CHR_MORRIGAN, CHR_MORRIGAN },   // P1 roster
    { CHR_MORRIGAN, CHR_MORRIGAN, CHR_MORRIGAN },   // P2 roster
    { 0, 0, 0 }, // P1 colors
    { 0, 0, 0 }, // P2 colors
    { 0, 0, 0 }, // P1 assists (Alpha)
    { 0, 0, 0 }, // P2 assists (Alpha)
    { false, false }, // Normal controls
    { false, false }, // Automatic super jump disabled
    { { 0, 0, 0 }, { 0, 0, 0 } }, // Character voice language
    0,    // Stage ID
    99   // Timer value; battle mode determines countdown behavior.
};
static bool g_hasBooted = false;
static bool g_hooksInitialized = false;
static uint64_t g_gameConfigLoadUnit = 0;
static bool g_nativeGameConfigLoaded = false;
static bool g_skipNativeConfigLoad = false;
static bool g_restartConfigPending = false;
static bool g_restartTransitionActive = false;
static bool g_restartSawActionSetup = false;
// The restart is waiting on the character select (TrainingBoot_BeginSelectRestart).
static bool g_restartAwaitingSelect = false;
static TrainingBootCpuMode g_cpuMode = TRAINING_CPU_NONE;
static bool g_fiveBarStart = false;
static bool g_initialSettingsSent = false;
static bool g_initialSettingsReceived = false;
static bool g_initialSettingsSynchronized = false;
// Each team's voice choice for every character (bit N for character id N, set
// = English), from the initial settings exchange. English until it is known.
static uint32_t g_voiceMask[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
static bool g_networkRequired = false;
static bool g_networkConnected = false;
static bool g_networkFailed = false;
static ULONGLONG g_initialSettingsStartTick = 0;

// ---------------------------------------------------------------------------
// Extern from dllmain.cpp
// ---------------------------------------------------------------------------
extern void HookLog(const char* fmt, ...);
extern void HookLog_MarkGameLoaded();
extern void BootTimer_Stamp(const char* label);

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
const TrainingBootConfig& TrainingBoot_GetDefaultConfig()
{
    return g_bootConfig;
}

void TrainingBoot_SetConfig(const TrainingBootConfig& cfg)
{
    g_bootConfig = cfg;
}

const TrainingBootConfig& TrainingBoot_GetConfig()
{
    return g_bootConfig;
}

static bool g_spectatorBoot = false;

void TrainingBoot_SetSpectatorBoot(bool enabled)
{
    g_spectatorBoot = enabled;
}

void TrainingBoot_SetNetworkRequired(bool required)
{
    g_networkRequired = required;
}

void TrainingBoot_NotifyNetworkConnected()
{
    g_networkConnected = true;
}

void TrainingBoot_FailNetwork(const char* reason)
{
    if (g_networkFailed)
        return;
    g_networkFailed = true;
    HookLog("TrainingBoot: Online boot stopped: %s\n", reason);
    if (Netplay_IsActive())
        Netplay_SendPlayerDisconnect(100);
    FrameStepper_ShowOpponentDisconnected();
}

void TrainingBoot_PrepareNextBoot(bool networkRequired)
{
    g_hasBooted = false;
    g_networkRequired = networkRequired;
    g_networkConnected = false;
    g_networkFailed = false;
    g_initialSettingsSent = false;
    g_initialSettingsReceived = false;
    g_initialSettingsSynchronized = false;
    g_initialSettingsStartTick = 0;
    g_restartConfigPending = false;
    g_restartTransitionActive = false;
    g_restartSawActionSetup = false;
    g_restartAwaitingSelect = false;
}

bool TrainingBoot_HasBooted()
{
    return g_hasBooted;
}

bool TrainingBoot_HasNetworkFailed()
{
    return g_networkFailed;
}

static bool IsBootNetworkReady()
{
    if (!g_networkRequired)
        return true;
    if (g_networkFailed)
        return false;
    if (Netplay_IsActive()) {
        g_networkConnected = true;
        return true;
    }
    if (g_networkConnected)
        TrainingBoot_FailNetwork("opponent disconnected before battle boot");
    return false;
}

void TrainingBoot_SetCpuMode(TrainingBootCpuMode mode)
{
    g_cpuMode = mode;
}

TrainingBootCpuMode TrainingBoot_GetCpuMode()
{
    return g_cpuMode;
}

void TrainingBoot_SetFiveBarStart(bool enabled)
{
    g_fiveBarStart = enabled;
}

bool TrainingBoot_GetFiveBarStart()
{
    return g_fiveBarStart;
}

// ---------------------------------------------------------------------------
// Write a character ID directly into sBattleSetting's char ID array
// ---------------------------------------------------------------------------
static void WriteCharacterId(uint64_t battleSetting, int team, int slot, int32_t charId)
{
    uintptr_t offset = CHAR_ID_ARRAY_OFFSET + ((slot % 3) + (team & 0xf) * 3) * 4;
    *(int32_t*)(battleSetting + offset) = charId;
}

// ---------------------------------------------------------------------------
// Populate a backup slot; restore copies six 0x58-byte records to bs+0x38.
// Lookup matches team at +0x08 and requires a positive character ID at +0x0C.
// ---------------------------------------------------------------------------
static void WriteSlotEntry(uint64_t bs, int index, int teamId, int32_t charId,
                           int32_t color, int32_t assist, uint8_t voiceLanguage)
{
    uint64_t entry = bs + 0x378 + index * 0x58;
    typedef void(__fastcall* SetSlotValue_t)(uint64_t, int32_t);

    *(int32_t*)(entry + 0x08) = teamId;
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_CHAR_ID))(entry, charId);
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_DRAW_MODE))(entry, color);
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_ASSIST_TYPE))(entry, assist);
    *(uint32_t*)(entry + 0x44) = voiceLanguage;
}

static void ConfigureTrainingMatchDefaults(uint64_t bs)
{
    // Native Very Hard CPU setting.
    typedef void(__fastcall* SetCpuLevel_t)(uint64_t, int32_t);
    ((SetCpuLevel_t)_addr(0x140146430))(bs, 0xff);

    // Native training meter defaults are in bars (10000 units each).
    const int32_t startingMeter = g_fiveBarStart ? 5 : 1;
    *(int32_t*)(bs + BS_P1_METER_DEFAULT_OFFSET) = startingMeter;
    *(int32_t*)(bs + BS_P2_METER_DEFAULT_OFFSET) = startingMeter;

    // Zero is the normal HP-drain behavior used by the native training path.
    *(int32_t*)(bs + BS_HP_DRAIN_MODE_OFFSET) = 0;


    for (int team = 0; team < 2; ++team) {
        const int32_t cpuControl =
            g_cpuMode == TRAINING_CPU_BOTH ||
            (g_cpuMode == TRAINING_CPU_P2_ONLY && team == 1);
        for (int slot = 0; slot < 3; ++slot) {
            *(int32_t*)(bs + 0x38 + (team * 3 + slot) * 0x58 +
                        SLOT_CPU_CONTROL_OFFSET) = cpuControl;
            *(int32_t*)(bs + 0x378 + (team * 3 + slot) * 0x58 +
                        SLOT_CPU_CONTROL_OFFSET) = cpuControl;
        }
    }
}

static void ApplySimpleModes()
{
    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef uint64_t(__fastcall* GetTeam_t)(uint64_t, int32_t);
    typedef void(__fastcall* SetEasyMode_t)(uint64_t, int32_t);
    const uint64_t characters = ((GetInstance_t)_addr(SCHARACTER_GET_INSTANCE))();
    if (!characters)
        return;

    for (int team = 0; team < 2; ++team) {
        const uint64_t chrTeam = ((GetTeam_t)_addr(SCHARACTER_GET_TEAM))(
            characters, team);
        if (chrTeam) {
            ((SetEasyMode_t)_addr(CCHRTEAM_SET_EASY_MODE))(
                chrTeam, g_bootConfig.simpleMode[team]);
        }
    }
}

static void ApplyTeamAutoJump()
{
    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef void(__fastcall* SetTeamAutoJump_t)(uint64_t, int32_t, bool);
    const uint64_t battleSetting =
        ((GetInstance_t)_addr(BATTLE_SETTING_GET_INSTANCE))();
    if (!battleSetting)
        return;

    for (int team = 0; team < 2; ++team) {
        ((SetTeamAutoJump_t)_addr(BATTLE_SETTING_SET_TEAM_AUTO_JUMP))(
            battleSetting, team, g_bootConfig.autoJump[team]);
    }
}

// Start/poll native asynchronous config loading without the menu parent unit.
// Returns true on terminal completion, including failure, not just successful load.
static bool LoadNativeGameConfig()
{
    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef uint64_t(__fastcall* StartLoad_t)(uint64_t);
    typedef void(__fastcall* SetFlag_t)(uint64_t, uint8_t);
    typedef uint64_t(__fastcall* AddUnit_t)(uint64_t, uint32_t, uint64_t);

    uint64_t config = ((GetInstance_t)_addr(SGAMECONFIG_GET_INSTANCE))();
    if (!config) {
        HookLog("TrainingBoot: WARNING - sGameConfig is not available for native load\n");
        return false;
    }

    if (!g_gameConfigLoadUnit) {
        g_gameConfigLoadUnit = ((StartLoad_t)_addr(SGAMECONFIG_START_NATIVE_LOAD))(config);
        if (!g_gameConfigLoadUnit) {
            HookLog("TrainingBoot: native sGameConfig load could not be started\n");
            return false;
        }

        ((SetFlag_t)_addr(MTOBJECT_SET_FLAG_2000))(g_gameConfigLoadUnit, 1);
        uint64_t sUnit = ((GetInstance_t)_addr(SUNIT_GET_INSTANCE))();
        ((AddUnit_t)_addr(SUNIT_ADD_UNIT))(
            sUnit, STORAGE_UNIT_CATEGORY, g_gameConfigLoadUnit);
        HookLog("TrainingBoot: started native sGameConfig storage unit\n");
        return false;
    }

    int32_t result = *(int32_t*)(g_gameConfigLoadUnit + STORAGE_RESULT_OFFSET);
    if (result == 0 || result == 1)
        return false;

    // Result 2 reports success; other terminal results are logged as failures.
    if (result == 2)
        HookLog("TrainingBoot: native sGameConfig loaded and applied\n");
    else
        HookLog("TrainingBoot: native sGameConfig load failed (result=%d)\n", result);

    // We own the storage child here; clear the config busy gate and retire it.
    *(uint8_t*)(config + SGAMECONFIG_STORAGE_BUSY_OFFSET) = 0;

    typedef void(__fastcall* KillUnit_t)(uint64_t);
    KillUnit_t killUnit = (KillUnit_t)*(uint64_t*)(*(uint64_t*)g_gameConfigLoadUnit + 0x78);
    killUnit(g_gameConfigLoadUnit);
    g_gameConfigLoadUnit = 0;
    return true;
}

// Both peers run this over the same packet, so the rule must not depend on
// either save file: a color is kept whenever its archive is installed.
static void NormalizeBootColors()
{
    for (int team = 0; team < 2; ++team) {
        for (int slot = 0; slot < 3; ++slot) {
            int32_t& character = team == 0
                ? g_bootConfig.p1Chars[slot] : g_bootConfig.p2Chars[slot];
            int32_t& color = team == 0
                ? g_bootConfig.p1Colors[slot] : g_bootConfig.p2Colors[slot];
            if (color == 0 || ChrColors_Available(character, color))
                continue;

            HookLog("TrainingBoot: color %d is not installed for chr %d; using color 0\n",
                    color, character);
            color = 0;
        }
    }

    // A mirror pick in the same color: player 1 keeps theirs and player 2's
    // copy takes the game's own substitute (color 1, or color 2 when the
    // clash is on color 1). Every peer and spectator boots from the same
    // packet, so they all see the same two colors.
    for (int slot = 0; slot < 3; ++slot) {
        const int32_t character = g_bootConfig.p2Chars[slot];
        int32_t& color = g_bootConfig.p2Colors[slot];
        if (character < 1) continue;
        for (int other = 0; other < 3; ++other) {
            if (g_bootConfig.p1Chars[other] != character ||
                g_bootConfig.p1Colors[other] != color)
                continue;
            const int32_t substitute = color == 0 ? 1 : 0;
            if (!ChrColors_Available(character, substitute))
                break; // a single-variant character (Galactus) cannot differ
            HookLog("TrainingBoot: chr %d color %d is on both teams; player 2 uses color %d\n",
                    character, color, substitute);
            color = substitute;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// The netplay client drives P2 with its own P1 bindings, so the match boot
// overwrites the P2 binding state in sGameConfig. That state is part of
// savedata.bin: the player's real P2 bindings are kept aside, swapped in for
// every save while the override is live, and put back after the match.
// ---------------------------------------------------------------------------
struct PlayerBindingState {
    uint32_t main[GAMEPLAY_BINDING_COUNT];
    uint32_t alternate[GAMEPLAY_BINDING_COUNT];
    int32_t layout;
    uint64_t tableCache;
};
static PlayerBindingState g_localP2Bindings = {};
static bool g_localP2BindingsSaved = false;
typedef void(__fastcall* PackSavePayload_t)(uint64_t);
static PackSavePayload_t oPackSavePayload = nullptr;

static void ReadPlayerTwoBindings(uint64_t config, PlayerBindingState& state)
{
    memcpy(state.main, (void*)(config + SGAMECONFIG_BINDINGS_MAIN + BINDING_PLAYER_STRIDE),
           sizeof(state.main));
    memcpy(state.alternate,
           (void*)(config + SGAMECONFIG_BINDINGS_ALTERNATE + BINDING_PLAYER_STRIDE),
           sizeof(state.alternate));
    state.layout = *(int32_t*)(config + SGAMECONFIG_BINDING_LAYOUT + 4);
    state.tableCache = *(uint64_t*)(config + SGAMECONFIG_BINDING_TABLE_CACHE + 8);
}

static void WritePlayerTwoBindings(uint64_t config, const PlayerBindingState& state)
{
    memcpy((void*)(config + SGAMECONFIG_BINDINGS_MAIN + BINDING_PLAYER_STRIDE), state.main,
           sizeof(state.main));
    memcpy((void*)(config + SGAMECONFIG_BINDINGS_ALTERNATE + BINDING_PLAYER_STRIDE),
           state.alternate, sizeof(state.alternate));
    *(int32_t*)(config + SGAMECONFIG_BINDING_LAYOUT + 4) = state.layout;
    *(uint64_t*)(config + SGAMECONFIG_BINDING_TABLE_CACHE + 8) = state.tableCache;
}

static void __fastcall Hook_packSavePayload(uint64_t config)
{
    if (!g_localP2BindingsSaved) {
        oPackSavePayload(config);
        return;
    }
    // Callers hold the config lock (sGameConfig__startSavedataSave).
    PlayerBindingState matchBindings;
    ReadPlayerTwoBindings(config, matchBindings);
    WritePlayerTwoBindings(config, g_localP2Bindings);
    oPackSavePayload(config);
    WritePlayerTwoBindings(config, matchBindings);
    HookLog("TrainingBoot: saved the player's own P2 bindings, not the netplay copy\n");
}

void TrainingBoot_RestoreLocalBindings()
{
    if (!g_localP2BindingsSaved)
        return;
    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef void(__fastcall* Lock_t)(uint64_t);
    const uint64_t config = ((GetInstance_t)_addr(SGAMECONFIG_GET_INSTANCE))();
    if (!config)
        return;
    ((Lock_t)_addr(CSYSTEM_ENTER_CS))(config);
    WritePlayerTwoBindings(config, g_localP2Bindings);
    g_localP2BindingsSaved = false;
    ((Lock_t)_addr(CSYSTEM_LEAVE_CS))(config);
    HookLog("TrainingBoot: restored the local P2 button bindings after netplay\n");
}

static bool IsNativeSavedataLoaded()
{
    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef bool(__fastcall* IsLoaded_t)(uint64_t);
    const uint64_t config = ((GetInstance_t)_addr(SGAMECONFIG_GET_INSTANCE))();
    return config && ((IsLoaded_t)_addr(SGAMECONFIG_IS_SAVEDATA_LOADED))(config);
}

void TrainingBoot_MarkNativeConfigLoaded()
{
    g_nativeGameConfigLoaded = true;
}

static void ApplyPlayerOneBindingsToPlayerTwo()
{
    if (!Netplay_IsActive() || Netplay_IsHost() || g_spectatorBoot)
        return;

    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef uint32_t(__fastcall* GetBinding_t)(uint64_t, uint32_t, uint32_t, uint8_t);
    typedef void(__fastcall* SetBindings_t)(uint64_t, uint32_t, const uint32_t*, uint8_t);
    typedef void(__fastcall* SelectBindings_t)(uint64_t, uint32_t, uint32_t, uint8_t);

    uint64_t config = ((GetInstance_t)_addr(SGAMECONFIG_GET_INSTANCE))();
    if (!config) {
        HookLog("TrainingBoot: WARNING - sGameConfig unavailable for netplay bindings\n");
        return;
    }

    // Keep the first snapshot across rematches and restarts: later boots see
    // the copy already in place, not the player's bindings.
    if (!g_localP2BindingsSaved) {
        typedef void(__fastcall* Lock_t)(uint64_t);
        ((Lock_t)_addr(CSYSTEM_ENTER_CS))(config);
        ReadPlayerTwoBindings(config, g_localP2Bindings);
        g_localP2BindingsSaved = true;
        ((Lock_t)_addr(CSYSTEM_LEAVE_CS))(config);
    }

    GetBinding_t getBinding = (GetBinding_t)_addr(SGAMECONFIG_GET_BUTTON_MAPPING);
    SetBindings_t setBindings = (SetBindings_t)_addr(SGAMECONFIG_SET_BINDING_TABLE);
    SelectBindings_t selectBindings =
        (SelectBindings_t)_addr(SGAMECONFIG_SELECT_BINDING_TABLE);

    for (uint8_t alternateMap = 0; alternateMap < 2; ++alternateMap) {
        uint32_t bindings[GAMEPLAY_BINDING_COUNT];
        for (uint32_t button = 0; button < GAMEPLAY_BINDING_COUNT; ++button)
            bindings[button] = getBinding(config, 0, button, alternateMap);

        setBindings(config, 1, bindings, alternateMap);
        // Selection 3 means the per-player custom table written above.
        selectBindings(config, 1, 3, alternateMap);
    }

    HookLog("TrainingBoot: Applied client P1 button bindings to local P2 input\n");
}

static uint8_t GetCharacterVoiceLanguage(uint64_t gameConfig, int32_t character)
{
    using GetVoice_t = bool(__fastcall*)(uint64_t, uint32_t);
    if (character > 25)
        return 1;
    return ((GetVoice_t)_addr(SGAMECONFIG_GET_CHARACTER_VOICE))(
        gameConfig, static_cast<uint32_t>(character)) ? 1u : 0u;
}

// The player's saved control type, or `fallback` when the game would ask at
// character select (no fixed control type set).
static bool GetConfiguredSimpleMode(uint64_t gameConfig, bool fallback)
{
    if (*(uint8_t*)(gameConfig + SGAMECONFIG_CTRL_TYPE_FIXED) != 1 ||
        *(uint8_t*)(gameConfig + SGAMECONFIG_CTRL_TYPE_SET) != 1)
        return fallback;
    return *(int32_t*)(gameConfig + SGAMECONFIG_CTRL_TYPE) == 1;
}

uint8_t TrainingBoot_GetVoiceLanguage(int team, int32_t character)
{
    if (team < 0 || team > 1 || character < 1 || character > 25)
        return 1;
    return (g_voiceMask[team] >> character) & 1u;
}

static bool SynchronizeInitialPlayerSettings()
{
    if (!IsBootNetworkReady())
        return false;
    if (g_initialSettingsSynchronized)
        return true;
    if (g_spectatorBoot) {
        // AutoJump and voices came with the match being watched.
        g_initialSettingsSynchronized = true;
        return true;
    }

    const int localTeam = Netplay_IsHost() ? 0 : 1;
    if (!g_initialSettingsSent) {
        using GetInstance_t = uint64_t(__fastcall*)();
        const uint64_t gameConfig =
            ((GetInstance_t)_addr(SGAMECONFIG_GET_INSTANCE))();
        if (!gameConfig)
            return false;

        const bool localEnabled = *(uint8_t*)(gameConfig + SGAMECONFIG_AUTO_JUMP) != 0;
        const int firstTeam = g_networkRequired ? localTeam : 0;
        const int lastTeam = g_networkRequired ? localTeam : 1;
        for (int team = firstTeam; team <= lastTeam; ++team) {
            g_bootConfig.autoJump[team] = localEnabled;
            // A fixed control type in the options wins, as at character
            // select; otherwise the roster's own choice stands.
            g_bootConfig.simpleMode[team] =
                GetConfiguredSimpleMode(gameConfig, g_bootConfig.simpleMode[team]);
            for (int member = 0; member < 3; ++member) {
                const int32_t character = team == 0
                    ? g_bootConfig.p1Chars[member] : g_bootConfig.p2Chars[member];
                g_bootConfig.voiceLanguage[team][member] =
                    GetCharacterVoiceLanguage(gameConfig, character);
            }
        }
        if (!g_networkRequired) {
            g_initialSettingsSynchronized = true;
            return true;
        }

        // Characters this player may yet be given (a member drawn again for
        // every game) speak as their owner chose too.
        uint32_t voiceMask = 0;
        for (int32_t character = 1; character <= 31; ++character)
            voiceMask |= static_cast<uint32_t>(
                GetCharacterVoiceLanguage(gameConfig, character)) << character;
        g_voiceMask[localTeam] = voiceMask;
        g_voiceMask[1 - localTeam] = 0xFFFFFFFFu;

        LaunchTrainingPacket packet = {};
        packet.type = PACKET_LAUNCH_TRAINING;
        packet.autoJump[localTeam] = localEnabled ? 1u : 0u;
        packet.autoJumpValidMask = 1u << localTeam;
        for (int member = 0; member < 3; ++member) {
            packet.voiceLanguage[localTeam][member] =
                g_bootConfig.voiceLanguage[localTeam][member];
        }
        packet.voiceValidMask = 1u << localTeam;
        packet.simpleMode[localTeam] = g_bootConfig.simpleMode[localTeam] ? 1u : 0u;
        packet.voiceMask = voiceMask;
        packet.flags = LAUNCH_TRAINING_SIMPLE_MODE_VALID | LAUNCH_TRAINING_VOICE_MASK_VALID;
        Netplay_SendLaunchTrainingSettings(packet);
        g_initialSettingsSent = true;
        g_initialSettingsStartTick = GetTickCount64();
        HookLog("TrainingBoot: Sent initial settings team=%d AutoJump=%d Simple=%u voice=%u/%u/%u\n",
                localTeam, localEnabled, packet.simpleMode[localTeam],
                packet.voiceLanguage[localTeam][0],
                packet.voiceLanguage[localTeam][1],
                packet.voiceLanguage[localTeam][2]);
    }

    if (GetTickCount64() - g_initialSettingsStartTick >= 30000) {
        TrainingBoot_FailNetwork("timed out exchanging initial player settings");
        return false;
    }

    LaunchTrainingPacket packet = {};
    if (!g_initialSettingsReceived &&
        Netplay_GetInboundLaunchTrainingSettings(packet)) {
        const int remoteTeam = 1 - localTeam;
        const uint32_t remoteMask = 1u << remoteTeam;
        if ((packet.autoJumpValidMask & remoteMask) != 0 &&
            (packet.voiceValidMask & remoteMask) != 0) {
            g_bootConfig.autoJump[remoteTeam] =
                packet.autoJump[remoteTeam] != 0;
            if ((packet.flags & LAUNCH_TRAINING_SIMPLE_MODE_VALID) != 0)
                g_bootConfig.simpleMode[remoteTeam] = packet.simpleMode[remoteTeam] != 0;
            if ((packet.flags & LAUNCH_TRAINING_VOICE_MASK_VALID) != 0)
                g_voiceMask[remoteTeam] = packet.voiceMask;
            for (int member = 0; member < 3; ++member) {
                const int32_t character = remoteTeam == 0
                    ? g_bootConfig.p1Chars[member] : g_bootConfig.p2Chars[member];
                g_bootConfig.voiceLanguage[remoteTeam][member] = character > 25
                    ? 1u : packet.voiceLanguage[remoteTeam][member];
            }
            g_initialSettingsReceived = true;
            HookLog("TrainingBoot: Received initial settings team=%d AutoJump=%u Simple=%d voice=%u/%u/%u\n",
                    remoteTeam, packet.autoJump[remoteTeam],
                    g_bootConfig.simpleMode[remoteTeam] ? 1 : 0,
                    g_bootConfig.voiceLanguage[remoteTeam][0],
                    g_bootConfig.voiceLanguage[remoteTeam][1],
                    g_bootConfig.voiceLanguage[remoteTeam][2]);
        }
    }

    g_initialSettingsSynchronized =
        g_initialSettingsSent && g_initialSettingsReceived;
    return g_initialSettingsSynchronized;
}

static void SetupPadToPlayerMapping()
{
    // Route slots 0/1 to P1/P2, leave 2/3 unassigned, and route keyboard to P1.
    uint64_t sMvc3Manager = *(uint64_t*)_addr(SMVC3MANAGER_INSTANCE_PTR);
    if (sMvc3Manager) {
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 0 * 4) = 0;
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 1 * 4) = 1;
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 2 * 4) = -1;
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 3 * 4) = -1;
    } else {
        HookLog("TrainingBoot: WARNING - sMvc3Manager not available for pad mapping\n");
    }

    // Enable keyboard input without reinitializing loaded device mappings.
    uint64_t sPad = *(uint64_t*)_addr(SPAD_INSTANCE_PTR);
    if (sPad) {
        *(uint8_t*)(sPad + PAD_KEYBOARD_BLOCKED_1) = 0;
        *(uint8_t*)(sPad + PAD_KEYBOARD_BLOCKED_2) = 0;


    } else {
        HookLog("TrainingBoot: WARNING - sPad not available for keyboard flag clear\n");
    }
}

// ---------------------------------------------------------------------------
// Load preferences, synchronize settings, configure the roster, and enter battle.
// Restarts reuse the active transition area instead of queueing it again.
// ---------------------------------------------------------------------------
void BootIntoTraining(bool queueAreaTransition)
{
    BootTimer_Stamp("BootIntoTraining start");

    if (g_hasBooted) {
        return;
    }

    UpdateLaunchNetworkConnection();
    if (g_networkFailed)
        return;

    // Wait for native config loading to finish; terminal errors also allow boot.
    // Once the title pre-menu has applied savedata.bin (the in-game lobby runs
    // it at startup), reloading would only discard settings not yet saved.
    if (!g_nativeGameConfigLoaded && IsNativeSavedataLoaded()) {
        g_nativeGameConfigLoaded = true;
        HookLog("TrainingBoot: savedata already loaded natively; no reload\n");
    }
    if (!g_skipNativeConfigLoad && !g_nativeGameConfigLoaded) {
        if (!LoadNativeGameConfig())
            return;
        g_nativeGameConfigLoaded = true;
    }

    // Resolve AutoJump/voice preferences before battle setup: host=P1, client=P2.
    if (!SynchronizeInitialPlayerSettings())
        return;
    if (!IsBootNetworkReady())
        return;

    NormalizeBootColors();

    ApplyPlayerOneBindingsToPlayerTwo();

    g_hasBooted = true;


    typedef uint64_t(__fastcall* GetInstance_t)();
    GetInstance_t getBattleSetting = (GetInstance_t)_addr(BATTLE_SETTING_GET_INSTANCE);
    uint64_t bs = getBattleSetting();
    if (!bs) {
        HookLog("TrainingBoot: FAILED - sBattleSetting instance is null\n");
        g_hasBooted = false;
        return;
    }

    if (queueAreaTransition) {
        // A preloaded host initializes before its dialog-free client. Seed at
        // the same native boot boundary on both, not at plugin initialization.
        using SeedGlobal_t = void(__fastcall*)();
        ((SeedGlobal_t)_addr(0x140A3D120))();
    }

    // --- Setup battle settings ---
    typedef void(__fastcall* SetInt_t)(uint64_t, int32_t);

    // Set stage and timer
    ((SetInt_t)_addr(BATTLE_SETTING_SET_STAGE_VALUE))(bs, g_bootConfig.stageId);
    ((SetInt_t)_addr(BATTLE_SETTING_SET_TIMER_VALUE))(bs, g_bootConfig.timerValue);

    // Write character IDs to the UI/display char ID array
    for (int slot = 0; slot < 3; slot++) {
        WriteCharacterId(bs, 0, slot, g_bootConfig.p1Chars[slot]);
        WriteCharacterId(bs, 1, slot, g_bootConfig.p2Chars[slot]);
    }

    // Native setters populate ID/DTI fields: P1 uses slots 0-2, P2 uses 3-5.
    for (int slot = 0; slot < 3; slot++) {
        WriteSlotEntry(bs, slot, 0, g_bootConfig.p1Chars[slot],
                       g_bootConfig.p1Colors[slot], g_bootConfig.p1Assists[slot],
                       g_bootConfig.voiceLanguage[0][slot]);
        WriteSlotEntry(bs, slot + 3, 1, g_bootConfig.p2Chars[slot],
                       g_bootConfig.p2Colors[slot], g_bootConfig.p2Assists[slot],
                       g_bootConfig.voiceLanguage[1][slot]);
    }
    // Restore reads the backup stage, so update it as well as the live field.
    *(int32_t*)(bs + BS_BACKUP_STAGE_ID_OFFSET) = g_bootConfig.stageId;
    // Enable backup restoration.
    *(char*)(bs + 0x590) = 1;

    // Use offline versus/default BattleType 0. Despite the boot API/log name,
    // this does not enable native training mode, whose predicate requires 5.
    // Battle type values at sBattleSetting+0x34C (confirmed via Ghidra):
    //   1 = Arcade
    //   2 = Attract mode
    //   3 = Online (player match)
    //   4 = Online spectator/watch battle
    //   5 = Training      <-- needed for isTrainingMode (CMP EAX,0x5)
    //   7 = Unknown mode 7
    //   8 = Heroes & Heralds
    //   9 = Online (alt)
    ((SetInt_t)_addr(BATTLE_SETTING_SET_BATTLE_TYPE))(bs, 0);

    ConfigureTrainingMatchDefaults(bs);

    typedef void(__fastcall* CommitOnePlay_t)(uint64_t, uint8_t);
    ((CommitOnePlay_t)_addr(BATTLE_SETTING_COMMIT_ONE_PLAY))(bs, 1);

    // Call the session helper used by the character-select exit path.
    GetInstance_t getAgentSession = (GetInstance_t)_addr(AGENTSESSION_GET_INSTANCE);
    uint64_t agent = getAgentSession();
    typedef void(__fastcall* AgentMethod_t)(uint64_t, uint32_t);
    ((AgentMethod_t)_addr(UUIBASE_METHOD_5560))(agent, 0);

    // The restore writes its backup control type into sGameConfig CtrlType,
    // which is saved. Back up the player's own value so it stays as it is;
    // ApplySimpleModes then sets each team's mode for this match.
    if (const uint64_t gameConfig = ((GetInstance_t)_addr(SGAMECONFIG_GET_INSTANCE))())
        *(int32_t*)(bs + BS_BACKUP_CTRL_MODE_OFFSET) =
            *(int32_t*)(gameConfig + SGAMECONFIG_CTRL_TYPE);

    // Restore backup roster/settings, then apply team control preferences.
    typedef void(__fastcall* RestoreBackup_t)(uint64_t);
     ((RestoreBackup_t)_addr(BATTLE_SETTING_RESTORE_BACKUP))(bs);
     ApplySimpleModes();
     ApplyTeamAutoJump();

     for (int team = 0; team < 2; ++team) {
        for (int member = 0; member < 3; ++member) {
            int found = -1;
            for (int index = 0, occurrence = 0; index < 6; ++index) {
                uint64_t entry = bs + 0x38 + index * 0x58;
                if (*(int32_t*)(entry + 0x08) == team &&
                    *(int32_t*)(entry + 0x0c) > 0) {
                    if (occurrence++ == member) {
                        found = *(int32_t*)(entry + 0x0c);
                        break;
                    }
                }
            }
            const int expected = team == 0
                ? g_bootConfig.p1Chars[member] : g_bootConfig.p2Chars[member];
            if (found != expected)
                HookLog("TrainingBoot: WRONG CHARACTER resolved team=%d member=%d charId=%d "
                        "expected=%d\n", team, member, found, expected);
        }
    }

    // Clear the UI field at +0x5B74 through its native helper.
    GetInstance_t getGameUI = (GetInstance_t)_addr(SGAMEUI_GET_INSTANCE);
    uint64_t ui = getGameUI();
    typedef void(__fastcall* ChrSelectUtil_t)(uint64_t, uint32_t);
    ((ChrSelectUtil_t)_addr(ACHRSELECT_UTIL_4AC0))(ui, 0);

    // Match the native training token used when leaving a training battle.
    ((SetInt_t)_addr(SGAMEUI_SET_BOOT_STATE))(ui, 13);

    // Set input routing before queueing the battle transition.
    SetupPadToPlayerMapping();

    // Initial boot queues aChrSelectBlank; restarts reuse that intermediary.
    if (queueAreaTransition) {
        uint64_t sArea = *(uint64_t*)_addr(SAREA_INSTANCE_PTR);
        if (!sArea) {
            HookLog("TrainingBoot: FAILED - sArea instance is null\n");
            g_hasBooted = false;
            return;
        }

        typedef void(__fastcall* SetArea_t)(uint64_t, void*);
        SetArea_t setArea = (SetArea_t)(*(uint64_t*)(*(uint64_t*)sArea + 0x50));
        setArea(sArea, (void*)_addr(ACHRSELBLANK_DTI));
    }
    // The whole boot in one line: characters, colors and assists per team.
    const auto& c = g_bootConfig;
    HookLog("TrainingBoot: booted P1 %d/%d/%d colors %d/%d/%d assists %d/%d/%d | "
            "P2 %d/%d/%d colors %d/%d/%d assists %d/%d/%d | stage %d timer %d "
            "simple %d/%d autojump %d/%d cpu %d%s\n",
            c.p1Chars[0], c.p1Chars[1], c.p1Chars[2], c.p1Colors[0], c.p1Colors[1], c.p1Colors[2],
            c.p1Assists[0], c.p1Assists[1], c.p1Assists[2],
            c.p2Chars[0], c.p2Chars[1], c.p2Chars[2], c.p2Colors[0], c.p2Colors[1], c.p2Colors[2],
            c.p2Assists[0], c.p2Assists[1], c.p2Assists[2],
            (int)c.stageId, (int)c.timerValue, (int)c.simpleMode[0], (int)c.simpleMode[1],
            (int)c.autoJump[0], (int)c.autoJump[1], (int)g_cpuMode,
            queueAreaTransition ? "" : " (restart)");
}

bool TrainingBoot_RestartWithConfig(const TrainingBootConfig& cfg)
{
    g_bootConfig = cfg;
    uint64_t sArea = *(uint64_t*)_addr(SAREA_INSTANCE_PTR);
    if (!sArea)
        return false;

    typedef void(__fastcall* SetArea_t)(uint64_t, void*);
    SetArea_t setArea = (SetArea_t)(*(uint64_t*)(*(uint64_t*)sArea + 0x50));
    setArea(sArea, (void*)_addr(ACHRSELBLANK_DTI));
    g_restartConfigPending = true;
    g_restartTransitionActive = true;
    g_restartSawActionSetup = false;
    HookLog("TrainingBoot: queued roster teardown before applying new settings\n");
    return true;
}

// A restart that goes through the character select: the battle has given way
// to the select screen, and the next one is booted from there once the teams
// and the stage are picked. Until then the restart counts as pending, which
// is what keeps the netplay start-up barrier from reading the old battle.
void TrainingBoot_BeginSelectRestart()
{
    g_restartTransitionActive = true;
    g_restartConfigPending = false;
    g_restartSawActionSetup = false;
    g_restartAwaitingSelect = true;
    HookLog("TrainingBoot: restart through the character select begun\n");
}

bool TrainingBoot_IsSelectRestart()
{
    return g_restartAwaitingSelect;
}

bool TrainingBoot_FinishSelectRestart(const TrainingBootConfig& cfg)
{
    if (!g_restartAwaitingSelect)
        return false;
    g_bootConfig = cfg;
    g_hasBooted = false;
    g_skipNativeConfigLoad = true;
    BootIntoTraining(true);
    g_skipNativeConfigLoad = false;
    if (!g_hasBooted)
        return false; // not ready this tick; the caller tries again
    g_restartAwaitingSelect = false;
    g_restartSawActionSetup = false;
    HookLog("TrainingBoot: booted the battle picked on the character select\n");
    return true;
}

void TrainingBoot_UpdateRestart()
{
    if (!g_restartTransitionActive)
        return;
    if (g_restartAwaitingSelect)
        return;

    uint64_t sArea = *(uint64_t*)_addr(SAREA_INSTANCE_PTR);
    if (!sArea)
        return;

    if (g_restartConfigPending) {
        // Wait for sArea to consume the pending transition before applying settings.
        if (*(uint64_t*)(sArea + 0x7040) != 0)
            return;

        g_hasBooted = false;
        g_skipNativeConfigLoad = true;
        BootIntoTraining(false);
        g_skipNativeConfigLoad = false;
        if (!g_hasBooted)
            return;

        g_restartConfigPending = false;
        g_restartSawActionSetup = false;
        HookLog("TrainingBoot: applied roster after old battle teardown\n");
        return;
    }

    typedef uint64_t(__fastcall* GetInstance_t)();
    const uint64_t action = ((GetInstance_t)_addr(0x140001A90))();
    const int32_t step = action ? *(int32_t*)(action + 0xD8) : 0;
    if (step != 2)
        g_restartSawActionSetup = true;
    if (g_restartSawActionSetup && step == 2 &&
        *(uint64_t*)(sArea + 0x7040) == 0) {
        g_restartTransitionActive = false;
        HookLog("TrainingBoot: new roster action reached completed intro setup\n");
    }
}

bool TrainingBoot_IsRestartPending()
{
    return g_restartTransitionActive;
}

// ---------------------------------------------------------------------------
// Retry boot from title updates until loading/settings synchronization completes.
// ---------------------------------------------------------------------------
typedef void(__fastcall* TitleUpdate_t)(uint64_t);
static TitleUpdate_t oTitleUpdate = nullptr;
typedef void(__fastcall* ExecuteMatchPhase_t)(uint64_t, int32_t, uint64_t, uint64_t);
static ExecuteMatchPhase_t oExecuteMatchPhase = nullptr;
typedef void(__fastcall* SetTeamAutoJump_t)(uint64_t, int32_t, bool);
static SetTeamAutoJump_t oSetTeamAutoJump = nullptr;

void __fastcall Hook_titleUpdate(uint64_t param_1)
{
    if (LobbyFlow_IsReleasedToMainMenu()) {
        // The player chose "Main Menu": the game runs its own title area
        // until the lobby is reopened.
        if (!LobbyFlow_ReleasedTitleTick(param_1) && oTitleUpdate) oTitleUpdate(param_1);
        return;
    }
    if (LobbyFlow_IsEnabled()) {
        // The lobby owns the title area: it is drawn here between matches and
        // starts each battle through the same boot path once a match is set.
        // Native title logic never runs; its menu units have been retired.
        if (!g_hasBooted && LobbyFlow_TitleTick(param_1)) {
            BootIntoTraining();
            if (g_hasBooted)
                LobbyFlow_OnBattleBootCommitted();
        }
        return;
    }

    if (!g_hasBooted) {
        BootIntoTraining();
        return; // Suppress title updates while boot is pending or starting.
    }

    if (oTitleUpdate) {
        oTitleUpdate(param_1);
    }
}

static void __fastcall Hook_setTeamAutoJump(uint64_t battleSetting,
                                             int32_t team, bool enabled)
{
    const uintptr_t caller =
        (uintptr_t)__builtin_return_address(0);
    const bool constructingBattle =
        caller == (uintptr_t)_addr(ACTION_AUTO_JUMP_P1_RETURN) ||
        caller == (uintptr_t)_addr(ACTION_AUTO_JUMP_P2_RETURN);
    if (constructingBattle) {
        const int configTeam = team == 0 ? 0 : 1;
        enabled = g_bootConfig.autoJump[configTeam];
    }
    oSetTeamAutoJump(battleSetting, team, enabled);
}

static void __fastcall Hook_executeMatchPhase(uint64_t action, int32_t phase,
                                               uint64_t unit, uint64_t context)
{
    oExecuteMatchPhase(action, phase, unit, context);
    // A battle is running: startup is over (direct training boots never pass
    // through the lobby).
    HookLog_MarkGameLoaded();
    if (!g_fiveBarStart || (phase != 1 && phase != 5))
        return;

    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef void(__fastcall* SetTeamMeter_t)(uint64_t, int32_t, float);
    uint64_t characters = ((GetInstance_t)_addr(SCHARACTER_GET_INSTANCE))();
    if (!characters)
        return;

    if (phase == 1) {
        SetTeamMeter_t setTeamMeter = (SetTeamMeter_t)_addr(SCHARACTER_SET_TEAM_METER);
        setTeamMeter(characters, 0, 50000.0f);
        setTeamMeter(characters, 1, 50000.0f);
        HookLog("TrainingBoot: Applied five-bar meter at round start\n");
        return;
    }

    // Apply Frank's level override at phase 5, after native initialization.
    typedef int32_t(__fastcall* CountCharacters_t)(uint64_t);
    typedef uint64_t(__fastcall* GetCharacter_t)(uint64_t, int32_t);
    const int32_t count = ((CountCharacters_t)_addr(SCHARACTER_COUNT_CHARACTERS))(characters);
    int32_t frankCount = 0;
    for (int32_t index = 0; index < count; ++index) {
        uint64_t character = ((GetCharacter_t)_addr(SCHARACTER_GET_CHARACTER_BY_INDEX))(
            characters, index);
        if (character && *(uintptr_t*)character == (uintptr_t)_addr(VTABLE_UFRANK)) {
            // The global roster includes benched characters on both teams.
            *(int32_t*)(character + UFRANK_LEVEL_OFFSET) = 3;
            ++frankCount;
        }
    }
    HookLog("TrainingBoot: Applied level 3 to %d Frank(s) on fighting entry (roster=%d)\n",
            frankCount, count);
}

// ---------------------------------------------------------------------------
// Install title-boot, battle-construction AutoJump, and match-phase hooks.
// ---------------------------------------------------------------------------
void TrainingBoot_InitHooks()
{
    if (g_hooksInitialized) return;

    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        HookLog("TrainingBoot: MH_Initialize failed (%d)\n", mhStatus);
        return;
    }

    ChrColors_InstallUnlock();

    if (MH_CreateHook(
        (LPVOID)_addr(0x140217200),
        (LPVOID)Hook_titleUpdate,
        (LPVOID*)&oTitleUpdate
    ) == MH_OK) {
        MH_EnableHook((LPVOID)_addr(0x140217200));
    }
    else {
        HookLog("TrainingBoot: FAILED to hook aTitle__update\n");
    }

    if (MH_CreateHook(
        (LPVOID)_addr(BATTLE_SETTING_SET_TEAM_AUTO_JUMP),
        (LPVOID)Hook_setTeamAutoJump,
        (LPVOID*)&oSetTeamAutoJump
    ) == MH_OK) {
        MH_EnableHook((LPVOID)_addr(BATTLE_SETTING_SET_TEAM_AUTO_JUMP));
    }
    else {
        HookLog("TrainingBoot: FAILED to hook team AutoJump setter\n");
    }

    if (MH_CreateHook(
        (LPVOID)_addr(ACTION_EXECUTE_MATCH_PHASE),
        (LPVOID)Hook_executeMatchPhase,
        (LPVOID*)&oExecuteMatchPhase
    ) == MH_OK) {
        MH_EnableHook((LPVOID)_addr(ACTION_EXECUTE_MATCH_PHASE));
    }
    else {
        HookLog("TrainingBoot: FAILED to hook sAction__executeMatchPhase\n");
    }

    if (MH_CreateHook(
        (LPVOID)_addr(SGAMECONFIG_PACK_SAVE_PAYLOAD),
        (LPVOID)Hook_packSavePayload,
        (LPVOID*)&oPackSavePayload
    ) == MH_OK) {
        MH_EnableHook((LPVOID)_addr(SGAMECONFIG_PACK_SAVE_PAYLOAD));
    }
    else {
        HookLog("TrainingBoot: FAILED to hook savedata payload packing\n");
    }

    g_hooksInitialized = true;
}
