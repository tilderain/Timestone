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
#include "steam_lobby.h"
#include "../kiero/minhook/include/MinHook.h"

#include <cmath>
#include <cstring>

extern void HookLog(const char* fmt, ...);

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
static void(__fastcall* oMainMove)(uint64_t) = nullptr;
static void(__fastcall* oSoundMove)(uint64_t) = nullptr;
static void(__fastcall* oGSoundVoiceDispatch)(void*, void*, char) = nullptr;
static uint64_t*(__fastcall* oGSoundCreateVoice)(void*, uint64_t*, void*, void*,
                                                 char) = nullptr;
static void(__fastcall* oSoundPlayWithDb)(void*, void*, uint32_t, uint64_t,
                                          uint32_t, float, uint8_t, int) = nullptr;
static void(__fastcall* oSceneUpdateSoundEnv)(uint64_t) = nullptr;
static void(__fastcall* oGameUiMove)(uint64_t) = nullptr;
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
    // Heap-backed native objects can be retired or remapped between frames.
    // Every accessibility helper remains cached within this frame generation.
    InvalidateMemoryQueryCache();
    Rollback_SetMainInstance(main);

    // A late packet can queue a correction just before the synchronized
    // restart packet arrives. Never carry that pre-reset request into the new
    // match, where restoring the old snapshot graph would retire fresh UI and
    // render resources.
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
    original(main);
    Rollback_FreezeWatchdogHeartbeat("native-main-complete", curFrame);

    if (TraceLifecycleFrame(curFrame)) {
        uint64_t replay = *(uint64_t*)_addr(0x140d4d9d8);
        int32_t replayFrame = replay ? *(int32_t*)(replay + 0x54) : -1;
        HookLog("TICKTRACE phase=exit frame=%d resim=0 replay=%d main=0x%llX\n",
                curFrame, replayFrame, main);
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
    oSoundMove(sound);
}

static void __fastcall hkGSoundVoiceDispatch(void* dispatcher, void* voice,
                                              char stop)
{
    // Sound state is not rewound. Replaying a start stacks another audible
    // voice, while stops still need to retire voices started on the live path.
    if (g_isResimulating && stop == 0)
        return;
    oGSoundVoiceDispatch(dispatcher, voice, stop);
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
        // Shot destructors may stop this cookie later. Return the engine's
        // ordinary disabled cRef instead of leaving an uninitialized handle.
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
        valid = valid && IsAccessibleRange(vtable + 0xA0, sizeof(uint64_t));
        uint64_t getMatrix = valid ? *(uint64_t*)(vtable + 0xA0) : 0;
        int32_t jointIndex = *(int32_t*)(scene + JOINT_INDEX_OFFSET);
        uint32_t positionMode = *(uint32_t*)(scene + POSITION_MODE_OFFSET);
        if (!valid || !IsExecutableAddress(getMatrix) ||
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
    RollbackHookProfileScope profile(RollbackHookProfileId::GameUiMove);
    // UI owns live menu/effect objects that are not part of rollback
    // snapshots. Mutating that graph during headless resimulation leaks or
    // retires presentation resources on every correction; let the next live
    // frame advance the native lifecycle instead.
    if (g_isResimulating)
        return;
    oGameUiMove(gameUi);
}

static void __fastcall hkFadeMove(uint64_t fade)
{
    // uFade is presentation state owned by the live sUnit graph. Advancing it
    // during a correction consumes its timer twice and can leave the screen
    // black after the result transition retires the live UI hierarchy.
    if (!g_isResimulating)
        oFadeMove(fade);
}

static void __fastcall hkBtGaugeLifeMove(uint64_t gauge)
{
    // sGameUI is suppressed during re-simulation, so its gauge dependencies
    // are not guaranteed to be initialized for the accompanying sUnit pass.
    if (g_isResimulating)
        return;
    oBtGaugeLifeMove(gauge);
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
    oMainMove(main);
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
    uint64_t vtable = IsAccessiblePointer(object) ? *(uint64_t*)object : 0;
    if (vtable == (uint64_t)_addr(MT_DTI_VTABLE_ADDR)) {
        return;
    }
    oMenuOptionKbdMove(object);
}

static uint64_t __fastcall hkMaterialResourceState(uint64_t material)
{
    if (!IsAccessibleRange(material, 0xA8))
        return 0;

    uint64_t resource = *(uint64_t*)(material + 0xA0);
    if (!resource || !IsAccessibleRange(resource, 0x28))
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

    // Gameplay bounds must use the logical camera, not peer-local screen shake.
    // Restore the shaken matrix immediately so presentation remains unchanged.
    alignas(16) float renderView[16];
    alignas(16) float gameplayView[16];
    void* viewMatrix = (void*)(viewport + VIEWPORT_VIEW_MATRIX_OFFSET);
    std::memcpy(renderView, viewMatrix, sizeof(renderView));
    ((CameraGetViewMatrixFn)getViewMatrix)(camera, gameplayView);
    std::memcpy(viewMatrix, gameplayView, sizeof(gameplayView));
    oRebuildFrustumPlanes(viewport);
    std::memcpy(viewMatrix, renderView, sizeof(renderView));
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

    EnableHook(FN_RENDER_END_FRAME, (void*)&hkCallSRender, (void**)&oCallSRender,
               "Rollback: hooked sRender call",
               "Rollback: failed to hook sRender call (%d)\n");
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
