// trainingboot.cpp
// Boot directly into a training match, bypassing title/menu/charsel.
#include "../pch.h"
#include "trainingboot.h"
#include "mk10utils.h"
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
static constexpr uintptr_t BATTLE_SETTING_SET_ASSIST_TYPE    = 0x1401461e0;

// sArea (to transition to battle)
static constexpr uintptr_t SAREA_INSTANCE_PTR                = 0x140e17a80;

// DTI for aNextBattle (type descriptor for the battle area)
static constexpr uintptr_t ANEXTBATTLE_DTI                   = 0x140d50618;

// aChrSelectBlank DTI (loading screen before battle)
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

// sMvc3Pad controller configuration manager.
// Ghidra: the public load/request method starts the storage-thread request;
// the worker itself must not be called from the title update thread.
static constexpr uintptr_t SMVC3PAD_INSTANCE_PTR             = 0x140E1B3E0;
static constexpr uintptr_t SMVC3PAD_REQUEST_LOAD              = 0x14075B970;
static constexpr uintptr_t SMVC3PAD_BUSY_OFFSET               = 0x3c;
static constexpr uintptr_t SMVC3PAD_RESULT_OFFSET             = 0x40;

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
    0,    // stage 
    99   // timer (99 = infinite in training)
};
static bool g_hasBooted = false;
static bool g_hooksInitialized = false;
static bool g_controllerConfigRequested = false;

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
static void WriteSlotEntry(uint64_t bs, int index, int teamId, int32_t charId)
{
    uint64_t entry = bs + 0x378 + index * 0x58;
    typedef void(__fastcall* SetSlotValue_t)(uint64_t, int32_t);

    *(int32_t*)(entry + 0x08) = teamId;
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_CHAR_ID))(entry, charId);
    ((SetSlotValue_t)_addr(BATTLE_SETTING_SET_ASSIST_TYPE))(entry, 0);
    HookLog("TrainingBoot: Native slot entry %d: team=%d charId=%d dti=%d\n",
            index, teamId, charId, *(int32_t*)(entry + 0x10));
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
static bool LoadPlayerOneControllerConfig()
{
    uint64_t config = *(uint64_t*)_addr(SMVC3PAD_INSTANCE_PTR);
    if (!config) {
        HookLog("TrainingBoot: WARNING - sMvc3Pad instance is not available\n");
        return true;
    }

    typedef uint64_t(__fastcall* RequestLoad_t)(uint64_t);
    RequestLoad_t requestLoad = (RequestLoad_t)_addr(SMVC3PAD_REQUEST_LOAD);

    if (!g_controllerConfigRequested) {
        uint64_t result = requestLoad(config);
        if (result == 8) {
            HookLog("TrainingBoot: sMvc3Pad config load is already busy\n");
        } else {
            HookLog("TrainingBoot: requested sMvc3Pad controller config load\n");
        }
        g_controllerConfigRequested = true;
        return false;
    }

    if (*(int32_t*)(config + SMVC3PAD_BUSY_OFFSET) != 0)
        return false;

    int32_t result = *(int32_t*)(config + SMVC3PAD_RESULT_OFFSET);
    if (result != 0) {
        HookLog("TrainingBoot: sMvc3Pad controller config load failed (result=%d)\n",
                result);
    } else {
        HookLog("TrainingBoot: sMvc3Pad controller config loaded\n");
    }
    return true;
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

    // savedata/controller loading is asynchronous. Keep the title update
    // hook active until the native sMvc3Pad request has settled.
    if (!LoadPlayerOneControllerConfig())
        return;

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
        WriteSlotEntry(bs, slot,     0, g_bootConfig.p1Chars[slot]);
        WriteSlotEntry(bs, slot + 3, 1, g_bootConfig.p2Chars[slot]);
    }
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
    //   4 = Online Ranked
    //   5 = Training      <-- needed for isTrainingMode (CMP EAX,0x5)
    //   7 = Unknown mode 7
    //   8 = Heroes & Heralds
    //   9 = Online (alt)
    ((SetInt_t)_addr(BATTLE_SETTING_SET_BATTLE_TYPE))(bs, 0);
    HookLog("TrainingBoot: Battle type set to 0\n");

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

    // 4. sGameUI__createLoadingScreen(ui, 0, 0) — create loading screen object
   //typedef void(__fastcall* CreateLoading_t)(uint64_t, uint8_t, uint32_t);
   // ((CreateLoading_t)_addr(SGAMEUI_CREATE_LOADING_SCREEN))(ui, 0, 0);
    //HookLog("TrainingBoot: Loading screen created\n");

    // 5. Area transition to aChrSelectBlank (loading screen → battle area)
    uint64_t sArea = *(uint64_t*)_addr(SAREA_INSTANCE_PTR);
    if (!sArea) {
        HookLog("TrainingBoot: FAILED - sArea instance is null\n");
        g_hasBooted = false;
        return;
    }

    typedef void(__fastcall* SetArea_t)(uint64_t, void*);
    SetArea_t setArea = (SetArea_t)(*(uint64_t*)(*(uint64_t*)sArea + 0x50));
    setArea(sArea, (void*)_addr(ACHRSELBLANK_DTI));
    HookLog("TrainingBoot: Area transition to aChrSelectBlank!\n");

    // 6. Set pad-to-player mapping + clear keyboard disable flags.
    //    In the normal Press-Start flow, savedata.bin loading does this
    //    during menu init. We bypass the menu, so do it directly here.
    SetupPadToPlayerMapping();
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
