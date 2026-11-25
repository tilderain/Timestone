#include "lobby_flow.h"
#include "../pch.h"

extern "C" int32_t Controller_FirstRollbackFrame();
extern "C" void Controller_NoteTickStart(int32_t frame);

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
#include "frame_pacing.h"
#include "load_diag.h"
#include "lag_diagnostics.h"
#include "rollback_unit.h"
#include "rollback_sound.h"
#include "steam_lobby.h"
#include "gameplay_hooks.h"
#include "xinput_discovery.h"
#include "mouse_disable.h"

#include <cmath>
#include <cstring>
#include <iterator>
#include <vector>

extern void HookLog(const char* fmt, ...);

static constexpr uintptr_t FN_SMVC3MAIN_MOVE = 0x1402594A0;
static constexpr uintptr_t FN_CCHR_SET_POS = 0x140002AF0;
static constexpr uintptr_t FN_RENDER_END_FRAME = 0x14053A250;
static constexpr uintptr_t FN_CREF_INIT_DISABLED = 0x1400239C0;
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

// The plugin's text along the top of a room match (a spectator's overlay, a
// player's set score), on three of uBtScore's text units that a netplay
// battle leaves hidden: Font_Press_1P (+0xC0), Font_Press_2P (+0xE8) and
// Font_Listen (+0x100) of ui\game\ga_sprite\ga_pos05. The game hides all
// eleven of its units at the top of every update, so one not set here stays
// hidden. Drawn as the lobby's settings screen previews it: the body font at
// the given size (1 = the 32 px cell) with its middle at `y`, centred on `x`
// or ending there. Field offsets and the two text-unit quirks (on-screen
// size goes with mScale squared; a single line hangs from its anchor) as
// lobby_native.cpp.
constexpr uint32_t kMiddleCentre = 4, kMiddleRight = 5; // uBaseSprite::mCenterAttr
static void SetOverlayLine(uint64_t unit, const char* text, float y, float scale, uint32_t rgba,
                           float x = 640.0f, uint32_t anchor = kMiddleCentre)
{
    if (!unit) return;
    const auto setVisible = (void(__fastcall*)(uint64_t, bool))_addr(FN_UI_SET_VISIBLE);
    if (!text || !text[0]) {
        setVisible(unit, false);
        return;
    }
    constexpr uint32_t kAttrShadow = 0x2, kAttrGradient = 0x10;
    ((void(__fastcall*)(uint64_t, const char*))_addr(FN_UI_FONT_SET_TEXT))(unit, text);
    // The anchor is baked into the glyph quads: a change needs a relayout.
    if (*(uint32_t*)(unit + 0x368) != anchor)
        *(int32_t*)(unit + 0x6F0) = *(int32_t*)(unit + 0x594) - 1;
    *(float*)(unit + 0x50) = x;
    *(float*)(unit + 0x54) = y - 16.0f * scale;
    *(float*)(unit + 0x70) = *(float*)(unit + 0x74) = sqrtf(scale);
    *(uint32_t*)(unit + 0x368) = anchor;
    *(uint32_t*)(unit + 0x310) = rgba;
    // One colour, not the layout's shading from mColor to mColor1, and no
    // shadow (the layout's is dark blue at (2, 3)).
    *(uint32_t*)(unit + 0x364) &= ~(kAttrGradient | kAttrShadow);
    *(uint8_t*)(unit + 0x108) = 1;
    setVisible(unit, true);
}

static void DrawTopText(uint64_t score)
{
    char line[160];
    char format[12] = "";
    const char* note = nullptr;
    bool desynced = false;
    // The layout fades its units in over its first 30 frames, writing their
    // position and colour as it goes: nothing is put on them until the name
    // plate (never written here) has reached full alpha.
    const uint64_t namePlate = *(uint64_t*)(score + 0xC8);
    if (!namePlate || *(uint8_t*)(namePlate + 0x313) != 0xFF) return;
    if (!LobbyFlow_IsSpectating()) {
        LobbyFlow_GetScoreText(format, sizeof(format), line, sizeof(line));
    } else if (!LobbyFlow_GetSpectatorOverlayText(line, sizeof(line), &note, &desynced)) {
        line[0] = '\0';
        note = nullptr;
    }
    constexpr uint32_t kWhite = 0xFFFFFFFF, kCyan = 0xFFFFDC78, kRed = 0xFF5A6EFF; // A B G R
    SetOverlayLine(*(uint64_t*)(score + 0xC0), line, 20.0f, 0.68f, kWhite);
    SetOverlayLine(*(uint64_t*)(score + 0xE8), note, 41.0f, 0.5f, desynced ? kRed : kCyan);
    // A player's set format ("FT3"), apart from the score: in the top right,
    // where Font_Listen is laid out (it ends at x 1050).
    SetOverlayLine(*(uint64_t*)(score + 0x100), format, 20.0f, 0.68f, kWhite, 1050.0f,
                   kMiddleRight);
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
    DrawTopText(score);
    // Watching: the two players' names, not ours, and none at all with the
    // spectator overlay set to none.
    if (LobbyFlow_IsSpectating() && LobbyFlow_GetSpectatorOverlay() == 0) {
        for (const uint64_t offset : {0xC8ull, 0xF0ull})
            if (const uint64_t widget = *(uint64_t*)(score + offset))
                ((void(__fastcall*)(uint64_t, bool))_addr(FN_UI_SET_VISIBLE))(widget, false);
        return;
    }
    if (LobbyFlow_IsSpectating() && !LobbyFlow_GetWatchedNames(&p1Name, &p2Name)) {
        p1Name = "Player 1";
        p2Name = "Player 2";
    }
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
        // Success is silent: `success` only names the hook for the reader here.
        GameplayHooks_QueueEnable((LPVOID)_addr(address));
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
    RollbackUnit_SetFrame(g_isResimulating ? g_resimulateCurrentFrame : Controller_GetNetFrameCount(),
                           g_isResimulating);
    RollbackUnit_Flush();
    if (!GameplayHooks_IsActive()) {
        (*g_originalMvc3MainMove)(main);
        return;
    }
    LagDiagnosticScope diagnostic(LagMetric::MainHook);
    if (!g_isResimulating) LiveBenchmark_BeginFrame(Controller_GetNetFrameCount());
    // Discard previous memory checks; native allocations may have changed.
    Rollback_SetMainInstance(main);

    // Discard pending corrections during restart or battle synchronization.
    if (Netplay_IsActive() &&
        (FrameStepper_IsMatchRestartPending() ||
         !FrameStepper_IsBattleSynchronized()))
        Rollback_ClearPendingReSimulation();

    int32_t curFrame = Controller_GetNetFrameCount();
    // Before anything in the tick runs: is this still an intro tick?
    if (!g_isResimulating) Controller_NoteTickStart(curFrame);
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
            // No replay begins in the intro: its ticks are played on neutral
            // input whatever was predicted, and their snapshots hold the
            // intro's camera (controller.cpp).
            if (shouldResim) {
                const int32_t first = Controller_FirstRollbackFrame();
                if (start < first) start = first;
                if (start >= end) shouldResim = false;
            }
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
    // Only the ordinary tick and draw: correction and snapshot work is above.
    const uint64_t benchmarkStart = LiveBenchmark_BeginNative();
    {
        LagDiagnosticScope diagnostic(LagMetric::Native);
        original(main);
    }
    LiveBenchmark_EndFrame(benchmarkStart, Controller_GetNetFrameCount());
    Rollback_FreezeWatchdogHeartbeat("native-main-complete", curFrame);

    if (TraceLifecycleFrame(curFrame)) {
        uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
        int32_t replayFrame = replay ? *(int32_t*)(replay + 0x54) : -1;
        HookLog("TICKTRACE phase=exit frame=%d resim=0 replay=%d main=0x%llX\n",
                curFrame, replayFrame, main);
    }
}

static bool g_undrawnLiveTicks = false;

void Rollback_SetUndrawnLiveTicks(bool enabled)
{
    g_undrawnLiveTicks = enabled;
}

static void __fastcall hkCallSRender(uint64_t render)
{
    LagDiagnosticScope diagnostic(LagMetric::RenderEnd);
    RollbackHookProfileScope profile(RollbackHookProfileId::RenderEndFrame);
    if (!g_isResimulating && !g_undrawnLiveTicks)
        oCallSRender(render);
}

static void __fastcall hkWindowFocus(uint64_t main)
{
    LagDiagnosticScope diagnostic(LagMetric::WindowFocus);
    oWindowFocus(main);
}

static thread_local bool g_timingRender = false;
static void __fastcall hkRenderMove(uint64_t object)
{
    Controller_PublishInputBeforeRender();
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
static void(__fastcall* oDisabledMouseMove)(uint64_t) = nullptr;
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
    // Timing-only callsites; native mouse move is separately disabled.
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
    // Obtain write permission for all callsites before changing any instruction.
    constexpr size_t span = 0x1406aaec0 - 0x1406aa1e4;
    DWORD previous;
    if (!VirtualProtect((void*)origin, span, PAGE_EXECUTE_READWRITE, &previous)) {
        HookLog("Input calls: could not make call sites writable\n");
        return;
    }
    for (size_t i = 0; i < std::size(sites); ++i) {
        const auto& site = sites[i];
        auto* address = (uint8_t*)_addr(site.address);
        targets[i] = site.replacement;
        const int32_t displacement = (int32_t)((intptr_t)&targets[i] - (intptr_t)(address + 6));
        uint8_t patch[8] = {0xff, 0x15, 0, 0, 0, 0, 0x90, 0x90};
        std::memcpy(patch + 2, &displacement, sizeof(displacement));
        GameplayHooks_RecordPatch(address, site.size);
        std::memcpy(address, patch, site.size);
    }
    DWORD ignored;
    const bool restored = VirtualProtect((void*)origin, span, previous, &ignored);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), (void*)origin, span);
    if (!restored || !flushed)
        HookLog("Input calls: protection/cache finalization failed\n");
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


// ---------------------------------------------------------------------------
// Battle theme (BGM) under rollback
//
// The theme is driven by sAction state: current cue +0x200, change stamp
// +0x208 (sMain::mTimer, 100-tick throttle), pending byte +0x210 and Wright's
// per-team override bits +0x214 (cue 0x79). Those fields are rewound with the
// rest of sAction. The BGM handle cRef at +0x1F0 stays live.
//
// During resim the theme functions run for their state only: playSystemCue is
// a no-op there, since natively it first stops every other theme and the
// rollback sound ledger never sees its start replayed. After resim the
// cue the corrected timeline wants is compared with the cue actually playing
// and, if different, started once through the native theme request.
// ---------------------------------------------------------------------------
static constexpr uintptr_t FN_GSOUND_PLAY_SYSTEM_CUE = 0x1402AC5C0;
static constexpr uintptr_t FN_SACTION_REQUEST_THEME = 0x140003D60;
static constexpr uintptr_t FN_SACTION_SET_THEME_OVERRIDE = 0x140003EA0;
static constexpr uintptr_t FN_SACTION_PLAY_INITIAL_THEME = 0x14000B570;
static constexpr uintptr_t PTR_SACTION_THEME = 0x140d47e68;
static constexpr uintptr_t PTR_SMAIN_THEME = 0x140e177e8;
static constexpr uint32_t SMAIN_TIMER_OFFSET = 0x38;
static constexpr uint32_t SACTION_BGM_REF_OFFSET = 0x1F0;   // cRef {vtable, value}
static constexpr uint32_t SACTION_THEME_CUE_OFFSET = 0x200;
static constexpr uint32_t SACTION_THEME_STAMP_OFFSET = 0x208;
static constexpr uint32_t SACTION_THEME_OVERRIDE_OFFSET = 0x214;
static constexpr uint32_t THEME_OVERRIDE_CUE = 0x79;
static constexpr uint64_t THEME_THROTTLE_TICKS = 0x65;

using PlaySystemCueFn = uint64_t*(__fastcall*)(void*, uint64_t*, uint32_t, int, char, char);
using RequestThemeFn = void(__fastcall*)(uint64_t, int);
using SetThemeOverrideFn = void(__fastcall*)(uint64_t, uint8_t, char, int);
using PlayInitialThemeFn = void(__fastcall*)(uint64_t);
static PlaySystemCueFn oPlaySystemCue = nullptr;
static RequestThemeFn oRequestTheme = nullptr;
static SetThemeOverrideFn oSetThemeOverride = nullptr;
static PlayInitialThemeFn oPlayInitialTheme = nullptr;

static uint32_t g_lastSystemCue = 0;
static uint32_t g_audibleTheme = 0;      // cue last started into sAction+0x1F0 on a live frame
static bool g_audibleThemeKnown = false;
static uint8_t g_resimBgmRef[16] = {};
static bool g_resimBgmRefSaved = false;

static uint64_t ThemeAction() { return *(uint64_t*)_addr(PTR_SACTION_THEME); }
static uint64_t BgmRefValue(uint64_t action)
{
    return *(uint64_t*)(action + SACTION_BGM_REF_OFFSET + 8);
}
static uint32_t DesiredThemeCue(uint64_t action)
{
    return *(uint32_t*)(action + SACTION_THEME_OVERRIDE_OFFSET)
        ? THEME_OVERRIDE_CUE
        : *(uint32_t*)(action + SACTION_THEME_CUE_OFFSET);
}

static uint64_t* __fastcall hkPlaySystemCue(void* sound, uint64_t* out, uint32_t cue,
                                            int channel, char flag5, char flag6)
{
    if (g_isResimulating) {
        return ((uint64_t*(__fastcall*)(uint64_t*, uint64_t))
            _addr(FN_CREF_INIT_DISABLED))(out, UINT64_MAX);
    }
    if (LobbyFlow_ShouldSkipSystemCue(cue)) {
        return ((uint64_t*(__fastcall*)(uint64_t*, uint64_t))
            _addr(FN_CREF_INIT_DISABLED))(out, UINT64_MAX);
    }
    g_lastSystemCue = cue;
    // The theme reconcile owns music: keep it out of the rollback sound ledger.
    RollbackSoundPresentationScope presentation;
    return oPlaySystemCue(sound, out, cue, channel, flag5, flag6);
}

// Record which theme a live theme function actually started.
template <typename Call>
static void TrackLiveTheme(uint64_t action, Call call)
{
    if (g_isResimulating || !action) {
        call();
        return;
    }
    const uint64_t before = BgmRefValue(action);
    g_lastSystemCue = 0;
    call();
    if (BgmRefValue(action) != before && g_lastSystemCue) {
        g_audibleTheme = g_lastSystemCue;
        g_audibleThemeKnown = true;
    }
}

static void __fastcall hkRequestTheme(uint64_t action, int cue)
{
    TrackLiveTheme(action, [&] { oRequestTheme(action, cue); });
}

static void __fastcall hkSetThemeOverride(uint64_t action, uint8_t team, char enable, int cue)
{
    TrackLiveTheme(action, [&] { oSetThemeOverride(action, team, enable, cue); });
}

static void __fastcall hkPlayInitialTheme(uint64_t action)
{
    TrackLiveTheme(action, [&] { oPlayInitialTheme(action); });
}

void Rollback_BeginThemeResim()
{
    uint64_t action = ThemeAction();
    g_resimBgmRefSaved = action != 0;
    if (action)
        std::memcpy(g_resimBgmRef, (void*)(action + SACTION_BGM_REF_OFFSET), sizeof(g_resimBgmRef));
}

void Rollback_EndThemeResim()
{
    uint64_t action = ThemeAction();
    if (!action || !g_resimBgmRefSaved || !oRequestTheme || !oSetThemeOverride) return;
    g_resimBgmRefSaved = false;
    // Resim requests wrote disabled handles; the live track is still playing.
    std::memcpy((void*)(action + SACTION_BGM_REF_OFFSET), g_resimBgmRef, sizeof(g_resimBgmRef));

    const uint32_t desired = DesiredThemeCue(action);
    if (!g_audibleThemeKnown || !desired || desired == g_audibleTheme) return;

    uint64_t sMain = *(uint64_t*)_addr(PTR_SMAIN_THEME);
    if (!sMain) return;
    uint32_t* cue = (uint32_t*)(action + SACTION_THEME_CUE_OFFSET);
    uint64_t* stamp = (uint64_t*)(action + SACTION_THEME_STAMP_OFFSET);
    uint32_t* overrideBits = (uint32_t*)(action + SACTION_THEME_OVERRIDE_OFFSET);
    const uint32_t savedCue = *cue;
    const uint32_t savedOverride = *overrideBits;
    const uint64_t savedStamp = *stamp;

    // Bypass dedup and throttle so the native request starts the track (and
    // stops the stale one) exactly as a live change would.
    *stamp = *(uint64_t*)(sMain + SMAIN_TIMER_OFFSET) - THEME_THROTTLE_TICKS - 1;
    *overrideBits = 0;
    HookLog("Rollback: BGM reconcile playing=0x%X desired=0x%X\n", g_audibleTheme, desired);
    if (savedOverride) {
        uint8_t team = (uint8_t)__builtin_ctz(savedOverride);
        hkSetThemeOverride(action, team, 1, 0);
    } else {
        *cue = UINT32_MAX;
        hkRequestTheme(action, (int)desired);
    }
    *overrideBits = savedOverride;
    *cue = savedCue;
    if (BgmRefValue(action) == *(uint64_t*)(g_resimBgmRef + 8))
        *stamp = savedStamp; // the request declined (mode or flag gate)
}

// ---------------------------------------------------------------------------
// Round-end presentation under rollback
//
// The battle UI does not run in resim and reacts to edges once, on live frames:
// uBattleUi__moveAfter turns sAction's step 6/7 into the K.O./time-over banner
// (which queues the announcer) and kills the combo counter, and uFinishScreen
// marks each defeated fighter for its finish portrait. Neither is undone when
// a predicted KO rolls back. Hold both until the frame on which the state
// began has confirmed remote input; the edge then fires once, a few frames
// late. Presentation only: no simulation state is touched.
// ---------------------------------------------------------------------------
static constexpr uintptr_t FN_UBATTLEUI_MOVE_AFTER = 0x1402D2410;
static constexpr uintptr_t FN_UBATTLEUI_EFFECTIVE_STEP = 0x1402D33C0;
static constexpr uintptr_t FN_UFINISHSCREEN_ASSIGN_SLOT = 0x14031BCB0;
static constexpr uintptr_t FN_CCHR_IS_MOVE_ACTIVE = 0x140056290;
static constexpr uintptr_t FN_UCHARACTER_CHECK_DEFEAT = 0x140056380;
static constexpr uintptr_t PTR_SACTION_PRESENTATION = 0x140d47e68;
static constexpr uintptr_t PTR_SCHARACTER_PRESENTATION = 0x140d44a70;
static constexpr uint32_t SACTION_STEP_OFFSET = 0xD8;
static constexpr uint32_t UBATTLEUI_CACHED_STEP_OFFSET = 0x4C;
static constexpr uint32_t CCHR_DEFAULT_MEMBER_OFFSET = 0x2DAC;
// Fallback so a gap in confirmation can never hide the result for good.
static constexpr int32_t PRESENTATION_CONFIRM_TIMEOUT = 120;

struct BattlePresentationFrame {
    int32_t frame = -1;
    int32_t step = 0;
    uint32_t defeated = 0; // bit team * 3 + default member id
};
static BattlePresentationFrame g_presentation[256];

using UiMoveFn = void(__fastcall*)(uint64_t);
using AssignSlotFn = void(__fastcall*)(uint64_t, int, int, uint64_t);
static UiMoveFn oBattleUiMoveAfter = nullptr;
static AssignSlotFn oFinishScreenAssignSlot = nullptr;

void Rollback_RecordBattlePresentation(int32_t frame)
{
    if (frame < 0) return;
    uint64_t action = *(uint64_t*)_addr(PTR_SACTION_PRESENTATION);
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER_PRESENTATION);
    if (!action || !sCharacter) return;
    auto isMoveActive = (bool(__fastcall*)(uint64_t))_addr(FN_CCHR_IS_MOVE_ACTIVE);
    auto isDefeated = (bool(__fastcall*)(uint64_t))_addr(FN_UCHARACTER_CHECK_DEFEAT);
    uint32_t defeated = 0;
    for (int team = 0; team < 2; ++team) {
        uint64_t node = sCharacter + (team == 0 ? 0x40 : 0x300) + 0x08;
        for (int member = 0; node && member < 64; ++member, node = *(uint64_t*)(node + 0x10)) {
            uint64_t character = *(uint64_t*)(node + 0x08);
            if (!character) continue;
            int32_t id = *(int32_t*)(character + CCHR_DEFAULT_MEMBER_OFFSET);
            if (id < 0 || id > 2 || !isMoveActive(character) || !isDefeated(character)) continue;
            defeated |= 1u << (team * 3 + id);
        }
    }
    BattlePresentationFrame& entry = g_presentation[(uint32_t)frame % 256];
    entry.frame = frame;
    entry.step = *(int32_t*)(action + SACTION_STEP_OFFSET);
    entry.defeated = defeated;
}

// True once the run of frames ending at `frame` in which `holds` is true began
// on a confirmed frame. Remote input arrives in order, so that frame and every
// earlier one are final.
template <typename Holds>
static bool PresentationConfirmed(int32_t frame, Holds holds)
{
    int32_t start = frame;
    for (int i = 0; i < 255; ++i) {
        const int32_t previous = start - 1;
        const BattlePresentationFrame& entry = g_presentation[(uint32_t)previous % 256];
        if (previous < 0 || entry.frame != previous || !holds(entry)) break;
        start = previous;
    }
    return Controller_IsFrameConfirmed(start) || frame - start >= PRESENTATION_CONFIRM_TIMEOUT;
}

static bool HoldPresentation()
{
    return Netplay_IsActive() && !g_isResimulating && Controller_GetExecutingFrame() >= 0;
}

// Live frames have no resim record; the first presentation hook of the frame
// takes it, after this frame's simulation lanes have run.
static void RecordLivePresentation(int32_t frame)
{
    if (g_presentation[(uint32_t)frame % 256].frame != frame)
        Rollback_RecordBattlePresentation(frame);
}

static void __fastcall hkBattleUiMoveAfter(uint64_t ui)
{
    if (HoldPresentation()) {
        const int32_t frame = Controller_GetExecutingFrame();
        RecordLivePresentation(frame);
        const int32_t step = ((int32_t(__fastcall*)(uint64_t))_addr(FN_UBATTLEUI_EFFECTIVE_STEP))(ui);
        if (step != *(int32_t*)(ui + UBATTLEUI_CACHED_STEP_OFFSET) && (step == 6 || step == 7) &&
            !PresentationConfirmed(frame, [step](const BattlePresentationFrame& e) { return e.step == step; })) {
            return;
        }
    }
    oBattleUiMoveAfter(ui);
}

static void __fastcall hkFinishScreenAssignSlot(uint64_t screen, int team, int member, uint64_t character)
{
    if (HoldPresentation() && team >= 0 && team < 2 && member >= 0 && member < 3) {
        const uint32_t bit = 1u << (team * 3 + member);
        const int32_t frame = Controller_GetExecutingFrame();
        RecordLivePresentation(frame);
        const BattlePresentationFrame& current = g_presentation[(uint32_t)frame % 256];
        if (current.frame == frame && (current.defeated & bit) &&
            !PresentationConfirmed(frame, [bit](const BattlePresentationFrame& e) { return (e.defeated & bit) != 0; })) {
            return; // retried next frame: the slot bit stays clear
        }
    }
    oFinishScreenAssignSlot(screen, team, member, character);
}

// Backstop, to be removed. The cause is handled in rollback_sound.cpp
// (Rollback_DropDeadSoundTargets, Rollback_ForgetSoundTarget): a resim runs
// without sSound::move, so the game never saw the followed unit die. This
// stays for a release to show, by its log line, whether anything still gets
// past that. It detaches a target that is neither a model nor an effect,
// which is what a freed and reused address looks like.
static void __fastcall hkSceneUpdateSoundEnv(uint64_t scene)
{
    constexpr uint32_t MODEL_OFFSET = 0x80;
    constexpr uint32_t JOINT_INDEX_OFFSET = 0x88;
    constexpr uint32_t POSITION_MODE_OFFSET = 0xC0;

    uint64_t* modelSlot = (uint64_t*)(scene + MODEL_OFFSET);
    uint64_t model = *modelSlot;
    if (model) {
        // Both families implement vtable +0xA0 as getWorldMatrix(joint):
        // uModel (0x140566330) and uEffect (0x1405E5470). Voices follow
        // effects too (a uGameEffect was detached here, log of 2026-11-23).
        bool valid = IsBaseModelObject((void*)model) || IsEffectObject((void*)model);
        uint64_t vtable = valid ? *(uint64_t*)model : 0;
        uint64_t getMatrix = valid ? *(uint64_t*)(vtable + 0xA0) : 0;
        int32_t jointIndex = *(int32_t*)(scene + JOINT_INDEX_OFFSET);
        uint32_t positionMode = *(uint32_t*)(scene + POSITION_MODE_OFFSET);
        // A negative joint is valid: uModel::getWorldMatrix (0x140566330)
        // then returns the object's own matrix. Rejecting it detached live
        // characters and shots (uHawkEye, uVergil, uShotVulcan..., log of
        // 2026-11-23), leaving their voices at a stale position.
        if (!valid || !getMatrix) {
            // Rollback_DropDeadSoundTargets should leave nothing for this to
            // catch. Logged at the 1st, 2nd, 4th, ... hit only: a line per hit
            // stalled the sound update for 11.5 ms in a lag capture.
            static uint32_t hits = 0;
            if ((++hits & (hits - 1)) == 0) {
                HookLog("Rollback: sound guard detached a voice target (hit %u) "
                        "object=0x%llX vtable=0x%llX mode=%u joint=%d\n",
                        hits, model, valid ? vtable : *(uint64_t*)model,
                        positionMode, jointIndex);
            }
            *modelSlot = 0;
        }
    }
    oSceneUpdateSoundEnv(scene);
}

// ---------------------------------------------------------------------------
// Battle system messages ("DOWN", "FIRST ATTACK", ...)
//
// Gameplay creates them directly: e.g. uCharacterPost__processDefeatTransitions
// -> sActionTime__setTeamDefeatState -> sGameUi__createBattleSystemMessage
// (team, 2 = DOWN). The message unit (uBtUiSubSysMsg, line 0) shows its text
// and queues its announcer lines from its setup on the next live frame. A
// predicted message survives the rollback that undoes it, and a replayed one
// is created again. Hold every creation on a frame that can still roll back,
// drop held ones a rollback replaces, and create them once confirmed.
// ---------------------------------------------------------------------------
static constexpr uintptr_t FN_SGAMEUI_CREATE_BATTLE_MESSAGE = 0x1402BF770;
using CreateBattleMessageFn = void(__fastcall*)(uint64_t, int, int);
static CreateBattleMessageFn oCreateBattleMessage = nullptr;

struct HeldBattleMessage { int32_t frame; int team; int text; };
static std::vector<HeldBattleMessage> g_heldBattleMessages;

static bool BattleMessageMayRollBack(int32_t frame)
{
    return Controller_IsRollbackChangePending() ||
           (!Controller_IsFrameConfirmed(frame) &&
            Controller_GetExecutingFrame() - frame < ROLLBACK_WINDOW);
}

static void __fastcall hkCreateBattleMessage(uint64_t gameUi, int team, int text)
{
    if (!Netplay_IsActive()) {
        oCreateBattleMessage(gameUi, team, text);
        return;
    }
    const int32_t frame = g_isResimulating ? g_resimulateCurrentFrame
                                           : Controller_GetExecutingFrame();
    if (!g_isResimulating && !BattleMessageMayRollBack(frame)) {
        oCreateBattleMessage(gameUi, team, text);
        return;
    }
    g_heldBattleMessages.push_back({frame, team, text});
}

void Rollback_DiscardHeldBattleMessages(int32_t fromFrame)
{
    std::erase_if(g_heldBattleMessages,
                  [fromFrame](const HeldBattleMessage& m) { return m.frame >= fromFrame; });
}

static void ReleaseHeldBattleMessages(uint64_t gameUi)
{
    size_t kept = 0;
    for (const HeldBattleMessage& message : g_heldBattleMessages) {
        if (BattleMessageMayRollBack(message.frame))
            g_heldBattleMessages[kept++] = message;
        else
            oCreateBattleMessage(gameUi, message.team, message.text);
    }
    g_heldBattleMessages.resize(kept);
}

static void __fastcall hkGameUiMove(uint64_t gameUi)
{
    LagDiagnosticScope diagnostic(LagMetric::GameUiMove);
    RollbackHookProfileScope profile(RollbackHookProfileId::GameUiMove);
    // Advance unsnapshotted UI objects only on live frames.
    if (g_isResimulating)
        return;
    if (!g_heldBattleMessages.empty()) ReleaseHeldBattleMessages(gameUi);
    oGameUiMove(gameUi);
}

// ---------------------------------------------------------------------------
// Rocket Raccoon's tail chain is disabled on live frames and in resim.
//
// Its solver (uChrChain move 0x140035F90 / moveAfter 0x140036130) drives the
// tail joints behind attack collision sets 11 and 34, but its node state is
// not part of any snapshot, so every rollback resumed it from the abandoned
// future. Resim already skips all uChrChain objects (rollback_tick.cpp); this
// stops Rocket's on live frames too, so both peers agree. A dead or missing
// owner still runs the native move, which kills the chain.
// See doc/rocket_chain_rollback.md for restoring it with save/restore instead.
// ---------------------------------------------------------------------------
static constexpr uintptr_t FN_UCHRCHAIN_MOVE = 0x140035F90;
static constexpr uintptr_t FN_UCHRCHAIN_MOVE_AFTER = 0x140036130;
static constexpr uintptr_t FN_MTOBJECT_IS_ALIVE = 0x140001B80;
static constexpr uintptr_t DTI_UROCKET_RACCOON = 0x140D459D8;
static constexpr uint32_t UCHRCHAIN_OWNER_OFFSET = 0x1320; // read by 0x140035DC0
static void(__fastcall* oChrChainMove)(uint64_t) = nullptr;
static void(__fastcall* oChrChainMoveAfter)(uint64_t) = nullptr;

static bool IsRocketTailChain(uint64_t chain)
{
    const uint64_t owner = *(uint64_t*)(chain + UCHRCHAIN_OWNER_OFFSET);
    return owner && ((bool(__fastcall*)(uint64_t))_addr(FN_MTOBJECT_IS_ALIVE))(owner) &&
           GetObjectDti(owner) == (uint64_t)_addr(DTI_UROCKET_RACCOON);
}

static void __fastcall hkChrChainMove(uint64_t chain)
{
    if (!IsRocketTailChain(chain)) oChrChainMove(chain);
}

static void __fastcall hkChrChainMoveAfter(uint64_t chain)
{
    if (!IsRocketTailChain(chain)) oChrChainMoveAfter(chain);
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

// sMain::move paces a frame with a loop of Sleep(1) until its real counter
// (+0x60) reaches the ideal one (+0x68): each frame then starts up to a
// millisecond or so late, by a different amount every time (measured: frame
// starts 15.7 to 17.7 ms apart). This waits out the same interval first, to
// the tick (FramePacing_WaitUntil: a timer sleep, then a short spin). The
// native loop then finds the time already up and does not sleep.
static void PreciseFrameWait(uint64_t main)
{
    if (!*(uint8_t*)(main + 0x58)) return;            // mFrameWait: fixed frame timing only
    const int64_t real = *(int64_t*)(main + 0x60);    // mRealCounter
    const int64_t ideal = *(int64_t*)(main + 0x68);   // mIdealCounter
    const int64_t previous = *(int64_t*)(main + 0x70); // mPrevCounter: QPC at the last update
    const float fps = *(float*)(main + 0x50);         // mFps scales elapsed ticks into the counters
    if (previous <= 0 || fps < 1.0f || ideal <= real) return;
    static int64_t frequency = 0;
    if (!frequency) {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        frequency = value.QuadPart;
    }
    // Ticks until the native sum reaches the ideal counter, and a little
    // over, so its float arithmetic cannot land just short and sleep again.
    const int64_t remaining = (int64_t)((double)(ideal - real) / (double)fps) + frequency / 20000;
    if (remaining > frequency / 20) return;           // more than 50 ms: not ordinary pacing
    FramePacing_WaitUntil(previous + remaining);
}

// Whole frame times to add before the next paced frame starts.
static int g_delayFrames = 0;
void Rollback_DelayNextFrame(int frames)
{
    if (frames > 0) g_delayFrames += frames;
}

static void __fastcall hkMainMove(uint64_t main)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::MainMove);
    if (g_isResimulating) {
        *(uint32_t*)(main + 0x48) = (uint32_t)g_resimulateCurrentFrame;
        *(float*)(main + 0x80) = 1.0f;
        return;
    }
    if (g_undrawnLiveTicks) {
        // No pacing: the frame stamp and step a paced tick ends up with.
        const int32_t frame = Controller_GetNetFrameCount();
        if (frame >= 0)
            *(uint32_t*)(main + 0x48) = (uint32_t)frame;
        *(float*)(main + 0x80) = 1.0f;
        return;
    }
    // Time sync (controller.cpp): while this game runs ahead of the
    // opponent's, each frame is asked to start a little later. In fixed frame
    // timing sMain::move waits until its real counter reaches the ideal one
    // (+0x68), and one frame is one second's worth of counter ticks on both;
    // moving the ideal counter on by a fraction of that lengthens this frame
    // by the same fraction, with no frame held or dropped.
    if (*(uint8_t*)(main + 0x58)) {
        const float stretch = Controller_GetFrameStretch();
        if (stretch > 0.0f) {
            static int64_t ticksPerFrame = 0;
            if (!ticksPerFrame) {
                LARGE_INTEGER frequency;
                QueryPerformanceFrequency(&frequency);
                ticksPerFrame = frequency.QuadPart;
            }
            *(int64_t*)(main + 0x68) += (int64_t)((double)stretch * (double)ticksPerFrame);
        }
    }
    if (g_delayFrames > 0) {
        // The ideal counter moves one second's worth of ticks per frame
        // (see the time sync note above): a frame's worth more is one frame
        // time more to wait, and the frames after it keep the new schedule.
        if (*(uint8_t*)(main + 0x58)) {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            *(int64_t*)(main + 0x68) += (int64_t)g_delayFrames * frequency.QuadPart;
        }
        g_delayFrames = 0;
    }
    const uint64_t benchmarkStart = LiveBenchmark_Now();
    {
        LagDiagnosticScope diagnostic(LagMetric::Pacing);
        PreciseFrameWait(main);
        oMainMove(main);
    }
    LiveBenchmark_EndPacing(benchmarkStart);
    FramePacing_OnFrameStart();
    LoadDiag_Tick();
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
    RollbackUnit_Install();
    XInputDiscovery_Init();
    if (!DisableNativeNetworkFrameUpdates())
        HookLog("Native network: component/agent/ranking/room frame updates patch FAILED\n");
    g_originalMvc3MainMove = originalMove;
    EnableHook(FN_SMVC3MAIN_MOVE, (void*)&hkMvc3MainMove, (void**)originalMove,
               "Rollback: hooked sMvc3Main__move",
               "Rollback: failed to hook sMvc3Main__move (%d)\n");

    MH_STATUS status = GameplayHooks_Create((LPVOID)_addr(FN_CCHR_SET_POS),
                                     (LPVOID)&hkCChrSetPos, (LPVOID*)&oCChrSetPos);
    if (status != MH_OK)
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
    EnableHook(0x1406aa990, (void*)&MouseDisable_Move, (void**)&oDisabledMouseMove,
               "Mouse: native movement/polling disabled", "Mouse: disable hook failed (%d)\n");
    if constexpr (LAG_BACKEND_TIMING_ENABLED) {
        InstallInputCallTiming();
        EnableHook(0x1406aa1b0, (void*)&KeyboardStateTiming::Invoke, (void**)&KeyboardStateTiming::original,
                   "Timing: keyboard state", "Timing: keyboard state failed (%d)\n");
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
    EnableHook(FN_GSOUND_PLAY_SYSTEM_CUE, (void*)&hkPlaySystemCue,
               (void**)&oPlaySystemCue,
               "Rollback: hooked system cue for BGM resim",
               "Rollback: failed to hook system cue (%d)\n");
    EnableHook(FN_SACTION_REQUEST_THEME, (void*)&hkRequestTheme,
               (void**)&oRequestTheme,
               "Rollback: hooked battle theme request",
               "Rollback: failed to hook battle theme request (%d)\n");
    EnableHook(FN_SACTION_SET_THEME_OVERRIDE, (void*)&hkSetThemeOverride,
               (void**)&oSetThemeOverride,
               "Rollback: hooked battle theme override",
               "Rollback: failed to hook battle theme override (%d)\n");
    EnableHook(FN_SACTION_PLAY_INITIAL_THEME, (void*)&hkPlayInitialTheme,
               (void**)&oPlayInitialTheme,
               "Rollback: hooked round-start theme",
               "Rollback: failed to hook round-start theme (%d)\n");
    // Still needed: with this off (0.6.5), a voice kept following a freed
    // object whose memory a uUiSprite3D had taken, and the position update
    // called that sprite's vtable +0xA0 (crash of 2026-11-22).
    EnableHook(FN_SCENE_UPDATE_SOUND_ENV, (void*)&hkSceneUpdateSoundEnv,
               (void**)&oSceneUpdateSoundEnv,
               "Rollback: guarded sound scene model binding",
               "Rollback: failed to guard sound scene model binding (%d)\n");
    EnableHook(FN_SGAMEUI_MOVE, (void*)&hkGameUiMove, (void**)&oGameUiMove,
               "Rollback: hooked sGameUI__move for re-simulation suppression",
               "Rollback: failed to hook sGameUI__move (%d)\n");
    EnableHook(FN_SGAMEUI_CREATE_BATTLE_MESSAGE, (void*)&hkCreateBattleMessage,
               (void**)&oCreateBattleMessage,
               "Rollback: holding battle system messages until confirmed",
               "Rollback: failed to hook sGameUi__createBattleSystemMessage (%d)\n");
    EnableHook(FN_UBATTLEUI_MOVE_AFTER, (void*)&hkBattleUiMoveAfter,
               (void**)&oBattleUiMoveAfter,
               "Rollback: holding K.O. banner until confirmed",
               "Rollback: failed to hook uBattleUi__moveAfter (%d)\n");
    EnableHook(FN_UFINISHSCREEN_ASSIGN_SLOT, (void*)&hkFinishScreenAssignSlot,
               (void**)&oFinishScreenAssignSlot,
               "Rollback: holding finish portraits until confirmed",
               "Rollback: failed to hook uFinishScreen slot assignment (%d)\n");
    EnableHook(FN_UCHRCHAIN_MOVE, (void*)&hkChrChainMove, (void**)&oChrChainMove,
               "Rollback: disabled Rocket Raccoon's tail chain move",
               "Rollback: failed to hook uChrChain__move (%d)\n");
    EnableHook(FN_UCHRCHAIN_MOVE_AFTER, (void*)&hkChrChainMoveAfter, (void**)&oChrChainMoveAfter,
               "Rollback: disabled Rocket Raccoon's tail chain solver",
               "Rollback: failed to hook uChrChain__moveAfter (%d)\n");
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
    LoadDiag_Install();
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
