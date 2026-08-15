// controller.cpp
// Decompiled pad/input functions from umvc3.exe, hooked via MinHook.
//
// Rollback netplay architecture:
//   Local input accumulates per-character in hkprocessFrameInput (identical to
//   original behavior).  At frame end the raw accumulated value is written into
//   a delay ring buffer at (currentFrame + delayFrames) and also sent over the
//   wire immediately.
//
//   On receipt, inbound inputs are stored at the sender's absolute frame index
//   (using inPkt.start_frame), binding both games to the same timeline.
//   Instead of blocking (spin-wait) when remote input hasn't arrived, we
//   predict using the last known remote value.  Late-arriving packets are
//   detected via g_usedRemoteInput vs g_receivedInput comparison, triggering
//   a rollback + re-simulation with corrected inputs.
#include "../pch.h"
#include "controller.h"
#include "mk10utils.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_profile.h"
#include "diag.h"
#include "autotest.h"
#include "framestepper.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

extern "C" bool Rollback_GetSnapshotInputs(int32_t frame, uint32_t* outLocal, uint32_t* outRemote);
extern "C" void Rollback_UpdateSnapshotInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput);
extern "C" void Rollback_CommitPendingNetplayLive(int32_t frame);
extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame);
extern "C" uint32_t FrameStepper_GetBattleGeneration();

static constexpr bool INPUT_PROVENANCE_TRACE = false;

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

// cChrInput__processFrameInput — the per-character input processor.
static constexpr uintptr_t ADDR_PROCESS_FRAME_INPUT       = 0x14002DD60;
static constexpr uintptr_t ADDR_IS_GAMEPLAY_ACTIVE        = 0x14002DCE0;
static constexpr uintptr_t ADDR_BUTTON_REMAP              = 0x14002DA30;
static constexpr uintptr_t ADDR_SWAP_LR_IF_FACE_LEFT      = 0x14002E560;
static constexpr uintptr_t ADDR_CPU_INPUT_SWAP_RETURN     = 0x14002DE57;
// uAmaterasu__getter_4AC0 — returns the team index (0=P1, 1=P2) from a character object.
static constexpr uintptr_t ADDR_GET_TEAM_INDEX            = 0x140004AC0;

// sAction__stepIntro — auto-skip intro during netplay
static constexpr uintptr_t ADDR_STEP_INTRO                 = 0x140007D90;
// sAction__stepFighting — replace offline local-delta fight clocks during netplay.
static constexpr uintptr_t ADDR_STEP_FIGHTING              = 0x140007080;

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

// processFrameInput trampoline
static void(__fastcall* oprocessFrameInput)(uint64_t cChrInput) = nullptr;
static uint32_t(__fastcall* obuttonRemap)(uint64_t playerIndex, uint32_t input,
                                          uint8_t alternateMap) = nullptr;
static uint32_t(__fastcall* oswapLRIfFaceLeft)(uint64_t cChrInput,
                                               uint32_t input) = nullptr;

// ===========================================================================
// Decompiled helper functions (inlined from game)
// ===========================================================================
namespace ControllerDecomp {

static uint32_t keyboardToGameMask(uint64_t sPad)
{
    typedef uint64_t(__fastcall* method_3D00_t)(uint64_t, uint32_t, uint32_t);
    static method_3D00_t fn3D00 = (method_3D00_t)_addr(ADDR_METHOD_3D00);

    uint32_t r = 0;

    // Movement/Directions
    if (fn3D00(sPad, 0x28, 0)) r |= IN_DOWN;
    if (fn3D00(sPad, 0x26, 0)) r |= IN_UP;
    if (fn3D00(sPad, 0x25, 0)) r |= IN_LEFT;
    if (fn3D00(sPad, 0x27, 0)) r |= IN_RIGHT;

    // Standard keyboard mappings for UMVC3 gameplay actions (Default layout)
    if (fn3D00(sPad, 0x41, 0)) r |= IN_X;       // Key 'A' -> Light Punch (IN_X)
    if (fn3D00(sPad, 0x53, 0)) r |= IN_Y;       // Key 'S' -> Medium Punch (IN_Y)
    if (fn3D00(sPad, 0x44, 0)) r |= IN_B;       // Key 'D' -> Heavy Punch (IN_B / Heavy)
    if (fn3D00(sPad, 0x5A, 0)) r |= IN_X;       // Key 'Z' -> Light Kick (IN_X)
    if (fn3D00(sPad, 0x58, 0)) r |= IN_Y;       // Key 'X' -> Medium Kick (IN_Y)
    if (fn3D00(sPad, 0x43, 0)) r |= IN_B;       // Key 'C' -> Heavy Kick (IN_B / Heavy)
    if (fn3D00(sPad, 0x46, 0)) r |= IN_A;       // Key 'F' -> Special Launcher (IN_A / Special)
    if (fn3D00(sPad, 0x51, 0)) r |= IN_LB;      // Key 'Q' -> Assist 1 (IN_LB)
    if (fn3D00(sPad, 0x57, 0)) r |= IN_RB;      // Key 'W' -> Assist 2 (IN_RB)

    // VK_RETURN and VK_A → unit flags (Start/Select in menus)
    if (fn3D00(sPad, 0x0D, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A70))(sPad);
    if (fn3D00(sPad, 0x41, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A70))(sPad);

    // VK_BACK and VK_B → UI anm flags
    if (fn3D00(sPad, 0x08, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A50))(sPad);
    if (fn3D00(sPad, 0x42, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A50))(sPad);

    // Modifiers / Menus
    if (fn3D00(sPad, 0xA2, 0)) r |= IN_LB;      // L-Shift
    if (fn3D00(sPad, 0xA0, 0)) r |= IN_LT;      // L-Ctrl
    if (fn3D00(sPad, 0xA4, 0)) r |= IN_L3;      // L-Alt
    if (fn3D00(sPad, 0xA3, 0)) r |= IN_RB;      // R-Shift
    if (fn3D00(sPad, 0xA1, 0)) r |= IN_RT;      // R-Ctrl
    if (fn3D00(sPad, 0xA5, 0)) r |= IN_R3;      // R-Alt
    if (fn3D00(sPad, 0x09, 0)) r |= IN_BACK;    // Tab
    if (fn3D00(sPad, 0x1B, 0)) r |= IN_START;   // Esc

    return r;
}

} // namespace ControllerDecomp

// ---------------------------------------------------------------------------
// ApplySOCD
// SOCD cleaning helper function.
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
// Netplay Input Buffers  (nifi.cpp-style shift buffer + frame-indexed array)
//
// g_oldInputs[] — shift buffer holding the last MAX_HISTORY frames of raw
//   accumulated local input.  Each frame everything shifts down by one and the
//   current accumulator is appended at the end.  Delayed local input is read
//   from g_oldInputs[MAX_HISTORY - g_delayFrames] BEFORE the shift.
//
// g_receivedInput[] — indexed by sender's absolute frame number (% RING_SIZE).
//   Inbound packets carry MAX_HISTORY slots.  Each slot is stored at
//   (inPkt.start_frame + slot_index) % RING_SIZE, binding both games to a
//   shared timeline and preventing drift.
// ===========================================================================
static constexpr int RING_SIZE      = 256;
static constexpr int MAX_HISTORY    = NET_HISTORY_SIZE;  // 16 — matches wire format

static uint32_t  g_oldInputs[MAX_HISTORY];     // local history (shift buffer)

// Local inputs keyed by their absolute execution frame. Unlike g_oldInputs,
// this schedule is not reinterpreted when delay changes: already queued inputs
// keep their original frame, while newly captured input uses the new delay.
static uint32_t  g_scheduledLocalInput[RING_SIZE];
static int32_t   g_scheduledLocalFrame[RING_SIZE];

// Remote inputs indexed by local frame number (mod RING_SIZE).
// All offsets are relative to the receiver's own frame counter.
static uint32_t  g_receivedInput[RING_SIZE];

// Tracks the exact absolute frame number currently occupying each slot in the ring buffer.
// Prevents reading stale inputs from frame - 256.
static int32_t   g_confirmedFrame[RING_SIZE];

// Tracks the exact absolute frame number of predicted inputs to avoid duplicate logs.
static int32_t   g_predictedFrame[RING_SIZE];

// Raw GetsCurrentInput_withSOCD result per frame x player (before delay).
// Used during resim to return the correct historical input from
// hkGetsCurrentInput_withSOCD so cChrInput__processFrameInput
// populates the history rings correctly.
// [playerIndex][frame % RING_SIZE]
static uint32_t  g_rawLocalInput[2][RING_SIZE];

// Final character-processed input history per team. This remains diagnostic;
// the netplay timeline carries button-remapped input before character-specific
// facing, ability, and suppression processing.
static uint32_t  g_recordedGameInput[2][RING_SIZE];

// Delay/rollback settings — defined here (above IsFrameConfirmed) because the
// startup-window confirmation check reads them.
static int32_t   g_delayFrames     = 2;
static int32_t   g_pendingDelayFrames = -1;
static int32_t   g_pendingDelayEffectiveFrame = -1;
static int32_t   g_rollbackFrames  = 8;

static bool IsFrameConfirmed(int32_t frame)
{
    if (frame < 0) return false;
    // Startup window: frames 0..(delay+rollback) are forced to zero input on
    // BOTH sides by the guards in hkprocessFrameInput / Controller_OnFrameEnd,
    // so the remote input for these frames is zero by construction.  Treat
    // them as confirmed locally — no seed packet needed, and no risk of a
    // stale seed packet falsely confirming frames beyond this window.
    if (frame <= g_delayFrames + g_rollbackFrames) return true;
    return g_confirmedFrame[frame % RING_SIZE] == frame;
}

extern "C" bool Controller_IsFrameConfirmed(int32_t frame)
{
    return IsFrameConfirmed(frame);
}

// Tracks whether a frame's remote input was predicted (vs confirmed).
static bool      g_predictedInput[RING_SIZE];

// Tracks what input we actually USED during simulation for each frame.
// Compared against g_receivedInput after each packet drain to detect
// late-arriving corrections that require a rollback + re-sim.
static uint32_t  g_usedRemoteInput[RING_SIZE];

// Records the exact delayed local input executed on each live frame.
// Read directly during re-simulation to replay the original local input
// instead of reading the real-time physical device, preventing input
// pollution that causes cyclic rollback conflicts.
static uint32_t  g_recordedLocalInput[RING_SIZE];

// Live frame reference record: captures post-override state during the original
// live tick so we can compare it against re-simulation of the same frame.


// Frame counter — read from the game's own sReplay singleton each frame.
// g_psReplay pointer   = *(uint64_t*)_addr(0x140d4d9d8)  (Ghidra confirmation)
// Frame counter uint32 = *(sReplay + 0x54)                (INC dword [RCX+0x54] at 0x1401cefdd)
static int32_t   g_netFrameCount   = -1;
// g_delayFrames / g_rollbackFrames are defined above IsFrameConfirmed.
static uint32_t  g_localPending    = 0;   // accumulator, ORed across team chars

// Tracks the latest frame for which remote input has been received.
// Used to hold-last-value when no new input has arrived for the current frame.
static int32_t   g_lastRemoteFrame = -1;
static uint32_t  g_lastRemoteInput = 0;
static int32_t   g_lastNonMultiCheckedFrame = -1;

bool g_multiResimEnabled = true; // Multi-resimulation toggle

// ---------------------------------------------------------------------------
// Desync detection — per-frame XOR checksums of all character state.
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
static int32_t   g_desyncChecked[RING_SIZE];   // frame number when checksum was compared, -1 = unchecked
static bool      g_desyncMismatch[RING_SIZE];  // true if that frame exceeded threshold
static int32_t   g_confirmedChecksumThrough = -1;
static int32_t   g_lastAdvertisedChecksumFrame = -1;

static constexpr uint32_t DESYNC_THRESHOLD         = 0;
// Restart setup can remain transient briefly after both peers report fighting.
// Start each checksum epoch after that state has settled instead of treating
// fresh startup differences as persistent match desyncs.
static constexpr int32_t  DESYNC_SKIP_FRAMES       = 250;
static constexpr int32_t  DESYNC_CONSECUTIVE_MIN   = 2;

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
    std::memset(g_desyncChecked, 0xFF, sizeof(g_desyncChecked));
    std::memset(g_desyncMismatch, 0, sizeof(g_desyncMismatch));
    g_confirmedChecksumThrough = -1;
    g_lastAdvertisedChecksumFrame = -1;
}

// ---------------------------------------------------------------------------
// Frame advantage (GGPO-style) — RTT-based frame-count timesync
// ---------------------------------------------------------------------------
int32_t      g_localFrameAdvantage  = 0;  // negative = we're ahead of remote
int32_t      g_remoteFrameAdvantage = 0;  // received from peer via packet

// Timesync window averaging (GGPO-style)
static constexpr int TIMESYNC_WINDOW_SIZE       = 40;
static constexpr int TIMESYNC_MIN_FRAME_ADVANTAGE = 1;
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

// Team + node traversal constants (mirrors rollback.cpp BuildRegions)
static constexpr uintptr_t TEAM0_OFFSET          = 0x40;
static constexpr uintptr_t TEAM1_OFFSET          = 0x300;
static constexpr uintptr_t TEAM_FIRST_NODE_OFFSET = 0x08;
static constexpr uintptr_t NODE_CHR_OFFSET       = 0x08;
static constexpr uintptr_t NODE_NEXT_OFFSET      = 0x10;

// ---------------------------------------------------------------------------
// Helper: read the game's authoritative frame counter from sReplay + 0x54.
// g_psReplay global at 0x140d4d9d8 (confirmed via Ghidra disassembly).
// Returns -1 if the replay system isn't initialized yet.
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
    // This runs at the mutual battle-release barrier. Using each process's
    // current native counter as a base gives the peers different logical frame
    // labels when they reach the barrier on different native ticks. Establish
    // the same explicit epoch on both peers instead.
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
    std::memset(g_recordedLocalInput, 0, sizeof(g_recordedLocalInput));
    std::memset(g_rawLocalInput,      0, sizeof(g_rawLocalInput));  // 2D [2][RING_SIZE]
    std::memset(g_recordedGameInput,  0, sizeof(g_recordedGameInput));
    std::memset(g_liveRef,            0, sizeof(g_liveRef));
    Diag_ResetBuffers();
    // g_netFrameCount is read from sReplay each frame; no reset needed.
    g_localPending    = 0;
    g_pendingDelayFrames = -1;
    g_pendingDelayEffectiveFrame = -1;
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
extern "C" int32_t Controller_GetNetFrameCount()    { return GetGameFrameCount(); }
extern "C" int32_t Controller_GetLastRemoteFrame()  { return g_lastRemoteFrame; }
extern "C" int32_t Controller_GetRollbackFrames()   { return g_rollbackFrames; }
extern "C" void Controller_ClearDesyncDetection()
{
    ResetDesyncDetection();
    // A reset can happen after the logical timeline has already advanced (for
    // example when both peers enter fighting after a restart). Start the new
    // checksum epoch at the current frame instead of waiting forever for
    // checksum records from earlier frames that were intentionally discarded.
    int32_t firstFrame = GetGameFrameCount();
    if (firstFrame < DESYNC_SKIP_FRAMES)
        firstFrame = DESYNC_SKIP_FRAMES;
    if (firstFrame > 0)
    {
        g_confirmedChecksumThrough = firstFrame - 1;
        g_lastAdvertisedChecksumFrame = firstFrame - 1;
    }
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

    // Frames handled under the previous mode must not be reconsidered by the
    // non-multi frontier. Their snapshots may already have rolled out.
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
    if (n == g_rollbackFrames)
        return;

    g_rollbackFrames = n;
    if (n == 0)
    {
        std::memset(g_predictedInput, 0, sizeof(g_predictedInput));
        std::memset(g_predictedFrame, -1, sizeof(g_predictedFrame));
        Rollback_ClearPendingReSimulation();
    }

    // A rollback-mode transition changes which frames are authoritative. Do not
    // compare checksum records produced under the previous mode.
    Controller_ClearDesyncDetection();
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

// Return the delayed local input that will be used for the current frame.
// At snapshot-save time (start of frame, before game tick), this value is
// g_oldInputs[MAX_HISTORY - g_delayFrames] — the input from g_delayFrames
// frames ago that hkprocessFrameInput will apply during the imminent tick.
extern "C" uint32_t Controller_GetCurrentFrameInput()
{
    int32_t frame = GetGameFrameCount();
    if (frame < 0) return 0;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    return g_scheduledLocalFrame[idx] == frame ? g_scheduledLocalInput[idx] : 0;
}

// Override offline resimulation's recorded input for |frame|. Netplay
// resimulation reads canonical pre-facing input directly from the snapshot.
extern "C" void Controller_InjectLocalInput(int32_t frame, uint32_t input)
{
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % RING_SIZE;
    uint32_t localTeam = Netplay_IsHost() ? 0 : 1;
    g_recordedGameInput[localTeam][idx] = input;
}

// Override the remote side's input for |frame| to |input|.
// Called during re-simulation after InjectLocalInput, using the value
// saved in that frame's snapshot slot.
// hkprocessFrameInput reads g_usedRemoteInput[idx] during re-sim for
// the remote team, so pre-populating it from the snapshot makes the
// hook replay the correct remote input.
extern "C" void Controller_InjectRemoteInput(int32_t frame, uint32_t input)
{
    uint32_t idx = (uint32_t)(frame >= 0 ? frame : 0) % RING_SIZE;
    g_usedRemoteInput[idx] = input;
}

// Save/restore the local input shift buffer around re-sim.  The re-sim loop
// calls SilentFrameEnd which shifts g_oldInputs with accumulated re-sim local
// input (which is 0 during re-sim since we bypass with g_recordedLocalInput).
// Restoring it prevents the next outbound packet from carrying a corrupted
// timeline that would cause spurious conflicts on the remote side.
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

// Centrally resolve the remote input for a given game frame into
// g_usedRemoteInput.  Called once per frame in Controller_OnFrameStart
// (for the current live frame) and once per re-simmed frame in the
// Rollback_ReSimulate loop.  g_lastRemoteFrame is never rolled back
// (excluded from snapshot save/restore), so the resolution stays
// accurate across rollback boundaries.
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

            int32_t pktMaxFrame = (int32_t)pkt.start_frame + NET_HISTORY_SIZE - 1;
            if (pktMaxFrame <= g_lastRemoteFrame) {
                TraceInputPacket("IPRX", "resolve-dropped", g_netFrameCount, pkt,
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

        // Delay-only netcode must never manufacture remote input. Its frame
        // start path blocks until confirmation; other callers may inspect a
        // future frame, but they must leave it unresolved rather than predict.
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

// ---------------------------------------------------------------------------
// Controller_LogResimNotify — called when remote peer reports a re-simulation.
// Logs both the packet data (sender's pre/post state for P1+P2 point chars)
// and the local g_liveRef historical live reference so both sides' states
// can be directly compared.
// ---------------------------------------------------------------------------
static void Controller_LogResimNotify(const ResimNotifyPacket& pkt)
{
    if (pkt.resimStartFrame >= pkt.resimEndFrame) return;
    HookLog("ResimNotify: remote re-simmed frames [%d, %d) correctedTeam=%u frameCount=%u\n",
            pkt.resimStartFrame, pkt.resimEndFrame, pkt.correctedTeam, pkt.frameCount);

    for (uint32_t fi = 0; fi < pkt.frameCount; fi++)
    {
        const auto& rf = pkt.frames[fi];
        uint32_t idx = (uint32_t)rf.frame % RING_SIZE;
        bool confirmed = IsFrameConfirmed(rf.frame);

        HookLog("ResimNotify frame=%d: used=0x%08X recv=0x%08X %s\n",
                rf.frame, rf.usedInput, rf.recvInput,
                confirmed ? "CONFIRMED" : "PREDICTED");

        // Packet data: sender's pre and post for both teams' point chars
        HookLog("ResimNotify frame=%d P1-pre:  act=0x%08X pos=(%.4f,%.4f,%.4f) fl=0x%08X mHeld=0x%08X fcnt=%llu rng=[0x%08X,0x%08X,0x%08X,0x%08X]\n",
                rf.frame,
                rf.p1Pre.actionId, rf.p1Pre.posX, rf.p1Pre.posY, rf.p1Pre.posZ,
                rf.p1Pre.flags, rf.p1Pre.mHeld, (unsigned long long)rf.p1Pre.frameCnt,
                rf.p1Pre.rng[0], rf.p1Pre.rng[1], rf.p1Pre.rng[2], rf.p1Pre.rng[3]);
        HookLog("ResimNotify frame=%d P1-post: act=0x%08X pos=(%.4f,%.4f,%.4f) fl=0x%08X mHeld=0x%08X fcnt=%llu rng=[0x%08X,0x%08X,0x%08X,0x%08X]\n",
                rf.frame,
                rf.p1Post.actionId, rf.p1Post.posX, rf.p1Post.posY, rf.p1Post.posZ,
                rf.p1Post.flags, rf.p1Post.mHeld, (unsigned long long)rf.p1Post.frameCnt,
                rf.p1Post.rng[0], rf.p1Post.rng[1], rf.p1Post.rng[2], rf.p1Post.rng[3]);
        HookLog("ResimNotify frame=%d P2-pre:  act=0x%08X pos=(%.4f,%.4f,%.4f) fl=0x%08X mHeld=0x%08X fcnt=%llu rng=[0x%08X,0x%08X,0x%08X,0x%08X]\n",
                rf.frame,
                rf.p2Pre.actionId, rf.p2Pre.posX, rf.p2Pre.posY, rf.p2Pre.posZ,
                rf.p2Pre.flags, rf.p2Pre.mHeld, (unsigned long long)rf.p2Pre.frameCnt,
                rf.p2Pre.rng[0], rf.p2Pre.rng[1], rf.p2Pre.rng[2], rf.p2Pre.rng[3]);
        HookLog("ResimNotify frame=%d P2-post: act=0x%08X pos=(%.4f,%.4f,%.4f) fl=0x%08X mHeld=0x%08X fcnt=%llu rng=[0x%08X,0x%08X,0x%08X,0x%08X]\n",
                rf.frame,
                rf.p2Post.actionId, rf.p2Post.posX, rf.p2Post.posY, rf.p2Post.posZ,
                rf.p2Post.flags, rf.p2Post.mHeld, (unsigned long long)rf.p2Post.frameCnt,
                rf.p2Post.rng[0], rf.p2Post.rng[1], rf.p2Post.rng[2], rf.p2Post.rng[3]);
    }
}

extern "C" void Controller_LogReceivedInputAvailability(int32_t startFrame, int32_t endFrame)
{
    return;
    if (startFrame >= endFrame) return;

    int32_t predictedCount = 0;
    int32_t confirmedCount = 0;
    int32_t correctPredictionCount = 0;
    int32_t conflictCount = 0;

    //HookLog("[%s] Remote input summary for re-sim frames [%d, %d):\n",
    //        Netplay_IsHost() ? "host" : "client", startFrame, endFrame);
    //HookLog("  %-8s %-12s %-12s %s\n", "Frame", "Predicted", "Received", "Status");
    for (int32_t f = startFrame; f < endFrame; f++)
    {
        uint32_t idx = (uint32_t)f % RING_SIZE;
        bool confirmed = IsFrameConfirmed(f);
        uint32_t predicted = g_usedRemoteInput[idx];
        uint32_t received = confirmed ? g_receivedInput[idx] : 0;
        const char* status;
        if (confirmed) {
            if (g_predictedInput[idx] && predicted == received) {
                status = "PREDICTED_CORRECT";
                correctPredictionCount++;
            } else if (predicted == received) {
                status = "CONFIRMED";
                confirmedCount++;
            } else {
                status = "CONFLICT";
                conflictCount++;
            }
        } else {
            status = "PREDICTED";
            predictedCount++;
        }
        //HookLog("  %-8d 0x%08X 0x%08X %s\n", f, predicted, received, status);
    }
    if (conflictCount == 0 && correctPredictionCount > 0)
    {
    //    HookLog("  >>> All predictions correct for frames [%d, %d)\n", startFrame, endFrame);
    }
    //HookLog("  summary: %d correct, %d confirmed, %d conflict, %d predicted out of %d frames\n",
    //        correctPredictionCount, confirmedCount, conflictCount, predictedCount, endFrame - startFrame);
}
// Silent frame-boundary helpers for re-simulation — maintain the local input
// shift buffer without draining inbound packets or sending outbound data.
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
    if (delay < 1)                  delay = 1;
    if (delay > MAX_HISTORY - 1)    delay = MAX_HISTORY - 1;

    if (delay == g_delayFrames)
        return;

    g_delayFrames = delay;
    HookLog("Netplay: delay changed to %d\n", delay);
}

extern "C" void Controller_ScheduleDelayFrames(int32_t delay, int32_t effectiveFrame)
{
    if (delay < 1) delay = 1;
    if (delay > MAX_HISTORY - 1) delay = MAX_HISTORY - 1;
    g_pendingDelayFrames = delay;
    g_pendingDelayEffectiveFrame = effectiveFrame;
    HookLog("Netplay: delay change to %d scheduled for frame %d\n", delay, effectiveFrame);
}

static void ApplyScheduledDelayIfReady()
{
    if (g_pendingDelayFrames < 0 || g_netFrameCount < g_pendingDelayEffectiveFrame)
        return;

    const int32_t newDelay = g_pendingDelayFrames;

    g_delayFrames = newDelay;
    g_pendingDelayFrames = -1;
    g_pendingDelayEffectiveFrame = -1;
    HookLog("Netplay: applied delay %d at frame %d\n",
            newDelay, g_netFrameCount);
}

// ===========================================================================
// Hooked functions
// ===========================================================================

extern "C" bool Controller_RemapSyntheticInput(uint32_t rawInput,
                                                 uint32_t* outInput)
{
    if (!obuttonRemap || !outInput)
        return false;

    // Index 1 bypasses the native index-0 focus/menu shortcut. Team ownership
    // is resolved separately by the cChrInput instance receiving the result.
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
            ? g_scheduledLocalInput[idx] : 0;
    }

    context->timelineInput = resolved;
    context->timelineResolved = true;
    return resolved;
}

static uint32_t __fastcall hkbuttonRemap(uint64_t playerIndex, uint32_t rawInput,
                                         uint8_t alternateMap)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::ButtonRemap);
    FrameInputRemapContext* context = g_frameInputRemapContext;
    if (!context || !Netplay_IsActive())
        return obuttonRemap(playerIndex, rawInput, alternateMap);

    uint32_t inputTeam = playerIndex < 2 ? (uint32_t)playerIndex
                                         : context->ownerTeam;
    uint32_t currentInput;
    uint32_t rawTestInput = 0;
    if (!g_isResimulating &&
        AutoTest_GetInput(context->frame, inputTeam, &rawTestInput)) {
        // Player index 0 has a native focus/menu shortcut that can ignore the
        // supplied mask. Index 1 always performs ordinary gameplay remapping.
        currentInput = obuttonRemap(1, rawTestInput, alternateMap);
    } else {
        currentInput = obuttonRemap(playerIndex, rawInput, alternateMap);

        // Training boot assigns the keyboard to P1, while a netplay client
        // owns P2. Poll P1's native keyboard remap here so its internal
        // focus/device gate remains authoritative, then merge the resulting
        // logical actions with P2's remapped controller input.
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

    // The ordinary path already received canonical timeline input from
    // hkbuttonRemap. Preserve native ability/team transformations applied
    // between button remapping and facing conversion.
    if (context->injectedAtButtonRemap)
        return oswapLRIfFaceLeft(cChrInput, input);

    // Every native source path that produces gameplay input converges on this
    // helper, including action/CPU paths that bypass buttonRemap. The early
    // action path leaves mHeld untouched after this call, so update it here as
    // well as substituting the value passed through native facing conversion.
    // The first swap call in the attached uChrCpu-controller path receives the
    // CPU's freshly generated logical mask. If this peer owns that team, treat
    // it exactly like freshly remapped physical input and schedule it onto the
    // authoritative timeline. Other bypass callsites are replay/special paths
    // and must not become new input authorities.
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    bool captureCpuInput = caller == (uintptr_t)_addr(ADDR_CPU_INPUT_SWAP_RETURN);
    uint32_t resolved = ResolveTimelineInput(
        context, context->timelineTeam, input, captureCpuInput);
    *(uint32_t*)(cChrInput + 0x14) = resolved;
    return oswapLRIfFaceLeft(cChrInput, resolved);
}

// ---------------------------------------------------------------------------
// hkGetsCurrentInput_withSOCD
// Hook on GetsCurrentInput_withSOCD at 0x1402B3700.
// ---------------------------------------------------------------------------
static uint64_t __fastcall hkGetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::GetCurrentInput);

    ///Note here this was changed in the commit after da18134e, it may be wrong
    bool is_active = Netplay_IsActive();

    // --- Determine raw input via the original function ---
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
                // Read the physical slot directly. Client keyboard input is
                // merged after native remapping in hkbuttonRemap.
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

    // During offline resimulation, replay the historical physical mask.
    // Netplay instead injects its canonical logical value in hkbuttonRemap.
    if (g_isResimulating)
    {
        uint32_t idx = (uint32_t)g_resimulateCurrentFrame % RING_SIZE;
        if (is_active)
        {
            // Netplay resimulation injects canonical logical input after the
            // native raw-to-logical remap. Do not reinterpret that value as a
            // physical pad mask here.
            return 0;
        }
        else
        {
            // Singleplayer offline resim: return per-player raw input recorded
            // during the live frames.  g_rawLocalInput is populated on every
            // non-resim frame regardless of netplay state (unlike
            // g_recordedLocalInput which is only written by
            // Controller_OnFrameEnd for netplay).
            if (playerIndex < 2)
                return g_rawLocalInput[playerIndex][idx];
            else
                return raw;
        }
    }

    return raw;
}

// ---------------------------------------------------------------------------
// hkStepIntro
// Hook on sAction__stepIntro at 0x140007D90.
// When netplay is active, lets the intro initialize for a few frames then
// skips deterministically at frame 10 (same frame on both sides via frame 0
// sync), avoiding both the black-screen from zero-frame intros and the
// desync from button-based skipping.
// ---------------------------------------------------------------------------
static void(__fastcall* oStepIntro)(uint64_t param_1) = nullptr;
static void(__fastcall* oStepFighting)(uint64_t param_1) = nullptr;
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" uint32_t FrameStepper_GetBattleGeneration();
extern "C" bool FrameStepper_IsMatchRestartPending();
extern "C" bool FrameStepper_IsDesyncCheckReady();
extern "C" void FrameStepper_NotifyFighting();

static void __fastcall hkStepIntro(uint64_t param_1)
{
    RollbackHookProfileScope profile(RollbackHookProfileId::StepIntro);
    if (Netplay_IsActive() && FrameStepper_IsBattleSynchronized()) {
        auto setCountdown = (void(__fastcall*)(uint64_t, int))_addr(0x14000AE00);
        setCountdown(param_1, 3);
        return;
    }
    oStepIntro(param_1);
}

// Native online play derives these clocks from the synchronized netpad cursor
// instead of accumulating each machine's local sMain delta.  The mod runs the
// game in an offline BattleType, so reproduce that invariant from our shared
// frame timeline.  Apply both before and after the original function: the
// pre-write makes timeout/action threshold checks see synchronized time, while
// the post-write removes the one local-delta increment performed by the
// offline branch.
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

    // Native sMvc3SyncPad divides its cursor delta by 60 as an integer before
    // converting to float, so sAction receives whole elapsed seconds.
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

// ---------------------------------------------------------------------------
// Frame Boundary Synchronization  (nifi.cpp-style)
//
// Controller_OnFrameStart (called before oMove):
//   - Increment frame counter, reset accumulator
//   - Drain inbound packets: store each history slot at (frame % RING_SIZE)
//
// Controller_OnFrameEnd (called after oMove):
//   - Read delayed local input from oldInputs (BEFORE shifting), then shift
//     the buffer and write the current accumulator at the end
//   - Send outbound packet: MAX_HISTORY slots, starting from
//     (currentFrame + g_delayFrames - MAX_HISTORY + 1)
//     → newest slot maps to (currentFrame + g_delayFrames) on the wire
//
// hkprocessFrameInput scopes the current character; hkbuttonRemap captures
// local logical input and substitutes the scheduled local or remote timeline.
// ---------------------------------------------------------------------------
static void DrainAndProcessPackets()
{
    InputPacket inPkt;
    while (Netplay_GetInboundPacket(inPkt))
    {
        if (inPkt.battle_generation != FrameStepper_GetBattleGeneration())
            continue;

        uint16_t acceptedMask = 0;
        uint16_t changedMask = 0;
        uint16_t priorConfirmedMask = 0;
        // 1. Process inputs from the packet
        size_t framesToProcess = NET_HISTORY_SIZE;
        for (size_t i = 0; i < framesToProcess; i++)
        {
            int32_t targetFrame = (int32_t)inPkt.start_frame + (int32_t)i;
            if (targetFrame >= 0)
            {
                bool wasConfirmed = IsFrameConfirmed(targetFrame);
                if (wasConfirmed) priorConfirmedMask |= (uint16_t)(1u << i);
                // History packets may revise a future frame during a delay
                // transition. Accept that newest value until the frame has
                // executed; past frames remain immutable in delay mode.
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

                // Track the absolute furthest remote frame for prediction reference
                if (targetFrame > g_lastRemoteFrame)
                {
                    g_lastRemoteFrame = targetFrame;
                    g_lastRemoteInput = inPkt.inputs[i];
                }
            }
        }

        // 2. Queue the piggybacked checksum. It is compared at frame end, after
        // any rollback requested by this packet has replaced speculative state.
        if (FrameStepper_IsDesyncCheckReady() &&
            g_netFrameCount >= DESYNC_SKIP_FRAMES)
        {
            int32_t csFrame = (int32_t)inPkt.checksum_frame;
            if (csFrame >= 0)
            {
                uint32_t ridx = (uint32_t)csFrame % RING_SIZE;
                g_remoteChecksum[ridx] = inPkt.checksum;
                g_remoteChecksumFrame[ridx] = csFrame;
            }
        }

        g_remoteFrameAdvantage = inPkt.frame_advantage;
        LogReceivedPacket(inPkt);
        TraceInputPacket("IPRX", "frame-start", g_netFrameCount, inPkt, 0,
                         acceptedMask, changedMask, priorConfirmedMask);
    }
}

static void WaitForConfirmedFrame(int32_t frame, const char* reason)
{
    if (IsFrameConfirmed(frame) || !Netplay_IsActive())
        return;

    //HookLog("Netplay: waiting for remote input frame %d (%s, local=%d, lastRemote=%d)\n",
    //        frame, reason, g_netFrameCount, g_lastRemoteFrame);
    while (!IsFrameConfirmed(frame) && Netplay_IsActive())
    {
        SwitchToThread();
        DrainAndProcessPackets();
    }
    //HookLog("Netplay: remote input frame %d wait complete (active=%d, lastRemote=%d)\n",
    //        frame, Netplay_IsActive() ? 1 : 0, g_lastRemoteFrame);
}

// ---------------------------------------------------------------------------
extern "C" void Controller_OnFrameStart()
{
    if (!Netplay_IsActive())
        return;

    g_netFrameCount = GetGameFrameCount();
    // If sReplay isn't ready yet, skip packet processing this frame.
    if (g_netFrameCount < 0) return;

    // Keep the native replay cursor on the shared logical epoch established at
    // the mutual battle-release barrier.
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (sReplay) {
        *(int32_t*)(sReplay + 0x54) = g_netFrameCount;
        g_gameFrameBase = 0;
    }

    ApplyScheduledDelayIfReady();

    g_localPending = 0;

    // Use the same ingestion path here and inside waits. Previously each path
    // had different stale-packet handling, so a packet could advance
    // g_lastRemoteFrame without confirming the frame a waiter needed.
    DrainAndProcessPackets();

    // Process inbound ground truth packets
    Rollback_ProcessInboundGroundTruth();
    
    // --- Drain resim-notify packets from the remote peer ---
    // The remote side notifies us whenever it re-simulates a range of frames
    // due to late-arriving input correction.  We dump our live character state
    // for those frames to enable cross-side diagnostic comparison.
    {
        ResimNotifyPacket rnPkt;
        while (Netplay_GetInboundResimNotify(rnPkt))
        {
            //Controller_LogResimNotify(rnPkt.resimStartFrame, rnPkt.resimEndFrame);
        }
    }

    if (g_rollbackFrames > 0)
    {
        // Only do rollback logic once the sAction battle step has started.
        typedef uint64_t(__fastcall* GetAction_t)();
        static GetAction_t getAction = (GetAction_t)_addr(0x140001A90);
        uint64_t action = getAction();
        int step = action ? *(int*)(action + 0xD8) : 0;
        if (step >= 2)
        {
            // Bound prediction by the configured input delay plus rollback
            // window. Unlike the old consecutive-miss counter, this measures
            // the actual oldest remote frame needed by the current timeline.
            int32_t predictionBudget = g_delayFrames + g_rollbackFrames;
            if (g_netFrameCount > predictionBudget)
            {
                int32_t oldestRequiredFrame = g_netFrameCount - predictionBudget;
                WaitForConfirmedFrame(oldestRequiredFrame, "prediction budget exhausted");
            }

        // Multi-resim checks the rolling window continuously. Non-multi only
        // inspects the newly confirmed frontier; if it finds a conflict, it
        // waits for a definitive batch before replaying that range once.
        bool shouldCheckRollback = g_netFrameCount > 0 &&
            (g_multiResimEnabled || g_lastRemoteFrame > g_lastNonMultiCheckedFrame);
        if (shouldCheckRollback)
        {
            // Match the prediction budget above. A frame confirmed by the
            // budget wait must still be included in this conflict scan.
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
                    // Non-multi does not replace snapshots inside the replay.
                    // Confirm the whole batch first so no frame in it can need
                    // correction a second time.
                    WaitForConfirmedFrame(g_netFrameCount, "non-multi definitive batch");
                    checkEnd = g_netFrameCount;
                }

                // Commit any pending correct frames BEFORE the earliest conflict
                for (int32_t f = checkStart; f < earliestConflict; f++)
                {
                    if (IsFrameConfirmed(f))
                    {
                        Rollback_CommitPendingNetplayLive(f);
                    }
                }

                // Update all snapshots in the range with definitive confirmed inputs
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
                // No conflict in the entire window! All confirmed frames are correct.
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
                        Rollback_CommitPendingNetplayLive(f);
                    }
                }
            }

            if (!g_multiResimEnabled)
            {
                // Never advance past a hole. A frame that was not confirmed in
                // this pass still needs comparison when a later packet fills it.
                while (g_lastNonMultiCheckedFrame + 1 < checkEnd &&
                       IsFrameConfirmed(g_lastNonMultiCheckedFrame + 1))
                {
                    g_lastNonMultiCheckedFrame++;
                }
            }
        }
    }
    }
    else
    {
        // --- Legacy delay-based fallback: spin-wait for current frame's input.
        int32_t neededFrame = g_netFrameCount;
        WaitForConfirmedFrame(neededFrame, "delay mode");
    }

    // --- Centrally resolve this frame's remote input so hkprocessFrameInput
    //     just reads from g_usedRemoteInput (no per-character resolution logic).
    //     Must run for both rollback and delay-based paths.
    Controller_ResolveRemoteInput(g_netFrameCount);

    // Rollback_SaveSnapshot runs outside this hook, before current-frame input
    // resolution. Attach the exact inputs this live tick will execute so a later
    // cascading resim does not read stale ring-slot data from the snapshot.
    if (g_rollbackFrames > 0)
    {
        uint32_t idx = (uint32_t)g_netFrameCount % RING_SIZE;
        uint32_t localInput = g_scheduledLocalFrame[idx] == g_netFrameCount
            ? g_scheduledLocalInput[idx] : 0;
        Rollback_UpdateSnapshotInputs(
            g_netFrameCount, localInput, g_usedRemoteInput[idx]);
    }

    // --- GGPO-style frame advantage timesync.
    //     Estimate remote frame: last received frame + RTT converted to frames.
    //     g_localFrameAdvantage = remote_estimate - local_frame
    //       positive = we're behind, negative = we're ahead
    if (g_netFrameCount >= 0 && g_lastRemoteFrame >= 0)
    {
        int32_t rttMs = (int32_t)Netplay_GetRTT();
        int32_t remoteFrameEst = g_lastRemoteFrame + (rttMs * 60 / 1000);
        g_localFrameAdvantage = remoteFrameEst - g_netFrameCount;

        // Advance timesync window
        uint32_t tidx = (uint32_t)g_timesyncFrameCount % TIMESYNC_WINDOW_SIZE;
        g_localAdvHistory[tidx]  = g_localFrameAdvantage;
        g_remoteAdvHistory[tidx] = g_remoteFrameAdvantage;
        g_timesyncFrameCount++;

        // Recommend wait duration — only if both peers agree we're ahead
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

            // Both must agree: only sleep if local < remote (they agree we're ahead)
            if (avgLocal < avgRemote)
            {
                int sleepFrames = (int)(((avgRemote - avgLocal) / 2.0f) + 0.5f);
                if (sleepFrames >= TIMESYNC_MIN_FRAME_ADVANTAGE)
                {
                    if (sleepFrames > TIMESYNC_MAX_FRAME_ADVANTAGE)
                        sleepFrames = TIMESYNC_MAX_FRAME_ADVANTAGE;
                    Sleep(sleepFrames);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Desync checksum — sum of quantized positions, HP, actionId, and independently
// hashed RNG state across all characters, then mix in the global gameplay RNG
// state. Character state uses order-independent addition and integer
// quantization to tolerate tiny frame-timing float differences that are not
// visible desyncs.
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

                // Hash the complete MtXorShift state independently, then add it
                // so team and linked-list traversal order cannot change cs.
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

    // A corrected frame invalidates any earlier comparison for that frame.
    // Confirmed-only comparison normally prevents this, but clearing the tags
    // also makes explicit/manual re-simulation safe.
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

    while (g_confirmedChecksumThrough < g_netFrameCount)
    {
        int32_t next = g_confirmedChecksumThrough + 1;
        uint32_t idx = (uint32_t)next % RING_SIZE;
        if (!IsFrameConfirmed(next) || g_localChecksumFrame[idx] != next)
            break;
        g_confirmedChecksumThrough = next;
    }

    int32_t first = g_confirmedChecksumThrough - RING_SIZE + 1;
    if (first < 0) first = 0;
    for (int32_t frame = first; frame <= g_confirmedChecksumThrough; ++frame)
    {
        uint32_t idx = (uint32_t)frame % RING_SIZE;
        if (g_localChecksumFrame[idx] != frame ||
            g_remoteChecksumFrame[idx] != frame ||
            g_desyncChecked[idx] == frame)
            continue;

        uint32_t local = g_localChecksum[idx];
        uint32_t remote = g_remoteChecksum[idx];
        uint32_t diff = local > remote ? local - remote : remote - local;
        g_desyncChecked[idx] = frame;
        g_desyncMismatch[idx] = diff > DESYNC_THRESHOLD;
        if (diff > DESYNC_THRESHOLD && !g_desyncSeen && !g_desyncDetected)
            HookLog("DESYNC MISMATCH at confirmed frame %d: diff=%u local=0x%08X remote=0x%08X\n",
                    frame, diff, local, remote);
    }
}

extern "C" void Controller_OnFrameEnd()
{
    if (!Netplay_IsActive())
        return;

    // Use the authoritative post-tick frame counter (advanced during oMove)
    // for the recording index so it matches the render thread's read index.
    int32_t completedFrame = GetGameFrameCount();

    // Input hooks force this startup window to zero, but we must still send its
    // history. In delay mode both peers otherwise reach frame delay+1 and wait
    // before either has ever queued a packet, producing a symmetric deadlock.

    // --- 1. Resolve remote input for the completed frame index so the rendering
    //         thread doesn't read stale/unresolved data from the previous ring buffer loop.
    if (g_rollbackFrames > 0 || IsFrameConfirmed(completedFrame))
        Controller_ResolveRemoteInput(completedFrame);

    // --- 2. Record the local input assigned to this absolute frame.
    //         Done at the frame boundary rather than inside hkprocessFrameInput
    //         because per-character processFrameInput is not called during
    //         cinematic freezes, hitstun, or transition screens, which would
    //         leave gaps in g_recordedLocalInput and cause input display flicker.
    {
        uint32_t idx = (uint32_t)g_netFrameCount % RING_SIZE;
        g_recordedLocalInput[idx] = g_scheduledLocalFrame[idx] == g_netFrameCount
            ? g_scheduledLocalInput[idx] : 0;
    }

    // --- 3. Assign newly captured input to its absolute execution frame.
    // Existing scheduled frames are never relabeled by a delay change.
    int32_t scheduledFrame = g_netFrameCount + g_delayFrames;
    uint32_t scheduledIdx = (uint32_t)scheduledFrame % RING_SIZE;
    if (g_scheduledLocalFrame[scheduledIdx] == scheduledFrame)
        g_scheduledLocalInput[scheduledIdx] |= g_localPending;
    else
        g_scheduledLocalInput[scheduledIdx] = g_localPending;
    g_scheduledLocalFrame[scheduledIdx] = scheduledFrame;

    // Keep the legacy history populated for rollback save/restore diagnostics.
    memmove(&g_oldInputs[0], &g_oldInputs[1],
            (MAX_HISTORY - 1) * sizeof(uint32_t));
    g_oldInputs[MAX_HISTORY - 1] = g_localPending;

    // --- 4. Build outbound packet — inputs[] are absolute-frame-indexed.
    //     start_frame = (currentFrame + delay) - MAX_HISTORY + 1
    //     inputs[0]                    → frame start_frame          (oldest)
    //     inputs[MAX_HISTORY-1]        → frame start_frame+MAX_HISTORY-1 (newest)
    //     checksum is piggybacked (Slippi-style) for desync detection.
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

        int32_t checksumFrame = g_lastAdvertisedChecksumFrame + 1;
        if (checksumFrame <= g_confirmedChecksumThrough)
        {
            uint32_t idx = (uint32_t)checksumFrame % RING_SIZE;
            if (g_localChecksumFrame[idx] == checksumFrame)
            {
                outPkt.checksum_frame = (uint32_t)checksumFrame;
                outPkt.checksum = g_localChecksum[idx];
                g_lastAdvertisedChecksumFrame = checksumFrame;
            }
        }
    }

    // Fill frame advantage (GGPO-style) — tell the remote peer our local estimate.
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

    // --- Desync comparison: scan backward from newest remote checksum. ---
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
            for (int32_t f = newestChecked; f >= 0 && consecutive < DESYNC_CONSECUTIVE_MIN; f--)
            {
                int32_t idx = f % RING_SIZE;
                if (g_desyncChecked[idx] == f)
                {
                    if (g_desyncMismatch[idx])
                        consecutive++;
                    else
                        break;  // matched within threshold — streak broken
                }
                // else: no checksum for this frame (lost packet) — skip it,
                // continue scanning older frames to find a longer streak
            }
            // Autotest performs its own move-scoped comparison between the
            // host's confirmed live frame and the client's forced resim frame.
            // The session-wide detector can still contain startup/setup
            // mismatches (often before the move's start frame), so it must not
            // publish a verdict while a suite move is active.
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
// hkprocessFrameInput
// Hook on processFrameInput at 0x14002DD60.
//
// Netplay timeline input is substituted by hkbuttonRemap while the original
// function is running. Native eligibility, facing, ability, pause, and team
// masks therefore remain authoritative.
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

    // Native processing clears reserve input, then returns here for ordinary
    // assists. Preserve the same gate for offline post-native test injection.
    typedef bool(__fastcall* isGameplayActive_t)(uint64_t);
    static isGameplayActive_t fnIsGameplayActive =
        (isGameplayActive_t)_addr(ADDR_IS_GAMEPLAY_ACTIVE);
    if (!fnIsGameplayActive(cChrInput))
        return;

    uint32_t idx = (uint32_t)lookupFrame % RING_SIZE;

    if (Netplay_IsActive())
    {
        // The original function has already applied the injected canonical
        // timeline value and all character-specific transformations.
        g_recordedGameInput[teamIdx & 1][idx] =
            *(uint32_t*)(cChrInput + 0x14);

        DiagCaptureLiveRef(cChrInput, teamIdx, g_netFrameCount);

        if (g_postResimFrame >= 0 && !g_isResimulating)
            DiagLogPostResim(cChrInput, teamIdx, g_netFrameCount);

        DiagLogActDbg(cChrInput, teamIdx, g_isResimulating ? g_resimulateCurrentFrame : g_netFrameCount, g_isResimulating);
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
                // Read from our unified Game Native Format history ring
                inputVal = g_recordedGameInput[teamIdx & 1][idx];
            }
            *(uint32_t*)(cChrInput + 0x14)  = inputVal; // mHeld
            *(uint32_t*)(cChrInput + 0x560) = inputVal; // mRemapped
            *(uint32_t*)(cChrInput + 0x56C) = inputVal; // mProcessed
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask
        }
        else
        {
            // Live run: capture the game native input translated by oprocessFrameInput
            inputVal = *(uint32_t*)(cChrInput + 0x14);
            g_recordedGameInput[teamIdx & 1][idx] = inputVal;
        }
    }
}


// ---------------------------------------------------------------------------
// Offline Mirroring & Standard Hook Implementations
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
    // Multiple game instances can poll the same XInput device. Only the
    // foreground instance should receive controller input; keyboard routing
    // remains independent so the inactive instance can still be controlled
    // with its own keyboard if desired.
    if (!IsGameProcessForeground()) {
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

    // GetsCurrentInput_withSOCD — primary hook for netplay P2 injection
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_GET_CURRENT_INPUT),
        (LPVOID)&hkGetsCurrentInput_withSOCD,
        (LPVOID*)&oGetsCurrentInput_withSOCD
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_GET_CURRENT_INPUT));
    else
        HookLog("Controller: Failed to hook GetsCurrentInput_withSOCD (%d)\n", s);

    // sAction__stepIntro — auto-skip intro during netplay
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_STEP_INTRO),
        (LPVOID)&hkStepIntro,
        (LPVOID*)&oStepIntro
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_STEP_INTRO));
    else
        HookLog("Controller: Failed to hook stepIntro (%d)\n", s);

    // sAction__stepFighting — derive fight clocks from the synchronized frame.
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_STEP_FIGHTING),
        (LPVOID)&hkStepFighting,
        (LPVOID*)&oStepFighting
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_STEP_FIGHTING));
    else
        HookLog("Controller: Failed to hook stepFighting (%d)\n", s);

    // Enforce timeline ownership at the common native facing-conversion seam.
    // This also covers action/CPU sources that bypass buttonRemap.
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_SWAP_LR_IF_FACE_LEFT),
        (LPVOID)&hkswapLRIfFaceLeft,
        (LPVOID*)&oswapLRIfFaceLeft
    );
    if (s == MH_OK)
        s = MH_EnableHook((LPVOID)_addr(ADDR_SWAP_LR_IF_FACE_LEFT));
    if (s != MH_OK)
        HookLog("Controller: Failed to hook swapLRIfFaceLeft (%d)\n", s);

    // Capture canonical local input after physical button bindings resolve.
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_BUTTON_REMAP),
        (LPVOID)&hkbuttonRemap,
        (LPVOID*)&obuttonRemap
    );
    if (s == MH_OK)
        s = MH_EnableHook((LPVOID)_addr(ADDR_BUTTON_REMAP));
    if (s != MH_OK)
        HookLog("Controller: Failed to hook buttonRemap (%d)\n", s);

    // cChrInput__processFrameInput — netplay client/host synchronization
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_PROCESS_FRAME_INPUT),
        (LPVOID)&hkprocessFrameInput,
        (LPVOID*)&oprocessFrameInput
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_PROCESS_FRAME_INPUT));
    else
        HookLog("Controller: Failed to hook processFrameInput (%d)\n", s);

    // padSlotReadDevice
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_PAD_SLOT_READ_DEVICE),
        (LPVOID)&hkpadSlotReadDevice,
        (LPVOID*)&opadSlotReadDevice
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_PAD_SLOT_READ_DEVICE));
    else
        HookLog("Controller: Failed to hook padSlotReadDevice (%d)\n", s);

    // padPollAllSlots
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_PAD_POLL_ALL_SLOTS),
        (LPVOID)&hkpadPollAllSlots,
        (LPVOID*)&opadPollAllSlots
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_PAD_POLL_ALL_SLOTS));
    else
        HookLog("Controller: Failed to hook padPollAllSlots (%d)\n", s);

    // xinputToGameMask
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_XINPUT_TO_GAME_MASK),
        (LPVOID)&hkxinputToGameMask,
        (LPVOID*)&oxinputToGameMask
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_XINPUT_TO_GAME_MASK));
    else
        HookLog("Controller: Failed to hook xinputToGameMask (%d)\n", s);

    // padSlotPostReadProcess
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_PAD_SLOT_POST_READ),
        (LPVOID)&hkpadSlotPostReadProcess,
        (LPVOID*)&opadSlotPostReadProcess
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_PAD_SLOT_POST_READ));
    else
        HookLog("Controller: Failed to hook padSlotPostReadProcess (%d)\n", s);

    // inputAutoRepeatTimerProcess
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_INPUT_AUTOREPEAT),
        (LPVOID)&hkinputAutoRepeatTimerProcess,
        (LPVOID*)&oinputAutoRepeatTimerProcess
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_INPUT_AUTOREPEAT));
    else
        HookLog("Controller: Failed to hook inputAutoRepeatTimerProcess (%d)\n", s);

    AutoTest_InitHooks();
    HookLog("Controller: Init complete — hooks installed\n");
}
