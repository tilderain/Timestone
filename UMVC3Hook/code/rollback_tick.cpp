#include "../pch.h"

#include "rollback.h"
#include "rollback_tick.h"
#include "mk10utils.h"

#include <cstring>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_UPDATE_MANAGERS       = 0x140258540;
static constexpr uintptr_t FN_PAD_PROCESS           = 0x140289c30;
static constexpr uintptr_t FN_MVC3MGR_GETINSTANCE   = 0x140001af0;
static constexpr uintptr_t FN_MVC3MGR_GETACTION     = 0x1409629d0;
static constexpr uintptr_t FN_AFTER_GETACTION       = 0x14014f410;
static constexpr uintptr_t FN_COLLISION_BEGIN_FRAME = 0x1405bb710;
static constexpr uintptr_t FN_CALL_AI               = 0x140647910;
static constexpr uintptr_t FN_UPDATE_RENDER_STATE   = 0x140259ea0;
static constexpr uintptr_t FN_RESET_HITNODES        = 0x140259e90;
static constexpr uintptr_t FN_PAD_METHOD_3D00       = 0x1402a3d00;

static constexpr uintptr_t FN_GETGAMECONFIG = 0x1401bcc50;
static constexpr uintptr_t FN_GETGAMEUI     = 0x14097d370;
static constexpr uintptr_t FN_GETSHOT       = 0x140683730;
static constexpr uintptr_t FN_GETGAMEEFFECT = 0x140015690;
static constexpr uintptr_t FN_GETHITSOLVER  = 0x140056130;
static constexpr uintptr_t FN_GETCHARACTER  = 0x1407bcd80;
static constexpr uintptr_t FN_GETSTAGE      = 0x140259460;
static constexpr uintptr_t FN_GETREPLAY     = 0x14013c430;
static constexpr uintptr_t FN_GETBATTLESET  = 0x1408f1f10;
static constexpr uintptr_t FN_GETMVC3SET    = 0x14004b000;
static constexpr uintptr_t FN_GETNETWORK    = 0x1407cda50;
static constexpr uintptr_t FN_GETLICENSE    = 0x14010a200;
static constexpr uintptr_t FN_GETRANKING    = 0x140703370;
static constexpr uintptr_t FN_GETNETROOM    = 0x14030e6c0;
static constexpr uintptr_t FN_GETEARTHATK   = 0x140327a10;

static constexpr uintptr_t SLEEP_CALL_FIXED  = 0x1405220b2;
static constexpr uintptr_t SLEEP_CALL_VAR    = 0x140521f93;
static constexpr uintptr_t PRESENT_CALL_SITE = 0x14053709c;
static constexpr uintptr_t SWAPCHAIN_CALL_A  = 0x1402598c4;
static constexpr uintptr_t SWAPCHAIN_CALL_B  = 0x1402598d1;
static constexpr uintptr_t SWAPCHAIN_CALL_C  = 0x140537113;

static uint8_t g_savedSleep[2][6] = {};
static uint8_t g_savedPresent[3] = {};
static uint8_t g_savedSwapchain[3][3] = {};
static bool g_sleepPatched = false;
static bool g_presentPatched = false;
static bool g_swapchainPatched = false;

static void VTableCall(uint64_t sMain, uint32_t memberOffset, uint32_t vtableOffset)
{
    uint64_t object = *(uint64_t*)(sMain + memberOffset);
    if (!object) return;
    uint64_t vtable = *(uint64_t*)object;
    if (!vtable) return;
    auto function = *(void(__fastcall**)(uint64_t))(vtable + vtableOffset);
    if (function) function(object);
}

static void DirectMemberCall(uint64_t sMain, uint32_t memberOffset, uintptr_t functionAddress)
{
    uint64_t object = *(uint64_t*)(sMain + memberOffset);
    if (object)
        ((void(__fastcall*)(uint64_t))_addr(functionAddress))(object);
}

static void MgrSubsysCall(uintptr_t getterAddress, uint32_t vtableOffset)
{
    uint64_t manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    if (!manager) return;
    uint64_t object = ((uint64_t(__fastcall*)(uint64_t))_addr(getterAddress))(manager);
    if (!object) return;
    uint64_t vtable = *(uint64_t*)object;
    if (!vtable) return;
    auto function = *(void(__fastcall**)(uint64_t))(vtable + vtableOffset);
    if (function) function(object);
}

void Rollback_ResyncMainTiming(uint64_t sMain)
{
    if (!sMain) return;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    *(uint64_t*)(sMain + 0x40) = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40048) = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x40078) = qpc.QuadPart;
    *(uint64_t*)(sMain + 0x70) = qpc.QuadPart;
    *(uint32_t*)(sMain + 0x38) = 16;
    *(float*)(sMain + 0x80) = 1.0f;
    *(uint64_t*)(sMain + 0x60) = 0;
    *(uint64_t*)(sMain + 0x68) = 0;
    *(uint32_t*)(sMain + 0x48) = 1;
}

void Rollback_PatchSleep(bool enable)
{
    static constexpr uintptr_t sites[] = {SLEEP_CALL_FIXED, SLEEP_CALL_VAR};
    if (enable == g_sleepPatched) return;

    for (int i = 0; i < 2; ++i) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(sites[i]), 6, PAGE_EXECUTE_READWRITE, &old);
        if (enable) {
            memcpy(g_savedSleep[i], (void*)_addr(sites[i]), 6);
            memset((void*)_addr(sites[i]), 0x90, 6);
        } else {
            memcpy((void*)_addr(sites[i]), g_savedSleep[i], 6);
        }
        VirtualProtect((LPVOID)_addr(sites[i]), 6, old, &old);
    }
    g_sleepPatched = enable;
}

void Rollback_PatchPresent(bool enable)
{
    if (enable == g_presentPatched) return;

    DWORD old = 0;
    VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, PAGE_EXECUTE_READWRITE, &old);
    if (enable) {
        memcpy(g_savedPresent, (void*)_addr(PRESENT_CALL_SITE), 3);
        memset((void*)_addr(PRESENT_CALL_SITE), 0x90, 3);
    } else {
        memcpy((void*)_addr(PRESENT_CALL_SITE), g_savedPresent, 3);
    }
    VirtualProtect((LPVOID)_addr(PRESENT_CALL_SITE), 3, old, &old);
    g_presentPatched = enable;
}

void Rollback_PatchSwapchain(bool enable)
{
    static constexpr uintptr_t sites[] = {
        SWAPCHAIN_CALL_A, SWAPCHAIN_CALL_B, SWAPCHAIN_CALL_C,
    };
    if (enable == g_swapchainPatched) return;

    for (int i = 0; i < 3; ++i) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(sites[i]), 3, PAGE_EXECUTE_READWRITE, &old);
        if (enable) {
            memcpy(g_savedSwapchain[i], (void*)_addr(sites[i]), 3);
            memset((void*)_addr(sites[i]), 0x90, 3);
        } else {
            memcpy((void*)_addr(sites[i]), g_savedSwapchain[i], 3);
        }
        VirtualProtect((LPVOID)_addr(sites[i]), 3, old, &old);
    }
    g_swapchainPatched = enable;
}

void Rollback_PatchDrawCalls(bool enable)
{
    static constexpr struct Site { uintptr_t address; int size; } sites[] = {
        {0x1402594d9, 3}, {0x1402594e6, 3}, {0x1402594f3, 3},
        {0x140259515, 3}, {0x140259568, 3}, {0x140259575, 3},
        {0x140259896, 3}, {0x140259525, 5}, {0x140259537, 5},
        {0x140259548, 5}, {0x140259559, 5}, {0x1402598ab, 3},
    };
    static uint8_t saved[sizeof(sites) / sizeof(sites[0])][6] = {};
    static bool patched = false;
    if (enable == patched) return;

    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        DWORD old = 0;
        VirtualProtect((LPVOID)_addr(sites[i].address), sites[i].size,
                       PAGE_EXECUTE_READWRITE, &old);
        if (enable) {
            memcpy(saved[i], (void*)_addr(sites[i].address), sites[i].size);
            memset((void*)_addr(sites[i].address), 0x90, sites[i].size);
        } else {
            memcpy((void*)_addr(sites[i].address), saved[i], sites[i].size);
        }
        VirtualProtect((LPVOID)_addr(sites[i].address), sites[i].size, old, &old);
    }
    patched = enable;
}


//currently unused
/*
void Rollback_HeadlessTick(uint64_t sMain)
{
    if (!sMain) {
        HookLog("Rollbackheadlesstick: smain is bad?:? :>\n");
        return;
    }

    HookLog("Rollback_HeadlessTick: begin\n");

    // Skip sMain__move because sampling real QPC during re-simulation corrupts
    // frame pacing. Game logic still needs to observe one valid tick.
    HookLog("Rollback_HeadlessTick: Phase1 step1 - Frame timing (skipping sMain__move)\n");
    *(uint32_t*)(sMain + 0x48) = 1;

    HookLog("Rollback_HeadlessTick: Phase1 step2 - Window-focus/manager state\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_UPDATE_MANAGERS))(sMain);

    HookLog("Rollback_HeadlessTick: Phase1 step3 - Input processing\n");
    uint64_t syncPad = *(uint64_t*)(sMain + 0x401c0);
    if (syncPad)
        ((void(__fastcall*)(uint64_t))_addr(FN_PAD_PROCESS))(syncPad);

    HookLog("Rollback_HeadlessTick: Phase1 step4 - Async loader update\n");
    VTableCall(sMain, 0x40260, 0x30);

    // Draw calls, collision deferred-state draining, render setup, and Present
    // are intentionally excluded from the headless pipeline.
    HookLog("Rollback_HeadlessTick: Phase2 begin - Post-render game-logic updates\n");
    VTableCall(sMain, 0x40200, 0x30);

    HookLog("Rollback_HeadlessTick: Phase2 step6 - sMvc3Manager singleton chain\n");
    {
        uint64_t manager =
            ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (manager) {
            uint64_t action =
                ((uint64_t(__fastcall*)(uint64_t))_addr(FN_MVC3MGR_GETACTION))(manager);
            if (action)
                ((void(__fastcall*)(uint64_t))_addr(FN_AFTER_GETACTION))(action);
        }
    }

    HookLog("Rollback_HeadlessTick: Phase2 step7 - Post-render vtable+0x30 updates\n");
    VTableCall(sMain, 0x40230, 0x30);
    VTableCall(sMain, 0x401c0, 0x30);
    VTableCall(sMain, 0x40210, 0x30);
    VTableCall(sMain, 0x40228, 0x30);
    VTableCall(sMain, 0x40218, 0x30);
    VTableCall(sMain, 0x401c0, 0x50);
    VTableCall(sMain, 0x401e8, 0x30);
    VTableCall(sMain, 0x40240, 0x30);

    HookLog("Rollback_HeadlessTick: Phase2 - Manager subsystems\n");
    MgrSubsysCall(FN_GETGAMECONFIG, 0x30);
    // sGameUI presentation and allocator-owned resources stay on the live timeline.
    VTableCall(sMain, 0x401a0, 0x30);
    VTableCall(sMain, 0x401e0, 0x30);
    MgrSubsysCall(FN_GETHITSOLVER, 0x30);
    VTableCall(sMain, 0x401a0, 0x68);
    VTableCall(sMain, 0x401a0, 0x58);

    HookLog("Rollback_HeadlessTick: Phase2 step8 - Direct AI call\n");
    DirectMemberCall(sMain, 0x40240, FN_CALL_AI);

    HookLog("Rollback_HeadlessTick: Phase2 step9 - Remaining vtable+0x30 updates\n");
    VTableCall(sMain, 0x401d8, 0x30);
    VTableCall(sMain, 0x40208, 0x30);
    VTableCall(sMain, 0x401b0, 0x30);
    VTableCall(sMain, 0x401f0, 0x30);
    VTableCall(sMain, 0x40248, 0x30);
    VTableCall(sMain, 0x40258, 0x30);

    HookLog("Rollback_HeadlessTick: Phase3 begin - Manager subsystem update loop\n");
    HookLog("Rollback_HeadlessTick: Phase3 - sMvc3Manager vtable+0x30\n");
    {
        uint64_t manager =
            ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (manager) {
            uint64_t vtable = *(uint64_t*)manager;
            if (vtable) {
                auto function = *(void(__fastcall**)(uint64_t))(vtable + 0x30);
                if (function) function(manager);
            }
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 - Character/Stage/Shot/GameEffect vtable+0x30\n");
    MgrSubsysCall(FN_GETCHARACTER, 0x30);
    MgrSubsysCall(FN_GETSTAGE, 0x30);
    MgrSubsysCall(FN_GETSHOT, 0x30);
    MgrSubsysCall(FN_GETGAMEEFFECT, 0x30);

    HookLog("Rollback_HeadlessTick: Phase3 - Hit solver reset\n");
    {
        uint64_t manager =
            ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
        if (manager) {
            uint64_t hitSolver =
                ((uint64_t(__fastcall*)(uint64_t))_addr(FN_GETHITSOLVER))(manager);
            if (hitSolver)
                ((void(__fastcall*)(uint64_t))_addr(FN_RESET_HITNODES))(hitSolver);
        }
    }

    HookLog("Rollback_HeadlessTick: Phase3 - Replay/Battle/Network/License vtable+0x30\n");
    MgrSubsysCall(FN_GETREPLAY, 0x30);
    // 0x140259270 is a ConcRT VirtualProcessor method, not a manager getter.
    MgrSubsysCall(FN_GETBATTLESET, 0x30);
    MgrSubsysCall(FN_GETMVC3SET, 0x30);
    MgrSubsysCall(FN_GETNETWORK, 0x30);
    MgrSubsysCall(FN_GETLICENSE, 0x30);

    HookLog("Rollback_HeadlessTick: Phase3 - Remaining member vtable+0x30\n");
    VTableCall(sMain, 0x401b8, 0x30);
    VTableCall(sMain, 0x401c8, 0x30);
    VTableCall(sMain, 0x401d0, 0x30);

    HookLog("Rollback_HeadlessTick: Phase3 - Ranking/NetRoom/EarthAtk vtable+0x30\n");
    MgrSubsysCall(FN_GETRANKING, 0x30);
    MgrSubsysCall(FN_GETNETROOM, 0x30);
    MgrSubsysCall(FN_GETEARTHATK, 0x30);

    HookLog("Rollback_HeadlessTick: Phase4 begin - Tail + finalize\n");
    VTableCall(sMain, 0x40198, 0x30);
    VTableCall(sMain, 0x40270, 0x30);

    HookLog("Rollback_HeadlessTick: Phase4 - sMain tail\n");
    ((void(__fastcall*)(uint64_t))_addr(FN_UPDATE_RENDER_STATE))(sMain);

    HookLog("Rollback_HeadlessTick: Phase4 - Final vtable+0x30\n");
    VTableCall(sMain, 0x401f8, 0x30);

    HookLog("Rollback_HeadlessTick: Phase5 begin - Pad methods\n");
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad) {
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))
                _addr(FN_PAD_METHOD_3D00))(pad, 0xa4, 1);
        }
    }

    HookLog("Rollback_HeadlessTick: Phase5 - Pad method 0xa5\n");
    {
        uint64_t pad = ((uint64_t(__fastcall*)())_addr(0x140001b00))();
        if (pad) {
            ((uint8_t(__fastcall*)(uint64_t, uint32_t, uint8_t))
                _addr(FN_PAD_METHOD_3D00))(pad, 0xa5, 1);
        }
    }
    // The native Alt+F4 exit check is deliberately skipped during re-simulation.

    HookLog("Rollback_HeadlessTick: end\n");
}*/
