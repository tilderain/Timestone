#include "../pch.h"

#include "rollback.h"
#include "mk10utils.h"
#include "netplay.h"

extern void HookLog(const char* fmt, ...);

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
