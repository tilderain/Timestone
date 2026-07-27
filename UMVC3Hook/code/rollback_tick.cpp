#include "../pch.h"

#include "rollback.h"
#include "rollback_profile.h"
#include "rollback_tick.h"
#include "mk10utils.h"

#include <cstring>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_SMAIN_MOVE              = 0x140521df0;
static constexpr uintptr_t FN_UPDATE_WINDOW_FOCUS     = 0x140258540;
static constexpr uintptr_t FN_NETPAD_PROCESS_INPUT    = 0x140289c30;
static constexpr uintptr_t FN_COLLISION_BEGIN_FRAME   = 0x1405bb710;
static constexpr uintptr_t FN_MVC3MGR_GETINSTANCE     = 0x140001af0;
static constexpr uintptr_t FN_GET_GAME_CONFIG         = 0x1401bcc50;
static constexpr uintptr_t FN_RENDER_GETINSTANCE      = 0x1400047a0;
static constexpr uintptr_t FN_RENDER_SET_ASPECT       = 0x14053de60;
static constexpr uintptr_t FN_RENDER_GET_VIEWPORT     = 0x14020c7d0;
static constexpr uintptr_t FN_PRIMITIVE_GETINSTANCE   = 0x1401e0ca0;
static constexpr uintptr_t FN_PRIMITIVE_SET_WIDTH     = 0x140932790;
static constexpr uintptr_t FN_PRIMITIVE_SET_HEIGHT    = 0x140447110;
static constexpr uintptr_t FN_RENDER_BEGIN_FRAME      = 0x140537080;
static constexpr uintptr_t FN_MVC3MGR_GETACTION       = 0x1409629d0;
static constexpr uintptr_t FN_ACTION_SUBSTEP          = 0x14014f410;
static constexpr uintptr_t FN_GET_GAME_UI             = 0x14097d370;
static constexpr uintptr_t FN_GET_SHOT                = 0x140683730;
static constexpr uintptr_t FN_GET_GAME_EFFECT         = 0x140015690;
static constexpr uintptr_t FN_GET_HIT_SOLVER          = 0x140056130;
static constexpr uintptr_t FN_AI_SUBSTEP              = 0x140647910;
static constexpr uintptr_t FN_GET_CHARACTER           = 0x1407bcd80;
static constexpr uintptr_t FN_GET_STAGE               = 0x140259460;
static constexpr uintptr_t FN_RESET_HIT_NODES         = 0x140259e90;
static constexpr uintptr_t FN_GET_REPLAY              = 0x14013c430;
static constexpr uintptr_t FN_GET_GSOUND              = 0x140259270;
static constexpr uintptr_t FN_GET_BATTLE_SETTING      = 0x1408f1f10;
static constexpr uintptr_t FN_GET_MVC3_SETTING        = 0x14004b000;
static constexpr uintptr_t FN_GET_NETWORK             = 0x1407cda50;
static constexpr uintptr_t FN_GET_LICENSE             = 0x14010a200;
static constexpr uintptr_t FN_GET_RANKING             = 0x140703370;
static constexpr uintptr_t FN_GET_NETWORK_ROOM        = 0x14030e6c0;
static constexpr uintptr_t FN_GET_EARTH_ATTACK        = 0x140327a10;
static constexpr uintptr_t FN_UPDATE_RENDER_STATE     = 0x140259ea0;
static constexpr uintptr_t FN_RENDER_END_FRAME        = 0x14053a250;
static constexpr uintptr_t FN_PAD_GETINSTANCE         = 0x140001b00;
static constexpr uintptr_t FN_PAD_KEY_HELD            = 0x1402b3d00;
static constexpr uintptr_t FN_PAD_KEY_TRIGGERED       = 0x1402b3dc0;
static constexpr uintptr_t FN_SMAIN_GETINSTANCE       = 0x140003150;
static constexpr uintptr_t FN_SMAIN_EXIT              = 0x1405213a0;

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

using ObjectFn = void(__fastcall*)(uint64_t);
using GetterFn = uint64_t(__fastcall*)(uint64_t);

static void VTableCall(uint64_t object, uint32_t vtableOffset)
{
    uint64_t vtable = *(uint64_t*)object;
    (*(ObjectFn*)(vtable + vtableOffset))(object);
}

static uint64_t MainMember(uint64_t sMain, uint32_t memberOffset)
{
    return *(uint64_t*)(sMain + memberOffset);
}

static uint64_t ManagerSubsystem(uintptr_t getterAddress)
{
    uint64_t manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    return ((GetterFn)_addr(getterAddress))(manager);
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


void Rollback_HeadlessTick(uint64_t sMain)
{
    RollbackHeadlessProfileTimer profile;

    ((ObjectFn)_addr(FN_SMAIN_MOVE))(sMain); // Advance base timing, pause, and frame pacing state.
    ((ObjectFn)_addr(FN_UPDATE_WINDOW_FOCUS))(sMain); // Refresh iconified, foreground, and focus flags.
    ((ObjectFn)_addr(FN_NETPAD_PROCESS_INPUT))(MainMember(sMain, 0x401c0)); // Poll/process online pad input.
    VTableCall(MainMember(sMain, 0x40260), 0x30); // mpPlatformUtil::move: service platform/async work.

        profile.Add(&RollbackProfile::headlessTimingInput,
        &RollbackProfile::headlessTimingInputMax);

    VTableCall(MainMember(sMain, 0x401a8), 0x48); // mpResource::draw.
    VTableCall(MainMember(sMain, 0x40198), 0x48); // mpTool::draw.
    VTableCall(MainMember(sMain, 0x401d8), 0x48); // mpEffect::draw.
    ((ObjectFn)_addr(FN_COLLISION_BEGIN_FRAME))(MainMember(sMain, 0x401e0)); // Drain deferred collision state.
    VTableCall(ManagerSubsystem(FN_GET_GAME_CONFIG), 0x48); // sGameConfig::draw.
        profile.Add(&RollbackProfile::headlessEarlyDrawCollision,
        &RollbackProfile::headlessEarlyDrawCollisionMax);

    // Copy the current render viewport dimensions into the primitive renderer.
    uint64_t render = ((uint64_t(__fastcall*)())_addr(FN_RENDER_GETINSTANCE))();
    ((void(__fastcall*)(uint64_t, uint32_t))_addr(FN_RENDER_SET_ASPECT))(render, 2); // Select aspect mode 2.
    uint32_t viewport[2];
    render = ((uint64_t(__fastcall*)())_addr(FN_RENDER_GETINSTANCE))();
    ((void(__fastcall*)(uint64_t, uint32_t*))_addr(FN_RENDER_GET_VIEWPORT))(render, viewport); // Read width and height.
    uint64_t primitive = ((uint64_t(__fastcall*)())_addr(FN_PRIMITIVE_GETINSTANCE))();
    ((void(__fastcall*)(uint64_t, uint32_t))_addr(FN_PRIMITIVE_SET_WIDTH))(primitive, viewport[0]); // Set field +0x40.
    primitive = ((uint64_t(__fastcall*)())_addr(FN_PRIMITIVE_GETINSTANCE))();
    ((void(__fastcall*)(uint64_t, uint32_t))_addr(FN_PRIMITIVE_SET_HEIGHT))(primitive, viewport[1]); // Set field +0x44.

        profile.Add(&RollbackProfile::headlessViewportSetup,
        &RollbackProfile::headlessViewportSetupMax);

    VTableCall(MainMember(sMain, 0x40270), 0x48); // mpPrimitive::draw.
    VTableCall(MainMember(sMain, 0x40208), 0x48); // mpGpuParticle::draw.
    ((ObjectFn)_addr(FN_RENDER_BEGIN_FRAME))(MainMember(sMain, 0x40190)); // Begin/submit the render frame.
    VTableCall(MainMember(sMain, 0x40200), 0x30); // mpArea::move.

        profile.Add(&RollbackProfile::headlessRenderBeginArea,
        &RollbackProfile::headlessRenderBeginAreaMax);

    // Advance the active battle action; this dispatches character frame logic.
    uint64_t manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    uint64_t action = ((GetterFn)_addr(FN_MVC3MGR_GETACTION))(manager); // Resolve the active sAction.
    ((ObjectFn)_addr(FN_ACTION_SUBSTEP))(action); // Run the action substep and character moves.

        profile.Add(&RollbackProfile::headlessAction,
        &RollbackProfile::headlessActionMax);

    VTableCall(MainMember(sMain, 0x40230), 0x30); // mpVibration::move.
    VTableCall(MainMember(sMain, 0x401c0), 0x30); // mpNetPad::move.
    VTableCall(MainMember(sMain, 0x40210), 0x30); // mpMouse::move.
    VTableCall(MainMember(sMain, 0x40228), 0x30); // mpId::move: advance unique-ID allocator state.
    VTableCall(MainMember(sMain, 0x40218), 0x30); // mpKeyboard::move: update held/triggered/released maps.
    VTableCall(MainMember(sMain, 0x401c0), 0x50); // mpNetPad::postFrame.
    
        profile.Add(&RollbackProfile::headlessInputDevices,
        &RollbackProfile::headlessInputDevicesMax);

    VTableCall(MainMember(sMain, 0x401e8), 0x30); // mpShader2::move.
    VTableCall(MainMember(sMain, 0x40240), 0x30); // mpAI::move.
    VTableCall(ManagerSubsystem(FN_GET_GAME_CONFIG), 0x30); // sGameConfig::move.
    VTableCall(ManagerSubsystem(FN_GET_GAME_UI), 0x30); // sGameUi::move.
    VTableCall(ManagerSubsystem(FN_GET_SHOT), 0x48); // sShot::draw.
    VTableCall(ManagerSubsystem(FN_GET_GAME_EFFECT), 0x48); // sGameEffect::draw.

        profile.Add(&RollbackProfile::headlessGameplaySetup,
        &RollbackProfile::headlessGameplaySetupMax);

    VTableCall(MainMember(sMain, 0x401a0), 0x30); // mpUnit::move.

        profile.Add(&RollbackProfile::headlessUnitMove,
        &RollbackProfile::headlessUnitMoveMax);

    VTableCall(MainMember(sMain, 0x401e0), 0x30); // mpCollision base cSystem::move.
    VTableCall(ManagerSubsystem(FN_GET_HIT_SOLVER), 0x30); // sHitSolver::move.

        profile.Add(&RollbackProfile::headlessCollisionHit,
        &RollbackProfile::headlessCollisionHitMax);

    // sUnit vtable slot 13 (0x68): sUnit__save_jobwrap. Begins sync phase 2,
    // iterates every active category, and dispatches that category through
    // sUnit vtable slot 12. The category/node pass performs the per-node
    // save/load transition work and synchronizes any queued unit jobs.
    VTableCall(MainMember(sMain, 0x401a0), 0x68); //REQUIRED
    // sUnit vtable slot 11 (0x58): sUnit__save. Begins sync phase 3 and
    // iterates every active category through sUnit vtable slot 10. This is
    // the normal non-job-wrapper category synchronization pass.
    VTableCall(MainMember(sMain, 0x401a0), 0x58); //REQUIRED

        profile.Add(&RollbackProfile::headlessUnitSync,
        &RollbackProfile::headlessUnitSyncMax);

    ((ObjectFn)_addr(FN_AI_SUBSTEP))(MainMember(sMain, 0x40240)); // Run the AI substep helper.
    VTableCall(MainMember(sMain, 0x401d8), 0x30); // mpEffect::move.
    VTableCall(MainMember(sMain, 0x40208), 0x30); // mpGpuParticle::move.
    VTableCall(MainMember(sMain, 0x401b0), 0x30); // mpCamera::move.
    VTableCall(MainMember(sMain, 0x401f0), 0x30); // mpSound::move.
    VTableCall(MainMember(sMain, 0x40248), 0x30); // mpScene::move.
    VTableCall(MainMember(sMain, 0x40258), 0x30); // mpShadow::move.

        profile.Add(&RollbackProfile::headlessSimulationTail,
        &RollbackProfile::headlessSimulationTailMax);

    manager = ((uint64_t(__fastcall*)())_addr(FN_MVC3MGR_GETINSTANCE))();
    VTableCall(manager, 0x30); // sMvc3Manager::move.
    VTableCall(ManagerSubsystem(FN_GET_CHARACTER), 0x30); // sCharacter::move.
    VTableCall(ManagerSubsystem(FN_GET_STAGE), 0x30); // sStage::move.
    VTableCall(ManagerSubsystem(FN_GET_SHOT), 0x30); // sShot::move.
    VTableCall(ManagerSubsystem(FN_GET_GAME_EFFECT), 0x30); // sGameEffect::move.
    ((ObjectFn)_addr(FN_RESET_HIT_NODES))(ManagerSubsystem(FN_GET_HIT_SOLVER)); // Clear active hit-solver nodes.
    VTableCall(ManagerSubsystem(FN_GET_REPLAY), 0x30); // sReplay::move.
    VTableCall(ManagerSubsystem(FN_GET_GSOUND), 0x30); // sGSound::move.
    VTableCall(ManagerSubsystem(FN_GET_BATTLE_SETTING), 0x30); // sBattleSetting::move.
    VTableCall(ManagerSubsystem(FN_GET_MVC3_SETTING), 0x30); // sMvc3Setting::move.

        profile.Add(&RollbackProfile::headlessManagerCore,
        &RollbackProfile::headlessManagerCoreMax);

    VTableCall(ManagerSubsystem(FN_GET_NETWORK), 0x30); // Manager-owned sNetwork::move.
    VTableCall(ManagerSubsystem(FN_GET_LICENSE), 0x30); // sLicense::move.
    VTableCall(MainMember(sMain, 0x401b8), 0x30); // mpNetwork::move.
    VTableCall(MainMember(sMain, 0x401c8), 0x30); // mpAgent::move.
    VTableCall(MainMember(sMain, 0x401d0), 0x30); // mpGuide::move.
    VTableCall(ManagerSubsystem(FN_GET_RANKING), 0x30); // sRanking::move.
    VTableCall(ManagerSubsystem(FN_GET_NETWORK_ROOM), 0x30); // sNetworkRoom::move.
    VTableCall(ManagerSubsystem(FN_GET_EARTH_ATTACK), 0x30); // sEarthAttack::move.
    VTableCall(MainMember(sMain, 0x40198), 0x30); // mpTool::move.

        profile.Add(&RollbackProfile::headlessNetworkServices,
        &RollbackProfile::headlessNetworkServicesMax);

    VTableCall(MainMember(sMain, 0x40270), 0x30); // mpPrimitive::move.
    VTableCall(MainMember(sMain, 0x401e0), 0x48); // mpCollision base cSystem::draw.
    ((ObjectFn)_addr(FN_UPDATE_RENDER_STATE))(sMain); // Update sMvc3Main render/camera state.
    VTableCall(MainMember(sMain, 0x40190), 0x30); // mpRender::move.
    ((ObjectFn)_addr(FN_RENDER_END_FRAME))(MainMember(sMain, 0x40190)); // End and present the render frame.

        profile.Add(&RollbackProfile::headlessRender,
        &RollbackProfile::headlessRenderMax);

    VTableCall(MainMember(sMain, 0x40208), 0x50); // mpGpuParticle::postFrame.
    VTableCall(MainMember(sMain, 0x40270), 0x50); // mpPrimitive::postFrame.
    VTableCall(MainMember(sMain, 0x401f8), 0x30); // mpCapture::move.

        profile.Add(&RollbackProfile::headlessPostFrame,
        &RollbackProfile::headlessPostFrameMax);

    // Alt+F4 bypasses the ordinary sPad keyboard-input suppression gates.
    using KeyQueryFn = bool(__fastcall*)(uint64_t, uint32_t, bool);
    auto getPad = (uint64_t(__fastcall*)())_addr(FN_PAD_GETINSTANCE);
    bool leftAlt = ((KeyQueryFn)_addr(FN_PAD_KEY_HELD))(getPad(), 0xa4, true); // Query VK_LMENU held.
    bool rightAlt = ((KeyQueryFn)_addr(FN_PAD_KEY_HELD))(getPad(), 0xa5, true); // Query VK_RMENU held.
    if (rightAlt || leftAlt) {
        bool f4 = ((KeyQueryFn)_addr(FN_PAD_KEY_TRIGGERED))(getPad(), 0x73, true); // Query new VK_F4 press.
        if (f4) {
            uint64_t main = ((uint64_t(__fastcall*)())_addr(FN_SMAIN_GETINSTANCE))();
            ((ObjectFn)_addr(FN_SMAIN_EXIT))(main); // Request orderly game shutdown.
        }
    }
        profile.Add(&RollbackProfile::headlessAltF4,
        &RollbackProfile::headlessAltF4Max);
        profile.Finish();
}
