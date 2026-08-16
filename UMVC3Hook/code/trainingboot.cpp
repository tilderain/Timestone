// trainingboot.cpp
// Boot directly into a training match, bypassing title/menu/charsel.
#include "../pch.h"
#include "trainingboot.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include <Windows.h>
#include "kiero/minhook/include/MinHook.h"

using namespace Memory::VP;

// ---------------------------------------------------------------------------
// Addresses (all VA, converted via _addr at runtime)
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

// sArea scene-chain manager
static constexpr uintptr_t SAREA_INSTANCE_PTR                = 0x140e17a80;

// aChrSelectBlank DTI (loading screen before battle)
static constexpr uintptr_t ACHRSELBLANK_DTI                  = 0x140d500f0;

// sGameUI
static constexpr uintptr_t SGAMEUI_GET_INSTANCE              = 0x140001ac0;
static constexpr uintptr_t SGAMEUI_IS_CHAR_VARIANT_SELECTABLE = 0x1402bc990;
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

// Training/versus options in sBattleSetting. These are consumed by the native
// training UI setup after the battle descriptor has been restored.
static constexpr uintptr_t BS_BACKUP_STAGE_ID_OFFSET             = 0x588;
static constexpr uintptr_t BS_P2_METER_DEFAULT_OFFSET         = 0x29c;
static constexpr uintptr_t BS_P1_METER_DEFAULT_OFFSET         = 0x2d4;
static constexpr uintptr_t BS_HP_DRAIN_MODE_OFFSET            = 0x2d8;
static constexpr uintptr_t SLOT_CPU_CONTROL_OFFSET             = 0x18;

// --- Pad / Keyboard input routing ---
// sPad singleton instance pointer (written by sPad__sPad_ctor @ 0x14050fbe3).
// The pad object has 4 slots at stride 0x2E0 starting at +0x62.
// sMvc3Manager stores the pad-to-player assignment at +0x140 (int32_t per slot).
// keyboardToGameMask_maybe (0x1402B3390) and GetsCurrentInput_withSOCD (0x1402B3700)
// check manager+0x140[0] to decide if keyboard maps to P1 (0) or another player.
// pad+0xc20 and pad+0xc30 are keyboard-disable flags — when non-zero, keyboard
// is blocked regardless of assignment.
//
// REFS:
//   sPad__sPad_ctor:          0x14050FBA0
//   sPad__initSlotInner:      0x1405142A0  (writes keyboard scan-to-button map)
//   GetsCurrentInput_withSOCD:0x1402B3700  (routes keyboard-vs-pad per player)
//   sMvc3Main__keyboardToGameMask_maybe: 0x1402B3390 (VK→game bitmask mapping)
//   sMvc3Manager__isPadAssigned:         0x14025AD10 (checks slot != -1)
//   sMvc3Manager__getPlayerCharIdx:      0x14025AC40 (reads manager+0x140[slot])
//   sKeyboard__sKeyboard:    0x1406A96C0  (DirectInput keyboard state buffer at +0x138)
//   FUN_1402950e0:           0x1402950E0  (returns keyboard state provider at 0x140E1A198)
//   sPad__instance:          0x140E174B8  (global, written at sPad__sPad_ctor+0x43)
//   sMvc3Manager pointer:    0x140D510A0  (global, deref to get manager instance)
static constexpr uintptr_t SPAD_INSTANCE_PTR                 = 0x140E174B8;
static constexpr uintptr_t SMVC3MANAGER_INSTANCE_PTR         = 0x140D510A0;

// sGameConfig owns the two gameplay button-binding tables consumed by
// cChrInput__buttonRemap. Netplay's client controls team P2, but should use
// that machine's P1 bindings before transmitting the remapped logical input.
static constexpr uintptr_t SGAMECONFIG_GET_INSTANCE           = 0x140001AB0;
static constexpr uintptr_t SGAMECONFIG_START_NATIVE_LOAD      = 0x140253620;
static constexpr uintptr_t SGAMECONFIG_GET_BUTTON_MAPPING     = 0x1402522A0;
static constexpr uintptr_t SGAMECONFIG_SET_BINDING_TABLE      = 0x140254A20;
static constexpr uintptr_t SGAMECONFIG_SELECT_BINDING_TABLE   = 0x140254C40;
static constexpr uintptr_t SGAMECONFIG_STORAGE_BUSY_OFFSET    = 0x4D350;
static constexpr uint32_t GAMEPLAY_BINDING_COUNT              = 11;

static constexpr uintptr_t SUNIT_GET_INSTANCE                 = 0x140003160;
static constexpr uintptr_t SUNIT_ADD_UNIT                      = 0x14051A6B0;
static constexpr uintptr_t MTOBJECT_SET_FLAG_2000              = 0x1402545A0;
static constexpr uint32_t STORAGE_UNIT_CATEGORY                = 0x19;
static constexpr uintptr_t STORAGE_RESULT_OFFSET               = 0x4C;

// sPad__initSlotInner receives the embedded Map object, not the slot base.
static constexpr uintptr_t PAD_SLOT_BASE_OFFSET               = 0x58;
static constexpr uintptr_t PAD_SLOT_MAP_OFFSET                = 0x198;

// Offsets within sMvc3Manager:
static constexpr uintptr_t MANAGER_PAD_ASSIGN_OFFSET         = 0x140;   // int32_t[4] pad-slot→player index (-1=unassigned, 0=P1, 1=P2)

// Offsets within sPad:
static constexpr uintptr_t PAD_KEYBOARD_BLOCKED_1            = 0xc20;   // byte: non-zero blocks keyboard
static constexpr uintptr_t PAD_KEYBOARD_BLOCKED_2            = 0xc30;   // byte: non-zero blocks keyboard

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static TrainingBootConfig g_bootConfig = {
    { CHR_MORRIGAN, CHR_MORRIGAN, CHR_MORRIGAN },   // P1 (all 3 slots = Wolverine)
    { CHR_MORRIGAN, CHR_MORRIGAN, CHR_MORRIGAN },   // P2 (all 3 slots = Wolverine)
    { 0, 0, 0 }, // P1 colors
    { 0, 0, 0 }, // P2 colors
    { 0, 0, 0 }, // P1 assists (Alpha)
    { 0, 0, 0 }, // P2 assists (Alpha)
    0,    // stage 
    99   // timer (99 = infinite in training)
};
static bool g_hasBooted = false;
static bool g_hooksInitialized = false;
static uint64_t g_gameConfigLoadUnit = 0;
static TrainingBootCpuMode g_cpuMode = TRAINING_CPU_NONE;

// ---------------------------------------------------------------------------
// Extern from dllmain.cpp
// ---------------------------------------------------------------------------
extern void HookLog(const char* fmt, ...);
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

void TrainingBoot_SetCpuMode(TrainingBootCpuMode mode)
{
    g_cpuMode = mode;
}

TrainingBootCpuMode TrainingBoot_GetCpuMode()
{
    return g_cpuMode;
}

// ---------------------------------------------------------------------------
// Write a character ID directly into sBattleSetting's char ID array
// ---------------------------------------------------------------------------
static void WriteCharacterId(uint64_t battleSetting, int team, int slot, int32_t charId)
{
    uintptr_t offset = CHAR_ID_ARRAY_OFFSET + ((slot % 3) + (team & 0xf) * 3) * 4;
    *(int32_t*)(battleSetting + offset) = charId;
    HookLog("TrainingBoot: Set team=%d slot=%d charId=%d\n", team, slot, charId);
}

// ---------------------------------------------------------------------------
// Write the fields used by the native slot lookup into the backup source.
// restoreBackupAndInit copies six 0x58-byte records from bs+0x378 to bs+0x38.
// Native lookup scans slot+0x08 for team and treats a positive character ID at
// slot+0x0c as an active entry.
// ---------------------------------------------------------------------------
static void WriteSlotEntry(uint64_t bs, int index, int teamId, int32_t charId,
                           int32_t color, int32_t assist)
{
    uint64_t entry = bs + 0x378 + index * 0x58;
    typedef void(__fastcall* SetSlotValue_t)(uint64_t, int32_t);

    *(int32_t*)(entry + 0x08) = teamId;
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_CHAR_ID))(entry, charId);
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_DRAW_MODE))(entry, color);
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_ASSIST_TYPE))(entry, assist);
    HookLog("TrainingBoot: Native slot entry %d: team=%d charId=%d dti=%d color=%d assist=%d\n",
            index, teamId, charId, *(int32_t*)(entry + 0x10), color, assist);
}

static void ConfigureTrainingMatchDefaults(uint64_t bs)
{
    // 0xff is the native "Very Hard" CPU level (the final entry in the
    // versus CPU-level table: 0x20, 0x60, 0xa0, 0xc0, 0xff).
    typedef void(__fastcall* SetCpuLevel_t)(uint64_t, int32_t);
    ((SetCpuLevel_t)_addr(0x140146430))(bs, 0xff);

    // Native training setup multiplies these values by 10000 when initializing
    // the two team meters. Keep both teams at the regular-versus default of 1.
    *(int32_t*)(bs + BS_P1_METER_DEFAULT_OFFSET) = 1;
    *(int32_t*)(bs + BS_P2_METER_DEFAULT_OFFSET) = 1;

    // Zero is the normal HP-drain behavior used by the native training path.
    *(int32_t*)(bs + BS_HP_DRAIN_MODE_OFFSET) = 0;

    HookLog("TrainingBoot: Defaults set (CPU=Very Hard, meter P1/P2=1, HP drain=normal)\n");

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
    HookLog("TrainingBoot: CPU control mode=%d\n", (int)g_cpuMode);
}

// ---------------------------------------------------------------------------
// Save data & pad-to-player mapping notes
// ---------------------------------------------------------------------------
// PROBLEM: In the normal "Press Start" flow, save data is loaded from
// savedata.bin ("savedata/umvc3/savedata.bin"). This does several things
// that our training boot bypasses:
//   1. Writes manager+0x140[0..3] — assigns pad slots 0-3 to player indices
//      (0=P1, 1=P2, -1=unassigned). Slot 0 also controls which player index
//      the keyboard feeds into (checked by GetsCurrentInput_withSOCD @ 0x1402B3700).
//   2. Clears pad+0xc20/pad+0xc30 keyboard-disable flags (set during license
//      check when no save data exists yet).
//   3. Initializes keyboard scan→button mapping per slot via initSlotInner
//      (0x1405142A0) — writes the 0x20-byte translation table at slot+0x118.
//   4. Loads controller config (button assignments, stick zones).
//
// The save data loading itself is orchestrated by sLicense (0x140279AE0)
// and uSaveDataAccess (0x14031CBC0), but we bypass it completely.
// SetupPadToPlayerMapping() below writes the bare minimum to get keyboard
// input routing working for Escape/pause and Start/skip-intro.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SetupPadToPlayerMapping: Directly writes the pad-to-player assignment and
// clears keyboard-disable flags, bypassing the need for savedata.bin loading.
//
// In the normal flow (title screen → "Press Start" → menu), the license/
// save-data init does this. We replicate the essential state here since our
// boot bypasses the entire menu flow.
//
// GetsCurrentInput_withSOCD (0x1402B3700) routing logic:
//   keyboardPlayerIdx = manager+0x140[0]
//   if (keyboardPlayerIdx == requestedPlayerIdx && pad+0xc30 == 0)
//       return keyboardInput(requestedPlayerIdx)
//   else
//       return padSlotInput[requestedPlayerIdx]  // pad + playerIdx*0x2E0 + 0x1BC
//
// sMvc3Main__method_3D00 (called from keyboard reader) also checks:
//   if (pad+0xc20 != 0 || pad+0xc30 != 0) → return 0 (blocked)
// ---------------------------------------------------------------------------
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

    // uStoragePC result 2 is success; 5 and 9 are native load errors. The
    // success path has already invoked the sGameConfig apply callback before
    // publishing the result.
    if (result == 2)
        HookLog("TrainingBoot: native sGameConfig loaded and applied\n");
    else
        HookLog("TrainingBoot: native sGameConfig load failed (result=%d)\n", result);

    // The regular menu's parent unit clears this after consuming its child.
    // Training boot owns the child directly, so release the busy gate here.
    *(uint8_t*)(config + SGAMECONFIG_STORAGE_BUSY_OFFSET) = 0;

    typedef void(__fastcall* KillUnit_t)(uint64_t);
    KillUnit_t killUnit = (KillUnit_t)*(uint64_t*)(*(uint64_t*)g_gameConfigLoadUnit + 0x78);
    killUnit(g_gameConfigLoadUnit);
    g_gameConfigLoadUnit = 0;
    return true;
}

static bool IsNativeColorSelectable(int32_t characterId, int32_t color)
{
    if (characterId < 1 || characterId > CHR_GALACTUS || color < 0)
        return false;

    typedef uint64_t(__fastcall* GetInstance_t)();
    typedef bool(__fastcall* IsSelectable_t)(uint64_t, uint32_t, int32_t);
    const uint64_t gameUi = ((GetInstance_t)_addr(SGAMEUI_GET_INSTANCE))();
    if (!gameUi)
        return false;

    return ((IsSelectable_t)_addr(SGAMEUI_IS_CHAR_VARIANT_SELECTABLE))(
        gameUi, static_cast<uint32_t>(characterId), color);
}

static void NormalizeBootColors()
{
    for (int team = 0; team < 2; ++team) {
        for (int slot = 0; slot < 3; ++slot) {
            int32_t& character = team == 0
                ? g_bootConfig.p1Chars[slot] : g_bootConfig.p2Chars[slot];
            int32_t& color = team == 0
                ? g_bootConfig.p1Colors[slot] : g_bootConfig.p2Colors[slot];
            if (IsNativeColorSelectable(character, color))
                continue;

            HookLog("TrainingBoot: color %d is locked for chr %d; using color 0\n",
                    color, character);
            color = 0;
        }
    }
}

static void ApplyPlayerOneBindingsToPlayerTwo()
{
    if (!Netplay_IsActive() || Netplay_IsHost())
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

static void SetupPadToPlayerMapping()
{
    // --- sMvc3Manager: pad-slot→player assignment ---
    // manager + 0x140 is an int32_t[4]; write slot→player mapping.
    // Slot 0 = P1 (also keyboard → P1). Slot 1 = P2.
    uint64_t sMvc3Manager = *(uint64_t*)_addr(SMVC3MANAGER_INSTANCE_PTR);
    if (sMvc3Manager) {
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 0 * 4) = 0;  // slot 0 → player 0 (P1, also keyboard)
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 1 * 4) = 1;  // slot 1 → player 1 (P2)
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 2 * 4) = -1; // slot 2 → unassigned
        *(int32_t*)(sMvc3Manager + MANAGER_PAD_ASSIGN_OFFSET + 3 * 4) = -1; // slot 3 → unassigned
        HookLog("TrainingBoot: Pad-to-player mapping set (slots 0→P1, 1→P2)\n");
    } else {
        HookLog("TrainingBoot: WARNING - sMvc3Manager not available for pad mapping\n");
    }

    // --- sPad: clear keyboard-disable flags ---
    uint64_t sPad = *(uint64_t*)_addr(SPAD_INSTANCE_PTR);
    if (sPad) {
        *(uint8_t*)(sPad + PAD_KEYBOARD_BLOCKED_1) = 0;
        *(uint8_t*)(sPad + PAD_KEYBOARD_BLOCKED_2) = 0;

        // Reapply the native P1 keyboard translation table. The pad
        // constructor normally does this before savedata/controller setup.
        typedef void(__fastcall* InitSlotInner_t)(uint64_t);
        InitSlotInner_t initSlotInner = (InitSlotInner_t)_addr(0x1405142A0);
        uint64_t p1Map = sPad + PAD_SLOT_BASE_OFFSET + PAD_SLOT_MAP_OFFSET;
        initSlotInner(p1Map);
        HookLog("TrainingBoot: Keyboard-disable flags cleared (pad+0x%x, pad+0x%x)\n",
                PAD_KEYBOARD_BLOCKED_1, PAD_KEYBOARD_BLOCKED_2);
        HookLog("TrainingBoot: Reinitialized P1 keyboard/controller map\n");

        // Force slot 1 (P2) device-active flags to non-zero so the game thinks
        // a physical controller is plugged in for P2. This prevents the
        // "Press Start" / wait-for-P2 state when only one pad is connected.
        // Slot layout (from sPadCheckSlotDeviceAlive_maybe disasm):
        //   slot_base = sPad + slotIndex * 0x2E0
        //   device_active at slot_base + 0x60
        //   ready_counter at slot_base + 0x63
    //    uint64_t slot1 = sPad + 1 * 0x2E0;
    //    *(uint8_t*)(slot1 + 0x60) = 1;  // device active flag
    //    *(uint8_t*)(slot1 + 0x63) = 1;  // ready counter
    //    HookLog("TrainingBoot: Forced slot 1 device flags to alive (sPad+0x%llx)\n",
    //            (unsigned long long)(slot1));
    } else {
        HookLog("TrainingBoot: WARNING - sPad not available for keyboard flag clear\n");
    }
}

// ---------------------------------------------------------------------------
// BootIntoTraining: configures sBattleSetting and transitions to battle.
// Follows the same flow as aChrSelect__onSelectionComplete for training mode (uVar3 == 6).
// Sequence: set up chars/stage → set battle type → agent reset → restore backup →
//           UI flags → create loading screen → area transition → pad mapping
// ---------------------------------------------------------------------------
void BootIntoTraining()
{
    BootTimer_Stamp("BootIntoTraining start");

    if (g_hasBooted) {
        return;
    }

    // Savedata loading is asynchronous. Keep the title move hook active until
    // the native uStoragePC has loaded and applied the complete sGameConfig.
    if (!LoadNativeGameConfig())
        return;

    NormalizeBootColors();

    ApplyPlayerOneBindingsToPlayerTwo();

    g_hasBooted = true;

    HookLog("TrainingBoot: Starting boot sequence...\n");

    typedef uint64_t(__fastcall* GetInstance_t)();
    GetInstance_t getBattleSetting = (GetInstance_t)_addr(BATTLE_SETTING_GET_INSTANCE);
    uint64_t bs = getBattleSetting();
    if (!bs) {
        HookLog("TrainingBoot: FAILED - sBattleSetting instance is null\n");
        g_hasBooted = false;
        return;
    }
    HookLog("TrainingBoot: sBattleSetting = 0x%llx\n", bs);

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

    // Populate backup records through the native character setter. In addition
    // to the ID at +0x0c, it resolves the character DTI into +0x10. Character
    // creation otherwise depends on asynchronous archive completion order.
    // P1 = entries 0-2 (team 0), P2 = entries 3-5 (team 1).
    for (int slot = 0; slot < 3; slot++) {
        WriteSlotEntry(bs, slot, 0, g_bootConfig.p1Chars[slot],
                       g_bootConfig.p1Colors[slot], g_bootConfig.p1Assists[slot]);
        WriteSlotEntry(bs, slot + 3, 1, g_bootConfig.p2Chars[slot],
                       g_bootConfig.p2Colors[slot], g_bootConfig.p2Assists[slot]);
    }
    // restoreBackupAndInit restores the stage from the backup descriptor,
    // not from the live mStageArea field set above.
    *(int32_t*)(bs + BS_BACKUP_STAGE_ID_OFFSET) = g_bootConfig.stageId;
    // Set the backup flag so restoreBackupAndInit performs the restore
    *(char*)(bs + 0x590) = 1;
    HookLog("TrainingBoot: Backup flag set\n");

    // Set battle type to training BEFORE restoreBackupAndInit so its
    // training-specific init path (isTrainingMode check) executes.
    // In the original aChrSelect flow, the battle type was already set
    // by the mode selection before onSelectionComplete is reached.
    //
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
    HookLog("TrainingBoot: Battle type set to training (0)\n");

    ConfigureTrainingMatchDefaults(bs);

    typedef void(__fastcall* CommitOnePlay_t)(uint64_t, uint8_t);
    ((CommitOnePlay_t)_addr(BATTLE_SETTING_COMMIT_ONE_PLAY))(bs, 1);

    // --- Follow training mode init flow (aChrSelect__onSelectionComplete, uVar3==6) ---

    // 1. sAgentSession::method_5560(agent, 0) — reset agent/session state
    GetInstance_t getAgentSession = (GetInstance_t)_addr(AGENTSESSION_GET_INSTANCE);
    uint64_t agent = getAgentSession();
    typedef void(__fastcall* AgentMethod_t)(uint64_t, uint32_t);
    ((AgentMethod_t)_addr(UUIBASE_METHOD_5560))(agent, 0);
    HookLog("TrainingBoot: Agent session reset\n");

    // 2. Restore backup — copies slot data from bs+0x378 to bs+0x38,
    //    sets up game UI with character selections, runs training init.
    typedef void(__fastcall* RestoreBackup_t)(uint64_t);
    ((RestoreBackup_t)_addr(BATTLE_SETTING_RESTORE_BACKUP))(bs);
    HookLog("TrainingBoot: Backup restored\n");

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
            HookLog("TrainingBoot: Resolved team=%d member=%d charId=%d expected=%d\n",
                    team, member, found, expected);
        }
    }

    // 3. aChrSelect__util_4AC0(ui, 0) — write 0 to ui+0x5b74 (training flag)
    GetInstance_t getGameUI = (GetInstance_t)_addr(SGAMEUI_GET_INSTANCE);
    uint64_t ui = getGameUI();
    typedef void(__fastcall* ChrSelectUtil_t)(uint64_t, uint32_t);
    ((ChrSelectUtil_t)_addr(ACHRSELECT_UTIL_4AC0))(ui, 0);
    HookLog("TrainingBoot: UI util flag set (ui+0x5b74 = 0)\n");

    // Match the native training token used when leaving a training battle.
    ((SetInt_t)_addr(SGAMEUI_SET_BOOT_STATE))(ui, 13);

    // 4. sGameUI__createLoadingScreen(ui, 0, 0) — create loading screen object
   //typedef void(__fastcall* CreateLoading_t)(uint64_t, uint8_t, uint32_t);
   // ((CreateLoading_t)_addr(SGAMEUI_CREATE_LOADING_SCREEN))(ui, 0, 0);
    //HookLog("TrainingBoot: Loading screen created\n");

    // 5. Set pad-to-player mapping before entering the battle area. setArea can
    //    synchronously initialize battle input, so doing this afterward is too
    //    late for the first controller-assignment check.
    SetupPadToPlayerMapping();

    // 6. Queue the native transition intermediary. It waits for stage resources,
    //    resolves aStage%03d, and queues that DTI; sArea then constructs the
    //    target's area-parent chain through aGame and aAction.
    uint64_t sArea = *(uint64_t*)_addr(SAREA_INSTANCE_PTR);
    if (!sArea) {
        HookLog("TrainingBoot: FAILED - sArea instance is null\n");
        g_hasBooted = false;
        return;
    }

    typedef void(__fastcall* SetArea_t)(uint64_t, void*);
    SetArea_t setArea = (SetArea_t)(*(uint64_t*)(*(uint64_t*)sArea + 0x50));
    setArea(sArea, (void*)_addr(ACHRSELBLANK_DTI));
    HookLog("TrainingBoot: Queued aChrSelectBlank battle transition\n");
}

// ---------------------------------------------------------------------------
// Hook on aTitle__update: fires BootIntoTraining once on first call.
// ---------------------------------------------------------------------------
typedef void(__fastcall* TitleUpdate_t)(uint64_t);
static TitleUpdate_t oTitleUpdate = nullptr;

void __fastcall Hook_titleUpdate(uint64_t param_1)
{
    if (!g_hasBooted) {
        BootIntoTraining();
        return; // Skip original — we're already transitioning areas.
    }

    if (oTitleUpdate) {
        oTitleUpdate(param_1);
    }
}

// ---------------------------------------------------------------------------
// Init: hooks aTitle__update.
// ---------------------------------------------------------------------------
void TrainingBoot_InitHooks()
{
    if (g_hooksInitialized) return;

    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        HookLog("TrainingBoot: MH_Initialize failed (%d)\n", mhStatus);
        return;
    }

    if (MH_CreateHook(
        (LPVOID)_addr(0x140217200),
        (LPVOID)Hook_titleUpdate,
        (LPVOID*)&oTitleUpdate
    ) == MH_OK) {
        MH_EnableHook((LPVOID)_addr(0x140217200));
        HookLog("TrainingBoot: Hooked aTitle__update\n");
    }
    else {
        HookLog("TrainingBoot: FAILED to hook aTitle__update\n");
    }

    g_hooksInitialized = true;
}
