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

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static TrainingBootConfig g_bootConfig = {
    { CHR_WOLVERINE, CHR_WOLVERINE, CHR_WOLVERINE },   // P1 (all 3 slots = Wolverine)
    { CHR_WOLVERINE, CHR_WOLVERINE, CHR_WOLVERINE },   // P2 (all 3 slots = Wolverine)
    0,    // stage Daily Bugle
    99   // timer (99 = infinite in training)
};
static bool g_hasBooted = false;
static bool g_hooksInitialized = false;

// ---------------------------------------------------------------------------
// Extern from dllmain.cpp
// ---------------------------------------------------------------------------
extern void HookLog(const char* fmt, ...);

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
// Write a slot entry into the backup source region (bs+0x378 + index*0x58).
// Each entry layout: +0x08=teamId, +0x0c=charId (0xffff = empty).
// restoreBackupAndInit copies these to the live slot area (bs+0x38) and
// method_CDB0 reads them back for UI init.
// ---------------------------------------------------------------------------
static void WriteSlotEntry(uint64_t bs, int index, int teamId, int32_t charId)
{
    uint64_t entry = bs + 0x378 + index * 0x58;
    *(int32_t*)(entry + 0x08) = teamId;
    *(int32_t*)(entry + 0x0c) = charId;
    HookLog("TrainingBoot: Slot entry %d: team=%d charId=%d\n", index, teamId, charId);
}

// ---------------------------------------------------------------------------
// BootIntoTraining: configures sBattleSetting and transitions to battle.
// Follows the same flow as aChrSelect__onSelectionComplete for training mode (uVar3 == 6).
// Sequence: set up chars/stage → set battle type → agent reset → restore backup →
//           UI flags → create loading screen → area transition to aChrSelectBlank
// ---------------------------------------------------------------------------
void BootIntoTraining()
{
    if (g_hasBooted) {
        return;
    }
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

    // Write slot entries at the backup source (bs+0x378) so
    // restoreBackupAndInit has valid player data to work with.
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
    ((SetInt_t)_addr(BATTLE_SETTING_SET_BATTLE_TYPE))(bs, 5);
    HookLog("TrainingBoot: Battle type set to 5 (training)\n");

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
