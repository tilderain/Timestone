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
// BootIntoTraining: configures sBattleSetting and transitions to aNextBattle.
// ---------------------------------------------------------------------------
void BootIntoTraining()
{
    if (g_hasBooted) {
        return;
    }
    g_hasBooted = true;

    HookLog("TrainingBoot: Starting boot sequence...\n");

    // 1. Get sBattleSetting instance
    typedef uint64_t(__fastcall* GetInstance_t)();
    GetInstance_t getBattleSetting = (GetInstance_t)_addr(BATTLE_SETTING_GET_INSTANCE);
    uint64_t bs = getBattleSetting();
    if (!bs) {
        HookLog("TrainingBoot: FAILED - sBattleSetting instance is null\n");
        g_hasBooted = false;
        return;
    }
    HookLog("TrainingBoot: sBattleSetting = 0x%llx\n", bs);

    // 2. Set battle type to 5 (training mode)
    typedef void(__fastcall* SetInt_t)(uint64_t, int32_t);
    ((SetInt_t)_addr(BATTLE_SETTING_SET_BATTLE_TYPE))(bs, 5);
    HookLog("TrainingBoot: Battle type set to 5 (training)\n");

    // 3. Set stage
    ((SetInt_t)_addr(BATTLE_SETTING_SET_STAGE_VALUE))(bs, g_bootConfig.stageId);

    // 4. Set timer
    ((SetInt_t)_addr(BATTLE_SETTING_SET_TIMER_VALUE))(bs, g_bootConfig.timerValue);

    // 5. Write character IDs for P1 (team 0) and P2 (team 1)
    for (int slot = 0; slot < 3; slot++) {
        WriteCharacterId(bs, 0, slot, g_bootConfig.p1Chars[slot]);
        WriteCharacterId(bs, 1, slot, g_bootConfig.p2Chars[slot]);
    }

    // 6. Commit config
    typedef void(__fastcall* CommitFunc_t)(uint64_t, uint8_t);
    ((CommitFunc_t)_addr(BATTLE_SETTING_COMMIT_ONE_PLAY))(bs, 1);
    HookLog("TrainingBoot: Config committed\n");

    // 7. Get sArea and transition to aNextBattle
    uint64_t sArea = *(uint64_t*)_addr(SAREA_INSTANCE_PTR);
    if (!sArea) {
        HookLog("TrainingBoot: FAILED - sArea instance is null\n");
        g_hasBooted = false;
        return;
    }
    HookLog("TrainingBoot: sArea = 0x%llx\n", sArea);

    // sArea vtable + 0x50 = setArea(DTI*)
    typedef void(__fastcall* SetArea_t)(uint64_t, void*);
    SetArea_t setArea = (SetArea_t)(*(uint64_t*)(*(uint64_t*)sArea + 0x50));
    setArea(sArea, (void*)_addr(ANEXTBATTLE_DTI));

    HookLog("TrainingBoot: Area transition to aNextBattle complete!\n");
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
