// Native input hooks and frame-indexed netplay input scheduling.
// Exchange button-remapped input before character-specific processing;
// predict missing remote input and replay frames when confirmation differs.
#include "../pch.h"
#include "controller.h"
#include "mk10utils.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_diagnostics.h"
#include "rollback_profile.h"
#include "autotest.h"
#include "framestepper.h"
#include "gameplay_hooks.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "LaunchMenu.h"

extern void HookLog(const char* fmt, ...);

extern "C" bool Rollback_GetSnapshotInputs(int32_t frame, uint32_t* outLocal, uint32_t* outRemote);
extern "C" void Rollback_UpdateSnapshotInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput);
extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame);
extern "C" uint32_t FrameStepper_GetBattleGeneration();

static constexpr bool INPUT_PROVENANCE_TRACE = false;
// Prediction waits and their popup checks all run on the game thread.
static bool g_predictionBudgetExhausted = false;
static uint64_t g_predictionBudgetExhaustedSince = 0;

static void ClearPredictionBudgetExhausted()
{
    g_predictionBudgetExhausted = false;
    g_predictionBudgetExhaustedSince = 0;
}

static void SetPredictionBudgetExhausted()
{
    if (!g_predictionBudgetExhausted) {
        g_predictionBudgetExhausted = true;
        g_predictionBudgetExhaustedSince = GetTickCount64();
    }
}

extern "C" bool Controller_IsPredictionBudgetExhausted()
{
    return g_predictionBudgetExhausted;
}

extern "C" bool Controller_ShouldShowPredictionBudgetExhausted()
{
    if (!Controller_IsPredictionBudgetExhausted())
        return false;
    const uint64_t since = g_predictionBudgetExhaustedSince;
    return since != 0 && GetTickCount64() - since >= 4000;
}

static void TraceInputPacket(const char* marker, const char* path, int32_t localFrame,
                             const InputPacket& packet, uint16_t validMask,
                             uint16_t acceptedMask, uint16_t changedMask,
                             uint16_t priorConfirmedMask)
{
    if (!INPUT_PROVENANCE_TRACE) return;

    char inputs[NET_HISTORY_SIZE * 11 + 1] = {};
    size_t offset = 0;
    for (size_t i = 0; i < NET_HISTORY_SIZE && offset < sizeof(inputs); ++i) {
        int written = std::snprintf(inputs + offset, sizeof(inputs) - offset,
                                    "%s%08X", i ? "," : "", packet.inputs[i]);
        if (written < 0 || (size_t)written >= sizeof(inputs) - offset) break;
        offset += (size_t)written;
    }

    HookLog("%s path=%s local=%d start=%d end=%d flags=%u valid=%04X "
            "accepted=%04X changed=%04X priorConfirmed=%04X inputs=%s\n",
            marker, path, localFrame, (int32_t)packet.start_frame,
            (int32_t)packet.start_frame + (int32_t)NET_HISTORY_SIZE - 1,
            packet.flags, validMask, acceptedMask, changedMask,
            priorConfirmedMask, inputs);
}

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
static constexpr uintptr_t ADDR_GET_CURRENT_INPUT        = 0x1402B3700;
static constexpr uintptr_t ADDR_PAD_SLOT_READ_DEVICE     = 0x140514B90;
static constexpr uintptr_t ADDR_PAD_POLL_ALL_SLOTS       = 0x140514570;
static constexpr uintptr_t ADDR_XINPUT_TO_GAME_MASK      = 0x1405111B0;
static constexpr uintptr_t ADDR_PAD_SLOT_POST_READ       = 0x1405146A0;
static constexpr uintptr_t ADDR_INPUT_AUTOREPEAT         = 0x140514F20;
static constexpr uintptr_t ADDR_PAD_GET_VIB_MOTOR        = 0x140510620;
static constexpr uintptr_t ADDR_PAD_VIB_PROCESS          = 0x140514D90;
static constexpr uintptr_t ADDR_PAD_CHECK_STOP_VIB       = 0x140514ED0;
static constexpr uintptr_t ADDR_KEYBOARD_TO_GAME_MASK    = 0x1402B2FA0;
static constexpr uintptr_t ADDR_PAD_GET_INSTANCE         = 0x140001B00;
static constexpr uintptr_t ADDR_MVC3_MGR_GET_INSTANCE    = 0x140001AF0;
static constexpr uintptr_t ADGR_MGR_GET_PLAYER_CHAR      = 0x14025AC40;
static constexpr uintptr_t ADDR_METHOD_3D00              = 0x1402B3D00;

// Native character-input processing and remapping.
static constexpr uintptr_t ADDR_PROCESS_FRAME_INPUT       = 0x14002DD60;
static constexpr uintptr_t ADDR_IS_GAMEPLAY_ACTIVE        = 0x14002DCE0;
static constexpr uintptr_t ADDR_BUTTON_REMAP              = 0x14002DA30;
static constexpr uintptr_t ADDR_OPENING_REMAP_RETURN      = 0x14000650C;
static constexpr uintptr_t ADDR_SWAP_LR_IF_FACE_LEFT      = 0x14002E560;
static constexpr uintptr_t ADDR_CPU_INPUT_SWAP_RETURN     = 0x14002DE57;
// Character team index: 0 = P1, 1 = P2.
static constexpr uintptr_t ADDR_GET_TEAM_INDEX            = 0x140004AC0;

// Hold the intro until the netplay battle barrier releases.
static constexpr uintptr_t ADDR_STEP_INTRO                 = 0x140007D90;
// Derive fight clocks from the shared netplay frame.
static constexpr uintptr_t ADDR_STEP_FIGHTING              = 0x140007080;
// Winner outro and result-screen flow.
static constexpr uintptr_t ADDR_STEP_RESULT8               = 0x14000A990;
static constexpr uintptr_t ADDR_STEP_RESULT9_10_11         = 0x14000A3B0;
static constexpr uintptr_t ADDR_PAD_RAW_INPUT              = 0x1402B3B80;
static constexpr uintptr_t ADDR_PAD_UNIT_FLAGS             = 0x140001A70;

// Character manager singleton (for desync checksum traversal)
static constexpr uintptr_t PTR_SCHARACTER                  = 0x140d44a70;
static constexpr uintptr_t PTR_GLOBAL_RNG                  = 0x140d765d8;

// ---------------------------------------------------------------------------
// Original function pointers (MinHook trampolines)
// ---------------------------------------------------------------------------
static GetsCurrentInput_withSOCD_t oGetsCurrentInput_withSOCD = nullptr;
static padSlotReadDevice_t         opadSlotReadDevice         = nullptr;
static padPollAllSlots_t           opadPollAllSlots           = nullptr;
static xinputToGameMask_t          oxinputToGameMask          = nullptr;
static padSlotPostReadProcess_t    opadSlotPostReadProcess    = nullptr;
static inputAutoRepeatTimerProcess_t oinputAutoRepeatTimerProcess = nullptr;

static void(__fastcall* oprocessFrameInput)(uint64_t cChrInput) = nullptr;
static uint32_t(__fastcall* obuttonRemap)(uint64_t playerIndex, uint32_t input,
                                          uint8_t alternateMap) = nullptr;
static uint32_t(__fastcall* oswapLRIfFaceLeft)(uint64_t cChrInput,
                                               uint32_t input) = nullptr;
static uint32_t g_openingMembers[2] = {};
static bool g_openingMembersValid = false;

// ===========================================================================
// Native-style input helpers
// ===========================================================================
namespace ControllerDecomp {

static uint32_t keyboardToGameMask(uint64_t sPad)
{
    typedef uint64_t(__fastcall* method_3D00_t)(uint64_t, uint32_t, uint32_t);
    static method_3D00_t fn3D00 = (method_3D00_t)_addr(ADDR_METHOD_3D00);

    uint32_t r = 0;

    // Arrow keys.
    if (fn3D00(sPad, 0x28, 0)) r |= IN_DOWN;
    if (fn3D00(sPad, 0x26, 0)) r |= IN_UP;
    if (fn3D00(sPad, 0x25, 0)) r |= IN_LEFT;
    if (fn3D00(sPad, 0x27, 0)) r |= IN_RIGHT;

    // Keyboard action bindings.
    if (fn3D00(sPad, 0x41, 0)) r |= IN_X;       // A: Light
    if (fn3D00(sPad, 0x53, 0)) r |= IN_Y;       // S: Medium
    if (fn3D00(sPad, 0x44, 0)) r |= IN_B;       // D: Heavy
    if (fn3D00(sPad, 0x5A, 0)) r |= IN_X;       // Z: Light
    if (fn3D00(sPad, 0x58, 0)) r |= IN_Y;       // X: Medium
    if (fn3D00(sPad, 0x43, 0)) r |= IN_B;       // C: Heavy
    if (fn3D00(sPad, 0x46, 0)) r |= IN_A;       // F: Special
    if (fn3D00(sPad, 0x51, 0)) r |= IN_LB;      // Q: Assist 1
    if (fn3D00(sPad, 0x57, 0)) r |= IN_RB;      // W: Assist 2

    // Enter/A and Backspace/B use the native menu masks.
    if (fn3D00(sPad, 0x0D, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A70))(sPad);
    if (fn3D00(sPad, 0x41, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A70))(sPad);

    if (fn3D00(sPad, 0x08, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A50))(sPad);
    if (fn3D00(sPad, 0x42, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A50))(sPad);

    // Modifiers and menu keys.
    if (fn3D00(sPad, 0xA2, 0)) r |= IN_LB;      // L-Ctrl
    if (fn3D00(sPad, 0xA0, 0)) r |= IN_LT;      // L-Shift
    if (fn3D00(sPad, 0xA4, 0)) r |= IN_L3;      // L-Alt
    if (fn3D00(sPad, 0xA3, 0)) r |= IN_RB;      // R-Ctrl
    if (fn3D00(sPad, 0xA1, 0)) r |= IN_RT;      // R-Shift
    if (fn3D00(sPad, 0xA5, 0)) r |= IN_R3;      // R-Alt
    if (fn3D00(sPad, 0x09, 0)) r |= IN_BACK;    // Tab
    if (fn3D00(sPad, 0x1B, 0)) r |= IN_START;   // Esc

    return r;
}

} // namespace ControllerDecomp

// ---------------------------------------------------------------------------
// Resolve simultaneous opposing direction bits.
// ---------------------------------------------------------------------------
static uint32_t ApplySOCD(uint32_t input)
{
    if ((input & 0x80020) == 0x80020) input &= 0xFFFFFFDF;
    if ((input & 0x20080) == 0x20080)   input &= 0xFFFDFFFF;
    if (((uint8_t)input & 0xA0) == 0xA0) input &= 0xFFFFFFDF;
    if ((input & 0xA0000) == 0xA0000)   input &= 0xFFFDFFFF;
    if ((input & 0x40010) == 0x40010)   input &= 0xFFFBFFFF;
    if ((input & 0x10040) == 0x10040)   input &= 0xFFFFFFBF;
    if (((uint8_t)input & 0x50) == 0x50) input &= 0xFFFFFFBF;
    if ((input & 0x50000) == 0x50000)   input &= 0xFFFBFFFF;
    return input;
}

static bool IsGameProcessForeground()
{
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

// ===========================================================================
// Netplay input history and absolute-frame schedules.
// Ring slots use frame % RING_SIZE; frame tags distinguish reused slots.
// ===========================================================================
static constexpr int RING_SIZE      = 256;
static constexpr int MAX_HISTORY    = NET_HISTORY_SIZE;

static uint32_t  g_oldInputs[MAX_HISTORY];     // Legacy capture history, not the delay schedule.

// Delay changes affect new captures, not the frame labels of queued input.
static uint32_t  g_scheduledLocalInput[RING_SIZE];
static int32_t   g_scheduledLocalFrame[RING_SIZE];

// Remote input on the shared frame timeline, tagged by g_confirmedFrame.
static uint32_t  g_receivedInput[RING_SIZE];

// Absolute frame tags prevent stale ring-slot reads.
static int32_t   g_confirmedFrame[RING_SIZE];

// Prediction frame tags, also used to suppress duplicate logs.
static int32_t   g_predictedFrame[RING_SIZE];

// Raw physical input for offline replay: [player][frame % RING_SIZE].
static uint32_t  g_rawLocalInput[2][RING_SIZE];

// Final character input for diagnostics and offline replay, not the netplay wire format.
static uint32_t  g_recordedGameInput[2][RING_SIZE];

// Input delay, prediction window, and scheduled setting changes.
static int32_t   g_delayFrames     = 3;
static int32_t   g_pendingDelayFrames = -1;
static int32_t   g_pendingDelayEffectiveFrame = -1;
static int32_t   g_rollbackFrames  = 7;
static int32_t   g_pendingRollbackFrames = -1;
static int32_t   g_pendingRollbackEffectiveFrame = -1;

static bool IsFrameConfirmed(int32_t frame)
{
    if (frame < 0) return false;
    // Both peers use neutral startup input; no packet is needed to confirm it.
    if (frame <= g_delayFrames + g_rollbackFrames) return true;
    return g_confirmedFrame[frame % RING_SIZE] == frame;
}

extern "C" bool Controller_IsFrameConfirmed(int32_t frame)
{
    return IsFrameConfirmed(frame);
}

// Whether the selected remote input was predicted.
static bool      g_predictedInput[RING_SIZE];

// Selected remote input, compared with received input to find rollback conflicts.
static uint32_t  g_usedRemoteInput[RING_SIZE];

// Input used by the last simulation of each frame, independent of later selection changes.
static uint32_t  g_processedRemoteInput[RING_SIZE];
static int32_t   g_processedRemoteFrame[RING_SIZE];

// Executed local input for diagnostics and resimulation fallback.
static uint32_t  g_recordedLocalInput[RING_SIZE];

// Shared logical frame, read from the native sReplay cursor.
static int32_t   g_netFrameCount   = -1;
static uint32_t  g_localPending    = 0;   // accumulator, ORed across team chars

// Newest received remote input; fallback when prediction finds no earlier confirmation.
static int32_t   g_lastRemoteFrame = -1;
static uint32_t  g_lastRemoteInput = 0;
static int32_t   g_lastNonMultiCheckedFrame = -1;

bool g_multiResimEnabled = true; // Multi-resimulation toggle

// ---------------------------------------------------------------------------
// Per-frame character/gameplay-RNG checksums and desync status.
// ---------------------------------------------------------------------------
bool     g_desyncDetected     = false;
bool     g_desyncSeen         = false;
int32_t  g_desyncFrame        = -1;
uint32_t g_desyncLocalCS      = 0;
uint32_t g_desyncRemoteCS     = 0;

static uint32_t  g_localChecksum[RING_SIZE];
static uint32_t  g_remoteChecksum[RING_SIZE];
static int32_t   g_localChecksumFrame[RING_SIZE];
static int32_t   g_remoteChecksumFrame[RING_SIZE];
static bool      g_localChecksumConfirmed[RING_SIZE];
static uint32_t  g_localChecksumRemoteInput[RING_SIZE];
static int32_t   g_localChecksumAdvertisedFrame[RING_SIZE];
static int32_t   g_desyncChecked[RING_SIZE];   // frame number when checksum was compared, -1 = unchecked
static bool      g_desyncMismatch[RING_SIZE];  // true if that frame exceeded threshold

static constexpr uint32_t DESYNC_THRESHOLD         = 0;
// Skip transient startup state before comparing checksums.
static constexpr int32_t  DESYNC_SKIP_FRAMES       = 50;
static constexpr int32_t  DESYNC_CONSECUTIVE_MIN   = 1;

static void ResetDesyncDetection()
{
    g_desyncDetected = false;
    g_desyncSeen = false;
    g_desyncFrame = -1;
    g_desyncLocalCS = 0;
    g_desyncRemoteCS = 0;
    std::memset(g_localChecksum, 0, sizeof(g_localChecksum));
    std::memset(g_remoteChecksum, 0, sizeof(g_remoteChecksum));
    std::memset(g_localChecksumFrame, 0xFF, sizeof(g_localChecksumFrame));
    std::memset(g_remoteChecksumFrame, 0xFF, sizeof(g_remoteChecksumFrame));
    std::memset(g_localChecksumConfirmed, 0, sizeof(g_localChecksumConfirmed));
    std::memset(g_localChecksumRemoteInput, 0,
                sizeof(g_localChecksumRemoteInput));
    std::memset(g_localChecksumAdvertisedFrame, 0xFF,
                sizeof(g_localChecksumAdvertisedFrame));
    std::memset(g_desyncChecked, 0xFF, sizeof(g_desyncChecked));
    std::memset(g_desyncMismatch, 0, sizeof(g_desyncMismatch));
}

// ---------------------------------------------------------------------------
// RTT-based frame-advantage estimates.
// ---------------------------------------------------------------------------
int32_t      g_localFrameAdvantage  = 0;  // negative = we're ahead of remote
int32_t      g_remoteFrameAdvantage = 0;  // received from peer via packet

// Averaging window and wait-recommendation limits; sleeping is currently disabled.
static constexpr int TIMESYNC_WINDOW_SIZE       = 40;
static constexpr int TIMESYNC_MIN_FRAME_ADVANTAGE = 3;
static constexpr int TIMESYNC_MAX_FRAME_ADVANTAGE = 14;

static int32_t g_localAdvHistory[TIMESYNC_WINDOW_SIZE] = {};
static int32_t g_remoteAdvHistory[TIMESYNC_WINDOW_SIZE] = {};
static int32_t g_timesyncFrameCount = 0;

// Log the contents of a received input packet: which frames got which inputs.
static void LogReceivedPacket(const InputPacket& pkt)
{
    return;
    int32_t start = (int32_t)pkt.start_frame;
    int32_t end = start + NET_HISTORY_SIZE - 1;
    HookLog("[%s] RCV pkt frames [%d-%d]:",
            Netplay_IsHost() ? "H" : "C", start, end);
    for (size_t i = 0; i < NET_HISTORY_SIZE; i++)
        HookLog(" %d=0x%04X", start + (int32_t)i, pkt.inputs[i]);
    HookLog("\n");
}

// Native character-team list layout.
static constexpr uintptr_t TEAM0_OFFSET          = 0x40;
static constexpr uintptr_t TEAM1_OFFSET          = 0x300;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t NODE_CHR_OFFSET       = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET      = 0x10;

// ---------------------------------------------------------------------------
// Read sReplay+0x54 relative to the frame epoch; return -1 before initialization.
// ---------------------------------------------------------------------------
static constexpr uintptr_t G_PSREPLAY_PTR = 0x140d4d9d8;
static int32_t g_gameFrameBase = 0;
static int32_t g_fightFrameBase = -1;

static int32_t GetGameFrameCount()
{
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (!sReplay) return -1;
    return *(int32_t*)(sReplay + 0x54) - g_gameFrameBase;
}

extern "C" void Controller_ResetFrameEpoch()
{
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    // Reset both native cursors at the battle barrier so frame labels match.
    if (sReplay)
        *(int32_t*)(sReplay + 0x54) = 0;
    g_gameFrameBase = 0;
    g_netFrameCount = 0;
    g_fightFrameBase = -1;
}

extern "C" void Controller_ResetFightTimer(uint64_t sAction)
{
    g_fightFrameBase = GetGameFrameCount();
    if (!sAction)
        return;

    *(float*)(sAction + 0xF8) = 180.0f;
    *(float*)(sAction + 0xFC) = 0.0f;
    *(float*)(sAction + 0x100) = 0.0f;
    *(float*)(sAction + 0x104) = 0.0f;
}

// ---------------------------------------------------------------------------
// Reset buffers — called from Netplay_Init
// ---------------------------------------------------------------------------
extern "C" void Controller_ResetBuffers()
{
    std::memset(g_oldInputs,          0, sizeof(g_oldInputs));
    std::memset(g_scheduledLocalInput, 0, sizeof(g_scheduledLocalInput));
    std::memset(g_scheduledLocalFrame, -1, sizeof(g_scheduledLocalFrame));
    std::memset(g_receivedInput,      0, sizeof(g_receivedInput));
    std::memset(g_confirmedFrame,    -1, sizeof(g_confirmedFrame));
    std::memset(g_predictedInput,     0, sizeof(g_predictedInput));
    std::memset(g_predictedFrame,    -1, sizeof(g_predictedFrame));
    std::memset(g_usedRemoteInput,    0, sizeof(g_usedRemoteInput));
    std::memset(g_processedRemoteInput, 0, sizeof(g_processedRemoteInput));
    std::memset(g_processedRemoteFrame, -1, sizeof(g_processedRemoteFrame));
    std::memset(g_recordedLocalInput, 0, sizeof(g_recordedLocalInput));
    std::memset(g_rawLocalInput,      0, sizeof(g_rawLocalInput));
    std::memset(g_recordedGameInput,  0, sizeof(g_recordedGameInput));
    // g_netFrameCount is read from sReplay each frame; no reset needed.
    g_localPending    = 0;
    g_pendingDelayFrames = -1;
    g_pendingDelayEffectiveFrame = -1;
    g_pendingRollbackFrames = -1;
    g_pendingRollbackEffectiveFrame = -1;
    g_lastRemoteFrame = -1;
    g_lastRemoteInput = 0;
    g_lastNonMultiCheckedFrame = -1;
    ResetDesyncDetection();
    g_localFrameAdvantage  = 0;
    g_remoteFrameAdvantage = 0;
    g_timesyncFrameCount   = 0;
    std::memset(g_localAdvHistory,  0, sizeof(g_localAdvHistory));
    std::memset(g_remoteAdvHistory, 0, sizeof(g_remoteAdvHistory));
}

// ---------------------------------------------------------------------------
// Debug accessors — called from framestepper draw
// ---------------------------------------------------------------------------
extern "C" int32_t Controller_GetDelayFrames()      { return g_delayFrames; }
extern "C" bool Controller_IsDelayChangePending()   { return g_pendingDelayFrames >= 0; }
extern "C" int32_t Controller_GetNetFrameCount()    { return GetGameFrameCount(); }
extern "C" int32_t Controller_GetLastRemoteFrame()  { return g_lastRemoteFrame; }
extern "C" int32_t Controller_GetRollbackFrames()   { return g_rollbackFrames; }
extern "C" bool Controller_IsRollbackChangePending(){ return g_pendingRollbackFrames >= 0; }
extern "C" void Controller_ClearDesyncDetection()
{
    ResetDesyncDetection();
    Rollback_ClearPendingDesyncCapture();
}
extern "C" uint32_t Controller_GetRecordedLocalInput(int32_t frame)
{
    if (frame < 0) return 0;
    return g_recordedLocalInput[(uint32_t)frame % RING_SIZE];
}
extern "C" uint32_t Controller_GetUsedRemoteInput(int32_t frame)
{
    if (frame < 0) return 0;
    return g_usedRemoteInput[(uint32_t)frame % RING_SIZE];
}
extern "C" uint32_t Controller_GetExpectedInput(uint32_t teamIdx, int32_t frame)
{
    if (frame < 0) return 0;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    return g_recordedGameInput[teamIdx & 1][idx];
}
extern "C" uint32_t Controller_GetReceivedRemoteInput(int32_t frame)
{
    if (frame < 0) return 0;
    return g_receivedInput[(uint32_t)frame % RING_SIZE];
}
extern "C" int32_t  Controller_GetLocalFrameAdvantage()  { return g_localFrameAdvantage; }
extern "C" int32_t  Controller_GetRemoteFrameAdvantage() { return g_remoteFrameAdvantage; }
extern "C" bool    Controller_IsRollbackEnabled()   { return g_rollbackFrames > 0; }

extern "C" void Controller_SetMultiResimEnabled(bool enabled)
{
    if (g_multiResimEnabled == enabled)
        return;

    g_multiResimEnabled = enabled;

    // Do not revisit pre-transition frames whose snapshots may have expired.
    int32_t transitionFrame = GetGameFrameCount();
    if (transitionFrame >= 0)
        g_lastNonMultiCheckedFrame = transitionFrame;

    HookLog("RollbackNet: multi-resim %s at frame %d\n",
            enabled ? "enabled" : "disabled", transitionFrame);
}

extern "C" void Controller_SetRollbackFrames(int32_t n)
{
    if (n < 0)                  n = 0;
    if (n > ROLLBACK_WINDOW)    n = ROLLBACK_WINDOW;
    if (n == 0 && (g_delayFrames == 0 || g_pendingDelayFrames == 0))
        n = 1;
    if (n == g_rollbackFrames)
        return;

    g_rollbackFrames = n;
    if (n == 0)
    {
        std::memset(g_predictedInput, 0, sizeof(g_predictedInput));
        std::memset(g_predictedFrame, -1, sizeof(g_predictedFrame));
        Rollback_ClearPendingReSimulation();
    }

    // Discard checksums produced under the previous rollback setting.
    Controller_ClearDesyncDetection();
}

extern "C" void Controller_ScheduleRollbackFrames(int32_t n, int32_t effectiveFrame)
{
    if (n < 0) n = 0;
    if (n > ROLLBACK_WINDOW) n = ROLLBACK_WINDOW;
    if (n == 0 && (g_delayFrames == 0 || g_pendingDelayFrames == 0))
        n = 1;
    g_pendingRollbackFrames = n;
    g_pendingRollbackEffectiveFrame = effectiveFrame;
    HookLog("Netplay: rollback change to %d scheduled for frame %d\n",
            n, effectiveFrame);
}

// ---------------------------------------------------------------------------
// Controller state save/restore for rollback snapshots
// ---------------------------------------------------------------------------
static uint32_t g_savedOldInputs[MAX_HISTORY];
static uint32_t g_savedLocalPending;

extern "C" void Controller_SaveRollbackState()
{
    memcpy(g_savedOldInputs, g_oldInputs, sizeof(g_oldInputs));
    g_savedLocalPending = g_localPending;
}

extern "C" void Controller_RestoreRollbackState()
{
    memcpy(g_oldInputs, g_savedOldInputs, sizeof(g_oldInputs));
    g_localPending = g_savedLocalPending;
}

// Return scheduled input for the current frame, or neutral if none is queued.
extern "C" uint32_t Controller_GetCurrentFrameInput()
{
    int32_t frame = GetGameFrameCount();
    if (frame < 0) return 0;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    return g_scheduledLocalFrame[idx] == frame ? g_scheduledLocalInput[idx] : 0;
}

// Override offline input history; netplay replay reads pre-facing snapshot input.
extern "C" void Controller_InjectLocalInput(int32_t frame, uint32_t input)
{
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % RING_SIZE;
    uint32_t localTeam = Netplay_IsHost() ? 0 : 1;
    g_recordedGameInput[localTeam][idx] = input;
}

// Set the remote input slot used by resimulation fallback.
extern "C" void Controller_InjectRemoteInput(int32_t frame, uint32_t input)
{
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % RING_SIZE;
    g_usedRemoteInput[idx] = input;
}

// Preserve live capture history and pending input across resimulation.
static uint32_t g_savedInputHistory[MAX_HISTORY];
static uint32_t g_savedLocalPendingReSim;
static int64_t g_saveRestorePairId = 0; // pairs Save/Restore calls for log analysis

// Helper: format g_oldInputs buffer into a HookLog line
static void LogOldInputs(const char* label)
{
    return;
    HookLog("[g_oldInputs %s] %08X %08X %08X %08X  %08X %08X %08X %08X  "
            "%08X %08X %08X %08X  %08X %08X %08X %08X  pending=%08X\n",
            label,
            g_oldInputs[0], g_oldInputs[1], g_oldInputs[2], g_oldInputs[3],
            g_oldInputs[4], g_oldInputs[5], g_oldInputs[6], g_oldInputs[7],
            g_oldInputs[8], g_oldInputs[9], g_oldInputs[10], g_oldInputs[11],
            g_oldInputs[12], g_oldInputs[13], g_oldInputs[14], g_oldInputs[15],
            g_localPending);
}

extern "C" void Controller_DumpOldInputs(const char* label)
{
    return;
    LogOldInputs(label);
}

extern "C" void Controller_SaveInputHistory()
{
    int64_t pairId = g_saveRestorePairId++;
    //HookLog("[InputHistory] Save #%lld — saving g_oldInputs before re-sim\n", pairId);
    LogOldInputs("pre-save");
    memcpy(g_savedInputHistory, g_oldInputs, sizeof(g_oldInputs));
    g_savedLocalPendingReSim = g_localPending;
}

extern "C" void Controller_RestoreInputHistory()
{
    //HookLog("[InputHistory] Restore — restoring g_oldInputs after re-sim\n");
    memcpy(g_oldInputs, g_savedInputHistory, sizeof(g_oldInputs));
    g_localPending = g_savedLocalPendingReSim;
    LogOldInputs("post-restore");
}

static void QueueInboundChecksum(const InputPacket& packet)
{
    if (!FrameStepper_IsDesyncCheckReady() ||
        g_netFrameCount < DESYNC_SKIP_FRAMES)
        return;

    int32_t frame = (int32_t)packet.checksum_frame;
    if (frame < 0)
        return;

    uint32_t idx = (uint32_t)frame % RING_SIZE;
    if (g_remoteChecksumFrame[idx] == frame &&
        g_remoteChecksum[idx] != packet.checksum &&
        g_desyncChecked[idx] == frame)
    {
        g_desyncChecked[idx] = -1;
        g_desyncMismatch[idx] = false;
    }
    g_remoteChecksum[idx] = packet.checksum;
    g_remoteChecksumFrame[idx] = frame;
}

// Select confirmed or predicted remote input for a live or resimulated frame.
extern "C" void Controller_ResolveRemoteInput(int32_t frame)
{
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    if (IsFrameConfirmed(frame))
    {
        g_predictedInput[idx] = false;
        g_usedRemoteInput[idx] = g_receivedInput[idx];
        if (INPUT_PROVENANCE_TRACE) {
            HookLog("IPUSE phase=%s frame=%d source=confirmed value=%08X\n",
                    g_isResimulating ? "resim" : "live", frame,
                    g_usedRemoteInput[idx]);
        }
    }
    else
    {
        // Drain any inbound packets that arrived since last check
        InputPacket pkt;
        while (Netplay_GetInboundPacket(pkt))
        {
            if (pkt.battle_generation != FrameStepper_GetBattleGeneration())
                continue;

            // Stale input packets may still carry a new checksum.
            QueueInboundChecksum(pkt);

            int32_t pktMaxFrame = (int32_t)pkt.start_frame + NET_HISTORY_SIZE - 1;
            if (pktMaxFrame <= g_lastRemoteFrame) {
                TraceInputPacket("IPRX", "resolve-checksum-only", g_netFrameCount, pkt,
                                 0, 0, 0, 0);
                continue;
            }
            uint16_t acceptedMask = 0;
            uint16_t changedMask = 0;
            uint16_t priorConfirmedMask = 0;
            for (size_t i = 0; i < NET_HISTORY_SIZE; i++)
            {
                int32_t targetFrame = (int32_t)pkt.start_frame + (int32_t)i;
                if (targetFrame >= 0)
                {
                    uint32_t targetIdx = (uint32_t)targetFrame % RING_SIZE;
                    bool wasConfirmed = IsFrameConfirmed(targetFrame);
                    uint32_t previous = g_receivedInput[targetIdx];
                    if (wasConfirmed) priorConfirmedMask |= (uint16_t)(1u << i);
                    if (!wasConfirmed || previous != pkt.inputs[i])
                        changedMask |= (uint16_t)(1u << i);
                    acceptedMask |= (uint16_t)(1u << i);
                    g_receivedInput[targetIdx] = pkt.inputs[i];
                    g_confirmedFrame[targetIdx] = targetFrame;
                    if (targetFrame > g_lastRemoteFrame)
                    {
                        g_lastRemoteFrame = targetFrame;
                        g_lastRemoteInput = pkt.inputs[i];
                    }
                }
            }

            LogReceivedPacket(pkt);
            TraceInputPacket("IPRX", "resolve", g_netFrameCount, pkt, 0,
                             acceptedMask, changedMask, priorConfirmedMask);
        }

        if (IsFrameConfirmed(frame))
        {
            g_predictedInput[idx] = false;
            g_usedRemoteInput[idx] = g_receivedInput[idx];
            if (INPUT_PROVENANCE_TRACE) {
                HookLog("IPUSE phase=%s frame=%d source=confirmed-after-drain value=%08X\n",
                        g_isResimulating ? "resim" : "live", frame,
                        g_usedRemoteInput[idx]);
            }
            return;
        }

        // Delay-only mode waits for confirmation; never predict for other callers.
        if (g_rollbackFrames == 0)
        {
            g_predictedInput[idx] = false;
            g_predictedFrame[idx] = -1;
            HookLog("Netplay: refusing to predict unconfirmed frame %d with rollback disabled\n",
                    frame);
            return;
        }

        if (g_isResimulating && !g_multiResimEnabled)
        {
            HookLog("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
                    "!!! HORRIBLE WARNING: ReSim frame %d — remote input NOT YET     !!!\n"
                    "!!! received! Injecting PREDICTED input instead. This means the !!!\n"
                    "!!! rollback may produce INCORRECT results!                      !!!\n"
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n",
                    frame);
        }

        uint32_t prediction = 0;
        bool found = false;
        int32_t predictionSourceFrame = -1;
        for (int32_t f = frame - 1; f >= 0 && f >= frame - 250; f--)
        {
            if (IsFrameConfirmed(f))
            {
                prediction = g_receivedInput[f % RING_SIZE];
                found = true;
                predictionSourceFrame = f;
                break;
            }
        }
        if (!found)
        {
            prediction = g_lastRemoteInput;
        }
        bool alreadyPredicted = (g_predictedFrame[idx] == frame);
        g_predictedInput[idx] = true;
        g_predictedFrame[idx] = frame;
        g_usedRemoteInput[idx] = prediction;

        if (INPUT_PROVENANCE_TRACE) {
            HookLog("IPUSE phase=%s frame=%d source=prediction sourceFrame=%d "
                    "value=%08X lastRemoteFrame=%d lastRemoteInput=%08X\n",
                    g_isResimulating ? "resim" : "live", frame,
                    predictionSourceFrame, prediction, g_lastRemoteFrame,
                    g_lastRemoteInput);
        }

        if (!alreadyPredicted)
        {
            //HookLog("Predicting for frame %d using input 0x%08X (lastRemote=%d)\n",
                    //frame, prediction, g_lastRemoteFrame);
        }
    }
}

extern "C" void Controller_MarkFrameInputProcessed(int32_t frame)
{
    if (frame < 0)
        return;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    g_processedRemoteInput[idx] = g_usedRemoteInput[idx];
    g_processedRemoteFrame[idx] = frame;
}

// Log the inputs used during a re-simmed frame for debugging.
extern "C" void Controller_LogReSimFrame(int32_t frame)
{
    return;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    HookLog("[%s] ReSim frame %d: local=0x%08X remote=0x%08X recv=0x%08X\n",
            Netplay_IsHost() ? "host" : "client",
            frame,
            g_recordedLocalInput[idx],
            g_usedRemoteInput[idx],
            IsFrameConfirmed(frame) ? g_receivedInput[idx] : 0);
}

// Update capture history during resimulation without exchanging packets.
extern "C" void Controller_SilentFrameStart()
{
    g_localPending = 0;
}

extern "C" void Controller_SilentFrameEnd()
{
    memmove(&g_oldInputs[0], &g_oldInputs[1],
            (MAX_HISTORY - 1) * sizeof(uint32_t));
    g_oldInputs[MAX_HISTORY - 1] = g_localPending;
}

extern "C" void Controller_SetDelayFrames(int32_t delay)
{
    if (delay < 0)                  delay = 0;
    if (delay > MAX_HISTORY - 1)    delay = MAX_HISTORY - 1;
    if (delay == 0 && g_rollbackFrames == 0)
        delay = 1;

    if (delay == g_delayFrames)
        return;

    g_delayFrames = delay;
    HookLog("Netplay: delay changed to %d\n", delay);
}

extern "C" void Controller_ScheduleDelayFrames(int32_t delay, int32_t effectiveFrame)
{
    if (delay < 0) delay = 0;
    if (delay > MAX_HISTORY - 1) delay = MAX_HISTORY - 1;
    if (delay == 0 &&
        (g_rollbackFrames == 0 || g_pendingRollbackFrames == 0))
        delay = 1;
    g_pendingDelayFrames = delay;
    g_pendingDelayEffectiveFrame = effectiveFrame;
    HookLog("Netplay: delay change to %d scheduled for frame %d\n", delay, effectiveFrame);
}

static void ApplyScheduledDelayIfReady()
{
    if (g_pendingDelayFrames < 0 || g_netFrameCount < g_pendingDelayEffectiveFrame)
        return;

    const int32_t newDelay = g_pendingDelayFrames;

    // Fill the gap from a delay increase with the newest queued input.
    // These frames have not been sent yet.
    if (newDelay > g_delayFrames)
    {
        const int32_t sourceFrame = g_netFrameCount + g_delayFrames - 1;
        const uint32_t sourceIdx = (uint32_t)sourceFrame % RING_SIZE;
        const uint32_t fillInput =
            g_scheduledLocalFrame[sourceIdx] == sourceFrame
                ? g_scheduledLocalInput[sourceIdx] : 0;
        for (int32_t frame = g_netFrameCount + g_delayFrames;
             frame < g_netFrameCount + newDelay; ++frame)
        {
            const uint32_t idx = (uint32_t)frame % RING_SIZE;
            g_scheduledLocalInput[idx] = fillInput;
            g_scheduledLocalFrame[idx] = frame;
        }
    }

    g_delayFrames = newDelay == 0 && g_rollbackFrames == 0 ? 1 : newDelay;
    g_pendingDelayFrames = -1;
    g_pendingDelayEffectiveFrame = -1;
    HookLog("Netplay: applied delay %d at frame %d\n",
            g_delayFrames, g_netFrameCount);
}

static bool IsRollbackChangeDue()
{
    return g_pendingRollbackFrames >= 0 &&
           g_netFrameCount >= g_pendingRollbackEffectiveFrame;
}

static void ApplyScheduledRollbackIfReady()
{
    if (!IsRollbackChangeDue())
        return;

    const int32_t rollbackFrames = g_pendingRollbackFrames;
    g_pendingRollbackFrames = -1;
    g_pendingRollbackEffectiveFrame = -1;
    Controller_SetRollbackFrames(rollbackFrames);
    HookLog("Netplay: applied rollback %d after frame %d\n",
            rollbackFrames, g_netFrameCount);
}

// ===========================================================================
// Hooked functions
// ===========================================================================

extern "C" bool Controller_RemapSyntheticInput(uint32_t rawInput,
                                                 uint32_t* outInput)
{
    if (!obuttonRemap || !outInput)
        return false;

    // Index 1 remaps the supplied mask without P1's focus/menu shortcut.
    *outInput = obuttonRemap(1, rawInput, 0);
    return true;
}

struct FrameInputRemapContext {
    uint32_t ownerTeam;
    uint32_t timelineTeam;
    int32_t frame;
    uint32_t timelineInput;
    bool timelineResolved;
    bool injectedAtButtonRemap;
};

static thread_local FrameInputRemapContext* g_frameInputRemapContext = nullptr;

static uint32_t ResolveTimelineInput(FrameInputRemapContext* context,
                                     uint32_t inputTeam, uint32_t currentInput,
                                     bool captureCurrent)
{
    if (context->timelineResolved && context->timelineTeam == inputTeam)
        return context->timelineInput;

    context->timelineTeam = inputTeam;
    uint32_t idx = (uint32_t)context->frame % RING_SIZE;
    bool isLocal = Netplay_IsHost() ? (inputTeam == 0) : (inputTeam == 1);
    uint32_t resolved;

    if (g_isResimulating) {
        uint32_t snapLocal = 0;
        uint32_t snapRemote = 0;
        if (Rollback_GetSnapshotInputs(context->frame, &snapLocal, &snapRemote))
            resolved = isLocal ? snapLocal : snapRemote;
        else
            resolved = isLocal ? g_recordedLocalInput[idx] : g_usedRemoteInput[idx];
    } else if (!isLocal) {
        resolved = g_usedRemoteInput[idx];
    } else if (context->frame <= g_delayFrames + g_rollbackFrames) {
        resolved = 0;
    } else {
        if (captureCurrent)
            g_localPending |= currentInput;
        resolved = g_scheduledLocalFrame[idx] == context->frame
            ? g_scheduledLocalInput[idx]
            : (g_delayFrames == 0 ? currentInput : 0);
    }

    context->timelineInput = resolved;
    context->timelineResolved = true;
    return resolved;
}

static uint32_t __fastcall hkbuttonRemap(uint64_t playerIndex, uint32_t rawInput,
                                         uint8_t alternateMap)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ButtonRemap);
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (Netplay_IsActive() && g_openingMembersValid && playerIndex < 2 &&
        caller == (uintptr_t)_addr(ADDR_OPENING_REMAP_RETURN)) {
        uint32_t member = g_openingMembers[playerIndex];
        return member == 2 ? IN_RB : (member == 1 ? IN_LB : 0);
    }

    FrameInputRemapContext* context = g_frameInputRemapContext;
    if (!context || !Netplay_IsActive())
        return obuttonRemap(playerIndex, rawInput, alternateMap);

    uint32_t inputTeam = playerIndex < 2 ? (uint32_t)playerIndex
                                         : context->ownerTeam;
    uint32_t currentInput;
    uint32_t rawTestInput = 0;
    if (!g_isResimulating &&
        AutoTest_GetInput(context->frame, inputTeam, &rawTestInput)) {
        // Remap test input without P1's focus/menu shortcut.
        currentInput = obuttonRemap(1, rawTestInput, alternateMap);
    } else {
        currentInput = obuttonRemap(playerIndex, rawInput, alternateMap);

        // Merge P1's native keyboard input into the client's P2 controller input.
        if (!Netplay_IsHost() && context->ownerTeam == 1 &&
            playerIndex == 1 && IsGameProcessForeground()) {
            currentInput |= obuttonRemap(0, 0, alternateMap);
        }
    }

    uint32_t resolved = ResolveTimelineInput(
        context, inputTeam, currentInput, true);
    context->injectedAtButtonRemap = true;
    return resolved;
}

static uint32_t __fastcall hkswapLRIfFaceLeft(uint64_t cChrInput,
                                              uint32_t input)
{
    FrameInputRemapContext* context = g_frameInputRemapContext;
    if (!context || !Netplay_IsActive())
        return oswapLRIfFaceLeft(cChrInput, input);

    // Do not replace native transformations made after timeline injection.
    if (context->injectedAtButtonRemap)
        return oswapLRIfFaceLeft(cChrInput, input);

    // Inject bypass paths before facing conversion and update mHeld explicitly.
    // Only the CPU-generation callsite may capture new local input here.
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    bool captureCpuInput = caller == (uintptr_t)_addr(ADDR_CPU_INPUT_SWAP_RETURN);
    uint32_t resolved = ResolveTimelineInput(
        context, context->timelineTeam, input, captureCpuInput);
    *(uint32_t*)(cChrInput + 0x14) = resolved;
    return oswapLRIfFaceLeft(cChrInput, resolved);
}

// ---------------------------------------------------------------------------
// Route physical input locally; replay raw masks only in offline resimulation.
// ---------------------------------------------------------------------------
static uint64_t __fastcall hkGetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::GetCurrentInput);

    bool is_active = Netplay_IsActive();

    // Suppress the remote slot during live netplay.
    uint64_t raw;
    if (is_active && !g_isResimulating)
    {
        if (Netplay_IsHost())
        {
            if (playerIndex == 1)
            {
                raw = 0;
            }
            else
            {
                raw = oGetsCurrentInput_withSOCD(pad, playerIndex);
            }
        }
        else
        {
            if (playerIndex == 0)
            {
                raw = 0;
            }
            else if (playerIndex == 1)
            {
                // Client keyboard input is merged later in hkbuttonRemap.
                raw = ApplySOCD(*(uint32_t*)(pad + PAD_SLOT_INPUT));
            }
            else
            {
                raw = oGetsCurrentInput_withSOCD(pad, playerIndex);
            }
        }
    }
    else
    {
        if (playerIndex == 1)
            raw = oGetsCurrentInput_withSOCD(pad, 1);
        else
            raw = oGetsCurrentInput_withSOCD(pad, playerIndex);
    }

    // Capture raw input for offline resimulation and input diagnostics.
    if (!g_isResimulating)
    {
        int32_t frame = GetGameFrameCount();
        if (frame >= 0 && playerIndex < 2)
        {
            g_rawLocalInput[playerIndex][(uint32_t)frame % RING_SIZE] = (uint32_t)raw;
        }
    }

    // Offline replay uses raw history; netplay injects logical input after remapping.
    if (g_isResimulating)
    {
        uint32_t idx = (uint32_t)g_resimulateCurrentFrame % RING_SIZE;
        if (is_active)
        {
            // Netplay input is injected later, not interpreted as a physical mask.
            return 0;
        }
        else
        {
            // Raw history is recorded even when netplay is inactive.
            if (playerIndex < 2)
                return g_rawLocalInput[playerIndex][idx];
            else
                return raw;
        }
    }

    return raw;
}

extern "C" uint32_t Controller_CaptureOpeningMember()
{
    if (!Netplay_IsActive() || !obuttonRemap || !oGetsCurrentInput_withSOCD)
        return 0;

    const uint32_t team = Netplay_IsHost() ? 0u : 1u;
    using GetInstanceFn = uint64_t(__fastcall*)();
    using GetTeamFn = uint64_t(__fastcall*)(uint64_t, uint32_t);
    using GetTeamFlagsFn = uint32_t(__fastcall*)(uint64_t);
    using GetPlayerPadFn = uint32_t(__fastcall*)(uint64_t, uint32_t);

    uint64_t sCharacter = ((GetInstanceFn)_addr(0x140001AA0))();
    uint64_t manager = ((GetInstanceFn)_addr(ADDR_MVC3_MGR_GET_INSTANCE))();
    uint64_t pad = ((GetInstanceFn)_addr(ADDR_PAD_GET_INSTANCE))();
    if (!sCharacter || !manager || !pad)
        return 0;

    uint64_t chrTeam = ((GetTeamFn)_addr(0x140001B60))(sCharacter, team);
    if (!chrTeam)
        return 0;

    uint32_t teamFlags = ((GetTeamFlagsFn)_addr(0x1406D1C80))(chrTeam);
    uint8_t alternateMap = (uint8_t)((teamFlags >> 12) & 1);
    uint32_t playerIndex =
        ((GetPlayerPadFn)_addr(ADGR_MGR_GET_PLAYER_CHAR))(manager, team);
    uint32_t rawInput = (uint32_t)hkGetsCurrentInput_withSOCD(pad, playerIndex);
    uint32_t logicalInput = obuttonRemap(team, rawInput, alternateMap);

    if (!Netplay_IsHost() && team == 1 && IsGameProcessForeground())
        logicalInput |= obuttonRemap(0, 0, alternateMap);

    if (logicalInput & IN_RB)
        return 2;
    if (logicalInput & IN_LB)
        return 1;
    return 0;
}

extern "C" void Controller_SetOpeningMembers(uint32_t team0Member,
                                               uint32_t team1Member)
{
    g_openingMembers[0] = team0Member <= 2 ? team0Member : 0;
    g_openingMembers[1] = team1Member <= 2 ? team1Member : 0;
    g_openingMembersValid = true;
}

extern "C" void Controller_ClearOpeningMembers()
{
    g_openingMembers[0] = 0;
    g_openingMembers[1] = 0;
    g_openingMembersValid = false;
}

// ---------------------------------------------------------------------------
// Allow native intro setup, but hold the countdown until both peers are ready.
// ---------------------------------------------------------------------------
static void(__fastcall* oStepIntro)(uint64_t param_1) = nullptr;
static void(__fastcall* oStepFighting)(uint64_t param_1) = nullptr;
static void(__fastcall* oStepResult8)(uint64_t param_1) = nullptr;
static void(__fastcall* oStepResult9_10_11)(uint64_t param_1) = nullptr;
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" uint32_t FrameStepper_GetBattleGeneration();
extern "C" bool FrameStepper_IsMatchRestartPending();
extern "C" bool FrameStepper_IsDesyncCheckReady();
extern "C" void FrameStepper_NotifyFighting();

static void __fastcall hkStepIntro(uint64_t param_1)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::StepIntro);
    if (!Netplay_IsActive()) {
        oStepIntro(param_1);
        return;
    }

    auto setNextStep = (void(__fastcall*)(uint64_t, int))_addr(0x14000AE00);
    if (FrameStepper_IsBattleSynchronized()) {
        setNextStep(param_1, 3);
        return;
    }

    oStepIntro(param_1);

    // Cancel any native transition queued before the battle barrier releases.
    if (*(int32_t*)(param_1 + 0xDC) >= 0)
        setNextStep(param_1, -1);
}

// Use shared-frame time for native checks, then undo the offline delta increment.
static void ApplySynchronizedFightTime(uint64_t sAction)
{
    if (!sAction || !Netplay_IsActive() || !FrameStepper_IsBattleSynchronized())
        return;

    int32_t frame = g_isResimulating ? g_resimulateCurrentFrame : GetGameFrameCount();
    if (frame < 0)
        return;

    if (g_fightFrameBase < 0)
    {
        g_fightFrameBase = frame;
        //HookLog("Netplay: captured sAction fight-time baseline at frame %d\n", frame);
    }

    int32_t elapsedFrames = frame - g_fightFrameBase;
    if (elapsedFrames < 0)
        elapsedFrames = 0;

    // Match native online timing: integer frame division yields whole seconds.
    float elapsedSeconds = (float)(elapsedFrames / 60);
    float remainingSeconds = 180.0f - elapsedSeconds;
    if (remainingSeconds < 0.0f)
        remainingSeconds = 0.0f;

    *(float*)(sAction + 0xF8)  = remainingSeconds; // mRTimeLimit
    *(float*)(sAction + 0xFC)  = elapsedSeconds;   // mRTimePass
    *(float*)(sAction + 0x100) = elapsedSeconds;   // mRTimePass2
    *(float*)(sAction + 0x104) = elapsedSeconds;   // mTimePass
}

static void __fastcall hkStepFighting(uint64_t sAction)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::StepFighting);
    FrameStepper_NotifyFighting();
    ApplySynchronizedFightTime(sAction);
    oStepFighting(sAction);
    ApplySynchronizedFightTime(sAction);
}

static bool IsEitherPlayerResultContinuePressed()
{
    auto getManager = (uint64_t(__fastcall*)())_addr(ADDR_MVC3_MGR_GET_INSTANCE);
    auto getPlayerPad = (int32_t(__fastcall*)(uint64_t, uint32_t))
        _addr(ADGR_MGR_GET_PLAYER_CHAR);
    auto getPad = (uint64_t(__fastcall*)())_addr(ADDR_PAD_GET_INSTANCE);
    auto getRawInput = (uint32_t(__fastcall*)(uint64_t, uint32_t))
        _addr(ADDR_PAD_RAW_INPUT);
    auto getUnitFlags = (uint32_t(__fastcall*)(uint64_t))
        _addr(ADDR_PAD_UNIT_FLAGS);

    const uint64_t manager = getManager();
    const uint64_t pad = getPad();
    if (!manager || !pad)
        return false;

    const uint32_t continueMask = getUnitFlags(pad);
    for (uint32_t team = 0; team < 2; ++team) {
        const int32_t padIndex = getPlayerPad(manager, team);
        if (padIndex >= 0 && (getRawInput(pad, (uint32_t)padIndex) & continueMask))
            return true;
    }
    return false;
}

static void __fastcall hkStepResult8(uint64_t sAction)
{
    if (Netplay_IsActive()) {
        if (!FrameStepper_IsMatchRestartPending() &&
            IsEitherPlayerResultContinuePressed()) {
            FrameStepper_RequestMatchRestart();
        }
        if (FrameStepper_IsMatchRestartPending())
            return;
    }

    oStepResult8(sAction);
}

static void __fastcall hkStepResult9_10_11(uint64_t sAction)
{
    if (Netplay_IsActive()) {
        if (!FrameStepper_IsMatchRestartPending() &&
            IsEitherPlayerResultContinuePressed()) {
            FrameStepper_RequestMatchRestart();
        }
        if (FrameStepper_IsMatchRestartPending())
            return;
    }

    oStepResult9_10_11(sAction);
}

// ---------------------------------------------------------------------------
// Frame boundaries: receive/resolve input before the tick; schedule/send captures after it.
// ---------------------------------------------------------------------------
static void DrainAndProcessPackets()
{
    InputPacket inPkt;
    while (Netplay_GetInboundPacket(inPkt))
    {
        if (inPkt.battle_generation != FrameStepper_GetBattleGeneration())
            continue;

        QueueInboundChecksum(inPkt);

        uint16_t acceptedMask = 0;
        uint16_t changedMask = 0;
        uint16_t priorConfirmedMask = 0;
        // Packet slots map to consecutive absolute execution frames.
        size_t framesToProcess = NET_HISTORY_SIZE;
        for (size_t i = 0; i < framesToProcess; i++)
        {
            int32_t targetFrame = (int32_t)inPkt.start_frame + (int32_t)i;
            if (targetFrame >= 0)
            {
                bool wasConfirmed = IsFrameConfirmed(targetFrame);
                if (wasConfirmed) priorConfirmedMask |= (uint16_t)(1u << i);
                // Accept revisions to future input, but keep confirmed past input fixed.
                if (!IsFrameConfirmed(targetFrame) || targetFrame > g_netFrameCount)
                {
                    uint32_t idx = (uint32_t)targetFrame % RING_SIZE;
                    uint32_t previous = g_receivedInput[idx];
                    if (!wasConfirmed || previous != inPkt.inputs[i])
                        changedMask |= (uint16_t)(1u << i);
                    acceptedMask |= (uint16_t)(1u << i);
                    g_receivedInput[idx] = inPkt.inputs[i];
                    g_confirmedFrame[idx] = targetFrame;
                    Rollback_UpdateRemoteInput(targetFrame, inPkt.inputs[i]);
                }

                // Track the newest remote frame for prediction.
                if (targetFrame > g_lastRemoteFrame)
                {
                    g_lastRemoteFrame = targetFrame;
                    g_lastRemoteInput = inPkt.inputs[i];
                }
            }
        }

        g_remoteFrameAdvantage = inPkt.frame_advantage;
        LogReceivedPacket(inPkt);
        TraceInputPacket("IPRX", "frame-start", g_netFrameCount, inPkt, 0,
                         acceptedMask, changedMask, priorConfirmedMask);
    }
}

static bool WaitForConfirmedFrame(int32_t frame, const char* reason)
{
    if (IsFrameConfirmed(frame) || !Netplay_IsActive())
        return true;

    Rollback_FreezeWatchdogHeartbeat(reason, g_netFrameCount);
    //HookLog("Netplay: waiting for remote input frame %d (%s, local=%d, lastRemote=%d)\n",
    //        frame, reason, g_netFrameCount, g_lastRemoteFrame);
    while (!IsFrameConfirmed(frame) && Netplay_IsActive() &&
           !Netplay_HasInboundRestartMatch())
    {
        SwitchToThread();
        DrainAndProcessPackets();
    }
    if (!IsFrameConfirmed(frame) && Netplay_HasInboundRestartMatch()) {
        Rollback_FreezeWatchdogHeartbeat("restart pending", g_netFrameCount);
        HookLog("Netplay: interrupted %s wait for pending match restart at frame %d\n",
                reason, g_netFrameCount);
        return false;
    }
    Rollback_FreezeWatchdogHeartbeat("confirmed-frame-wait-complete",
                                     g_netFrameCount);
    //HookLog("Netplay: remote input frame %d wait complete (active=%d, lastRemote=%d)\n",
    //        frame, Netplay_IsActive() ? 1 : 0, g_lastRemoteFrame);
    return IsFrameConfirmed(frame) || !Netplay_IsActive();
}

static bool WaitForPredictionBudgetFrame(int32_t frame)
{
    SetPredictionBudgetExhausted();
    Rollback_FreezeWatchdogHeartbeat("prediction budget exhausted",
                                     g_netFrameCount);
    while (!IsFrameConfirmed(frame) && Netplay_IsActive() &&
           !Netplay_HasInboundRestartMatch() &&
           !Controller_ShouldShowPredictionBudgetExhausted())
    {
        SwitchToThread();
        DrainAndProcessPackets();
    }

    if (IsFrameConfirmed(frame)) {
        ClearPredictionBudgetExhausted();
        Rollback_FreezeWatchdogHeartbeat("prediction-budget-wait-complete",
                                         g_netFrameCount);
        return true;
    }
    if (!Netplay_IsActive() || Netplay_HasInboundRestartMatch())
        ClearPredictionBudgetExhausted();
    return false;
}

// ---------------------------------------------------------------------------
extern "C" void Controller_OnFrameStart()
{
    if (!Netplay_IsActive()) {
        ClearPredictionBudgetExhausted();
        return;
    }

    g_netFrameCount = GetGameFrameCount();
    // If sReplay isn't ready yet, skip packet processing this frame.
    if (g_netFrameCount < 0) return;

    // Keep the native replay cursor on the shared frame epoch.
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (sReplay) {
        *(int32_t*)(sReplay + 0x54) = g_netFrameCount;
        g_gameFrameBase = 0;
    }

    ApplyScheduledDelayIfReady();

    g_localPending = 0;

    // Use the same packet acceptance rules here and inside waits.
    DrainAndProcessPackets();

    // Confirm completed frames before changing the rollback window.
    // Do not wait for current-frame input: zero-delay peers have not sent it yet.
    if (IsRollbackChangeDue())
    {
        const int32_t lastCompletedFrame = g_netFrameCount - 1;
        if (lastCompletedFrame >= 0) {
            if (!WaitForConfirmedFrame(lastCompletedFrame,
                                       "rollback setting transition"))
                return;
        }
    }

    if (g_rollbackFrames > 0)
    {
        // Check rollback only after the intro step begins.
        typedef uint64_t(__fastcall* GetAction_t)();
        static GetAction_t getAction = (GetAction_t)_addr(0x140001A90);
        uint64_t action = getAction();
        int step = action ? *(int*)(action + 0xD8) : 0;
        if (step >= 2)
        {
            // Input delay does not extend the prediction window.
            int32_t predictionBudget = g_rollbackFrames;
            if (g_netFrameCount > predictionBudget)
            {
                int32_t oldestRequiredFrame = g_netFrameCount - predictionBudget;
                if (!IsFrameConfirmed(oldestRequiredFrame)) {
                    if (!WaitForPredictionBudgetFrame(oldestRequiredFrame))
                        return;
                }
            }

        // Multi-resim scans the rolling window; non-multi checks new confirmations only.
        bool shouldCheckRollback = g_netFrameCount > 0 &&
            (g_multiResimEnabled || g_lastRemoteFrame > g_lastNonMultiCheckedFrame);
        if (shouldCheckRollback)
        {
            // Include the oldest frame covered by the prediction-budget wait.
            int32_t checkStart;
            if (g_multiResimEnabled)
            {
                checkStart = g_netFrameCount - predictionBudget;
                if (checkStart < 0) checkStart = 0;
            }
            else
            {
                checkStart = g_lastNonMultiCheckedFrame + 1;
            }
            int32_t checkEnd = g_netFrameCount;
            if (!g_multiResimEnabled && checkEnd > g_lastRemoteFrame + 1)
                checkEnd = g_lastRemoteFrame + 1;
            int32_t earliestConflict = -1;
            for (int32_t f = checkStart; f < checkEnd; f++)
            {
                if (IsFrameConfirmed(f))
                {
                    uint32_t recv = g_receivedInput[(uint32_t)f % RING_SIZE];
                    uint32_t used = g_usedRemoteInput[(uint32_t)f % RING_SIZE];
                    uint32_t idx = (uint32_t)f % RING_SIZE;
                    if (g_predictedInput[idx] && g_predictedFrame[idx] == f)
                    {
                        //HookLog("Prediction check frame %d: used=0x%08X recv=0x%08X %s\n",
                        //        f, used, recv, used == recv ? "MATCH" : "CONFLICT");
                    }
                    if (recv != used)
                    {
                        earliestConflict = f;
                        //HookLog("RollbackNet: conflict at frame %d  used=0x%08X recv=0x%08X diff=0x%08X re-sim [%d, %d)\n",
                        //        f, used, recv, used ^ recv,
                        //        f, g_netFrameCount);
                        break;
                    }
                }
            }
            if (earliestConflict >= 0)
            {
                if (!g_multiResimEnabled)
                {
                    // Confirm the whole batch before replaying it once.
                    if (!WaitForConfirmedFrame(g_netFrameCount,
                                               "non-multi definitive batch"))
                        return;
                    checkEnd = g_netFrameCount;
                }

                // Placeholder for committing confirmed frames before the conflict.
                for (int32_t f = checkStart; f < earliestConflict; f++)
                {
                    if (IsFrameConfirmed(f))
                    {
                    }
                }

                // Patch snapshots with confirmed remote input where available.
                for (int32_t f = earliestConflict; f < g_netFrameCount; f++)
                {
                    uint32_t idx = (uint32_t)f % RING_SIZE;
                    uint32_t trueLocal = 0;
                    Rollback_GetSnapshotInputs(f, &trueLocal, nullptr);
                    uint32_t trueRemote = IsFrameConfirmed(f)
                        ? g_receivedInput[idx]
                        : g_usedRemoteInput[idx];
                    Rollback_UpdateSnapshotInputs(f, trueLocal, trueRemote);
                }

                Rollback_RequestReSimulate(earliestConflict, g_netFrameCount);
            }
            else
            {
                // Clear prediction flags for confirmed inputs that matched.
                int32_t correctPredictions = 0;
                for (int32_t f = checkStart; f < g_netFrameCount; f++)
                {
                    uint32_t idx = (uint32_t)f % RING_SIZE;
                    if (IsFrameConfirmed(f) &&
                        g_predictedInput[idx] && g_predictedFrame[idx] == f)
                    {
                        //HookLog("Prediction confirmed frame %d: input=0x%08X\n",
                        //        f, g_receivedInput[idx]);
                        correctPredictions++;
                        g_predictedInput[idx] = false;
                    }
                }
                if (correctPredictions > 0)
                {
                    //HookLog("  >>> All predictions correct for frames [%d, %d) (%d frames verified)\n",
                            //checkStart, g_netFrameCount, correctPredictions);
                }

                for (int32_t f = checkStart; f <= g_netFrameCount; f++)
                {
                    if (IsFrameConfirmed(f))
                    {
                    }
                }
            }

            if (!g_multiResimEnabled)
            {
                // Stop at the first unconfirmed frame so it is checked later.
                while (g_lastNonMultiCheckedFrame + 1 < checkEnd &&
                       IsFrameConfirmed(g_lastNonMultiCheckedFrame + 1))
                {
                    g_lastNonMultiCheckedFrame++;
                }
            }
            }
        }
        ClearPredictionBudgetExhausted();
    }
    else
    {
        ClearPredictionBudgetExhausted();
        // Delay-only mode must confirm the current frame before simulation.
        int32_t neededFrame = g_netFrameCount;
        if (!WaitForConfirmedFrame(neededFrame, "delay mode"))
            return;
    }

    // Resolve remote input once for all characters in this tick.
    Controller_ResolveRemoteInput(g_netFrameCount);
    Controller_MarkFrameInputProcessed(g_netFrameCount);

    // Attach resolved input to the snapshot saved before this hook.
    if (g_rollbackFrames > 0)
    {
        uint32_t idx = (uint32_t)g_netFrameCount % RING_SIZE;
        uint32_t localInput = g_scheduledLocalFrame[idx] == g_netFrameCount
            ? g_scheduledLocalInput[idx] : 0;
        Rollback_UpdateSnapshotInputs(
            g_netFrameCount, localInput, g_usedRemoteInput[idx]);
    }

    // Estimate remote frame from received input and RTT; negative advantage means ahead.
    if (g_netFrameCount >= 0 && g_lastRemoteFrame >= 0)
    {
        int32_t rttMs = (int32_t)Netplay_GetAverageRTT();
        int32_t remoteFrameEst = g_lastRemoteFrame + (static_cast<float>(rttMs) * 60 / 1000 + 0.5);
        g_localFrameAdvantage = remoteFrameEst - g_netFrameCount;

        // Advance timesync window
        uint32_t tidx = (uint32_t)g_timesyncFrameCount % TIMESYNC_WINDOW_SIZE;
        g_localAdvHistory[tidx]  = g_localFrameAdvantage;
        g_remoteAdvHistory[tidx] = g_remoteFrameAdvantage;
        g_timesyncFrameCount++;

        // Compute a wait recommendation from both peers' averaged estimates.
        int count = g_timesyncFrameCount < TIMESYNC_WINDOW_SIZE
                    ? g_timesyncFrameCount : TIMESYNC_WINDOW_SIZE;
        if (count > 0)
        {
            float avgLocal = 0, avgRemote = 0;
            for (int i = 0; i < count; i++)
            {
                avgLocal  += (float)g_localAdvHistory[i];
                avgRemote += (float)g_remoteAdvHistory[i];
            }
            avgLocal  /= count;
            avgRemote /= count;

            // Sleeping is disabled; retain the recommendation calculation.
            if (avgLocal < avgRemote)
            {
                int sleepFrames = (int)(((avgRemote - avgLocal) / 2.0f) + 0.5f);
                if (sleepFrames >= TIMESYNC_MIN_FRAME_ADVANTAGE)
                {
                    if (sleepFrames > TIMESYNC_MAX_FRAME_ADVANTAGE)
                        sleepFrames = TIMESYNC_MAX_FRAME_ADVANTAGE;
                    //Sleep(sleepFrames);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sum integer-truncated positions/HP, action IDs, and per-character RNG hashes.
// Character order does not affect the sum; mix in global gameplay RNG last.
// ---------------------------------------------------------------------------
static uint32_t ComputeChecksum()
{
    uint64_t sCharacter = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!sCharacter) return 0;

    uint32_t cs = 0;
    for (int ti = 0; ti < 2; ++ti)
    {
        uint64_t team = sCharacter + (ti == 0 ? TEAM0_OFFSET : TEAM1_OFFSET);
        uint64_t node = team + TEAM_FIRST_NODE_OFFSET;
        while (node)
        {
            uint64_t character = *(uint64_t*)(node + NODE_CHR_OFFSET);
            if (character)
            {
                int32_t qx = (int32_t)*(float*)(character + 0x50);
                int32_t qy = (int32_t)*(float*)(character + 0x54);
                int32_t hp = (int32_t)*(float*)(character + 0x1550);
                cs += (uint32_t)(qx < 0 ? -qx : qx);
                cs += (uint32_t)(qy < 0 ? -qy : qy);
                cs += (uint32_t)(hp < 0 ? -hp : hp);
                cs += *(uint32_t*)(character + 0x1310); // actionId

                // Hash each character's RNG separately to keep the sum order-independent.
                const uint32_t* characterRng =
                    (const uint32_t*)(character + 0x2D80);
                uint32_t rngHash = 2166136261u;
                for (int i = 0; i < 4; ++i)
                    rngHash = (rngHash ^ characterRng[i]) * 16777619u;
                cs += rngHash;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }

    const uint32_t* globalRng = (const uint32_t*)_addr(PTR_GLOBAL_RNG);
    for (int i = 0; i < 4; ++i)
        cs = (cs ^ globalRng[i]) * 16777619u;

    return cs;
}

extern "C" void Controller_RecordFrameChecksum(int32_t frame)
{
    if (!FrameStepper_IsDesyncCheckReady() ||
        FrameStepper_IsMatchRestartPending() ||
        frame < DESYNC_SKIP_FRAMES)
        return;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    g_localChecksum[idx] = ComputeChecksum();
    g_localChecksumFrame[idx] = frame;
    g_localChecksumRemoteInput[idx] = g_processedRemoteInput[idx];
    g_localChecksumAdvertisedFrame[idx] = -1;
    g_localChecksumConfirmed[idx] =
        g_processedRemoteFrame[idx] == frame && IsFrameConfirmed(frame) &&
        g_localChecksumRemoteInput[idx] == g_receivedInput[idx];

    // Re-recording a frame invalidates its previous checksum comparison.
    if (g_desyncChecked[idx] == frame)
    {
        g_desyncChecked[idx] = -1;
        g_desyncMismatch[idx] = false;
    }
}

extern "C" bool Controller_GetFrameChecksum(int32_t frame, uint32_t* outChecksum)
{
    if (frame < DESYNC_SKIP_FRAMES || !outChecksum)
        return false;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    if (g_localChecksumFrame[idx] != frame)
        return false;
    *outChecksum = g_localChecksum[idx];
    return true;
}

static void UpdateConfirmedChecksums()
{
    if (!FrameStepper_IsDesyncCheckReady() || FrameStepper_IsMatchRestartPending())
        return;

    // Confirm records independently so missing checksums do not block later frames.
    int32_t first = g_netFrameCount - RING_SIZE + 1;
    if (first < DESYNC_SKIP_FRAMES) first = DESYNC_SKIP_FRAMES;
    for (int32_t frame = first; frame <= g_netFrameCount; ++frame)
    {
        uint32_t idx = (uint32_t)frame % RING_SIZE;
        if (g_localChecksumFrame[idx] != frame || !IsFrameConfirmed(frame))
            continue;

        // Confirm against the input that produced this checksum, not a later selection.
        if (!g_localChecksumConfirmed[idx] &&
            g_processedRemoteFrame[idx] == frame &&
            g_localChecksumRemoteInput[idx] == g_receivedInput[idx])
            g_localChecksumConfirmed[idx] = true;

        if (!g_localChecksumConfirmed[idx] ||
            g_remoteChecksumFrame[idx] != frame ||
            g_desyncChecked[idx] == frame)
            continue;

        uint32_t local = g_localChecksum[idx];
        uint32_t remote = g_remoteChecksum[idx];
        uint32_t diff = local > remote ? local - remote : remote - local;
        g_desyncChecked[idx] = frame;
        g_desyncMismatch[idx] = diff > DESYNC_THRESHOLD;
        if (diff > DESYNC_THRESHOLD && !g_desyncSeen && !g_desyncDetected)
            HookLog("Note frame %d: local=0x%08X remote=0x%08X\n",
                    frame, local, remote);
    }
}

extern "C" void Controller_OnFrameEnd()
{
    if (!Netplay_IsActive())
        return;

    // Send history even during neutral startup so delay-only peers can progress.

    // Record every live frame, even when no character-input hook ran.
    {
        uint32_t idx = (uint32_t)g_netFrameCount % RING_SIZE;
        g_recordedLocalInput[idx] =
            g_scheduledLocalFrame[idx] == g_netFrameCount
                ? g_scheduledLocalInput[idx]
                : (g_delayFrames == 0 ? g_localPending : 0);

        // Zero-delay input is captured after snapshot save; attach it now for replay.
        if (g_delayFrames == 0 && g_rollbackFrames > 0)
            Rollback_UpdateSnapshotInputs(g_netFrameCount,
                                          g_recordedLocalInput[idx],
                                          g_usedRemoteInput[idx]);
    }

    // Schedule captured input without moving or replacing existing queued values.
    int32_t scheduledFrame = g_netFrameCount + g_delayFrames;
    uint32_t scheduledIdx = (uint32_t)scheduledFrame % RING_SIZE;
    if (g_scheduledLocalFrame[scheduledIdx] != scheduledFrame)
    {
        g_scheduledLocalInput[scheduledIdx] = g_localPending;
        g_scheduledLocalFrame[scheduledIdx] = scheduledFrame;
    }
    // After a delay decrease, already-advertised input takes precedence over new captures.

    // Keep the legacy history populated for rollback save/restore diagnostics.
    memmove(&g_oldInputs[0], &g_oldInputs[1],
            (MAX_HISTORY - 1) * sizeof(uint32_t));
    g_oldInputs[MAX_HISTORY - 1] = g_localPending;

    // Send consecutive input history ending at the newest queued frame.
    // Each slot maps to start_frame + index; optionally include a confirmed checksum.
    InputPacket outPkt = {};
    outPkt.battle_generation = FrameStepper_GetBattleGeneration();
    outPkt.checksum_frame = 0xFFFFFFFFu;
    int32_t packetEndFrame = g_netFrameCount + g_delayFrames;
    for (int32_t frame = g_netFrameCount; frame < g_netFrameCount + MAX_HISTORY; ++frame)
    {
        uint32_t idx = (uint32_t)frame % RING_SIZE;
        if (g_scheduledLocalFrame[idx] == frame && frame > packetEndFrame)
            packetEndFrame = frame;
    }
    outPkt.start_frame = (uint32_t)(packetEndFrame - MAX_HISTORY + 1);
    uint16_t validMask = 0;
    for (size_t i = 0; i < MAX_HISTORY; i++)
    {
        int32_t frame = (int32_t)outPkt.start_frame + (int32_t)i;
        uint32_t idx = (uint32_t)frame % RING_SIZE;
        if (g_scheduledLocalFrame[idx] == frame) {
            validMask |= (uint16_t)(1u << i);
            outPkt.inputs[i] = g_scheduledLocalInput[idx];
        } else {
            outPkt.inputs[i] = 0;
        }
    }

    if (!g_isResimulating && FrameStepper_IsDesyncCheckReady() &&
        !FrameStepper_IsMatchRestartPending() &&
        g_netFrameCount >= DESYNC_SKIP_FRAMES)
    {
        Controller_RecordFrameChecksum(g_netFrameCount);
        uint32_t liveChecksum = 0;
        if (Controller_GetFrameChecksum(g_netFrameCount, &liveChecksum))
            AutoTest_OnLiveFrame(g_netFrameCount, liveChecksum);
        UpdateConfirmedChecksums();

        // Send the oldest unsent confirmed checksum; corrected frames can be sent again.
        int32_t first = g_netFrameCount - RING_SIZE + 1;
        if (first < DESYNC_SKIP_FRAMES) first = DESYNC_SKIP_FRAMES;
        for (int32_t checksumFrame = first;
             checksumFrame <= g_netFrameCount; ++checksumFrame)
        {
            uint32_t idx = (uint32_t)checksumFrame % RING_SIZE;
            if (g_localChecksumFrame[idx] == checksumFrame &&
                g_localChecksumConfirmed[idx] &&
                g_localChecksumAdvertisedFrame[idx] != checksumFrame)
            {
                outPkt.checksum_frame = (uint32_t)checksumFrame;
                outPkt.checksum = g_localChecksum[idx];
                g_localChecksumAdvertisedFrame[idx] = checksumFrame;
                break;
            }
        }
    }

    // Include our frame-advantage estimate for the peer.
    outPkt.frame_advantage = g_localFrameAdvantage;
    outPkt.flags           = 1;  // non-zero = real packet (seed packet has flags=0)

    if (INPUT_PROVENANCE_TRACE) {
        uint32_t executedIdx = (uint32_t)g_netFrameCount % RING_SIZE;
        HookLog("IPSCHEDULE frame=%d delay=%d captured=%08X scheduledFrame=%d "
                "scheduledValue=%08X executed=%08X\n",
                g_netFrameCount, g_delayFrames, g_localPending, scheduledFrame,
                g_scheduledLocalInput[scheduledIdx],
                g_recordedLocalInput[executedIdx]);
        TraceInputPacket("IPTX", "frame-end", g_netFrameCount, outPkt,
                         validMask, 0, 0, 0);
    }

    Netplay_SendPacket(outPkt);

    ApplyScheduledRollbackIfReady();

    // Count consecutive mismatches backward from the newest compared frame.
    if (!g_isResimulating && FrameStepper_IsDesyncCheckReady() &&
        !FrameStepper_IsMatchRestartPending() &&
        g_netFrameCount >= DESYNC_SKIP_FRAMES)
    {
        int32_t newestChecked = -1;
        for (int i = 0; i < RING_SIZE; i++)
        {
            if (g_desyncChecked[i] > newestChecked)
                newestChecked = g_desyncChecked[i];
        }

        if (newestChecked >= 0)
        {
            int consecutive = 0;
            for (int32_t f = newestChecked;
                 f >= 0 && consecutive < DESYNC_CONSECUTIVE_MIN; --f)
            {
                int32_t idx = f % RING_SIZE;
                if (g_desyncChecked[idx] != f || !g_desyncMismatch[idx])
                    break;
                consecutive++;
            }
            // Autotest uses move-scoped checks; suppress session-wide verdicts during tests.
            if (consecutive >= DESYNC_CONSECUTIVE_MIN && !AutoTest_IsActive())
            {
                if (!g_desyncSeen && !g_desyncDetected)
                {
                    g_desyncDetected = true;
                    g_desyncFrame    = newestChecked;
                    g_desyncLocalCS  = g_localChecksum[newestChecked % RING_SIZE];
                    g_desyncRemoteCS = g_remoteChecksum[newestChecked % RING_SIZE];
                    Netplay_SendDesyncTrigger(g_desyncFrame, g_desyncLocalCS,
                                               g_desyncRemoteCS);
                    Rollback_NotifyDesyncTriggered(newestChecked);
                    HookLog("DESYNC TRIGGER at frame %d: %d consecutive mismatches\n",
                            newestChecked, consecutive);
                }
            }
        }
    }

    // Log this frame's local input and what was sent.
    {
        uint32_t localFrame = (uint32_t)g_netFrameCount % RING_SIZE;
        //HookLog("Frame %d: local=0x%08X\n", g_netFrameCount, g_recordedLocalInput[localFrame]);
        //for (size_t i = 0; i < MAX_HISTORY; i++)
        //    HookLog(" 0x%04X", outPkt.inputs[i] & 0xFFFF);
        //HookLog("]\n");
    }
}

// ---------------------------------------------------------------------------
// Scope timeline injection to this character, leaving native input processing intact.
// ---------------------------------------------------------------------------
static void __fastcall hkprocessFrameInput(uint64_t cChrInput)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ProcessFrameInput);
    uint64_t character = *(uint64_t*)(cChrInput + 0x8);
    typedef uint32_t(__fastcall* getTeamIdx_t)(uint64_t);
    static getTeamIdx_t fnGetTeamIdx = (getTeamIdx_t)_addr(ADDR_GET_TEAM_INDEX);
    uint32_t teamIdx = character ? fnGetTeamIdx(character) : 0;
    int32_t lookupFrame = g_isResimulating ? g_resimulateCurrentFrame
                                            : GetGameFrameCount();

    FrameInputRemapContext context = {
        teamIdx, teamIdx, lookupFrame, 0, false, false
    };
    FrameInputRemapContext* previousContext = g_frameInputRemapContext;
    if (Netplay_IsActive() && character && lookupFrame >= 0)
        g_frameInputRemapContext = &context;
    oprocessFrameInput(cChrInput);
    g_frameInputRemapContext = previousContext;

    if (!character || lookupFrame < 0)
        return;

    // Respect native gameplay eligibility before offline injection or diagnostics.
    typedef bool(__fastcall* isGameplayActive_t)(uint64_t);
    static isGameplayActive_t fnIsGameplayActive =
        (isGameplayActive_t)_addr(ADDR_IS_GAMEPLAY_ACTIVE);
    if (!fnIsGameplayActive(cChrInput))
        return;

    uint32_t idx = (uint32_t)lookupFrame % RING_SIZE;

    if (Netplay_IsActive())
    {
        // Record the final native result, after timeline injection and character processing.
        g_recordedGameInput[teamIdx & 1][idx] =
            *(uint32_t*)(cChrInput + 0x14);
    }
    else // Offline / Singleplayer / F7 test
    {
        if (!g_isResimulating)
        {
            uint32_t rawTestInput = 0;
            if (AutoTest_GetInput(lookupFrame, teamIdx, &rawTestInput))
            {
                uint32_t resolved = 0;
                if (!Controller_RemapSyntheticInput(rawTestInput, &resolved))
                    resolved = 0;
                *(uint32_t*)(cChrInput + 0x14)  = resolved;
                *(uint32_t*)(cChrInput + 0x560) = resolved;
                *(uint32_t*)(cChrInput + 0x56C) = resolved;
                *(uint32_t*)(cChrInput + 0x568) = 0;
            }
        }

        uint32_t inputVal;
        if (g_isResimulating)
        {
            uint32_t snapLocal = 0;
            uint32_t snapRemote = 0;
            if (Rollback_GetSnapshotInputs(lookupFrame, &snapLocal, &snapRemote))
            {
                inputVal = (teamIdx == 0) ? snapLocal : snapRemote;
                //HookLog("[ResimInput] the local and remote input for this frame was local=0x%08X remote=0x%08X from the snapshot frame %d\n",
                //        snapLocal, snapRemote, lookupFrame);
            }
            else
            {
                // Fall back to recorded character input if no snapshot is available.
                inputVal = g_recordedGameInput[teamIdx & 1][idx];
            }
            *(uint32_t*)(cChrInput + 0x14)  = inputVal; // mHeld
            *(uint32_t*)(cChrInput + 0x560) = inputVal; // mRemapped
            *(uint32_t*)(cChrInput + 0x56C) = inputVal; // mProcessed
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask
        }
        else
        {
            // Record native character input for offline replay.
            inputVal = *(uint32_t*)(cChrInput + 0x14);
            g_recordedGameInput[teamIdx & 1][idx] = inputVal;
        }
    }
}


// ---------------------------------------------------------------------------
// Device hooks
// ---------------------------------------------------------------------------
static void __fastcall hkpadPollAllSlots(uint64_t sPad)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::PadPollAll);
    opadPollAllSlots(sPad);
}

static uint64_t __fastcall hkpadSlotReadDevice(uint64_t slotBase)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::PadSlotRead);
    return opadSlotReadDevice(slotBase);
}

static void __fastcall hkxinputToGameMask(uint64_t sPad, uint64_t slotBase, const XInputState* pState)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::XInputToMask);
    // Dual-training instances share devices; only the foreground instance receives XInput.
    if (GameplayHooks_IsActive() && GetLaunchOption() == LAUNCH_DUAL_TRAINING && !IsGameProcessForeground()) {
        static const XInputState kEmptyState = {};
        oxinputToGameMask(sPad, slotBase, &kEmptyState);
        return;
    }

    oxinputToGameMask(sPad, slotBase, pState);
}

static void __fastcall hkpadSlotPostReadProcess(uint64_t sPad, uint64_t slotBase, uint32_t slotIdx)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::PadPostRead);
    opadSlotPostReadProcess(sPad, slotBase, slotIdx);
}

static void __fastcall hkinputAutoRepeatTimerProcess(uint64_t slotBase)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::InputAutoRepeat);
    oinputAutoRepeatTimerProcess(slotBase);
}

// ===========================================================================
// Hook installation
// ===========================================================================
void Controller_InitHooks()
{
    MH_STATUS s;

    // Route physical input to the local player.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_GET_CURRENT_INPUT),
        (LPVOID)&hkGetsCurrentInput_withSOCD,
        (LPVOID*)&oGetsCurrentInput_withSOCD
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_GET_CURRENT_INPUT));
    else
        HookLog("Controller: Failed to hook GetsCurrentInput_withSOCD (%d)\n", s);

    // Hold the intro at the netplay battle barrier.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_STEP_INTRO),
        (LPVOID)&hkStepIntro,
        (LPVOID*)&oStepIntro
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_STEP_INTRO));
    else
        HookLog("Controller: Failed to hook stepIntro (%d)\n", s);

    // Synchronize fight clocks.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_STEP_FIGHTING),
        (LPVOID)&hkStepFighting,
        (LPVOID*)&oStepFighting
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_STEP_FIGHTING));
    else
        HookLog("Controller: Failed to hook stepFighting (%d)\n", s);

    // Either player's result confirmation requests a synchronized rematch.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_STEP_RESULT8),
        (LPVOID)&hkStepResult8,
        (LPVOID*)&oStepResult8
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_STEP_RESULT8));
    else
        HookLog("Controller: Failed to hook stepResult8 (%d)\n", s);

    // Apply the same rematch handling to the result menu.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_STEP_RESULT9_10_11),
        (LPVOID)&hkStepResult9_10_11,
        (LPVOID*)&oStepResult9_10_11
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_STEP_RESULT9_10_11));
    else
        HookLog("Controller: Failed to hook stepResult9_10_11 (%d)\n", s);

    // Cover input sources that bypass button remapping.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_SWAP_LR_IF_FACE_LEFT),
        (LPVOID)&hkswapLRIfFaceLeft,
        (LPVOID*)&oswapLRIfFaceLeft
    );
    if (s == MH_OK)
        s = GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_SWAP_LR_IF_FACE_LEFT));
    if (s != MH_OK)
        HookLog("Controller: Failed to hook swapLRIfFaceLeft (%d)\n", s);

    // Capture/inject logical input after button remapping.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_BUTTON_REMAP),
        (LPVOID)&hkbuttonRemap,
        (LPVOID*)&obuttonRemap
    );
    if (s == MH_OK)
        s = GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_BUTTON_REMAP));
    if (s != MH_OK)
        HookLog("Controller: Failed to hook buttonRemap (%d)\n", s);

    // Scope timeline injection to native character-input processing.
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_PROCESS_FRAME_INPUT),
        (LPVOID)&hkprocessFrameInput,
        (LPVOID*)&oprocessFrameInput
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_PROCESS_FRAME_INPUT));
    else
        HookLog("Controller: Failed to hook processFrameInput (%d)\n", s);

    // padSlotReadDevice
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_PAD_SLOT_READ_DEVICE),
        (LPVOID)&hkpadSlotReadDevice,
        (LPVOID*)&opadSlotReadDevice
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_PAD_SLOT_READ_DEVICE));
    else
        HookLog("Controller: Failed to hook padSlotReadDevice (%d)\n", s);

    // padPollAllSlots
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_PAD_POLL_ALL_SLOTS),
        (LPVOID)&hkpadPollAllSlots,
        (LPVOID*)&opadPollAllSlots
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_PAD_POLL_ALL_SLOTS));
    else
        HookLog("Controller: Failed to hook padPollAllSlots (%d)\n", s);

    // xinputToGameMask
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_XINPUT_TO_GAME_MASK),
        (LPVOID)&hkxinputToGameMask,
        (LPVOID*)&oxinputToGameMask
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_XINPUT_TO_GAME_MASK));
    else
        HookLog("Controller: Failed to hook xinputToGameMask (%d)\n", s);

    // padSlotPostReadProcess
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_PAD_SLOT_POST_READ),
        (LPVOID)&hkpadSlotPostReadProcess,
        (LPVOID*)&opadSlotPostReadProcess
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_PAD_SLOT_POST_READ));
    else
        HookLog("Controller: Failed to hook padSlotPostReadProcess (%d)\n", s);

    // inputAutoRepeatTimerProcess
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_INPUT_AUTOREPEAT),
        (LPVOID)&hkinputAutoRepeatTimerProcess,
        (LPVOID*)&oinputAutoRepeatTimerProcess
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_INPUT_AUTOREPEAT));
    else
        HookLog("Controller: Failed to hook inputAutoRepeatTimerProcess (%d)\n", s);

    AutoTest_InitHooks();
    HookLog("Controller: Init complete - hooks installed\n");
}
