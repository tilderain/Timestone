#include "../pch.h"

#include "rollback_hooks.h"

#include "controller.h"
#include "autotest.h"
#include "mk10utils.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_diagnostics.h"
#include "rollback_profile.h"
#include "live_benchmark.h"
#include "lag_diagnostics.h"
#include "steam_lobby.h"
#include "gameplay_hooks.h"
#include "xinput_discovery.h"
#include "mouse_disable.h"

#include <cmath>
#include <cstring>
#include <iterator>

extern void HookLog(const char* fmt, ...);

// Experimental: false respects the native GRAPHICS/SLI configuration.
static constexpr bool FORCE_NATIVE_SLI_ON = true;

static constexpr uintptr_t FN_SMVC3MAIN_MOVE = 0x1402594A0;
static constexpr uintptr_t FN_CCHR_SET_POS = 0x140002AF0;
static constexpr uintptr_t FN_RENDER_END_FRAME = 0x14053A250;
static constexpr uintptr_t FN_SSOUND_MOVE = 0x1405CC040;
static constexpr uintptr_t FN_GSOUND_VOICE_DISPATCH = 0x1402A3380;
static constexpr uintptr_t FN_GSOUND_CREATE_VOICE = 0x1402A21F0;
static constexpr uintptr_t FN_SOUND_PLAY_WITH_DB = 0x1405D28B0;
static constexpr uintptr_t FN_CREF_INIT_DISABLED = 0x1400239C0;
static constexpr uint32_t GSOUND_VOICE_REQUEST_FLAGS_OFFSET = 0x30;
static constexpr uint32_t GSOUND_VOICE_REQUEST_LOOP = 0x80;
static constexpr uintptr_t FN_SCENE_UPDATE_SOUND_ENV = 0x1405D6CD0;
static constexpr uintptr_t FN_SGAMEUI_MOVE = 0x1402BE610;
static constexpr uintptr_t FN_UBTGAUGEHC_MOVE = 0x1402F2AC0;
static constexpr uintptr_t FN_UBTGAUGELIFE_MOVE = 0x1402F6800;
static constexpr uintptr_t FN_RCHRCPU_CLASS_FUNC = 0x140117280;
static constexpr uintptr_t FN_SMAIN_MOVE = 0x140521DF0;
static constexpr uintptr_t MT_DTI_VTABLE_ADDR = 0x140A69ED8;
static constexpr uintptr_t FN_MENU_OPTION_KBD_MOVE = 0x140001C40;
static constexpr uintptr_t FN_MATERIAL_RESOURCE_STATE = 0x1405DD640;
static constexpr uintptr_t FN_REBUILD_FRUSTUM_PLANES = 0x1405351D0;
static constexpr uintptr_t FN_UFADE_MOVE = 0x14044D510;
static constexpr uintptr_t FN_UBTSCORE_UPDATE = 0x14030BC50;
static constexpr uintptr_t FN_UI_FONT_SET_TEXT = 0x1402CEF30;
static constexpr uintptr_t FN_UI_SET_VISIBLE = 0x140002A70;
static constexpr uintptr_t FN_GAME_CONFIG_GET_INSTANCE = 0x140001AB0;
static constexpr uintptr_t FN_FORMAT_PLAYER_NAME = 0x14024F720;
static constexpr uintptr_t PTR_SCAMERA = 0x140E17930;
static constexpr uint32_t SCAMERA_VIEWPORT0_OFFSET = 0x50;
static constexpr uint32_t VIEWPORT_CAMERA_OFFSET = 0x08;
static constexpr uint32_t VIEWPORT_VIEW_MATRIX_OFFSET = 0xA0;
static constexpr uint32_t CAMERA_GET_VIEW_MATRIX_VTABLE_OFFSET = 0x90;

static RollbackMainMoveFn* g_originalMvc3MainMove = nullptr;
static void(__fastcall* oCallSRender)(uint64_t) = nullptr;
static void(__fastcall* oWindowFocus)(uint64_t) = nullptr;
static void(__fastcall* oRenderMove)(uint64_t) = nullptr;
static void(__fastcall* oUnitMove)(uint64_t) = nullptr;
static void(__fastcall* oNativeInput)(uint64_t) = nullptr;
static void(__fastcall* oMainMove)(uint64_t) = nullptr;
static void(__fastcall* oSoundMove)(uint64_t) = nullptr;
static void(__fastcall* oDisabledMouseMove)(uint64_t) = nullptr;
static void(__fastcall* oGSoundVoiceDispatch)(void*, void*, char) = nullptr;
static uint64_t*(__fastcall* oGSoundCreateVoice)(void*, uint64_t*, void*, void*,
                                                 char) = nullptr;
static void(__fastcall* oSoundPlayWithDb)(void*, void*, uint32_t, uint64_t,
                                          uint32_t, float, uint8_t, int) = nullptr;
static void(__fastcall* oSceneUpdateSoundEnv)(uint64_t) = nullptr;
static void(__fastcall* oGameUiMove)(uint64_t) = nullptr;
static void(__fastcall* oBtGaugeHcMove)(uint64_t) = nullptr;
static void(__fastcall* oBtGaugeLifeMove)(uint64_t) = nullptr;
static bool(__fastcall* oRChrCpuClassFunc)(void*, void*) = nullptr;
using CChrSetPosFn = void(__fastcall*)(uint64_t, const float*);
static CChrSetPosFn oCChrSetPos = nullptr;
using RebuildFrustumPlanesFn = void(__fastcall*)(uint64_t);
using CameraGetViewMatrixFn = void(__fastcall*)(uint64_t, float*);
static RebuildFrustumPlanesFn oRebuildFrustumPlanes = nullptr;
static void(__fastcall* oFadeMove)(uint64_t) = nullptr;
using BtScoreUpdateFn = void(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
static BtScoreUpdateFn oBtScoreUpdate = nullptr;

static void CopySafeUtf8Name(char (&out)[32], const char* name)
{
    size_t written = 0;
    const auto* source = (const uint8_t*)name;
    while (source && *source && written < sizeof(out) - 1) {
        const uint8_t lead = *source;
        size_t length = 1;
        bool valid = lead >= 0x20 && lead != 0x7F;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            valid = (source[1] & 0xC0) == 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            valid = (source[1] & 0xC0) == 0x80 &&
                    (source[2] & 0xC0) == 0x80 &&
                    (lead != 0xE0 || source[1] >= 0xA0) &&
                    (lead != 0xED || source[1] < 0xA0);
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            valid = (source[1] & 0xC0) == 0x80 &&
                    (source[2] & 0xC0) == 0x80 &&
                    (source[3] & 0xC0) == 0x80 &&
                    (lead != 0xF0 || source[1] >= 0x90) &&
                    (lead != 0xF4 || source[1] < 0x90);
        } else if (lead >= 0x80) {
            valid = false;
        }

        if (!valid) {
            out[written++] = '?';
            ++source;
            continue;
        }
        if (written + length >= sizeof(out)) break;
        std::memcpy(out + written, source, length);
        written += length;
        source += length;
    }
    out[written] = '\0';
}

//Shows the opponent names underneath the HUD
static void SetFightingName(uint64_t widget, const char* name)
{
    if (!widget || !name || !name[0]) return;

    char safeName[32] = {};
    CopySafeUtf8Name(safeName, name);
    uint64_t gameConfig = ((uint64_t(__fastcall*)())
        _addr(FN_GAME_CONFIG_GET_INSTANCE))();
    const char* displayName = gameConfig
        ? ((const char*(__fastcall*)(uint64_t, const void*, uint32_t))
               _addr(FN_FORMAT_PLAYER_NAME))(gameConfig, safeName, 16)
        : safeName;
    ((void(__fastcall*)(uint64_t, const char*))_addr(FN_UI_FONT_SET_TEXT))(
        widget, displayName);
    ((void(__fastcall*)(uint64_t, bool))_addr(FN_UI_SET_VISIBLE))(widget, true);
}

static void __fastcall hkBtScoreUpdate(uint64_t score, uint64_t a2,
                                        uint64_t a3, uint64_t a4)
{
    oBtScoreUpdate(score, a2, a3, a4);
    if (!Netplay_IsActive()) return;

    const bool hasSteamNames = SteamLobby_HasPeerProfile();
    const char* localName = hasSteamNames ? SteamLobby_GetLocalUsername() : nullptr;
    const char* peerName = hasSteamNames ? SteamLobby_GetPeerUsername() : nullptr;
    const char* localDisplayName = localName && localName[0] ? localName : "Me";
    const char* peerDisplayName = peerName && peerName[0] ? peerName : "Opp";
    const bool localIsP1 = Netplay_IsHost();
    const char* p1Name = localIsP1 ? localDisplayName : peerDisplayName;
    const char* p2Name = localIsP1 ? peerDisplayName : localDisplayName;
    SetFightingName(*(uint64_t*)(score + 0xC8), p1Name);
    SetFightingName(*(uint64_t*)(score + 0xF0), p2Name);
}

static bool TraceLifecycleFrame(int32_t frame)
{
    return false;
}

extern "C" int32_t Controller_GetNetFrameCount();
extern "C" bool Controller_IsRollbackEnabled();
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" bool FrameStepper_IsMatchRestartPending();
extern "C" void Controller_ResolveRemoteInput(int32_t frame);
extern bool g_multiResimEnabled;

static void EnableHook(uintptr_t address, void* replacement, void** original,
                       const char* success, const char* failure)
{
    MH_STATUS status = GameplayHooks_Create((LPVOID)_addr(address), replacement, original);
    if (status == MH_OK) {
        GameplayHooks_QueueEnable((LPVOID)_addr(address));
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
    if (!GameplayHooks_IsActive()) {
        (*g_originalMvc3MainMove)(main);
        return;
    }
    LagDiagnosticScope diagnostic(LagMetric::MainHook);
    const uint64_t benchmarkStart = g_isResimulating ? 0 :
        LiveBenchmark_BeginFrame(Controller_GetNetFrameCount());
    // Discard previous memory checks; native allocations may have changed.
    Rollback_SetMainInstance(main);

    // Discard pending corrections during restart or battle synchronization.
    if (Netplay_IsActive() &&
        (FrameStepper_IsMatchRestartPending() ||
         !FrameStepper_IsBattleSynchronized()))
        Rollback_ClearPendingReSimulation();

    int32_t curFrame = Controller_GetNetFrameCount();
    Rollback_FreezeWatchdogHeartbeat("main-enter", curFrame);
    int32_t traceFrame = g_isResimulating ? g_resimulateCurrentFrame : curFrame;
    if (TraceLifecycleFrame(traceFrame)) {
        uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
        int32_t replayFrame = replay ? *(int32_t*)(replay + 0x54) : -1;
        HookLog("TICKTRACE phase=enter frame=%d resim=%d net=%d replay=%d main=0x%llX\n",
                traceFrame, g_isResimulating ? 1 : 0, curFrame, replayFrame, main);
    }
    if (g_restoreRequestedFrame >= 0) {
        int32_t restoreFrame = g_restoreRequestedFrame;
        g_restoreRequestedFrame = -1;
        HookLog("Rollback: executing deferred restore to frame %d\n", restoreFrame);
        Rollback_RestoreSnapshot(restoreFrame);
    }

    if (g_f7State == 1) {
        g_f7StartFrame = Controller_GetNetFrameCount();
        Rollback_SaveSnapshot(g_f7StartFrame);
        g_f7State = 2;
        HookLog("F7: snapshot saved at frame %d, recording %d frames\n",
                g_f7StartFrame, g_f7Remaining);
    }
    if (g_f7State == 2 && --g_f7Remaining <= 0) {
        g_f7State = 0;
        HookLog("F7: %d frames recorded, restoring frame %d and re-simulating\n",
                8, g_f7StartFrame);
        Rollback_ReSimulate(g_f7StartFrame, g_f7StartFrame + 8);
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
            bool shouldResim = Rollback_ConsumeReSimRequest(start, end);
            if (!shouldResim)
                shouldResim = AutoTest_GetForcedClientResim(curFrame, &start, &end);
            if (shouldResim) {
                Rollback_FreezeWatchdogHeartbeat("rollback-resimulate", curFrame);
                Rollback_ReSimulate(start, end);
                const uint64_t postResimStart = Profile_Now();
                Controller_ResolveRemoteInput(curFrame);
                const uint64_t postSaveStart = Profile_Now();
                Rollback_SaveSnapshot(curFrame);
                const uint64_t postSaveEnd = Profile_Now();
                if (Profile_Enabled()) {
                    Profile_DumpPostResim(curFrame,
                                          postSaveStart - postResimStart,
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
        Rollback_FreezeWatchdogHeartbeat("resim-native-main-move",
                                         g_resimulateCurrentFrame);
        original(main);
        Rollback_FreezeWatchdogHeartbeat("resim-native-main-complete",
                                         g_resimulateCurrentFrame);
        if (TraceLifecycleFrame(g_resimulateCurrentFrame)) {
            uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
            int32_t replayFrame = replay ? *(int32_t*)(replay + 0x54) : -1;
            HookLog("TICKTRACE phase=exit frame=%d resim=1 replay=%d main=0x%llX\n",
                    g_resimulateCurrentFrame, replayFrame, main);
        }
        return;
    }

    curFrame = Controller_GetNetFrameCount();

    Rollback_FreezeWatchdogHeartbeat("native-main-move", curFrame);
    {
        LagDiagnosticScope diagnostic(LagMetric::Native);
        original(main);
    }
    Rollback_FreezeWatchdogHeartbeat("native-main-complete", curFrame);

    if (TraceLifecycleFrame(curFrame)) {
        uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
        int32_t replayFrame = replay ? *(int32_t*)(replay + 0x54) : -1;
        HookLog("TICKTRACE phase=exit frame=%d resim=0 replay=%d main=0x%llX\n",
                curFrame, replayFrame, main);
    }

    LiveBenchmark_EndFrame(benchmarkStart, Controller_GetNetFrameCount());
}

static void __fastcall hkCallSRender(uint64_t render)
{
    LagDiagnosticScope diagnostic(LagMetric::RenderEnd);
    RollbackHookProfileScope profile(RollbackHookProfileId::RenderEndFrame);
    if (!g_isResimulating)
        oCallSRender(render);
}

static void __fastcall hkWindowFocus(uint64_t main)
{
    LagDiagnosticScope diagnostic(LagMetric::WindowFocus);
    oWindowFocus(main);
}

static void __fastcall hkSoundMove(uint64_t sound)
{
    LagDiagnosticScope diagnostic(LagMetric::SoundMove);
    RollbackHookProfileScope profile(RollbackHookProfileId::SoundMove);
    if (g_isResimulating)
        return;
    oSoundMove(sound);
}

static thread_local bool g_timingRender = false;
static void __fastcall hkRenderMove(uint64_t object)
{
    LagDiagnosticScope diagnostic(LagMetric::RenderMove);
    const bool previousRender = g_timingRender;
    g_timingRender = true;
    oRenderMove(object);
    g_timingRender = previousRender;
}

static void __fastcall hkUnitMove(uint64_t object)
{
    LagDiagnosticScope diagnostic(LagMetric::UnitMove);
    oUnitMove(object);
}

static void __fastcall hkNativeInput(uint64_t object)
{
    LagDiagnosticScope diagnostic(LagMetric::NativeInput);
    oNativeInput(object);
}

template<LagMetric Metric, typename... Args>
struct TimedNativeCall {
    static inline void(__fastcall* original)(Args...) = nullptr;
    static void __fastcall Invoke(Args... args) {
        LagDiagnosticScope diagnostic(Metric);
        original(args...);
    }
};

using DrawCacheTiming = TimedNativeCall<LagMetric::DrawCache, uint64_t>;
using CameraDrawTiming = TimedNativeCall<LagMetric::CameraDraw, uint64_t, uint64_t, uint64_t, uint32_t>;
using UnitDrawTiming = TimedNativeCall<LagMetric::UnitDraw, uint64_t, uint64_t>;
using DrawFrameCacheTiming = TimedNativeCall<LagMetric::DrawFrameCache, uint64_t, uint64_t>;
using JobsTiming = TimedNativeCall<LagMetric::Jobs, uint64_t, uint32_t>;
using JobDispatchTiming = TimedNativeCall<LagMetric::JobDispatch, uint64_t>;
using UnitSyncTiming = TimedNativeCall<LagMetric::UnitSync, uint64_t>;
using UnitAfterTiming = TimedNativeCall<LagMetric::UnitAfter, uint64_t>;
using SceneSortTiming = TimedNativeCall<LagMetric::SceneSort, uint64_t>;
using EffectMoveTiming = TimedNativeCall<LagMetric::EffectMove, uint64_t>;
using ParticlePreTiming = TimedNativeCall<LagMetric::ParticlePre, uint64_t>;
using ParticlePostTiming = TimedNativeCall<LagMetric::ParticlePost, uint64_t>;
using CameraMoveTiming = TimedNativeCall<LagMetric::CameraMove, uint64_t>;
using GraphicsUpdateTiming = TimedNativeCall<LagMetric::GraphicsUpdate, uint64_t>;
using AiAfterTiming = TimedNativeCall<LagMetric::AiAfter, uint64_t>;

template<LagMetric Metric, typename Result, typename... Args>
struct TimedBackendCall {
    static inline Result(__fastcall* original)(Args...) = nullptr;
    static Result __fastcall Invoke(Args... args) {
        LagDiagnosticScope diagnostic(Metric);
        return original(args...);
    }
};
using DetectTiming = TimedBackendCall<LagMetric::PadDetect, uint8_t, uint64_t, int32_t, uint32_t>;
using XStateTiming = TimedBackendCall<LagMetric::XInputState, uint32_t, uint32_t, void*>;
using XCapsTiming = TimedBackendCall<LagMetric::XInputCaps, uint64_t, uint64_t, uint64_t>;
using DInputFindTiming = TimedBackendCall<LagMetric::DirectInputFind, uint8_t, uint64_t, uint64_t>;
using WmiTiming = TimedBackendCall<LagMetric::InputWmi, int, void*>;
using PlaybackTiming = TimedNativeCall<LagMetric::RenderPlayback, uint64_t>;
static void(__fastcall* oRenderGpuFence)(uint64_t) = nullptr;
static void __fastcall hkRenderGpuFence(uint64_t render)
{
    LagDiagnosticScope diagnostic(LagMetric::RenderGpuFence);
    oRenderGpuFence(render);
}
static void(__fastcall* oRenderCreateDevice)(uint64_t, uint64_t, uint64_t, uint64_t) = nullptr;
static void __fastcall hkRenderCreateDevice(uint64_t render, uint64_t a2, uint64_t a3, uint64_t a4)
{
    oRenderCreateDevice(render, a2, a3, a4);
    // Initialization reads GRAPHICS/SLI directly, before the setter is involved.
    if constexpr (FORCE_NATIVE_SLI_ON)
        *(uint8_t*)(render + 0x867730) = 1;
}
static void(__fastcall* oRenderSetSli)(uint64_t, uint8_t) = nullptr;
static void __fastcall hkRenderSetSli(uint64_t render, uint8_t enabled)
{
    oRenderSetSli(render, FORCE_NATIVE_SLI_ON ? 1 : enabled);
}
using RetireTiming = TimedNativeCall<LagMetric::RenderRetire, uint64_t>;
using CaptureTiming = TimedNativeCall<LagMetric::RenderCapture, uint64_t>;
using KeyboardStateTiming = TimedNativeCall<LagMetric::KeyboardState, uint64_t, uint32_t*>;
using MouseMoveTiming = TimedNativeCall<LagMetric::MouseMove, uint64_t>;
using MouseStateTiming = TimedNativeCall<LagMetric::MouseState, uint64_t, int32_t*>;
using NetComponentTiming = TimedNativeCall<LagMetric::NetComponent, uint64_t>;
using NativeNetworkTiming = TimedNativeCall<LagMetric::NativeNetwork, uint64_t>;
using AdapterQueryTiming = TimedBackendCall<LagMetric::AdapterQuery, uint32_t, uint64_t>;
using NetContextDestructTiming = TimedNativeCall<LagMetric::NetContextDestruct, uint64_t>;
using ShaderMoveTiming = TimedNativeCall<LagMetric::ShaderMove, uint64_t>;
using ShaderTableTiming = TimedNativeCall<LagMetric::ShaderTable, uint64_t>;
using GameConfigMoveTiming = TimedNativeCall<LagMetric::GameConfigMove, uint64_t>;
using UiScreenCaptureTiming = TimedNativeCall<LagMetric::UiScreenCapture, uint64_t>;
using CollisionBeginTiming = TimedNativeCall<LagMetric::CollisionBegin, uint64_t>;
using CollisionMoveTiming = TimedNativeCall<LagMetric::CollisionMove, uint64_t>;
using GameSoundMoveTiming = TimedNativeCall<LagMetric::GameSoundMove, uint64_t>;
using ShadowMoveTiming = TimedNativeCall<LagMetric::ShadowMove, uint64_t>;
using CaptureMoveTiming = TimedNativeCall<LagMetric::CaptureMove, uint64_t>;
using StageMoveTiming = TimedNativeCall<LagMetric::StageMove, uint64_t>;

template<LagMetric Metric, unsigned Slot, unsigned StateSize = 0>
static int32_t __fastcall TraceInputDevice(void* device, uint32_t, void* state)
{
    const uint64_t start = LagDiagnostics_Begin(Metric);
    void* target = (*(void***)device)[Slot];
    int32_t result;
    if constexpr (StateSize)
        result = ((int32_t(__fastcall*)(void*, uint32_t, void*))target)(device, StateSize, state);
    else
        result = ((int32_t(__fastcall*)(void*))target)(device);
    LagDiagnostics_EndResult(Metric, start, result);
    return result;
}

template<LagMetric Metric, uintptr_t Import, typename... Args>
static BOOL WINAPI TraceMouseApi(Args... args)
{
    LagDiagnosticScope diagnostic(Metric);
    return ((BOOL(WINAPI*)(Args...))*(void**)_addr(Import))(args...);
}

static void InstallInputCallTiming()
{
    // Replace only verified call sequences. The native retry loop and decoding
    // remain untouched; each wrapper resolves the device's current vtable.
    struct Site { uintptr_t address; const char* bytes; size_t size; void* replacement; };
    const Site sites[] = {
        {0x1406aa1e4, "\xff\x90\xc8\x00\x00\x00", 6, (void*)&TraceInputDevice<LagMetric::KeyboardPoll, 25>},
        {0x1406aa1f5, "\x48\x8b\x01\xff\x50\x38", 6, (void*)&TraceInputDevice<LagMetric::KeyboardAcquire, 7>},
        {0x1406aa209, "\x48\x8b\x01\xff\x50\x38", 6, (void*)&TraceInputDevice<LagMetric::KeyboardAcquireRetry, 7>},
        // Includes mov edx,esi; the wrapper supplies the verified 256-byte size.
        {0x1406aa24a, "\x48\x8b\x01\x8b\xd6\xff\x50\x48", 8, (void*)&TraceInputDevice<LagMetric::KeyboardGetState, 9, 256>},
        {0x1406aae64, "\xff\x90\xc8\x00\x00\x00", 6, (void*)&TraceInputDevice<LagMetric::MousePoll, 25>},
        {0x1406aae75, "\x48\x8b\x01\xff\x50\x38", 6, (void*)&TraceInputDevice<LagMetric::MouseAcquire, 7>},
        {0x1406aae89, "\x48\x8b\x01\xff\x50\x38", 6, (void*)&TraceInputDevice<LagMetric::MouseAcquireRetry, 7>},
        {0x1406aaeba, "\x48\x8b\x01\xff\x50\x48", 6, (void*)&TraceInputDevice<LagMetric::MouseGetState, 9, 20>},
        {0x1406aadce, "\xff\x15\xec\x07\x3b\x00", 6, (void*)&TraceMouseApi<LagMetric::NativeMouseRect, 0x140a5b5c0, HWND, RECT*>},
        {0x1406aade5, "\xff\x15\xad\x07\x3b\x00", 6, (void*)&TraceMouseApi<LagMetric::NativeMouseCursor, 0x140a5b598, POINT*>},
        {0x1406aadfd, "\xff\x15\x85\x07\x3b\x00", 6, (void*)&TraceMouseApi<LagMetric::NativeMouseClient, 0x140a5b588, HWND, POINT*>},
    };
    for (const auto& site : sites) {
        if (std::memcmp((void*)_addr(site.address), site.bytes, site.size)) {
            HookLog("Input timing: instruction mismatch at 0x%llX; no call sites patched\n", (unsigned long long)site.address);
            return;
        }
    }
    // RIP-relative indirect calls need nearby pointer storage, not executable stubs.
    // Keep this page for the process lifetime so in-flight calls remain valid.
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t origin = _addr(sites[0].address);
    const uintptr_t aligned = origin - origin % info.dwAllocationGranularity;
    void** targets = nullptr;
    for (uintptr_t distance = info.dwAllocationGranularity; distance < 0x70000000; distance += info.dwAllocationGranularity) {
        targets = (void**)VirtualAlloc((void*)(aligned + distance), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (targets) break;
    }
    if (!targets) {
        HookLog("Input timing: could not allocate nearby call targets\n");
        return;
    }
    for (size_t i = 0; i < std::size(sites); ++i) {
        const auto& site = sites[i];
        auto* address = (uint8_t*)_addr(site.address);
        targets[i] = site.replacement;
        const int32_t displacement = (int32_t)((intptr_t)&targets[i] - (intptr_t)(address + 6));
        uint8_t patch[8] = {0xff, 0x15, 0, 0, 0, 0, 0x90, 0x90};
        std::memcpy(patch + 2, &displacement, sizeof(displacement));
        DWORD previous;
        if (!VirtualProtect(address, site.size, PAGE_EXECUTE_READWRITE, &previous)) {
            HookLog("Input timing: protection failed at 0x%llX\n", (unsigned long long)site.address);
            continue;
        }
        GameplayHooks_RecordPatch(address, site.size);
        std::memcpy(address, patch, site.size);
        DWORD ignored;
        VirtualProtect(address, site.size, previous, &ignored);
        FlushInstructionCache(GetCurrentProcess(), address, site.size);
        HookLog("Input timing: patched call at 0x%llX\n", (unsigned long long)site.address);
    }
}
static void(__fastcall* oRenderSyncTiming)(uint64_t) = nullptr;
static void __fastcall hkRenderSyncTiming(uint64_t render)
{
    LagDiagnosticScope diagnostic(*(uint8_t*)(render + 0x3E)
        ? LagMetric::RenderWorkerWait : LagMetric::RenderInlinePlayback);
    oRenderSyncTiming(render);
}

static void __fastcall hkTimedJobs(uint64_t main, uint32_t mode)
{
    if (g_timingRender && g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed)) {
        LagDiagnostics_Count(LagEvent::RenderBatches, 1);
        LagDiagnostics_Count(LagEvent::RenderJobs, *(uint32_t*)(main + 0x98));
        LagDiagnostics_Count(LagEvent::RenderWorkers, *(uint32_t*)(main + 0xA4));
    }
    JobsTiming::Invoke(main, mode);
}

static void __fastcall hkTimedSceneSort(uint64_t scene)
{
    if (g_lagDiagnosticsMayBeEnabled.load(std::memory_order_relaxed))
        LagDiagnostics_Count(LagEvent::SceneDrawables, *(uint32_t*)(scene + 0x40));
    SceneSortTiming::Invoke(scene);
}

static void __fastcall hkGSoundVoiceDispatch(void* dispatcher, void* voice,
                                              char stop)
{
    // Sound is not rewound: suppress replayed starts, but allow stops for live voices.
    if (g_isResimulating && stop == 0)
        return;
    oGSoundVoiceDispatch(dispatcher, voice, stop);
}

static void(__fastcall* oRenderMove)(uint64_t) = nullptr;
static void __fastcall hkRenderMove(uint64_t object)
{
    Controller_PublishInputBeforeRender();
    oRenderMove(object);
}

static void(__fastcall* oRenderCreateDevice)(uint64_t, uint64_t, uint64_t, uint64_t) = nullptr;
static void __fastcall hkRenderCreateDevice(uint64_t render, uint64_t a2, uint64_t a3, uint64_t a4)
{
    oRenderCreateDevice(render, a2, a3, a4);
    // Initialization reads GRAPHICS/SLI directly, before the setter is involved.
    if constexpr (FORCE_NATIVE_SLI_ON)
        *(uint8_t*)(render + 0x867730) = 1;
}
static void(__fastcall* oRenderSetSli)(uint64_t, uint8_t) = nullptr;
static void __fastcall hkRenderSetSli(uint64_t render, uint8_t enabled)
{
    oRenderSetSli(render, FORCE_NATIVE_SLI_ON ? 1 : enabled);
}

static uint64_t* __fastcall hkGSoundCreateVoice(void* dispatcher,
                                                uint64_t* cookie,
                                                void* voiceData,
                                                void* request,
                                                char recursive)
{
    const bool isLooping = request &&
        (*(const uint32_t*)((const uint8_t*)request +
                            GSOUND_VOICE_REQUEST_FLAGS_OFFSET) &
         GSOUND_VOICE_REQUEST_LOOP) != 0;
    if (g_isResimulating && isLooping) {
        // Return a disabled handle that later shot cleanup can safely stop.
        return ((uint64_t*(__fastcall*)(uint64_t*, uint64_t))
            _addr(FN_CREF_INIT_DISABLED))(cookie, UINT64_MAX);
    }
    return oGSoundCreateVoice(dispatcher, cookie, voiceData, request, recursive);
}

static void __fastcall hkSoundPlayWithDb(void* sound, void* resource,
                                         uint32_t bank, uint64_t cue,
                                         uint32_t index, float gain,
                                         uint8_t flags, int gainIsDb)
{
    if (!g_isResimulating) {
        oSoundPlayWithDb(sound, resource, bank, cue, index, gain, flags,
                         gainIsDb);
    }
}

static void __fastcall hkSceneUpdateSoundEnv(uint64_t scene)
{
    constexpr uint32_t MODEL_OFFSET = 0x80;
    constexpr uint32_t JOINT_INDEX_OFFSET = 0x88;
    constexpr uint32_t POSITION_MODE_OFFSET = 0xC0;

    uint64_t* modelSlot = (uint64_t*)(scene + MODEL_OFFSET);
    uint64_t model = *modelSlot;
    if (model) {
        bool valid = IsBaseModelObject((void*)model);
        uint64_t vtable = valid ? *(uint64_t*)model : 0;
        uint64_t getMatrix = valid ? *(uint64_t*)(vtable + 0xA0) : 0;
        int32_t jointIndex = *(int32_t*)(scene + JOINT_INDEX_OFFSET);
        uint32_t positionMode = *(uint32_t*)(scene + POSITION_MODE_OFFSET);
        if (!valid || !getMatrix ||
            (positionMode == 2 && jointIndex < 0)) {
            HookLog("Rollback: detached stale sound scene model=0x%llX "
                    "vtable=0x%llX mode=%u joint=%d\n",
                    model, vtable, positionMode, jointIndex);
            *modelSlot = 0;
        }
    }
    oSceneUpdateSoundEnv(scene);
}

static void __fastcall hkGameUiMove(uint64_t gameUi)
{
    LagDiagnosticScope diagnostic(LagMetric::GameUiMove);
    RollbackHookProfileScope profile(RollbackHookProfileId::GameUiMove);
    // Advance unsnapshotted UI objects only on live frames.
    if (g_isResimulating)
        return;
    oGameUiMove(gameUi);
}

static void __fastcall hkFadeMove(uint64_t fade)
{
    // Do not advance live fade timers again during replay.
    if (!g_isResimulating)
        oFadeMove(fade);
}

static void __fastcall hkBtGaugeLifeMove(uint64_t gauge)
{
    // Keep gauge updates live-only and skip a missing backing object.
    if (g_isResimulating)
        return;
    if (!*(uint64_t*)(gauge + 0x58))
        return;
    oBtGaugeLifeMove(gauge);
}

static void __fastcall hkBtGaugeHcMove(uint64_t gauge)
{
    if (g_isResimulating)
        return;
    // Skip partially torn-down gauges whose embedded material jobs lost their vtables.
    if (!*(uint64_t*)(gauge + 0xC0) || !*(uint64_t*)(gauge + 0xC40))
        return;
    oBtGaugeHcMove(gauge);
}

static bool __fastcall hkRChrCpuClassFunc(void* cpu, void* object)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ChrCpuClassFunc);
    return oRChrCpuClassFunc(cpu, object);
}

static void __fastcall hkMainMove(uint64_t main)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::MainMove);
    if (g_isResimulating) {
        *(uint32_t*)(main + 0x48) = (uint32_t)g_resimulateCurrentFrame;
        *(float*)(main + 0x80) = 1.0f;
        return;
    }
    const uint64_t benchmarkStart = LiveBenchmark_Now();
    {
        LagDiagnosticScope diagnostic(LagMetric::Pacing);
        oMainMove(main);
    }
    LiveBenchmark_EndPacing(benchmarkStart);
    if (Netplay_IsActive()) {
        int32_t frame = Controller_GetNetFrameCount();
        if (frame >= 0 && FrameStepper_IsBattleSynchronized())
            *(uint32_t*)(main + 0x48) = (uint32_t)frame;
        *(float*)(main + 0x80) = 1.0f;
    }
}

static void(__fastcall* oMenuOptionKbdMove)(uint64_t) = nullptr;
static uint64_t(__fastcall* oMaterialResourceState)(uint64_t) = nullptr;

static void __fastcall hkMenuOptionKbdMove(uint64_t object)
{
    uint64_t vtable = object ? *(uint64_t*)object : 0;
    if (vtable == (uint64_t)_addr(MT_DTI_VTABLE_ADDR)) {
        return;
    }
    oMenuOptionKbdMove(object);
}

static uint64_t __fastcall hkMaterialResourceState(uint64_t material)
{
    if (!material)
        return 0;

    uint64_t resource = *(uint64_t*)(material + 0xA0);
    if (!resource)
        return 0;

    return oMaterialResourceState(material);
}

static void __fastcall hkRebuildFrustumPlanes(uint64_t viewport)
{
    uint64_t sCamera = *(uint64_t*)_addr(PTR_SCAMERA);
    if (!Netplay_IsActive() || !sCamera ||
        viewport != sCamera + SCAMERA_VIEWPORT0_OFFSET) {
        oRebuildFrustumPlanes(viewport);
        return;
    }

    uint64_t camera = *(uint64_t*)(viewport + VIEWPORT_CAMERA_OFFSET);
    uint64_t vtable = camera ? *(uint64_t*)camera : 0;
    uint64_t getViewMatrix = vtable
        ? *(uint64_t*)(vtable + CAMERA_GET_VIEW_MATRIX_VTABLE_OFFSET) : 0;
    if (!camera || !vtable || !getViewMatrix) {
        oRebuildFrustumPlanes(viewport);
        return;
    }

    // Build gameplay bounds without screen shake, then restore the render matrix.
    alignas(16) float renderView[16];
    alignas(16) float gameplayView[16];
    void* viewMatrix = (void*)(viewport + VIEWPORT_VIEW_MATRIX_OFFSET);
    std::memcpy(renderView, viewMatrix, sizeof(renderView));
    ((CameraGetViewMatrixFn)getViewMatrix)(camera, gameplayView);
    std::memcpy(viewMatrix, gameplayView, sizeof(gameplayView));
    oRebuildFrustumPlanes(viewport);
    std::memcpy(viewMatrix, renderView, sizeof(renderView));
}

static bool DisableNativeNetworkFrameUpdates()
{
    // Cut only the four frame roots, not shared request pumps used by teardown.
    // Component includes session move/receive/send; its after/sync slots are no-ops.
    // See doc/native_network_disable.md for the Guide/input/Steam exclusions.
    static constexpr struct { uintptr_t address; uint8_t modrm; } sites[] = {
        {0x140259813, 0x50}, // nNet::sComponent
        {0x140259820, 0x50}, // sAgentSession
        {0x140259843, 0x52}, // sMvc3Ranking
        {0x140259859, 0x52}, // sNetworkRoom
    };
    for (const auto& site : sites) {
        const uint8_t expected[] = {0xFF, site.modrm, 0x30};
        if (std::memcmp((void*)_addr(site.address), expected, sizeof(expected)) != 0) {
            HookLog("Native network: frame patch rejected at 0x%llX\n",
                    (unsigned long long)site.address);
            return false;
        }
    }
    auto* first = (uint8_t*)_addr(sites[0].address);
    constexpr size_t span = 0x14025985C - 0x140259813;
    DWORD protection = 0;
    if (!VirtualProtect(first, span, PAGE_EXECUTE_READWRITE, &protection))
        return false;
    for (const auto& site : sites) {
        void* target = (void*)_addr(site.address);
        GameplayHooks_RecordPatch(target, 3);
        std::memset(target, 0x90, 3);
    }
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), first, span);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(first, span, protection, &ignored);
    return flushed && restored;
}

void Rollback_InitMainHooks(RollbackMainMoveFn* originalMove)
{
    XInputDiscovery_Init();
    EnableHook(0x14053caf0, (void*)&hkRenderMove, (void**)&oRenderMove,
               "Input: hooked pre-render publication", "Input: pre-render hook failed (%d)\n");
    EnableHook(0x1406aa990, (void*)&MouseDisable_Move, (void**)&oDisabledMouseMove,
               "Mouse: native movement/polling disabled", "Mouse: disable hook failed (%d)\n");
    HookLog("Native network: component/agent/ranking/room frame updates %s\n",
            DisableNativeNetworkFrameUpdates() ? "disabled (12-byte patch)" : "patch failed");
    g_originalMvc3MainMove = originalMove;
    EnableHook(FN_SMVC3MAIN_MOVE, (void*)&hkMvc3MainMove, (void**)originalMove,
               "Rollback: hooked sMvc3Main__move",
               "Rollback: failed to hook sMvc3Main__move (%d)\n");

    MH_STATUS status = GameplayHooks_Create((LPVOID)_addr(FN_CCHR_SET_POS),
                                     (LPVOID)&hkCChrSetPos, (LPVOID*)&oCChrSetPos);
    if (status == MH_OK)
        HookLog("Rollback: hooked cChr__setPos for resim diagnostics\n");
    else
        HookLog("Rollback: failed to hook cChr__setPos (%d)\n", status);

    EnableHook(0x140258540, (void*)&hkWindowFocus, (void**)&oWindowFocus,
               "Rollback: hooked window focus timing",
               "Rollback: failed to hook window focus timing (%d)\n");
    EnableHook(0x14053caf0, (void*)&hkRenderMove, (void**)&oRenderMove,
               "Rollback: hooked render move timing",
               "Rollback: failed to hook render move timing (%d)\n");
    EnableHook(0x14051c070, (void*)&DrawCacheTiming::Invoke, (void**)&DrawCacheTiming::original,
               "Timing: draw cache", "Timing: draw cache hook failed (%d)\n");
    EnableHook(0x140532830, (void*)&CameraDrawTiming::Invoke, (void**)&CameraDrawTiming::original,
               "Timing: camera draw", "Timing: camera draw hook failed (%d)\n");
    EnableHook(0x14051ad90, (void*)&UnitDrawTiming::Invoke, (void**)&UnitDrawTiming::original,
               "Timing: unit draw", "Timing: unit draw hook failed (%d)\n");
    EnableHook(0x140541150, (void*)&DrawFrameCacheTiming::Invoke, (void**)&DrawFrameCacheTiming::original,
               "Timing: frame cache", "Timing: frame cache hook failed (%d)\n");
    EnableHook(0x140521230, (void*)&hkTimedJobs, (void**)&JobsTiming::original,
               "Timing: jobs", "Timing: jobs hook failed (%d)\n");
    EnableHook(0x140521a40, (void*)&JobDispatchTiming::Invoke, (void**)&JobDispatchTiming::original,
               "Timing: dispatch", "Timing: dispatch hook failed (%d)\n");
    EnableHook(0x14051bff0, (void*)&UnitSyncTiming::Invoke, (void**)&UnitSyncTiming::original,
               "Timing: unit sync", "Timing: unit sync hook failed (%d)\n");
    EnableHook(0x14051b850, (void*)&UnitAfterTiming::Invoke, (void**)&UnitAfterTiming::original,
               "Timing: unit after", "Timing: unit after hook failed (%d)\n");
    EnableHook(0x14077ced0, (void*)&hkTimedSceneSort, (void**)&SceneSortTiming::original,
               "Timing: scene sort", "Timing: scene sort hook failed (%d)\n");
    EnableHook(0x140543d30, (void*)&EffectMoveTiming::Invoke, (void**)&EffectMoveTiming::original,
               "Timing: effects", "Timing: effects hook failed (%d)\n");
    EnableHook(0x1406a03b0, (void*)&ParticlePreTiming::Invoke, (void**)&ParticlePreTiming::original,
               "Timing: particles pre", "Timing: particles pre hook failed (%d)\n");
    EnableHook(0x1406a83d0, (void*)&ParticlePostTiming::Invoke, (void**)&ParticlePostTiming::original,
               "Timing: particles post", "Timing: particles post hook failed (%d)\n");
    EnableHook(0x140532d80, (void*)&CameraMoveTiming::Invoke, (void**)&CameraMoveTiming::original,
               "Timing: camera move", "Timing: camera move hook failed (%d)\n");
    EnableHook(0x140259ea0, (void*)&GraphicsUpdateTiming::Invoke, (void**)&GraphicsUpdateTiming::original,
               "Timing: graphics update", "Timing: graphics update hook failed (%d)\n");
    EnableHook(0x140647910, (void*)&AiAfterTiming::Invoke, (void**)&AiAfterTiming::original,
               "Timing: AI after", "Timing: AI after hook failed (%d)\n");
    EnableHook(0x14051b670, (void*)&hkUnitMove, (void**)&oUnitMove,
               "Rollback: hooked unit move timing",
               "Rollback: failed to hook unit move timing (%d)\n");
    EnableHook(0x140289c30, (void*)&hkNativeInput, (void**)&oNativeInput,
               "Rollback: hooked native input timing",
               "Rollback: failed to hook native input timing (%d)\n");
    if constexpr (LAG_BACKEND_TIMING_ENABLED) {
        InstallInputCallTiming();
        EnableHook(0x1406aa1b0, (void*)&KeyboardStateTiming::Invoke, (void**)&KeyboardStateTiming::original,
                   "Timing: keyboard state", "Timing: keyboard state failed (%d)\n");
        EnableHook(0x1406aa990, (void*)&MouseMoveTiming::Invoke, (void**)&MouseMoveTiming::original,
                   "Timing: mouse move", "Timing: mouse move failed (%d)\n");
        EnableHook(0x1406aad00, (void*)&MouseStateTiming::Invoke, (void**)&MouseStateTiming::original,
                   "Timing: mouse state", "Timing: mouse state failed (%d)\n");
        EnableHook(0x140458320, (void*)&NetComponentTiming::Invoke, (void**)&NetComponentTiming::original,
                   "Timing: network component", "Timing: network component failed (%d)\n");
        EnableHook(0x1406864d0, (void*)&NativeNetworkTiming::Invoke, (void**)&NativeNetworkTiming::original,
                   "Timing: native network", "Timing: native network failed (%d)\n");
        EnableHook(0x1404da610, (void*)&AdapterQueryTiming::Invoke, (void**)&AdapterQueryTiming::original,
                   "Timing: adapter query", "Timing: adapter query failed (%d)\n");
        EnableHook(0x1404faf70, (void*)&NetContextDestructTiming::Invoke, (void**)&NetContextDestructTiming::original,
                   "Timing: network context destruction", "Timing: network context destruction failed (%d)\n");
        EnableHook(0x14065cb20, (void*)&ShaderMoveTiming::Invoke, (void**)&ShaderMoveTiming::original,
                   "Timing: shader move", "Timing: shader move failed (%d)\n");
        EnableHook(0x14065be60, (void*)&ShaderTableTiming::Invoke, (void**)&ShaderTableTiming::original,
                   "Timing: shader table", "Timing: shader table failed (%d)\n");
        EnableHook(0x140253490, (void*)&GameConfigMoveTiming::Invoke, (void**)&GameConfigMoveTiming::original,
                   "Timing: game config", "Timing: game config failed (%d)\n");
        EnableHook(0x1402b9670, (void*)&UiScreenCaptureTiming::Invoke, (void**)&UiScreenCaptureTiming::original,
                   "Timing: UI screen capture", "Timing: UI screen capture failed (%d)\n");
        EnableHook(0x1405bb710, (void*)&CollisionBeginTiming::Invoke, (void**)&CollisionBeginTiming::original,
                   "Timing: collision begin", "Timing: collision begin failed (%d)\n");
        EnableHook(0x1405baf60, (void*)&CollisionMoveTiming::Invoke, (void**)&CollisionMoveTiming::original,
                   "Timing: collision move", "Timing: collision move failed (%d)\n");
        EnableHook(0x1402aa6c0, (void*)&GameSoundMoveTiming::Invoke, (void**)&GameSoundMoveTiming::original,
                   "Timing: game sound", "Timing: game sound failed (%d)\n");
        EnableHook(0x1406d3100, (void*)&ShadowMoveTiming::Invoke, (void**)&ShadowMoveTiming::original,
                   "Timing: shadow move", "Timing: shadow move failed (%d)\n");
        EnableHook(0x14069cb50, (void*)&CaptureMoveTiming::Invoke, (void**)&CaptureMoveTiming::original,
                   "Timing: capture move", "Timing: capture move failed (%d)\n");
        EnableHook(0x1401f0aa0, (void*)&StageMoveTiming::Invoke, (void**)&StageMoveTiming::original,
                   "Timing: stage move", "Timing: stage move failed (%d)\n");
        EnableHook(0x140514950, (void*)&DetectTiming::Invoke, (void**)&DetectTiming::original,
                   "Timing: redetect", "Timing: redetect failed (%d)\n");
        EnableHook(0x1409981c1, (void*)&XStateTiming::Invoke, (void**)&XStateTiming::original,
                   "Timing: XInputGetState", "Timing: XInputGetState failed (%d)\n");
        EnableHook(0x140514450, (void*)&XCapsTiming::Invoke, (void**)&XCapsTiming::original,
                   "Timing: XInput capabilities", "Timing: XInput capabilities failed (%d)\n");
        EnableHook(0x140513f10, (void*)&DInputFindTiming::Invoke, (void**)&DInputFindTiming::original,
                   "Timing: DirectInput discovery", "Timing: DirectInput discovery failed (%d)\n");
        EnableHook(0x140510220, (void*)&WmiTiming::Invoke, (void**)&WmiTiming::original,
                   "Timing: input WMI", "Timing: input WMI failed (%d)\n");
        EnableHook(0x140541030, (void*)&hkRenderSyncTiming, (void**)&oRenderSyncTiming,
                   "Timing: render sync", "Timing: render sync failed (%d)\n");
        EnableHook(0x14053a660, (void*)&PlaybackTiming::Invoke, (void**)&PlaybackTiming::original,
                   "Timing: playback", "Timing: playback failed (%d)\n");
        EnableHook(0x14053a570, (void*)&RetireTiming::Invoke, (void**)&RetireTiming::original,
                   "Timing: render retire", "Timing: render retire failed (%d)\n");
        EnableHook(0x14053b5a0, (void*)&CaptureTiming::Invoke, (void**)&CaptureTiming::original,
                   "Timing: render capture", "Timing: render capture failed (%d)\n");
    }
    if constexpr (FORCE_NATIVE_SLI_ON) {
        EnableHook(0x14053bc50, (void*)&hkRenderCreateDevice, (void**)&oRenderCreateDevice,
                   "Renderer: forcing native SLI on", "Renderer: SLI initialization hook failed (%d)\n");
        EnableHook(0x14053f9d0, (void*)&hkRenderSetSli, (void**)&oRenderSetSli,
                   "Renderer: native SLI setter forced on", "Renderer: SLI setter hook failed (%d)\n");
    }
    if constexpr (LAG_BACKEND_TIMING_ENABLED) {
        EnableHook(0x14053b650, (void*)&hkRenderGpuFence, (void**)&oRenderGpuFence,
                   "Timing: native GPU fence (obeys SLI)",
                   "GPU fence: hook failed (%d)\n");
    }
    EnableHook(FN_RENDER_END_FRAME, (void*)&hkCallSRender, (void**)&oCallSRender,
               "Rollback: hooked sRender call",
               "Rollback: failed to hook sRender call (%d)\n");
    if constexpr (FORCE_NATIVE_SLI_ON) {
        EnableHook(0x14053bc50, (void*)&hkRenderCreateDevice, (void**)&oRenderCreateDevice,
                   "Renderer: forcing native SLI on", "Renderer: SLI initialization hook failed (%d)\n");
        EnableHook(0x14053f9d0, (void*)&hkRenderSetSli, (void**)&oRenderSetSli,
                   "Renderer: native SLI setter forced on", "Renderer: SLI setter hook failed (%d)\n");
    }
    EnableHook(FN_SSOUND_MOVE, (void*)&hkSoundMove, (void**)&oSoundMove,
               "Rollback: hooked sSound__move for headless re-simulation",
               "Rollback: failed to hook sSound__move (%d)\n");
    EnableHook(FN_GSOUND_VOICE_DISPATCH, (void*)&hkGSoundVoiceDispatch,
               (void**)&oGSoundVoiceDispatch,
               "Rollback: suppressed replayed sound starts",
               "Rollback: failed to suppress replayed sound starts (%d)\n");
    EnableHook(FN_GSOUND_CREATE_VOICE, (void*)&hkGSoundCreateVoice,
               (void**)&oGSoundCreateVoice,
               "Rollback: suppressed replayed looping voice allocation",
               "Rollback: failed to suppress replayed looping voice allocation (%d)\n");
    EnableHook(FN_SOUND_PLAY_WITH_DB, (void*)&hkSoundPlayWithDb,
               (void**)&oSoundPlayWithDb,
               "Rollback: suppressed replayed raw sound starts",
               "Rollback: failed to suppress replayed raw sound starts (%d)\n");
    EnableHook(FN_SCENE_UPDATE_SOUND_ENV, (void*)&hkSceneUpdateSoundEnv,
               (void**)&oSceneUpdateSoundEnv,
               "Rollback: guarded sound scene model binding",
               "Rollback: failed to guard sound scene model binding (%d)\n");
    EnableHook(FN_SGAMEUI_MOVE, (void*)&hkGameUiMove, (void**)&oGameUiMove,
               "Rollback: hooked sGameUI__move for re-simulation suppression",
               "Rollback: failed to hook sGameUI__move (%d)\n");
    EnableHook(FN_UFADE_MOVE, (void*)&hkFadeMove, (void**)&oFadeMove,
               "Rollback: hooked uFade__move for re-simulation suppression",
               "Rollback: failed to hook uFade__move (%d)\n");
    EnableHook(FN_UBTGAUGEHC_MOVE, (void*)&hkBtGaugeHcMove,
               (void**)&oBtGaugeHcMove,
               "Rollback: guarded uBtGaugeHc__move",
               "Rollback: failed to guard uBtGaugeHc__move (%d)\n");
    EnableHook(FN_UBTGAUGELIFE_MOVE, (void*)&hkBtGaugeLifeMove,
                (void**)&oBtGaugeLifeMove,
                "Rollback: hooked uBtGaugeLife__move for re-simulation suppression",
                "Rollback: failed to hook uBtGaugeLife__move (%d)\n");
    EnableHook(FN_UBTSCORE_UPDATE, (void*)&hkBtScoreUpdate,
               (void**)&oBtScoreUpdate,
               "Rollback: hooked native fighting-name display",
               "Rollback: failed to hook native fighting-name display (%d)\n");
    EnableHook(FN_RCHRCPU_CLASS_FUNC, (void*)&hkRChrCpuClassFunc,
               (void**)&oRChrCpuClassFunc,
               "Rollback: guarded rChrCpu script object cast",
               "Rollback: failed to guard rChrCpu script object cast (%d)\n");
    EnableHook(FN_SMAIN_MOVE, (void*)&hkMainMove, (void**)&oMainMove,
               "Rollback: hooked sMain__move",
               "Rollback: failed to hook sMain__move (%d)\n");
    EnableHook(FN_MENU_OPTION_KBD_MOVE, (void*)&hkMenuOptionKbdMove,
               (void**)&oMenuOptionKbdMove,
               "Rollback: guarded stale keyboard menu move",
               "Rollback: failed to guard keyboard menu move (%d)\n");
    EnableHook(FN_MATERIAL_RESOURCE_STATE, (void*)&hkMaterialResourceState,
                (void**)&oMaterialResourceState,
                "Rollback: guarded material resource state",
                "Rollback: failed to guard material resource state (%d)\n");
    EnableHook(FN_REBUILD_FRUSTUM_PLANES, (void*)&hkRebuildFrustumPlanes,
               (void**)&oRebuildFrustumPlanes,
               "Rollback: isolated camera shake from gameplay bounds",
               "Rollback: failed to isolate camera shake from gameplay bounds (%d)\n");
}
