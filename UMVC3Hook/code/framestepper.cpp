// framestepper.cpp
// Frame advance mod for UMvC3 training mode.
// Ported from umvc3_framestep by Gneiss and EternalYoshi.
#include "../pch.h"
#include "framestepper.h"
#include "mk10utils.h"       // _addr
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include <Windows.h>
#include <d3d9.h>
#include "../imgui/imgui.h"
#include "../kiero/minhook/include/MinHook.h"
#include "netplay.h"
#include "LaunchMenu.h"
#include "rollback.h"
#include "controller.h"
#include "autotest.h"
#include "rollback_cleanup.h"

// Netplay debug info from controller.cpp
extern "C" int32_t Controller_GetDelayFrames();
extern "C" int32_t Controller_GetNetFrameCount();
extern "C" int32_t Controller_GetLastRemoteFrame();
extern "C" int32_t Controller_GetRollbackFrames();
extern "C" uint32_t Controller_GetExpectedInput(uint32_t teamIdx, int32_t frame);

// Multi-resimulation toggle from controller.cpp
extern bool g_multiResimEnabled;

// Desync detection flags from controller.cpp
extern bool     g_desyncDetected;
extern bool     g_desyncSeen;
extern int32_t  g_desyncFrame;
extern uint32_t g_desyncLocalCS;
extern uint32_t g_desyncRemoteCS;

using namespace Memory::VP;

// HookLog from dllmain.cpp
extern void HookLog(const char* fmt, ...);

// Netplay state queries from netplay.cpp
extern bool Netplay_HasRemoteInput();
extern bool Netplay_GetInboundDelay(int32_t& outDelay, int32_t& outEffectiveFrame);
extern bool Netplay_GetInboundRollback(int32_t& outRollback);
extern void Netplay_SendDelay(int32_t delay, int32_t effectiveFrame);
extern void Netplay_SendRollback(int32_t rollbackFrames);
extern bool Netplay_GetInboundRestartMatch(int32_t& outTargetFrame, uint32_t& outRoundSeed,
                                           uint32_t& outKind, int32_t& outButtonRestart);
extern void Netplay_SendRestartMatch(int32_t targetFrame, uint32_t roundSeed);

// Controller setters from controller.cpp
extern "C" void Controller_SetDelayFrames(int32_t delay);
extern "C" void Controller_ScheduleDelayFrames(int32_t delay, int32_t effectiveFrame);
extern "C" void Controller_SetRollbackFrames(int32_t n);
extern "C" void Controller_SetMultiResimEnabled(bool enabled);
extern "C" void Controller_ResetBuffers();
extern "C" void Controller_ResetFrameEpoch();

// True after both peers have reached the post-initialization intro barrier.
static bool g_battleSynchronized = false;
static int32_t g_restartTargetFrame = -1;
static uint32_t g_restartRoundSeed = 0;
static uint32_t g_restartKind = RESTART_FULL_MATCH;
static int32_t g_restartButtonMode = 0;
static bool g_localReady = false;
static bool g_remoteReady = false;
static bool g_localRelease = false;
static bool g_remoteRelease = false;
static bool g_roundSeedReady = false;
static uint32_t g_roundSeed = 0;
// Generation 0 is the pre-battle setup epoch. The first mutually released
// battle advances to generation 1, just as every restart advances before its
// replacement timeline becomes active.
static uint32_t g_battleGeneration = 0;
static uint32_t g_advertisedBattleFlags = 0;
static uint32_t g_battleAdvertiseTick = 0;
static bool g_trainingRestartBarrier = false;
static bool g_trainingRestartLeftFighting = false;
static bool g_localFighting = false;
static bool g_remoteFighting = false;
static bool g_desyncCheckReady = false;
static bool g_autoTestLaunchPending = false;
static bool g_autoTestLaunchRunning = false;
static DWORD g_autoTestExitTick = 0;
extern "C" bool FrameStepper_IsBattleSynchronized() { return g_battleSynchronized; }
extern "C" uint32_t FrameStepper_GetBattleGeneration() { return g_battleGeneration; }
extern "C" bool FrameStepper_IsDesyncCheckReady() { return g_desyncCheckReady; }
extern "C" void FrameStepper_NotifyFighting()
{
    if (!g_battleSynchronized || g_localFighting)
        return;

    g_localFighting = true;
    g_advertisedBattleFlags = 0;
    g_battleAdvertiseTick = 30;
    HookLog("Netplay: local fighting state reached (generation=%u frame=%d)\n",
            g_battleGeneration, Controller_GetNetFrameCount());
}
extern "C" bool FrameStepper_IsMatchRestartPending()
{
    return g_restartTargetFrame >= 0 || g_trainingRestartBarrier;
}

extern "C" void FrameStepper_ScheduleAutoTestRestart(uint32_t roundSeed)
{
    if (!Netplay_IsActive() || !Netplay_IsHost() || g_restartTargetFrame >= 0)
        return;
    g_restartRoundSeed = roundSeed;
    g_restartKind = RESTART_TRAINING;
    g_restartButtonMode = 12;
    g_restartTargetFrame = Controller_GetNetFrameCount() + 25;
    Netplay_SendTrainingRestart(g_restartTargetFrame, g_restartRoundSeed,
                                g_restartButtonMode);
    HookLog("AutoTestNet: synchronized mode-12 training restart scheduled for frame %d seed=0x%08X\n",
            g_restartTargetFrame, g_restartRoundSeed);
}

static void ScheduleNetplayDelay(int32_t delay)
{
    if (delay < 1) delay = 1;
    if (delay > 15) delay = 15;

    // Give the reliable control packet one RTT to arrive before both peers
    // establish the synchronized neutral boundary.
    int32_t networkFrames = (int32_t)(Netplay_GetRTT() * 60 / 1000) + 2;
    int32_t leadFrames = networkFrames + 1;
    if (leadFrames < 6) leadFrames = 6;
    int32_t effectiveFrame = Controller_GetNetFrameCount() + leadFrames;

    Controller_ScheduleDelayFrames(delay, effectiveFrame);
    Netplay_SendDelay(delay, effectiveFrame);
}

// ---------------------------------------------------------------------------
// Addresses (VAs, converted at runtime via _addr)
// ---------------------------------------------------------------------------

// vtable entry for sMvc3Main::move
static constexpr uintptr_t MOVE_VTABLE_ENTRY = 0x140B091F0;

// CALL at umvc3.exe+31CD59 calls sub_1402CE750 (SetText)
static constexpr uintptr_t HOOK_CALL_SITE = 0x14031CD59;

// Pointers
static constexpr uintptr_t MVC3_MAIN_PTR    = 0x140E177E8;
static constexpr uintptr_t MVC3_MANAGER_PTR = 0x140D510A0;
static constexpr uintptr_t CHARACTER_PTR    = 0x140D44A70;
static constexpr uintptr_t GAME_CONFIG_PTR  = 0x140D50F88;
static constexpr uintptr_t MEMORY_ALLOCATOR_TABLE = 0x140D762F0;
static constexpr uint32_t MEMORY_ALLOCATOR_COUNT = 64;
static constexpr uint32_t EFFECT_ALLOCATOR_SLOT = 12;

// Functions
static constexpr uintptr_t SHOW_LOADING_ADDR = 0x140253380;
static constexpr uintptr_t SET_TEXT_FUNC     = 0x1402CE750;
static constexpr uintptr_t REPLAY_ROUND_START_ADDR = 0x1401CFA90;

static bool SynchronizeRoundRng(uint32_t seed)
{
    uint64_t sReplay = *(uint64_t*)_addr(0x140D4D9D8);
    if (!sReplay) return false;

    *(uint32_t*)(sReplay + 0x23424) = seed;
    *(uint32_t*)(sReplay + 0x58) = seed;
    using ReplayRoundStartFn = void(__fastcall*)(uint64_t);
    ((ReplayRoundStartFn)_addr(REPLAY_ROUND_START_ADDR))(sReplay);
    return true;
}

// ---------------------------------------------------------------------------
// Offsets
// ---------------------------------------------------------------------------
static constexpr uintptr_t BattleSetting_offset = 0x40;
static constexpr uintptr_t GameMode_offset      = 0x34C;
// Battle type values at sBattleSetting+0x34C (confirmed via Ghidra):
//   1 = Arcade
//   2 = Attract mode
//   3 = Online (player match)
//   4 = Online spectator/watch battle
//   5 = Training
//   7 = Unknown mode 7
//   8 = Heroes & Heralds
//   9 = Online (alt)
static constexpr uintptr_t P1ChrNode_offset     = 0x58;

// ---------------------------------------------------------------------------
// Message IDs (from ColorExpand msg_sys_menu_en.msd)
// ---------------------------------------------------------------------------
static constexpr int32_t MSG_NOW_LOADING = 0x762; // "NOW LOADING"
static constexpr int32_t MSG_PAUSED      = 0xC59; // "PAUSED"

// ---------------------------------------------------------------------------
// Configurable keybinds
// ---------------------------------------------------------------------------
static int KEY_PAUSE   = VK_F2;
static int KEY_ADVANCE = VK_F3;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool isPaused     = false;
static bool advanceFrame = false;

// Frame counter: incremented each time a game frame actually advances while paused
static int frameCounter  = 0;

// aNextBattle tracker for one-time flag init
static uint64_t s_lastBattleObj = 0;

// Network input capture (captured from HookedMove after oMove runs)
static uint32_t g_netP1Input   = 0; // battleState+0xd58 — P1's processed input (what would be sent)
static uint32_t g_bsP2Input    = 0; // battleState+0xd60 — P2 input candidate
static uint32_t g_netP2Read    = 0; // GetsCurrentInput_withSOCD(pad,1)
static uint32_t g_padP2Input   = 0; // pad + 0x49c — raw pad slot 1 input
static bool     g_netDataValid = false;


// Input buffer data — captured from cChrInput__getInputBuffer (0x1401cc5d0)
// and the buffer reader FUN_1402894d0 (0x1402894d0)
static uint64_t   g_inputBufBase = 0;     // input buffer base pointer (cChrInput object)
static uint64_t   g_inputBufPtr  = 0;     // actual buffer pointer (from +0x2030)
static uint32_t   g_inputBuf[10] = {0};   // first 10 uint32 from buffer
static int        g_inputBufFrames = 0;   // how many frames of data seen
static bool       g_inputBufValid = false;

// D3D9 device pointer, set from FrameStepper_SetDevice
static LPDIRECT3DDEVICE9 g_pDevice = nullptr;

// Game window handle for forcing repaints
static HWND g_hWnd = nullptr;

// Pause frame snapshot: captures the back buffer once when pause starts
// and whenever a frame is advanced, so the frozen game image stays
// consistent across D3D9 buffer swaps.
static IDirect3DTexture9* g_pPauseTexture    = nullptr;
static bool               g_captureRequested = false;

static const char* DecodeDirection(uint32_t input)
{
    switch (input & 0x0F) {
    case 0x01: return "6";
    case 0x02: return "4";
    case 0x04: return "8";
    case 0x05: return "9";
    case 0x06: return "7";
    case 0x08: return "2";
    case 0x09: return "3";
    case 0x0A: return "1";
    default:   return "5";
    }
}

static void AppendButtons(char* buf, size_t len, uint32_t input)
{
    char tmp[128] = "";
    auto append = [&](const char* name, uint32_t bit) {
        if (input & bit) {
            if (tmp[0]) strcat(tmp, " ");
            strcat(tmp, name);
        }
    };
    append("L", 0x0010);
    append("M", 0x0020);
    append("H", 0x0040);
    append("S", 0x0080);
    append("A1", 0x0100);
    append("A2", 0x0200);
    append("L+M", 0x0400);
    append("L+H", 0x0800);
    append("M+H", 0x1000);
    append("L+M+H", 0x2000);
    strncpy(buf, tmp, len - 1);
    buf[len - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Input display function pointers (used during pause to show current inputs)
// ---------------------------------------------------------------------------
typedef uint64_t(__fastcall* sPad__getInstance_t)();
static constexpr uintptr_t SPAD_GET_INSTANCE_ADDR = 0x140001B00;

typedef uint64_t(__fastcall* GetsCurrentInput_withSOCD_t)(uint64_t padInstance, uint32_t playerIndex);
static constexpr uintptr_t GET_CURRENT_INPUT_ADDR = 0x1402B3700;

// ---------------------------------------------------------------------------
// Original function pointer for the vtable-hooked move
// ---------------------------------------------------------------------------
typedef void(__fastcall* Move_t)(void*);
static Move_t oMove = nullptr;

// Forward declarations
static void CaptureNetInput();
static bool IsTrainingMode();
static constexpr uintptr_t SGAMEUI_GET_INSTANCE              = 0x140001ac0;


extern "C" void Controller_OnFrameStart();
extern "C" void Controller_OnFrameEnd();
// ---------------------------------------------------------------------------
// Hooked sMvc3Main::move — skips the call when paused unless advancing.
// ---------------------------------------------------------------------------
static void __fastcall HookedMove(void* sMvc3Main)
{
    // Rollback_ReSimulate owns the input boundary while replaying frames.
    // Re-entering the live path here can spin waiting for confirmed input.
    if (g_isResimulating)
    {
        oMove(sMvc3Main);
        return;
    }

    if (isPaused)
    {
        if (advanceFrame)
        {
            advanceFrame = false;
            frameCounter++;
            if (Netplay_IsActive())
            {
                Controller_OnFrameStart();
            }
            oMove(sMvc3Main);
            if (Netplay_IsActive())
            {
                Controller_OnFrameEnd();
            }
        }
        else if (g_pDevice)
        {
            if (SUCCEEDED(g_pDevice->BeginScene()))
                g_pDevice->EndScene();
            g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
        }
        return;
    }

    // Restart immediately offline or at the agreed frame during netplay.
    // Perform it before the game tick, then let the intro run again.
    const bool restartNetplay = Netplay_IsActive();
    if (g_restartTargetFrame >= 0 &&
        (!restartNetplay || Controller_GetNetFrameCount() >= g_restartTargetFrame))
    {
        static constexpr uintptr_t MVC3_MANAGER_PTR = 0x140D510A0;
        static constexpr uintptr_t SMVC3MGR_ACTION_OFFSET = 0x48;
        static constexpr uintptr_t SREPLAY_PTR = 0x140D4D9D8;
        uint64_t sMvc3Manager = *(uint64_t*)_addr(MVC3_MANAGER_PTR);
        uint64_t sAction = sMvc3Manager
            ? *(uint64_t*)(sMvc3Manager + SMVC3MGR_ACTION_OFFSET) : 0;
        uint64_t sReplay = *(uint64_t*)_addr(SREPLAY_PTR);

        Controller_ClearDesyncDetection();

        if (restartNetplay && g_restartKind == RESTART_TRAINING) {
            Rollback_ClearPendingReSimulation();
            Netplay_ClearInputQueues();
            Controller_ResetBuffers();
            Rollback_ResetNetplayLiveLog();
            if (sAction) {
                using SetRestartModeFn = void(__fastcall*)(uint64_t, int32_t);
                ((SetRestartModeFn)_addr(0x14000AF90))(sAction, 3);
                ((SetRestartModeFn)_addr(0x140207530))(sAction,
                                                       g_restartButtonMode);
                Controller_ResetFightTimer(sAction);
            }
            Rollback_FreeSnapshots();
            Rollback_ResetNetplayHistoryBuffers();
            g_battleSynchronized = false;
            g_localReady = false;
            g_remoteReady = false;
            g_localRelease = false;
            g_remoteRelease = false;
            g_roundSeed = g_restartRoundSeed;
            g_roundSeedReady = true;
            g_trainingRestartBarrier = true;
            g_trainingRestartLeftFighting = false;
            g_localFighting = false;
            g_remoteFighting = false;
            g_desyncCheckReady = false;
            ++g_battleGeneration;
            if (g_battleGeneration == 0)
                g_battleGeneration = 1;
            g_advertisedBattleFlags = 0;
            g_battleAdvertiseTick = 0;
            HookLog("FrameStepper: requested synchronized training restart buttonMode=%d frame=%d generation=%u seed=0x%08X\n",
                    g_restartButtonMode, Controller_GetNetFrameCount(),
                    g_battleGeneration, g_restartRoundSeed);
            g_restartTargetFrame = -1;
            g_restartKind = RESTART_FULL_MATCH;
            g_restartButtonMode = 0;
        } else {

        if (restartNetplay)
            Rollback_ApplyNetplayDeterminism();
        if (sReplay) {
            *(uint32_t*)(sReplay + 0x23424) = g_restartRoundSeed;
            *(uint32_t*)(sReplay + 0x58) = g_restartRoundSeed;
        }
        if (sAction) {
            // Follow the native rematch path. Phase 12 retires the previous
            // camera, stage, shot, effect, and sUnit entries before returning
            // to phase 2; jumping straight to phase 2 leaves stale unit nodes.
            using SetRestartModeFn = void(__fastcall*)(uint64_t, int32_t);
            using SetNextStepFn = void(__fastcall*)(uint64_t, int32_t);
            ((SetRestartModeFn)_addr(0x14000AF90))(sAction, 1);
            ((SetNextStepFn)_addr(0x14000AE00))(sAction, 12);
        }

        Rollback_FreeSnapshots();
        if (restartNetplay) {
            Netplay_ClearInputQueues();
            Controller_ResetBuffers();
            Controller_ResetFrameEpoch();
            Rollback_ResetNetplayLiveLog();

            g_battleSynchronized = false;
            g_localReady = false;
            g_remoteReady = false;
            g_localRelease = false;
            g_remoteRelease = false;
            g_roundSeedReady = false;
            g_roundSeed = 0;
            g_localFighting = false;
            g_remoteFighting = false;
            g_desyncCheckReady = false;
            ++g_battleGeneration;
            if (g_battleGeneration == 0)
                g_battleGeneration = 1;
            g_advertisedBattleFlags = 0;
            g_battleAdvertiseTick = 0;
        }
        g_restartTargetFrame = -1;

        HookLog("FrameStepper: restarted match (%s, generation=%u seed=0x%08X)\n",
                 restartNetplay ? "netplay" : "offline",
                 g_battleGeneration, g_restartRoundSeed);
        }
    }

    if (Netplay_IsActive())
    {
        // Drain any inbound delay/rollback-control packets — keeps both sides
        // in sync when the user adjusts either value on either machine.
        int32_t remoteDelay = -1;
        int32_t remoteDelayFrame = -1;
        while (Netplay_GetInboundDelay(remoteDelay, remoteDelayFrame))
        {
            if (remoteDelay >= 1 && remoteDelay <= 15)
            {
                if (remoteDelayFrame < Controller_GetNetFrameCount())
                    remoteDelayFrame = Controller_GetNetFrameCount();
                Controller_ScheduleDelayFrames(remoteDelay, remoteDelayFrame);
            }
        }
        int32_t remoteRollback = -1;
        while (Netplay_GetInboundRollback(remoteRollback))
        {
            if (remoteRollback >= 0 && remoteRollback <= 16)
                Controller_SetRollbackFrames(remoteRollback);
        }
        bool remoteMultiResim = false;
        while (Netplay_GetInboundMultiResim(remoteMultiResim))
            Controller_SetMultiResimEnabled(remoteMultiResim);

        uint32_t remoteGeneration = 0;
        uint32_t remoteFlags = 0;
        uint32_t remoteRoundSeed = 0;
        while (Netplay_GetInboundBattleReady(remoteGeneration, remoteFlags,
                                              remoteRoundSeed))
        {
            if (remoteGeneration == g_battleGeneration)
            {
                g_remoteReady |= (remoteFlags & BATTLE_SYNC_READY) != 0;
                g_remoteRelease |= (remoteFlags & BATTLE_SYNC_RELEASE) != 0;
                g_remoteFighting |= (remoteFlags & BATTLE_SYNC_FIGHTING) != 0;
                if (!Netplay_IsHost() && (remoteFlags & BATTLE_SYNC_SEED) != 0) {
                    g_roundSeed = remoteRoundSeed;
                    g_roundSeedReady = true;
                }
            }
        }

        typedef uint64_t(__fastcall* GetAction_t)();
        static GetAction_t getAction = (GetAction_t)_addr(0x140001A90);
        uint64_t action = getAction();
        int32_t step = action ? *(int32_t*)(action + 0xD8) : 0;

        if (!g_battleSynchronized)
        {
            int32_t pendingStep = action ? *(int32_t*)(action + 0xDC) : -1;
            int32_t introSubstate = action ? *(int32_t*)(action + 0xE0) : 0;

            if (g_trainingRestartBarrier) {
                if (step != 5)
                    g_trainingRestartLeftFighting = true;
                if (!g_localReady && g_trainingRestartLeftFighting && step == 5) {
                    g_localReady = true;
                    HookLog("AutoTestNet: local training restart ready (generation=%u)\n",
                            g_battleGeneration);
                }
            } else if (!g_localReady && step == 2 && pendingStep < 0 && introSubstate > 0)
            {
                g_localReady = true;
                if (Netplay_IsHost()) {
                    uint64_t sReplay = *(uint64_t*)_addr(0x140D4D9D8);
                    if (sReplay) {
                        g_roundSeed = *(uint32_t*)(sReplay + 0x58);
                        g_roundSeedReady = true;
                    }
                }
                HookLog("Netplay: Local intro ready (generation=%u, substate=%d)\n",
                         g_battleGeneration, introSubstate);
            }

            // RELEASE acknowledges that this peer observed the remote READY.
            // A one-way READY is insufficient: its sender may otherwise stay
            // frozen while the receiver starts and predicts frames without it.
            if (g_localReady && g_remoteReady && g_roundSeedReady)
                g_localRelease = true;
        }

        uint32_t localFlags = (g_localReady ? BATTLE_SYNC_READY : 0) |
                              (g_localRelease ? BATTLE_SYNC_RELEASE : 0) |
                              (g_localFighting ? BATTLE_SYNC_FIGHTING : 0) |
                              (Netplay_IsHost() && g_roundSeedReady ? BATTLE_SYNC_SEED : 0);
        g_battleAdvertiseTick++;
        if (localFlags != 0 &&
            (localFlags != g_advertisedBattleFlags || g_battleAdvertiseTick >= 30))
        {
            Netplay_SendBattleReady(g_battleGeneration, localFlags, g_roundSeed);
            g_advertisedBattleFlags = localFlags;
            g_battleAdvertiseTick = 0;
        }

        if (!g_battleSynchronized && g_localRelease && g_remoteRelease)
        {
            if (!SynchronizeRoundRng(g_roundSeed)) {
                HookLog("Netplay: cannot synchronize round RNG; sReplay unavailable\n");
                return;
            }
            AutoTest_ApplyMatchStartPositions();

            // Unlike restart generations, the initial setup handshake runs in
            // generation 0. Advance before clearing the queues so anything the
            // service thread delivers from setup is rejected by the active
            // timeline's generation check.
            if (g_battleGeneration == 0)
                g_battleGeneration = 1;
            Netplay_ClearInputQueues();
            Controller_ResetBuffers();
            Controller_ResetFrameEpoch();
            // Establish the fight-clock epoch at the same mutual-release
            // boundary on both peers. Otherwise the first stepFighting hook
            // can capture a local pre-release frame and shift the round clock.
            uint64_t releasedManager = *(uint64_t*)_addr(0x140D510A0);
            uint64_t releasedAction = releasedManager
                ? *(uint64_t*)(releasedManager + 0x48) : 0;
            Controller_ResetFightTimer(releasedAction);
            Rollback_FreeSnapshots();
            g_battleSynchronized = true;
            if (g_trainingRestartBarrier) {
                g_trainingRestartBarrier = false;
                g_trainingRestartLeftFighting = false;
                AutoTest_OnTrainingRestartExecuted();
            }
            Netplay_SetTimeout(AutoTest_IsActive() ? 2000 : 5000);
            HookLog("Netplay: Intro barrier released after RNG sync (generation=%u seed=0x%08X)\n",
                     g_battleGeneration, g_roundSeed);
        }

        if (g_battleSynchronized && !g_desyncCheckReady &&
            g_localFighting && g_remoteFighting)
        {
            Controller_ClearDesyncDetection();
            g_desyncCheckReady = true;
            HookLog("Netplay: desync checks enabled in fighting state (generation=%u frame=%d)\n",
                    g_battleGeneration, Controller_GetNetFrameCount());
        }

        // Keep loading normally until this machine reaches the barrier. Once
        // ready, freeze it until the mutual acknowledgement is complete.
        if (g_localReady && !g_battleSynchronized)
            return;

        if (g_battleSynchronized)
            Controller_OnFrameStart();
    }

    oMove(sMvc3Main);

    if (Netplay_IsActive() && g_battleSynchronized)
    {
        Controller_OnFrameEnd();
    }

    CaptureNetInput();

    // ===== Netplay: logging only =====
    if (Netplay_IsActive())
    {
        // Check the sAction step to know if gameplay has started.
        typedef uint64_t(__fastcall* GetAction_t)();
        GetAction_t getAction = (GetAction_t)_addr(0x140001A90);
        uint64_t action = getAction();
        int step = action ? *(int*)(action + 0xD8) : 0;

        static bool s_inputStarted = false;
        if (step >= 0 && !s_inputStarted) {
            s_inputStarted = true;
            HookLog("Netplay: First frame input ready (step=%d, sAction=0x%llX) — blasting packets\n",
                    step, action);
        }

        if (step >= 0)
        {
            auto sPadGetInstance = (sPad__getInstance_t)_addr(SPAD_GET_INSTANCE_ADDR);
            uint64_t pad = sPadGetInstance();
            if (pad) {
                static uint32_t s_netplayFrameCounter = 0;
                s_netplayFrameCounter++;
                if (s_netplayFrameCounter % 600 == 0) {
                    uint32_t recvInput = Netplay_HasRemoteInput() ? Netplay_GetRemoteInput() : 0;
                    HookLog("[Netplay] Frame %u | Recv: 0x%08X | RTT: %u ms\n",
                            s_netplayFrameCounter, recvInput, Netplay_GetRTT());
                }
            }
        }
    }

}

// ---------------------------------------------------------------------------
// Capture the data that would be sent/received over network — called after oMove
// ---------------------------------------------------------------------------
static void CaptureNetInput()
{
    auto sPadGetInstance = (sPad__getInstance_t)_addr(SPAD_GET_INSTANCE_ADDR);
    auto getInput = (GetsCurrentInput_withSOCD_t)_addr(GET_CURRENT_INPUT_ADDR);
    uint64_t pad = sPadGetInstance();
    if (pad) {
        g_netP1Input = (uint32_t)getInput(pad, 0);
        g_netP2Read  = (uint32_t)getInput(pad, 1);
        g_padP2Input = *(uint32_t*)(pad + PAD_SLOT_STRIDE * 1 + PAD_SLOT_INPUT);
    } else {
        g_netP1Input = 0;
        g_netP2Read  = 0;
        g_padP2Input = 0;
    }
    g_bsP2Input = 0;
    g_netDataValid = true;
}

// ---------------------------------------------------------------------------
// Runtime check: is the game currently in a training-mode match?
// ---------------------------------------------------------------------------
static bool IsTrainingMode()
{
    uint64_t sMvc3Manager = *(uint64_t*)_addr(MVC3_MANAGER_PTR);
    if (!sMvc3Manager) return false;

    uint64_t sBattleSetting = *(uint64_t*)(sMvc3Manager + BattleSetting_offset);
    if (!sBattleSetting) return false;

    int GameMode = *(int*)(sBattleSetting + GameMode_offset);
    //if (GameMode != 5) return false; // 5 = training (see GameMode_offset comment for full enum)

    uint64_t sCharacter = *(uint64_t*)_addr(CHARACTER_PTR);
    if (!sCharacter) return false;

    uint64_t P1ChrNode = *(uint64_t*)(sCharacter + P1ChrNode_offset);
    return P1ChrNode != 0;
}

// ---------------------------------------------------------------------------
// Load keybind config from UMVC3Hook.ini  [FrameStepper] section
// ---------------------------------------------------------------------------
static void LoadConfig()
{
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
    wcsrchr(iniPath, L'\\')[1] = L'\0';
    wcscat(iniPath, L"UMVC3Hook.ini");

    KEY_PAUSE   = GetPrivateProfileIntW(L"FrameStepper", L"PauseKey",   VK_F2, iniPath);
    KEY_ADVANCE = GetPrivateProfileIntW(L"FrameStepper", L"AdvanceKey", VK_F3, iniPath);
}

// ---------------------------------------------------------------------------
// Hooked SetText — show "PAUSED" instead of "NOW LOADING" when stepper is on
// ---------------------------------------------------------------------------
static int64_t __fastcall HookedSetText(uint64_t fontWidget, int32_t msgID)
{
    if (msgID == MSG_NOW_LOADING && isPaused)
        msgID = MSG_PAUSED;

    typedef int64_t(__fastcall* SetText_t)(uint64_t, int32_t);
    return ((SetText_t)_addr(SET_TEXT_FUNC))(fontWidget, msgID);
}

// ---------------------------------------------------------------------------
// Show the loading/pause popup
// ---------------------------------------------------------------------------
static void ShowPopup()
{
    uint64_t sGameConfig = *(uint64_t*)_addr(GAME_CONFIG_PTR);
    if (!sGameConfig) return;

    typedef void(__fastcall* ShowLoading_t)(uint64_t);
    ((ShowLoading_t)_addr(SHOW_LOADING_ADDR))(sGameConfig);
}

// ---------------------------------------------------------------------------
// Store the D3D9 device so we can force a Present when entering pause
// ---------------------------------------------------------------------------
void FrameStepper_SetDevice(LPDIRECT3DDEVICE9 device)
{
    g_pDevice = device;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (device && SUCCEEDED(device->GetCreationParameters(&cp)))
        g_hWnd = cp.hFocusWindow;
}

void FrameStepper_InvalidateDeviceObjects()
{
    if (g_pPauseTexture) {
        g_pPauseTexture->Release();
        g_pPauseTexture = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Capture the current back buffer into a GPU texture so it can be drawn as a
// static background during pause (avoids flicker from D3D9 buffer swaps).
// ---------------------------------------------------------------------------
static void CaptureBackBuffer(LPDIRECT3DDEVICE9 pDevice)
{
    if (!pDevice) return;

    IDirect3DSurface9* pBackBuffer = nullptr;
    if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)))
        return;

    D3DSURFACE_DESC desc;
    pBackBuffer->GetDesc(&desc);

    // Free old texture
    if (g_pPauseTexture) {
        g_pPauseTexture->Release();
        g_pPauseTexture = nullptr;
    }

    // Create a render-target texture matching the back buffer
    if (SUCCEEDED(pDevice->CreateTexture(desc.Width, desc.Height, 1,
        D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &g_pPauseTexture, nullptr)))
    {
        IDirect3DSurface9* pTexSurface = nullptr;
        if (SUCCEEDED(g_pPauseTexture->GetSurfaceLevel(0, &pTexSurface))) {
            pDevice->StretchRect(pBackBuffer, nullptr, pTexSurface, nullptr, D3DTEXF_NONE);
            pTexSurface->Release();
        }
    }
    pBackBuffer->Release();
}

// ---------------------------------------------------------------------------
// Called from hkEndScene after the game has finished rendering the current
// frame — resolves any pending capture requests.
// ---------------------------------------------------------------------------
void FrameStepper_Capture(LPDIRECT3DDEVICE9 pDevice)
{
    if (g_captureRequested) {
        CaptureBackBuffer(pDevice);
        g_captureRequested = false;
    }
}

// Input buffer hook — hooks cChrInput__getInputBuffer (0x1401cc5d0)
// Returns the input buffer pointer from *(qword*)(RCX + 0x2030)
// ---------------------------------------------------------------------------
typedef uint64_t(__fastcall* GetInputBuf_t)(uint64_t obj);
static GetInputBuf_t oGetInputBuf = nullptr;

static uint64_t __fastcall HookedGetInputBuf(uint64_t obj)
{
    uint64_t bufPtr = oGetInputBuf(obj);

    if (!g_inputBufValid) {
        g_inputBufBase = obj;
        g_inputBufPtr  = bufPtr;
        if (bufPtr) {
            for (int i = 0; i < 10; i++)
                g_inputBuf[i] = *(uint32_t*)(bufPtr + i * 4);
            g_inputBufFrames = 10;
        }
        g_inputBufValid = true;
    }

    return bufPtr;
}

// ---------------------------------------------------------------------------
// Public: initialise hooks — call once from OnInitializeHook
// ---------------------------------------------------------------------------
void FrameStepper_InitNetplay()
{
    const LaunchDialogConfig& cfg = GetLaunchDialogConfig();
    if (cfg.hostEnabled && cfg.enetHost && cfg.enetPeer && !Netplay_IsActive()) {
        Netplay_Init(cfg.enetHost, cfg.enetPeer);
    }
    if (Netplay_IsActive()) {
        HookLog("FrameStepper: Netplay initialized from launch config\n");
        g_autoTestLaunchPending = IsAutoTestLaunchEnabled() && Netplay_IsHost();
        Rollback_PatchNativeNetplayDeterminism();
        Rollback_ApplyNetplayDeterminism();
    }
}

void FrameStepper_Init()
{
    LoadConfig();

    // Hook sMvc3Main::move via vtable patch
    oMove = (Move_t)(*(uintptr_t*)_addr(MOVE_VTABLE_ENTRY));
    Patch((void*)_addr(MOVE_VTABLE_ENTRY), (uintptr_t)&HookedMove);

    // Hook SetText call site (CALL instruction override)
    Trampoline* tramp = Trampoline::MakeTrampoline(GetModuleHandleW(nullptr));
    InjectHook(_addr(HOOK_CALL_SITE), tramp->Jump(&HookedSetText), PATCH_CALL);

    // Hook cChrInput__getInputBuffer (0x1401cc5d0) to capture the input buffer pointer
    static bool inputBufHooked = false;
    if (!inputBufHooked) {
        MH_CreateHook((LPVOID)_addr(0x1401cc5d0), (LPVOID)&HookedGetInputBuf, (LPVOID*)&oGetInputBuf);
        MH_EnableHook((LPVOID)_addr(0x1401cc5d0));
        inputBufHooked = true;
    }

    // Initialize immediately for normal launches and auto-connect clients.
    // A dual-training host starts this after its deferred peer handshake.
    FrameStepper_InitNetplay();

    // Initialize rollback snapshot engine (hooks sMvc3Main__move)
    Rollback_InitHooks();
}

// ---------------------------------------------------------------------------
// Public: per-frame processing — call from HookProcessStuff
// ---------------------------------------------------------------------------
// Edge-detection helper for use on threads without a message queue.
// GetAsyncKeyState's &1 bit is per-thread and requires a message pump;
// on the HookUpdate thread it always returns 0.  This helper uses the
// global &0x8000 (key-is-down) bit with tracked prior state instead.
static inline bool KeyPressed(int vk, bool& prevState)
{
    bool cur = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = cur && !prevState;
    prevState = cur;
    return pressed;
}

static void CloseGameAfterAutoTest()
{
    if (g_hWnd && PostMessageW(g_hWnd, WM_CLOSE, 0, 0))
        return;
    ExitProcess(0);
}

void FrameStepper_Process()
{
    AutoTest_Process();

    if (AutoTest_IsActive() && Netplay_TakePeerDisconnected()) {
        HookLog("AutoTestSuite: remote peer disconnected; closing game\n");
        CloseGameAfterAutoTest();
        return;
    }

    if (Netplay_GetInboundAutoTestComplete()) {
        HookLog("AutoTestSuite: remote command-line suite complete; closing game\n");
        CloseGameAfterAutoTest();
        return;
    }

    if (g_autoTestExitTick && GetTickCount() - g_autoTestExitTick < 0x80000000u) {
        HookLog("AutoTestSuite: completion delivered; closing host game\n");
        g_autoTestExitTick = 0;
        CloseGameAfterAutoTest();
        return;
    }

    if (g_autoTestLaunchRunning && !AutoTest_GetSuiteStatus().active) {
        g_autoTestLaunchRunning = false;
        Netplay_SendAutoTestComplete();
        g_autoTestExitTick = GetTickCount() + 500;
        HookLog("AutoTestSuite: command-line suite complete; notifying client before exit\n");
    }

    if (g_autoTestLaunchPending && g_battleSynchronized &&
        Netplay_IsActive() && Netplay_IsHost()) {
        typedef uint64_t(__fastcall* GetAction_t)();
        static GetAction_t getAction = (GetAction_t)_addr(0x140001A90);
        uint64_t action = getAction();
        if (action && *(int32_t*)(action + 0xD8) == 5 &&
            AutoTest_StartSuite(0, false)) {
            g_autoTestLaunchPending = false;
            g_autoTestLaunchRunning = true;
            HookLog("AutoTestSuite: command-line suite started after synchronized training readiness\n");
        }
    }

    static bool prevDumpNDJSON = false;
    if (KeyPressed(VK_F10, prevDumpNDJSON))
    {
        const int32_t frame = Controller_GetNetFrameCount();
        Rollback_DumpNetplayHistory(frame);
        if (Netplay_IsActive())
            Netplay_SendDumpNDJSON(frame);
        HookLog("FrameStepper: F10 dumped NDJSON through frame %d%s\n",
                frame, Netplay_IsActive() ? " on both peers" : "");
    }

    int32_t remoteDumpFrame = -1;
    while (Netplay_GetInboundDumpNDJSON(remoteDumpFrame))
    {
        Rollback_DumpNetplayHistory(remoteDumpFrame);
        HookLog("FrameStepper: remote F10 dumped NDJSON through frame %d\n",
                remoteDumpFrame);
    }

    static bool prevRestart = false;
    if (KeyPressed(VK_F11, prevRestart))
    {
        Controller_ClearDesyncDetection();
        Rollback_ClearPendingReSimulation();
        int32_t targetFrame = Controller_GetNetFrameCount();
        uint64_t sReplay = *(uint64_t*)_addr(0x140D4D9D8);
        if (sReplay)
            g_restartRoundSeed = *(uint32_t*)(sReplay + 0x58);
        if (Netplay_IsActive())
        {
            // Give the reliable packet time to arrive. The target itself is
            // still absolute, so both machines perform the write together.
            targetFrame += 25;
            Netplay_SendRestartMatch(targetFrame, g_restartRoundSeed);
        }
        g_restartKind = RESTART_FULL_MATCH;
        g_restartButtonMode = 0;
        g_restartTargetFrame = targetFrame;
        HookLog("FrameStepper: scheduled match restart for frame %d (%s)\n",
                targetFrame, Netplay_IsActive() ? "netplay" : "offline");
    }

    int32_t remoteRestartFrame = -1;
    uint32_t remoteRestartSeed = 0;
    uint32_t remoteRestartKind = RESTART_FULL_MATCH;
    int32_t remoteRestartButton = 0;
    while (Netplay_GetInboundRestartMatch(remoteRestartFrame, remoteRestartSeed,
                                           remoteRestartKind, remoteRestartButton)) {
        AutoTest_PrepareForRestart();
        // Keep a delayed desync snapdump alive until the synchronized restart
        // actually executes. Clearing here cancels the client's capture as
        // soon as it receives the host's restart packet; execution clears the
        // same state at the target frame below.
        Rollback_ClearPendingReSimulation();
        g_restartTargetFrame = remoteRestartFrame;
        g_restartRoundSeed = remoteRestartSeed;
        g_restartKind = remoteRestartKind;
        g_restartButtonMode = remoteRestartButton;
        HookLog("FrameStepper: received restart kind=%u target=%d seed=0x%08X\n",
                remoteRestartKind, remoteRestartFrame, remoteRestartSeed);
    }

    // Netplay delay adjustment via [ (decrease) and ] (increase) keys.
    // Works regardless of training mode so both sides can adjust mid-match.
    static bool prevBracketL = false, prevBracketR = false;
    if (Netplay_IsActive())
    {
        if (KeyPressed(VK_OEM_4, prevBracketL))  // [ = decrease delay
        {
            int32_t delay = Controller_GetDelayFrames() - 1;
            if (delay < 1) delay = 1;
            ScheduleNetplayDelay(delay);
        }
        if (KeyPressed(VK_OEM_6, prevBracketR))  // ] = increase delay
        {
            int32_t delay = Controller_GetDelayFrames() + 1;
            if (delay > 15) delay = 15;
            ScheduleNetplayDelay(delay);
        }
    }

    // Apply remote desync-screen actions on the game thread. These actions do
    // not send a response, preventing trigger/unpause packets from bouncing.
    DesyncPacket desyncPacket;
    while (Netplay_GetInboundDesync(desyncPacket))
    {
        // Discard stale alerts from the previous generation while the restart
        // barrier is rebuilding the shared frame and RNG epochs.
        // Autotest queues that restart immediately after a mismatch; keep the
        // trigger so the client can write its snapdump before the reset.
        const bool autoTestTrigger = desyncPacket.action == DESYNC_TRIGGER &&
                                     AutoTest_IsActive();
        if (Netplay_IsActive() &&
            (!FrameStepper_IsBattleSynchronized() ||
             (FrameStepper_IsMatchRestartPending() && !autoTestTrigger) ||
             desyncPacket.battle_generation != g_battleGeneration))
            continue;

        if (desyncPacket.action == DESYNC_TRIGGER)
        {
            g_desyncDetected = true;
            g_desyncSeen = false;
            g_desyncFrame = desyncPacket.frame;
            g_desyncLocalCS = desyncPacket.remoteChecksum;
            g_desyncRemoteCS = desyncPacket.localChecksum;
            Rollback_NotifyDesyncTriggered(desyncPacket.frame);
            AutoTest_OnDesyncDetected(desyncPacket.frame);
        }
        else if (desyncPacket.action == DESYNC_UNPAUSE)
        {
            g_desyncDetected = false;
            g_desyncSeen = true;
            isPaused = false;
            advanceFrame = false;
        }
    }

    const int32_t desyncProcessFrame = Controller_GetNetFrameCount();
    const bool desyncCapturePending = g_desyncDetected &&
        Rollback_ProcessDesyncDelay(desyncProcessFrame);

    // --- Desync auto-pause (works in any mode). ---
    // P key dismisses the alert and unpauses.  The overlay persists
    // (g_desyncSeen remains true) but future mismatches won't re-pause.
    static bool prevDesyncP = false;
    bool desyncActive = g_desyncDetected || g_desyncSeen;
    if (desyncActive)
    {
        if (KeyPressed(0x50, prevDesyncP))
        {
            g_desyncDetected = false;
            g_desyncSeen     = true;
            isPaused         = false;
            advanceFrame     = false;
            if (Netplay_IsActive())
                Netplay_SendDesyncUnpause();
        }
        else if (g_desyncDetected && !desyncCapturePending && !AutoTest_IsActive())
        {
            isPaused = true;
        }
    }

    if (!IsTrainingMode())
    {
        if (isPaused && !g_desyncDetected) {
            isPaused = false;
            if (g_pPauseTexture) {
                g_pPauseTexture->Release();
                g_pPauseTexture = nullptr;
            }
        }
        return;
    }

    // Toggle pause
    static bool prevPause = false;
    if (KeyPressed(KEY_PAUSE, prevPause))
    {
        isPaused = !isPaused;
        if (isPaused)
        {
            frameCounter = 0;
            ShowPopup();
            g_captureRequested = true; // snapshot the current frame
        }
        else
        {
            // Free the snapshot on unpause
            if (g_pPauseTexture) {
                g_pPauseTexture->Release();
                g_pPauseTexture = nullptr;
            }
        }
    }

    // Advance one frame while paused
    static bool prevAdvance = false;
    if (isPaused && KeyPressed(KEY_ADVANCE, prevAdvance))
    {
        advanceFrame = true;
        g_captureRequested = true; // snapshot the advanced frame
    }

    // --- Rollback save/restore test keys (F5 / F6 / F7) ---
    {
        // Read the game's absolute frame counter from sReplay+0x54
        static int32_t lastSaveFrame = 0;
        auto sReplay = *(uint64_t*)_addr(0x140d4d9d8);
        int32_t curFrame = sReplay ? *(int32_t*)(sReplay + 0x54) : 0;

        static bool prevF5 = false, prevF6 = false, prevF7 = false;
        if (KeyPressed(VK_F5, prevF5)) {
            Rollback_SaveSnapshot(curFrame);
            lastSaveFrame = curFrame;
            HookLog("Rollback: saved snapshot at game frame %d\n", curFrame);
        }
        if (KeyPressed(VK_F6, prevF6)) {
            g_restoreRequestedFrame = lastSaveFrame;
            HookLog("Rollback: requesting restore to frame %d (deferred to frame boundary)\n", lastSaveFrame);
        }
        if (KeyPressed(VK_F7, prevF7)) {
            g_f7State = 1;
            g_f7StartFrame = curFrame;
            g_f7Remaining = 8;
            HookLog("F7: recording start at frame %d — save → 8 frames → restore+resim\n", curFrame);
        }
    }
}

// ---------------------------------------------------------------------------
// Public: draw pause indicator overlay — call from hkEndScene (after ImGui::NewFrame)
// ---------------------------------------------------------------------------
static bool IsReadableMemory(uint64_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION info = {};
    if (!address || !VirtualQuery((void*)address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    uint64_t regionEnd = (uint64_t)info.BaseAddress + info.RegionSize;
    return address <= regionEnd && size <= regionEnd - address;
}

static void DrawAllocatorMemory()
{
    static bool expanded = false;
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    float height = displaySize.y - 24.0f;
    if (height > 460.0f) height = 460.0f;
    if (height < 220.0f) height = 220.0f;
    float windowHeight = expanded ? height : 135.0f;

    ImGui::SetNextWindowPos(ImVec2(displaySize.x - 12.0f, 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, windowHeight), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(0.0f, 0.0f, 0.0f, 0.90f));
    ImGui::Begin("##FrameStepperAllocators", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                       "MT ALLOCATOR MEMORY");
    ImGui::SameLine();
    if (ImGui::SmallButton(expanded ? "Collapse" : "Expand"))
        expanded = !expanded;
    ImGui::Separator();

    uint64_t totalUsed = 0;
    uint64_t totalPeak = 0;
    uint64_t totalCapacity = 0;
    uint32_t activeCount = 0;
    bool unitMemoryFull = false;
    uint64_t* allocatorTable =
        (uint64_t*)_addr(MEMORY_ALLOCATOR_TABLE);

    if (ImGui::BeginTable("##AllocatorMemoryTable", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
            ImVec2(0.0f, expanded ? height - 92.0f : 58.0f))) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Used / Peak / Capacity / Free (MB)");
        ImGui::TableHeadersRow();

        for (uint32_t slot = 0; slot < MEMORY_ALLOCATOR_COUNT; ++slot) {
            uint64_t allocator = allocatorTable[slot];
            if (!IsReadableMemory(allocator, 0x60)) continue;

            uint64_t used = *(uint64_t*)(allocator + 0x08);
            uint64_t peak = *(uint64_t*)(allocator + 0x10);
            uint64_t capacity = *(uint64_t*)(allocator + 0x18);
            uint64_t freeBytes = capacity > used ? capacity - used : 0;
            const char* name = (const char*)(allocator + 0x21);

            totalUsed += used;
            totalPeak += peak;
            totalCapacity += capacity;
            ++activeCount;
            if (slot == EFFECT_ALLOCATOR_SLOT)
                unitMemoryFull = capacity && freeBytes < 0x40000;
            if (!expanded && slot != EFFECT_ALLOCATOR_SLOT) continue;

            ImGui::TableNextRow();
            if (slot == EFFECT_ALLOCATOR_SLOT) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    IM_COL32(105, 45, 145, 210));
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", slot);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.*s", 15, name);
            ImGui::TableSetColumnIndex(2);
            if (capacity && freeBytes < 0x40000) {
                ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.2f, 1.0f),
                    "%.1f / %.1f / %.1f / %.1f",
                    (double)used / (1024.0 * 1024.0),
                    (double)peak / (1024.0 * 1024.0),
                    (double)capacity / (1024.0 * 1024.0),
                    (double)freeBytes / (1024.0 * 1024.0));
            } else {
                ImGui::Text("%.1f / %.1f / %.1f / %.1f",
                    (double)used / (1024.0 * 1024.0),
                    (double)peak / (1024.0 * 1024.0),
                    (double)capacity / (1024.0 * 1024.0),
                    (double)freeBytes / (1024.0 * 1024.0));
            }
        }
        ImGui::EndTable();
    }

    uint64_t totalFree = totalCapacity > totalUsed
        ? totalCapacity - totalUsed : 0;
    constexpr double MIB = 1024.0 * 1024.0;
    if (expanded) {
        ImGui::Text("Active: %u  Used %.2f MB  Peak %.2f MB",
            activeCount, (double)totalUsed / MIB, (double)totalPeak / MIB);
        ImGui::Text("Capacity %.2f MB  Free %.2f MB",
            (double)totalCapacity / MIB, (double)totalFree / MIB);
        ImGui::TextDisabled(
            "Free is aggregate; fragmentation can still fail allocations.");
    }
    if (unitMemoryFull) {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                           "Unit memory is full!!");
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

void FrameStepper_Draw()
{
    if (!IsTrainingMode())
        return;

    // --- Render the captured static frame as background when paused ---
    // This eliminates the D3D9 double/triple-buffer flicker by ensuring
    // every Present shows the same frozen game image behind the overlays.
    if (isPaused && g_pPauseTexture)
    {
        ImDrawList* bgDrawList = ImGui::GetBackgroundDrawList();
        if (bgDrawList)
        {
            bgDrawList->AddImage(
                (ImTextureID)g_pPauseTexture,
                ImVec2(0.0f, 0.0f),
                ImGui::GetIO().DisplaySize
            );
        }
    }

    // --- Always-visible P1/P2 pad input window (top-left) ---
    if (Controller_GetNetFrameCount() >= 0)
    {
        // sReplay advances during oMove, but processFrameInput records under the
        // frame value from the start of that tick. Display the input that made
        // the currently rendered frame, not the device state for the next tick.
        int32_t frame = Controller_GetNetFrameCount() - 1;
        uint32_t p1 = Controller_GetExpectedInput(0, frame);
        uint32_t p2 = Controller_GetExpectedInput(1, frame);

        char btns1[128], btns2[128];
        AppendButtons(btns1, sizeof(btns1), p1);
        AppendButtons(btns2, sizeof(btns2), p2);

        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        ImGui::Begin("##FrameStepperPad", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize);

        // Netplay status (always visible when netplay is active, shown at the top)
        if (Netplay_IsActive()) {
            const char* role = Netplay_IsHost() ? "HOST" : "CLIENT";
            ImVec4 color = Netplay_IsHost()
                ? ImVec4(1.0f, 0.9f, 0.1f, 1.0f)   // yellow for host
                : ImVec4(0.3f, 0.7f, 1.0f, 1.0f);   // blue for client
            ImGui::TextColored(color, "NETPLAY: %s", role);
            ImGui::TextColored(color, "Ping: %ums", Netplay_GetRTT() / 2);
            int32_t curDelay = Controller_GetDelayFrames();
            int32_t curRollback = Controller_GetRollbackFrames();
            ImGui::TextColored(color, "Frame: %d", Controller_GetNetFrameCount());
            ImGui::TextColored(color, "LastRecv: %d", Controller_GetLastRemoteFrame());
            ImGui::TextColored(color, "Delay: %d  Rollback: %d", curDelay, curRollback);

            // Keep each control row vertical so the overlay stays compact in width.
            if (ImGui::SmallButton("D-"))
            {
                curDelay = Controller_GetDelayFrames() - 1;
                if (curDelay < 1) curDelay = 1;
                ScheduleNetplayDelay(curDelay);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("D+"))
            {
                curDelay = Controller_GetDelayFrames() + 1;
                if (curDelay > 15) curDelay = 15;
                ScheduleNetplayDelay(curDelay);
            }

            if (ImGui::SmallButton("RB-"))
            {
                curRollback = Controller_GetRollbackFrames() - 1;
                if (curRollback < 0) curRollback = 0;
                Controller_SetRollbackFrames(curRollback);
                Netplay_SendRollback(curRollback);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("RB+"))
            {
                curRollback = Controller_GetRollbackFrames() + 1;
                if (curRollback > 16) curRollback = 16;
                Controller_SetRollbackFrames(curRollback);
                Netplay_SendRollback(curRollback);
            }

            bool multiResimEnabled = g_multiResimEnabled;
            if (ImGui::Checkbox("Multi ReSim", &multiResimEnabled))
            {
                Controller_SetMultiResimEnabled(multiResimEnabled);
                Netplay_SendMultiResim(multiResimEnabled);
            }
        }

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "P1: [%s]  0x%08X", DecodeDirection(p1), p1);
        if (btns1[0])
            ImGui::SameLine(), ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", btns1);

        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "P2: [%s]  0x%08X", DecodeDirection(p2), p2);
        if (btns2[0])
            ImGui::SameLine(), ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.8f, 1.0f), "%s", btns2);

        ImGui::PopStyleColor();
        ImGui::End();
    }

    // --- Desync detected overlay (visible in all modes) ---
    if (g_desyncDetected || g_desyncSeen)
    {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                       ImGui::GetIO().DisplaySize.y * 0.18f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.0f, 0.0f, 0.92f));
        ImGui::Begin("##DesyncOverlay", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoInputs);

        ImGui::PushStyleColor(ImGuiCol_Text,
            g_desyncDetected ? ImVec4(1.0f, 0.1f, 0.1f, 1.0f)
                             : ImVec4(0.8f, 0.5f, 0.1f, 1.0f));
        ImGui::SetWindowFontScale(2.5f);
        ImGui::Text("DESYNC DETECTED");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
            "Frame %d: local 0x%08X  remote 0x%08X",
            g_desyncFrame, g_desyncLocalCS, g_desyncRemoteCS);
        if (g_desyncDetected && AutoTest_IsActive()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                "Autotest continuing without pause");
        } else if (g_desyncDetected) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                "Press P to unpause or F11 for match restart");
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                ":(");
        }

        ImGui::End();
        ImGui::PopStyleColor();
    }

    // --- Pause-only content: shown below the pad window when paused ---
    if (!isPaused)
        return;

    ImGui::SetNextWindowPos(ImVec2(12.0f, 80.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
    ImGui::Begin("##FrameStepperPause", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize);

    // Draw pause bars in the dummy space, then text after
    const float sz = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(pos.x + 2.0f, pos.y + 3.0f), ImVec2(pos.x + sz * 0.32f, pos.y + sz - 3.0f),
        IM_COL32(220, 50, 50, 220), 2.0f);
    dl->AddRectFilled(ImVec2(pos.x + sz * 0.38f + 2.0f, pos.y + 3.0f), ImVec2(pos.x + sz * 0.68f + 2.0f, pos.y + sz - 3.0f),
        IM_COL32(220, 50, 50, 220), 2.0f);

    ImGui::Dummy(ImVec2(sz + 6.0f, sz));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "PAUSED  %d", frameCounter);
    /*
    // Input buffer display (captured from cChrInput__getInputBuffer hook)
    if (g_inputBufValid) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "InputBuf @ 0x%llX (obj=0x%llX)", g_inputBufPtr, g_inputBufBase);
        ImGui::Text("  idx      value");
        for (int i = 0; i < 10 && i < g_inputBufFrames; i++) {
            ImGui::Text("  [%d]  0x%08X", i, g_inputBuf[i]);
        }
    }

    // Network-relevant input display (captured from HookedMove after oMove)
    if (g_netDataValid) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Net send(bs+0xd58): 0x%08X", g_netP1Input);
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.8f, 1.0f), "P2 getInput(pad,1): 0x%08X", g_netP2Read);
    }

    // 1. Get the native sAction battle-state singleton.
    typedef uint64_t(__fastcall* GetAction_t)();
    GetAction_t getAction = (GetAction_t)_addr(0x140001A90);
    uint64_t action = getAction();

    if (action) {
        uint32_t flags      = *(uint32_t*)(action + 0xC4);
        int      step       = *(int*)(action + 0xD8);
        int      phase      = *(int*)(action + 0xDC);
        int      transition = *(int*)(action + 0x174);

        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "--- DIAGNOSTICS ---");
        ImGui::Text("sAction: 0x%llX", action);
        ImGui::Text("Flags (+0xC4): 0x%08X", flags);
        ImGui::Text("Step (+0xD8): %d", step);
        ImGui::Text("Phase (+0xDC): %d", phase);
        ImGui::Text("Trans (+0x174): 0x%08X", transition);
    }

    // 2. Get the sGameUI instance
    typedef uint64_t(__fastcall* GetGameUI_t)();
    GetGameUI_t getGameUI = (GetGameUI_t)_addr(0x140001AC0);
    uint64_t ui = getGameUI();

    if (ui) {
        uint64_t uiElement = *(uint64_t*)(ui + 0x5AA8);
        ImGui::Text("sGameUI: 0x%llX", ui);
        ImGui::Text("UI Element (+0x5AA8): 0x%llX", uiElement);
    }*/

    ImGui::PopStyleColor();
    ImGui::End();
    DrawAllocatorMemory();
}

void FrameStepper_DrawAutoTest()
{
    static int scriptIndex = 0;
    static int playerIndex = 0;
    static bool facingLeft = false;

    if (ImGui::BeginCombo("Move", AutoTest_GetScriptName(scriptIndex))) {
        for (int i = 0; i < AutoTest_GetScriptCount(); ++i) {
            bool selected = i == scriptIndex;
            if (ImGui::Selectable(AutoTest_GetScriptName(i), selected))
                scriptIndex = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::RadioButton("P1", &playerIndex, 0);
    ImGui::SameLine();
    ImGui::RadioButton("P2", &playerIndex, 1);
    ImGui::Checkbox("Facing left", &facingLeft);

    AutoTestStatus status = AutoTest_GetStatus();
    AutoTestSuiteStatus suite = AutoTest_GetSuiteStatus();
    if (!suite.active) {
        if (ImGui::SmallButton("Run all commands"))
            AutoTest_StartSuite(playerIndex, facingLeft);
        ImGui::SameLine();
        if (ImGui::SmallButton("Run all variants"))
            AutoTest_StartVariantSuite(playerIndex, facingLeft);
    } else {
        if (ImGui::SmallButton("Stop suite"))
            AutoTest_Stop();
        ImGui::Text("Suite: %d/%d  passed=%d failed=%d",
                    suite.currentCommand + 1, suite.commandCount,
                    suite.passedCount, suite.failedCount);
        ImGui::Text("Current: %s [M%d L%d]",
                    suite.currentName, suite.currentMatcher, suite.currentLine);
        ImGui::Text("Phase: %s",
                    suite.waitingForReset ? "resetting through sAction step 12" :
                    suite.waitingForCompletion ? "waiting for idle" : "running");
        if (suite.waitingForCompletion) {
            ImGui::Text("Quiet: %d/%d  point chars idle=%s (%08X/%08X) shots=%d cutin=%s damage=%s",
                        suite.quietFrames, suite.requiredQuietFrames,
                        suite.charactersStable ? "yes" : "no",
                        suite.attackerAction, suite.defenderAction,
                        suite.shotCount,
                        suite.cutinClear ? "clear" : "active",
                        suite.dealtDamage ? "yes" : "no");
        }
        if (suite.lastResult[0])
            ImGui::TextWrapped("Last: %s", suite.lastResult);
    }

    if (!status.active) {
        if (ImGui::SmallButton("Run move"))
            AutoTest_Start(scriptIndex, playerIndex, facingLeft);
    } else if (ImGui::SmallButton("Stop move")) {
        AutoTest_Stop();
    }

    status = AutoTest_GetStatus();
    ImGui::Text("Input playback: %s  frame: %d  input: %08X",
                status.active ? "running" : "idle",
                status.scriptFrame, status.currentInput);
    if (status.lastSuccessFrame >= 0) {
        ImGui::Text("Success: f%d cmd%d line%d", status.lastSuccessFrame,
                    status.lastSuccessMatcher, status.lastSuccessLine);
        ImGui::Text("list=%d anm=%d action=%08X",
                    status.lastSuccessListId,
                    status.lastSuccessAnimation,
                    status.lastAction);
    }

    if (!ImGui::TreeNode("Runtime commands"))
        return;

    static int commandPlayer = 0;
    static int commandMatcher = -1;
    static bool activeOnly = false;
    static AutoTestRuntimeCommand commands[AUTOTEST_MAX_RUNTIME_COMMANDS];

    int commandCount = AutoTest_GetRuntimeCommands(
        commands, AUTOTEST_MAX_RUNTIME_COMMANDS);

    ImGui::RadioButton("Commands P1", &commandPlayer, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Commands P2", &commandPlayer, 1);
    ImGui::RadioButton("Both matchers", &commandMatcher, -1);
    ImGui::SameLine();
    ImGui::RadioButton("Matcher 0", &commandMatcher, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Matcher 1", &commandMatcher, 1);
    ImGui::Checkbox("Active progress only", &activeOnly);

    const char* executionError = AutoTest_GetRuntimeExecutionError();
    if (executionError[0])
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "%s", executionError);

    int visibleCount = 0;
    for (int i = 0; i < commandCount; ++i) {
        const AutoTestRuntimeCommand& command = commands[i];
        if (command.playerIndex != commandPlayer ||
            (commandMatcher >= 0 && command.matcherIndex != commandMatcher) ||
            (activeOnly && command.cmdStep == 0 && command.time == 0.0f &&
             command.chargeTime == 0.0f && command.padBuf == 0 &&
             command.failTime == 0.0f))
            continue;

        ++visibleCount;
        ImGui::PushID(i);
        if (status.active) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Run"))
            AutoTest_StartRuntimeCommand(commandPlayer, command.matcherIndex,
                                          command.lineIndex, facingLeft,
                                          command.commandListVariantToken);
        if (status.active) ImGui::EndDisabled();
        ImGui::SameLine();
        bool open = ImGui::TreeNodeEx(
            "command", ImGuiTreeNodeFlags_SpanAvailWidth,
            "%s%s%s  [M%d L%d/%d]  step=%d/%d  time=%.3f  charge=%.3f  "
            "pad=%08X  fail=%.3f",
            command.commandListName[0] ? command.commandListName : command.name,
            command.commandListName[0] ? "  |  " : "",
            command.commandListName[0] ? command.name : "",
            command.matcherIndex, command.lineIndex,
            command.lineCount, command.cmdStep, command.commandStepCount,
            command.time, command.chargeTime, command.padBuf, command.failTime);
        if (open) {
            ImGui::Text("command-list name: %s",
                        command.commandListName[0]
                            ? command.commandListName : "<not resolved>");
            ImGui::Text("text resource: %s  records=%d",
                        command.commandListPath[0]
                            ? command.commandListPath : "<not requested>",
                        command.commandListTextCount);
            ImGui::Text("lineFlags=%08X commandState=%d commandFlags=%08X",
                        command.lineFlags, command.commandState,
                        command.commandFlags);
            ImGui::Text("character=%p matcher=%p resource=%p",
                        (void*)command.character, (void*)command.matcher,
                        (void*)command.resource);
            ImGui::Text("cached success: list=%d anm=%d throw=%d "
                        "tdown=%d delay=%.3f work=%d",
                        command.successListId, command.successAnimation,
                        command.successThrow, command.successTdown,
                        command.successDelay, command.successWork);
            ImGui::Separator();
            ImGui::Text("Matcher restrictions");
            ImGui::Text("life %.3f..%.3f  gauge %.3f..%.3f  height %.3f..%.3f",
                        command.lifeRateMin, command.lifeRateMax,
                        command.gaugeRateMin, command.gaugeRateMax,
                        command.heightMin, command.heightMax);
            ImGui::Text("status req=%d forbid=%d  ability req=%d forbid=%d",
                        command.statusRequired, command.statusForbidden,
                        command.abilityRequired, command.abilityForbidden);
            ImGui::Text("jump=%d airSpecial=%d limitFlags=%08X",
                        command.jumpCount, command.airSpecialCount,
                        command.limitFlags);
            ImGui::Text("chr req=%08X forbid=%08X  anm req=%08X forbid=%08X",
                        command.characterFlagsRequired,
                        command.characterFlagsForbidden,
                        command.animationFlagsRequired,
                        command.animationFlagsForbidden);
            ImGui::Text("prg req=%08X forbid=%08X",
                        command.programFlagsRequired,
                        command.programFlagsForbidden);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("%d visible / %d captured", visibleCount, commandCount);
    if (commandCount == AUTOTEST_MAX_RUNTIME_COMMANDS)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                           "Command capture truncated");
    ImGui::TreePop();
}
