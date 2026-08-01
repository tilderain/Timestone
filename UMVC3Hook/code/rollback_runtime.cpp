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

template <size_t N>
static bool PatchBytes(uintptr_t address, const uint8_t (&expected)[N],
                       const uint8_t (&replacement)[N])
{
    uint8_t* target = (uint8_t*)_addr(address);
    if (memcmp(target, replacement, N) == 0)
        return true;
    if (memcmp(target, expected, N) != 0)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, N, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    memcpy(target, replacement, N);
    FlushInstructionCache(GetCurrentProcess(), target, N);
    DWORD ignored = 0;
    VirtualProtect(target, N, oldProtect, &ignored);
    return true;
}

void Rollback_PatchRenderCommandAllocationFailure()
{
    // These native paths all continue through null arena allocations. Redirect
    // them to their existing failure/cleanup paths instead.
    static constexpr uint8_t NULL_COMMAND[] = {0x0F, 0x84, 0x09, 0x01, 0x00, 0x00};
    static constexpr uint8_t RETURN_FAILURE[] = {0x0F, 0x84, 0x9D, 0xFF, 0xFF, 0xFF};
    static constexpr uint8_t NULL_BUCKET[] = {
        0x74, 0x0C, 0x48, 0x05, 0x00, 0x10, 0x00, 0x00, 0x48,
        0x89, 0x43, 0x30, 0xEB, 0x04, 0x48, 0x89, 0x73, 0x30,
    };
    static constexpr uint8_t CLEANUP_10[] = {
        0x0F, 0x84, 0x40, 0x02, 0x00, 0x00, 0x48, 0x05, 0x00,
        0x10, 0x00, 0x00, 0x48, 0x89, 0x43, 0x30, 0x90, 0x90,
    };
    static constexpr uint8_t CLEANUP_40[] = {
        0x0F, 0x84, 0x74, 0x01, 0x00, 0x00, 0x48, 0x05, 0x00,
        0x10, 0x00, 0x00, 0x48, 0x89, 0x43, 0x30, 0x90, 0x90,
    };
    static constexpr uint8_t CLEANUP_30[] = {
        0x0F, 0x84, 0xD9, 0x00, 0x00, 0x00, 0x48, 0x05, 0x00,
        0x10, 0x00, 0x00, 0x48, 0x89, 0x43, 0x30, 0x90, 0x90,
    };
    static constexpr uint8_t TYPE_3[] = {0x0F, 0x85, 0x6B, 0x02, 0x00, 0x00};
    static constexpr uint8_t SKIP_TYPE_3[] = {0x0F, 0x85, 0xC0, 0x02, 0x00, 0x00};
    static constexpr uint8_t CALL_DTOR[] = {0xFF, 0x50, 0x28};
    static constexpr uint8_t SKIP_DTOR[] = {0x90, 0x90, 0x90};

    bool ok = PatchBytes(0x140640707, NULL_COMMAND, RETURN_FAILURE) &&
              PatchBytes(0x14063C6AF, TYPE_3, SKIP_TYPE_3) &&
              PatchBytes(0x14063C755, NULL_BUCKET, CLEANUP_10) &&
              PatchBytes(0x14063C821, NULL_BUCKET, CLEANUP_40) &&
              PatchBytes(0x14063C8BC, NULL_BUCKET, CLEANUP_30) &&
              PatchBytes(0x14055FC1A, CALL_DTOR, SKIP_DTOR);
    HookLog("Render: %s native failure guards\n", ok ? "installed" : "failed to install");
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
