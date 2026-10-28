#include "../pch.h"
#include "diag.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../kiero/minhook/include/MinHook.h"

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t ADDR_INPUT_MASK_POST_PROCESS = 0x14002E5A0;
static void(__fastcall* oInputMaskPostProcess)(uint64_t cChrInput) = nullptr;

/// @brief Move this
/// @param cChrInput  Move this
/// @return  Move this
static void __fastcall hkInputMaskPostProcess(uint64_t cChrInput)
{
    // Fix the end of match fade causing a permanent black screen
    uint32_t* gameFlags = nullptr;
    bool restoreFadeFlag = false;
    if (Netplay_IsActive()) {
        auto getAction = (uint64_t(__fastcall*)())_addr(0x140001A90);
        const uint64_t action = getAction();
        if (action && *(int32_t*)(action + 0xD8) == 7) {
            gameFlags = (uint32_t*)(action + 0xC4);
            restoreFadeFlag = (*gameFlags & 0x800000) != 0;
            *gameFlags &= ~0x800000u;
        }
    }

    oInputMaskPostProcess(cChrInput);

    if (restoreFadeFlag)
        *gameFlags |= 0x800000;
}

void Diag_InitHooks()
{
    MH_STATUS s;

    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_INPUT_MASK_POST_PROCESS),
        (LPVOID)&hkInputMaskPostProcess,
        (LPVOID*)&oInputMaskPostProcess
    );
    if (s == MH_OK)
    {
        MH_EnableHook((LPVOID)_addr(ADDR_INPUT_MASK_POST_PROCESS));
    }
    else
        HookLog("Diag: Failed to hook inputMaskPostProcess (%d)\n", s);
}
