#include "../pch.h"

#include "rollback.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../kiero/minhook/include/MinHook.h"

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_IS_ONLINE_BATTLE = 0x140006AF0;
static constexpr uintptr_t ONLINE_CALLER_RETURNS[] = {
    0x14003F083, // cChr__resolveActionTransitionState
    0x14003F251, // cChr__resolveAdvancedGuardTransition
    0x14004026A, // cChr__resolveDamageReactionTransition
    0x1400409D6, // cChr__tryStartThrowTransition
    0x140040F41, // cChr__tryStartVariableCounterTransition
    0x1400417BF, // cChr__resolveDamageReactionAction
    0x140048CBD, // cChr__populateHitReactionResult
    0x140203869, // aAction__init deterministic setup
};
using IsOnlineBattleFn = bool(__fastcall*)(uint64_t battleSetting);
static IsOnlineBattleFn g_originalIsOnlineBattle = nullptr;

static bool __fastcall hkIsOnlineBattle(uint64_t battleSetting)
{
    if (Netplay_IsActive()) {
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        for (uintptr_t address : ONLINE_CALLER_RETURNS) {
            if (caller == (uintptr_t)_addr(address))
                return true;
        }
    }

    return g_originalIsOnlineBattle && g_originalIsOnlineBattle(battleSetting);
}

void Rollback_InitOnlineBattleHook()
{
    MH_STATUS status = MH_CreateHook(
        (LPVOID)_addr(FN_IS_ONLINE_BATTLE),
        (LPVOID)&hkIsOnlineBattle,
        (LPVOID*)&g_originalIsOnlineBattle);
    if (status != MH_OK) {
        HookLog("Netplay: failed to hook g__isOnlineBattle (%d)\n", status);
        return;
    }

    status = MH_EnableHook((LPVOID)_addr(FN_IS_ONLINE_BATTLE));
    if (status == MH_OK)
        HookLog("Netplay: enabled scoped native online combat behavior\n");
    else
        HookLog("Netplay: failed to enable g__isOnlineBattle hook (%d)\n", status);
}

bool Rollback_PatchNativeNetplayDeterminism()
{
    // aAction__init normally reaches its serialization setup only when replay
    // playback or g__isOnlineBattle is true. Custom ENet play intentionally
    // keeps an offline BattleType, so force only this branch rather than the
    // global online predicate, whose other callers expect native netplay state.
    static constexpr uintptr_t DETERMINISM_BRANCH = 0x14020385f;
    uint8_t* branch = (uint8_t*)_addr(DETERMINISM_BRANCH);
    if (branch[0] == 0xeb && branch[1] == 0x12)
        return true;
    if (branch[0] != 0x75 || branch[1] != 0x12) {
        HookLog("Netplay: native determinism branch has unexpected bytes %02X %02X\n",
                branch[0], branch[1]);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(branch, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLog("Netplay: failed to make native determinism branch writable\n");
        return false;
    }
    branch[0] = 0xeb; // JNZ -> JMP into the native online/replay setup block.
    FlushInstructionCache(GetCurrentProcess(), branch, 1);
    DWORD ignored = 0;
    VirtualProtect(branch, 1, oldProtect, &ignored);

    HookLog("Netplay: forced aAction native online determinism branch\n");
    return true;
}

void Rollback_ApplyNetplayDeterminism()
{
    using SetFrameWaitFn = void(__fastcall*)(uint64_t, bool);
    using SetMoveLineParallelFn = void(__fastcall*)(uint64_t, int, bool);
    using GetCollisionMoveMatrixFn = uint64_t(__fastcall*)(uint64_t);
    using GetInstanceFn = uint64_t(__fastcall*)();
    using SetBoolFn = void(__fastcall*)(uint64_t, bool);

    uint64_t sMain = *(uint64_t*)_addr(0x140e177e8);
    uint64_t sUnit = *(uint64_t*)_addr(0x140e17698);
    uint64_t sCollision = *(uint64_t*)_addr(0x140e18250);
    uint64_t sHitSolver = *(uint64_t*)_addr(0x140d44060);
    uint64_t battleRuntime = ((GetInstanceFn)_addr(0x140004730))();

    if (sMain)
        ((SetFrameWaitFn)_addr(0x140522420))(sMain, true);
    if (sUnit) {
        auto setMoveLineParallel = (SetMoveLineParallelFn)_addr(0x140207b10);
        for (int category = 0; category < 64; ++category)
            setMoveLineParallel(sUnit, category, false);
    }
    if (sCollision) {
        uint64_t moveMatrix = ((GetCollisionMoveMatrixFn)_addr(0x1402033d0))(sCollision);
        if (moveMatrix)
            ((SetBoolFn)_addr(0x1405b7bf0))(moveMatrix, false);
    }
    if (sHitSolver)
        ((SetBoolFn)_addr(0x140207660))(sHitSolver, false);
    if (battleRuntime)
        ((SetBoolFn)_addr(0x140207c30))(battleRuntime, false);

    HookLog("Netplay: applied native gameplay parallelism settings\n");
}
