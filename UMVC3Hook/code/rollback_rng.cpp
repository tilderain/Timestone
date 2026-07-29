#include "../pch.h"

#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_rng.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../kiero/minhook/include/MinHook.h"

#include <cstring>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_RNG_RANDOM_INT = 0x14014EB30;
static constexpr uintptr_t FN_HSIENKO_SELECT_ANKI = 0x1400A1260;
static constexpr uintptr_t FN_SET_RESERVED_ANKI = 0x1400D4D90;
static constexpr uintptr_t FN_RCHRCPU_RNG_MOD_N = 0x14010FE50;
static constexpr uintptr_t GLOBAL_RNG_STATE = 0x140D765D8;

using RandomIntRangeFn = int(__fastcall*)(void*, int, int, void*);
using HsienKoSelectAnkiFn = int(__fastcall*)(void*);
using SetReservedAnkiFn = void(__fastcall*)(void*, int);
using RChrCpuRngModNFn = uint32_t(__fastcall*)(void*, uint32_t);

static RandomIntRangeFn oRandomIntRange = nullptr;
static HsienKoSelectAnkiFn oHsienKoSelectAnki = nullptr;
static SetReservedAnkiFn oSetReservedAnki = nullptr;
static RChrCpuRngModNFn oRChrCpuRngModN = nullptr;
static int g_rngTraceFrame = -1;
static int g_rngTraceCount = 0;
static int g_ankiTraceFrame = -1;
static int g_ankiTraceCount = 0;
static int g_ankiReserveTraceFrame = -1;
static int g_ankiReserveTraceCount = 0;

static uint32_t __fastcall hkRChrCpuRngModN(void* cpu, uint32_t modulus)
{
    if (!Netplay_IsActive())
        return oRChrCpuRngModN(cpu, modulus);

    uint32_t savedState[4];
    void* globalState = (void*)_addr(GLOBAL_RNG_STATE);
    std::memcpy(savedState, globalState, sizeof(savedState));
    uint32_t result = oRChrCpuRngModN(cpu, modulus);
    std::memcpy(globalState, savedState, sizeof(savedState));
    return result;
}

static int RngTraceFrame()
{
    uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
    return replay ? *(int32_t*)(replay + 0x54) : -1;
}

static bool IsAnkihouRngCaller(uint64_t caller, void* rngOwner)
{
    if (rngOwner && GetObjectDti((uint64_t)rngOwner) == (uint64_t)_addr(0x140D455F8))
        return true;
    return caller == (uint64_t)_addr(0x1400A1289) ||
           caller == (uint64_t)_addr(0x14018BAF1) ||
           caller == (uint64_t)_addr(0x14018B5C2) ||
           caller == (uint64_t)_addr(0x14018D245 + 5) ||
           caller == (uint64_t)_addr(0x14018D299 + 5);
}

static int __fastcall hkRandomIntRange(void* stateArg, int low, int high,
                                       void* rngOwner)
{
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    if (!IsAnkihouRngCaller(caller, rngOwner))
        return oRandomIntRange(stateArg, low, high, rngOwner);

    int frame = RngTraceFrame();
    if (frame != g_rngTraceFrame) {
        g_rngTraceFrame = frame;
        g_rngTraceCount = 0;
    }

    uint32_t globalBefore[4] = {};
    uint32_t ownerBefore[4] = {};
    std::memcpy(globalBefore, (void*)_addr(0x140D765D8), sizeof(globalBefore));
    if (rngOwner)
        std::memcpy(ownerBefore, (uint8_t*)rngOwner + 0x2D80, sizeof(ownerBefore));

    int result = oRandomIntRange(stateArg, low, high, rngOwner);

    uint32_t globalAfter[4] = {};
    uint32_t ownerAfter[4] = {};
    std::memcpy(globalAfter, (void*)_addr(0x140D765D8), sizeof(globalAfter));
    if (rngOwner)
        std::memcpy(ownerAfter, (uint8_t*)rngOwner + 0x2D80, sizeof(ownerAfter));

    if (g_rngTraceCount++ < 32) {
        HookLog("RNGTRACE frame=%d resim=%d caller=0x%llX stateArg=0x%llX owner=0x%llX "
                "range=[%d,%d] result=%d global=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X "
                "ownerRng(+0x2D80)=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)stateArg,
                (uint64_t)rngOwner, low, high, result,
                globalBefore[0], globalBefore[1], globalBefore[2], globalBefore[3],
                globalAfter[0], globalAfter[1], globalAfter[2], globalAfter[3],
                ownerBefore[0], ownerBefore[1], ownerBefore[2], ownerBefore[3],
                ownerAfter[0], ownerAfter[1], ownerAfter[2], ownerAfter[3]);
    }
    return result;
}

static int __fastcall hkHsienKoSelectAnki(void* character)
{
    if (!character)
        return oHsienKoSelectAnki(character);

    int frame = RngTraceFrame();
    if (frame != g_ankiTraceFrame) {
        g_ankiTraceFrame = frame;
        g_ankiTraceCount = 0;
    }

    uint32_t reservedBefore = *(uint32_t*)((uint8_t*)character + 0x68FC);
    uint32_t commandBefore = *(uint32_t*)((uint8_t*)character + 0x13C4);
    uint32_t actionId = *(uint32_t*)((uint8_t*)character + 0x1310);
    uint32_t rngBefore[4] = {};
    std::memcpy(rngBefore, (uint8_t*)character + 0x2D80, sizeof(rngBefore));

    int result = oHsienKoSelectAnki(character);

    uint32_t rngAfter[4] = {};
    std::memcpy(rngAfter, (uint8_t*)character + 0x2D80, sizeof(rngAfter));
    uint64_t caller = (uint64_t)__builtin_return_address(0);

    if (g_ankiTraceCount++ < 64) {
        HookLog("ANKITRACE frame=%d resim=%d caller=0x%llX char=0x%llX action=0x%08X "
                "reserved=%d commandBefore(+0x13C4)=%08X callbackResult=%d "
                "rng(+0x2D80)=%08X,%08X,%08X,%08X->%08X,%08X,%08X,%08X\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)character,
                actionId, (int32_t)reservedBefore, commandBefore, result,
                rngBefore[0], rngBefore[1], rngBefore[2], rngBefore[3],
                rngAfter[0], rngAfter[1], rngAfter[2], rngAfter[3]);
    }
    return result;
}

static void __fastcall hkSetReservedAnki(void* character, int value)
{
    if (!character)
        return oSetReservedAnki(character, value);

    int frame = RngTraceFrame();
    if (frame != g_ankiReserveTraceFrame) {
        g_ankiReserveTraceFrame = frame;
        g_ankiReserveTraceCount = 0;
    }

    uint32_t before = *(uint32_t*)((uint8_t*)character + 0x68FC);
    uint32_t actionId = *(uint32_t*)((uint8_t*)character + 0x1310);
    uint64_t caller = (uint64_t)__builtin_return_address(0);
    oSetReservedAnki(character, value);

    if (g_ankiReserveTraceCount++ < 32) {
        HookLog("ANKIRESERVE frame=%d resim=%d caller=0x%llX object=0x%llX "
                "action=0x%08X before=%d after=%d\n",
                frame, g_isResimulating ? 1 : 0, caller, (uint64_t)character,
                actionId, (int32_t)before, value);
    }
}

void Rollback_InitRngHooks()
{
    MH_STATUS status = MH_CreateHook((LPVOID)_addr(FN_RCHRCPU_RNG_MOD_N),
                                      (LPVOID)&hkRChrCpuRngModN,
                                      (LPVOID*)&oRChrCpuRngModN);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)_addr(FN_RCHRCPU_RNG_MOD_N));
        if (status == MH_OK)
            HookLog("Rollback: disabled CPU global RNG advancement during netplay\n");
        else
            HookLog("Rollback: failed to enable CPU RNG hook (%d)\n", status);
    } else {
        HookLog("Rollback: failed to hook CPU RNG dispatch (%d)\n", status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_RNG_RANDOM_INT),
                                     (LPVOID)&hkRandomIntRange,
                                     (LPVOID*)&oRandomIntRange);
    if (status == MH_OK) {
        // Intentionally left disabled; this hook is diagnostic-only.
        HookLog("Rollback: hooked Ankihou RNG trace at 0x%llX\n",
                (uint64_t)_addr(FN_RNG_RANDOM_INT));
    } else {
        HookLog("Rollback: failed to hook Ankihou RNG trace (%d)\n", status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_HSIENKO_SELECT_ANKI),
                           (LPVOID)&hkHsienKoSelectAnki,
                           (LPVOID*)&oHsienKoSelectAnki);
    if (status == MH_OK) {
        // Intentionally left disabled; this hook is diagnostic-only.
        HookLog("Rollback: hooked Hsien-Ko Anki selector trace at 0x%llX\n",
                (uint64_t)_addr(FN_HSIENKO_SELECT_ANKI));
    } else {
        HookLog("Rollback: failed to hook Hsien-Ko Anki selector trace (%d)\n", status);
    }

    status = MH_CreateHook((LPVOID)_addr(FN_SET_RESERVED_ANKI),
                           (LPVOID)&hkSetReservedAnki,
                           (LPVOID*)&oSetReservedAnki);
    if (status == MH_OK) {
        // Intentionally left disabled; this hook is diagnostic-only.
        HookLog("Rollback: hooked Hsien-Ko Anki reserve trace at 0x%llX\n",
                (uint64_t)_addr(FN_SET_RESERVED_ANKI));
    } else {
        HookLog("Rollback: failed to hook Hsien-Ko Anki reserve trace (%d)\n", status);
    }
}
