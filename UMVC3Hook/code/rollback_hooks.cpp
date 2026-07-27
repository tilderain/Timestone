#include "../pch.h"

#include "rollback_hooks.h"

#include "controller.h"
#include "mk10utils.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_profile.h"
#include "rollback_ndjson.h"
#include "../kiero/minhook/include/MinHook.h"

#include <cmath>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_SMVC3MAIN_MOVE = 0x1402594A0;
static constexpr uintptr_t FN_CCHR_SET_POS = 0x140002AF0;
static constexpr uintptr_t FN_RENDER_END_FRAME = 0x14053A250;
static constexpr uintptr_t FN_SSOUND_MOVE = 0x1405CC040;
static constexpr uintptr_t FN_SGAMEUI_MOVE = 0x1402BE610;
static constexpr uintptr_t FN_UBTGAUGELIFE_MOVE = 0x1402F6800;
static constexpr uintptr_t FN_SCHEDULER_REBUILD = 0x14050F010;
static constexpr uintptr_t FN_RCHRCPU_CLASS_FUNC = 0x140117280;
static constexpr uintptr_t FN_SMAIN_MOVE = 0x140521DF0;
static constexpr uintptr_t FN_APPLY_COLLISION_PUSH = 0x14004AD20;

static RollbackMainMoveFn* g_originalMvc3MainMove = nullptr;
static void(__fastcall* oCallSRender)(uint64_t) = nullptr;
static void(__fastcall* oMainMove)(uint64_t) = nullptr;
static void(__fastcall* oSoundMove)(uint64_t) = nullptr;
static void(__fastcall* oGameUiMove)(uint64_t) = nullptr;
static void(__fastcall* oBtGaugeLifeMove)(uint64_t) = nullptr;
static void(__fastcall* oSchedulerRebuild)(uint64_t) = nullptr;
static bool(__fastcall* oRChrCpuClassFunc)(void*, void*) = nullptr;
static void(__fastcall* oApplyCollisionPushCorrection)(
    uint64_t, int, uint64_t, uint64_t, uint64_t, uint32_t) = nullptr;

using CChrSetPosFn = void(__fastcall*)(uint64_t, const float*);
static CChrSetPosFn oCChrSetPos = nullptr;

extern "C" int32_t Controller_GetNetFrameCount();
extern "C" bool Controller_IsRollbackEnabled();
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" void Controller_ResolveRemoteInput(int32_t frame);
extern bool g_multiResimEnabled;

static void EnableHook(uintptr_t address, void* replacement, void** original,
                       const char* success, const char* failure)
{
    MH_STATUS status = MH_CreateHook((LPVOID)_addr(address), replacement, original);
    if (status == MH_OK) {
        MH_EnableHook((LPVOID)_addr(address));
        HookLog("%s\n", success);
    } else {
        HookLog(failure, status);
    }
}

static void __fastcall hkCChrSetPos(uint64_t character, const float* position)
{
    if (g_isResimulating && character && position) {
        const float* oldPosition = (const float*)(character + 0x50);
        float deltaX = position[0] - oldPosition[0];
        if (std::isfinite(deltaX) && std::fabs(deltaX) >= 0.001f) {
            HookLog("[ResimSetPos] frame=%d chr=0x%llX caller=0x%llX "
                    "old=[%.9g,%.9g,%.9g] src=[%.9g,%.9g,%.9g] dx=%.9g\n",
                    g_resimulateCurrentFrame, character,
                    (uint64_t)__builtin_return_address(0),
                    oldPosition[0], oldPosition[1], oldPosition[2],
                    position[0], position[1], position[2], deltaX);
        }
    }
    oCChrSetPos(character, position);
}

static void __fastcall hkMvc3MainMove(uint64_t main)
{
    Rollback_SetMainInstance(main);

    int32_t curFrame = Controller_GetNetFrameCount();
    if (g_restoreRequestedFrame >= 0) {
        int32_t restoreFrame = g_restoreRequestedFrame;
        g_restoreRequestedFrame = -1;
        HookLog("Rollback: executing deferred restore to frame %d\n", restoreFrame);
        Rollback_RestoreSnapshot(restoreFrame);
    }

    if (g_f7State == 1) {
        g_f7StartFrame = Controller_GetNetFrameCount();
        Rollback_SaveSnapshot(g_f7StartFrame);
        g_f7DiagSession = NDJSON_BeginSession(true);
        g_f7State = 2;
        HookLog("F7: snapshot saved at frame %d, recording %d frames\n",
                g_f7StartFrame, g_f7Remaining);
    }
    if (g_f7State == 2 && --g_f7Remaining <= 0) {
        if (g_f7DiagSession >= 0)
            NDJSON_DumpFrame(curFrame, 0);
        g_f7State = 0;
        HookLog("F7: %d frames recorded, restoring frame %d and re-simulating\n",
                8, g_f7StartFrame);
        Rollback_ReSimulate(g_f7StartFrame, g_f7StartFrame + 8);
        g_f7DiagSession = -1;
        int32_t updatedFrame = Controller_GetNetFrameCount();
        if (updatedFrame >= 0) {
            Rollback_SaveSnapshot(updatedFrame);
            g_postResimFrame = updatedFrame;
        }
    }

    if (Netplay_IsActive() && FrameStepper_IsBattleSynchronized() &&
        !g_isResimulating && Controller_IsRollbackEnabled()) {
        using GetActionFn = uint64_t(__fastcall*)();
        static GetActionFn getAction = (GetActionFn)_addr(0x140001A90);
        uint64_t action = getAction();
        int step = action ? *(int*)(action + 0xD8) : 0;
        if (step >= 0) {
            g_postResimFrame = -1;
            int32_t start = -1;
            int32_t end = -1;
            if (Rollback_ConsumeReSimRequest(start, end)) {
                Rollback_ReSimulate(start, end);
                const uint64_t postResimStart = Profile_Now();
                if (g_ndjsonLoggingEnabled)
                    NDJSON_SendResimNotify(start, end);
                const uint64_t notifyEnd = Profile_Now();
                Controller_ResolveRemoteInput(curFrame);
                const uint64_t postSaveStart = Profile_Now();
                Rollback_SaveSnapshot(curFrame);
                const uint64_t postSaveEnd = Profile_Now();
                if (Profile_Enabled()) {
                    Profile_DumpPostResim(curFrame, notifyEnd - postResimStart,
                                          postSaveStart - notifyEnd,
                                          postSaveEnd - postSaveStart,
                                          postSaveEnd - postResimStart);
                }
                g_postResimFrame = curFrame;
            } else {
                Rollback_SaveSnapshot(curFrame);
            }
        }
    }

    RollbackMainMoveFn original = g_originalMvc3MainMove ? *g_originalMvc3MainMove : nullptr;
    if (g_isResimulating) {
        original(main);
        return;
    }

    curFrame = Controller_GetNetFrameCount();
    if (Netplay_IsActive() && curFrame >= 0)
        NDJSON_DumpNetplayLive(curFrame);
    if (curFrame >= 0)
        NDJSON_CaptureFrame(curFrame, false, false);

    original(main);

    if (curFrame >= 0)
        NDJSON_CaptureFrame(curFrame, false, true);
    if (g_f7DiagSession >= 0 && g_f7State == 2)
        NDJSON_DumpFrame(curFrame, 0);
}

static void __fastcall hkApplyCollisionPushCorrection(
    uint64_t character, int phase, uint64_t collisionObject,
    uint64_t otherCharacter, uint64_t collisionResult, uint32_t collisionSlot)
{
    if (phase != 0 && oApplyCollisionPushCorrection) {
        oApplyCollisionPushCorrection(character, phase, collisionObject,
                                      otherCharacter, collisionResult, collisionSlot);
    }
}

static void __fastcall hkCallSRender(uint64_t render)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::RenderEndFrame);
    if (!g_isResimulating)
        oCallSRender(render);
}

static void __fastcall hkSoundMove(uint64_t sound)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::SoundMove);
    if (g_isResimulating)
        return;

    constexpr uint32_t COUNT_OFFSET = 0x1CF50;
    constexpr uint32_t FIRST_OFFSET = 0x70;
    constexpr uint32_t STRIDE = 0xB50;
    constexpr uint32_t MODEL_OFFSET = 0x80;
    constexpr uint32_t MAX_COUNT = 40;
    uint32_t count = *(uint32_t*)(sound + COUNT_OFFSET);
    if (count <= MAX_COUNT) {
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t scene = sound + FIRST_OFFSET + (uint64_t)i * STRIDE;
            uint64_t* modelSlot = (uint64_t*)(scene + MODEL_OFFSET);
            uint64_t model = *modelSlot;
            if (!model)
                continue;
            bool valid = IsBaseModelObject((void*)model);
            uint64_t vtable = valid ? *(uint64_t*)model : 0;
            valid = valid && IsAccessiblePointer(vtable) &&
                IsAccessibleAddress(vtable + 0xA0);
            uint64_t getMatrix = valid ? *(uint64_t*)(vtable + 0xA0) : 0;
            if (!valid || !IsExecutableAddress(getMatrix)) {
                HookLog("Rollback: detached stale sound scene model scene=%u model=0x%llX vtable=0x%llX\n",
                        i, model, vtable);
                *modelSlot = 0;
            }
        }
    }
    oSoundMove(sound);
}

static void __fastcall hkGameUiMove(uint64_t gameUi)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::GameUiMove);
    if (!g_isResimulating)
        oGameUiMove(gameUi);
}

static void __fastcall hkBtGaugeLifeMove(uint64_t gauge)
{
    // sGameUI is suppressed during re-simulation, so its gauge dependencies
    // are not guaranteed to be initialized for the accompanying sUnit pass.
    if (!g_isResimulating)
        oBtGaugeLifeMove(gauge);
}

static void __fastcall hkSchedulerRebuild(uint64_t scheduler)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::SchedulerRebuild);
    if (g_isResimulating) {
        uint32_t* state = (uint32_t*)(scheduler + 0x10);
        *state = (*state & ~7u) | 3u;
        return;
    }
    oSchedulerRebuild(scheduler);
}

static bool __fastcall hkRChrCpuClassFunc(void* cpu, void* object)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ChrCpuClassFunc);
    uint64_t address = (uint64_t)object;
    if (!IsAccessiblePointer(address))
        return false;
    uint64_t vtable = *(uint64_t*)address;
    if (!IsAccessiblePointer(vtable) || !IsAccessibleAddress(vtable + 0x20))
        return false;
    uint64_t getDti = *(uint64_t*)(vtable + 0x20);
    if (!IsExecutableAddress(getDti))
        return false;
    return oRChrCpuClassFunc(cpu, object);
}

static void __fastcall hkMainMove(uint64_t main)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::MainMove);
    if (g_isResimulating) {
        *(uint32_t*)(main + 0x48) = 0;
        *(float*)(main + 0x80) = 1.0f;
        return;
    }
    oMainMove(main);
    if (Netplay_IsActive())
        *(float*)(main + 0x80) = 1.0f;
}

void Rollback_InitMainHooks(RollbackMainMoveFn* originalMove)
{
    g_originalMvc3MainMove = originalMove;
    EnableHook(FN_SMVC3MAIN_MOVE, (void*)&hkMvc3MainMove, (void**)originalMove,
               "Rollback: hooked sMvc3Main__move",
               "Rollback: failed to hook sMvc3Main__move (%d)\n");

    MH_STATUS status = MH_CreateHook((LPVOID)_addr(FN_CCHR_SET_POS),
                                     (LPVOID)&hkCChrSetPos, (LPVOID*)&oCChrSetPos);
    if (status == MH_OK)
        HookLog("Rollback: hooked cChr__setPos for resim diagnostics\n");
    else
        HookLog("Rollback: failed to hook cChr__setPos (%d)\n", status);

    status = MH_CreateHook((LPVOID)_addr(FN_APPLY_COLLISION_PUSH),
                           (LPVOID)&hkApplyCollisionPushCorrection,
                           (LPVOID*)&oApplyCollisionPushCorrection);
    if (status != MH_OK)
        HookLog("Rollback: failed to disable player collision pushboxes (%d)\n", status);

    EnableHook(FN_RENDER_END_FRAME, (void*)&hkCallSRender, (void**)&oCallSRender,
               "Rollback: hooked sRender call",
               "Rollback: failed to hook sRender call (%d)\n");
    EnableHook(FN_SSOUND_MOVE, (void*)&hkSoundMove, (void**)&oSoundMove,
               "Rollback: hooked sSound__move for headless re-simulation",
               "Rollback: failed to hook sSound__move (%d)\n");
    EnableHook(FN_SGAMEUI_MOVE, (void*)&hkGameUiMove, (void**)&oGameUiMove,
               "Rollback: hooked sGameUI__move for re-simulation suppression",
               "Rollback: failed to hook sGameUI__move (%d)\n");
    EnableHook(FN_UBTGAUGELIFE_MOVE, (void*)&hkBtGaugeLifeMove,
               (void**)&oBtGaugeLifeMove,
               "Rollback: hooked uBtGaugeLife__move for re-simulation suppression",
               "Rollback: failed to hook uBtGaugeLife__move (%d)\n");
    EnableHook(FN_SCHEDULER_REBUILD, (void*)&hkSchedulerRebuild,
               (void**)&oSchedulerRebuild,
               "Rollback: hooked uScheduler rebuild for re-simulation suppression",
               "Rollback: failed to hook uScheduler rebuild (%d)\n");
    EnableHook(FN_RCHRCPU_CLASS_FUNC, (void*)&hkRChrCpuClassFunc,
               (void**)&oRChrCpuClassFunc,
               "Rollback: guarded rChrCpu script object cast",
               "Rollback: failed to guard rChrCpu script object cast (%d)\n");
    EnableHook(FN_SMAIN_MOVE, (void*)&hkMainMove, (void**)&oMainMove,
               "Rollback: hooked sMain__move",
               "Rollback: failed to hook sMain__move (%d)\n");
}
