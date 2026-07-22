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
#include "diag.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

extern "C" bool Rollback_GetSnapshotInputs(int32_t frame, uint32_t* outLocal, uint32_t* outRemote);
extern "C" void Rollback_UpdateSnapshotInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput);
extern "C" void Rollback_CommitPendingNetplayLive(int32_t frame);
extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame);

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
// uAmaterasu__getter_4AC0 — returns the team index (0=P1, 1=P2) from a character object.
static constexpr uintptr_t ADDR_GET_TEAM_INDEX            = 0x140004AC0;

// sAction__stepIntro — auto-skip intro during netplay
static constexpr uintptr_t ADDR_STEP_INTRO                 = 0x140007D90;
// sAction__stepFighting — replace offline local-delta fight clocks during netplay.
static constexpr uintptr_t ADDR_STEP_FIGHTING              = 0x140007080;

// Character manager singleton (for desync checksum traversal)
static constexpr uintptr_t PTR_SCHARACTER                  = 0x140d44a70;

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

// Game-native format input history per team.
// Captured from cChrInput+0x14 (mHeld) AFTER oprocessFrameInput translation.
// Used during offline resim to replay correct game-native values.
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

bool g_multiResimEnabled = false; // Multi-resimulation toggle

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
static int32_t   g_desyncChecked[RING_SIZE];   // frame number when checksum was compared, -1 = unchecked
static bool      g_desyncMismatch[RING_SIZE];  // true if that frame exceeded threshold

static constexpr uint32_t DESYNC_THRESHOLD         = 5;
static constexpr int32_t  DESYNC_SKIP_FRAMES       = 0;
static constexpr int32_t  DESYNC_CONSECUTIVE_MIN   = 15;

// ---------------------------------------------------------------------------
// Frame advantage (GGPO-style) — RTT-based frame-count timesync
// ---------------------------------------------------------------------------
int32_t      g_localFrameAdvantage  = 0;  // negative = we're ahead of remote
int32_t      g_remoteFrameAdvantage = 0;  // received from peer via packet

// Timesync window averaging (GGPO-style)
static constexpr int TIMESYNC_WINDOW_SIZE       = 40;
static constexpr int TIMESYNC_MIN_FRAME_ADVANTAGE = 1;
static constexpr int TIMESYNC_MAX_FRAME_ADVANTAGE = 9;

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
    g_gameFrameBase = sReplay ? *(int32_t*)(sReplay + 0x54) : 0;
    g_netFrameCount = 0;
    g_fightFrameBase = -1;
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
    g_desyncDetected   = false;
    g_desyncSeen       = false;
    g_desyncFrame      = -1;
    g_desyncLocalCS    = 0;
    g_desyncRemoteCS   = 0;
    std::memset(g_localChecksum,    0, sizeof(g_localChecksum));
    std::memset(g_remoteChecksum,   0, sizeof(g_remoteChecksum));
    std::memset(g_desyncChecked,  0xFF, sizeof(g_desyncChecked));   // -1 per int32_t
    std::memset(g_desyncMismatch, 0,    sizeof(g_desyncMismatch));
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
    if (Netplay_IsActive())
    {
        bool isLocal = Netplay_IsHost() ? (teamIdx == 0) : (teamIdx == 1);
        if (isLocal)
            return g_recordedGameInput[teamIdx & 1][idx];
        else
            return g_usedRemoteInput[idx];
    }
    else
    {
        return g_recordedGameInput[teamIdx & 1][idx];
    }
}
extern "C" uint32_t Controller_GetReceivedRemoteInput(int32_t frame)
{
    if (frame < 0) return 0;
    return g_receivedInput[(uint32_t)frame % RING_SIZE];
}
extern "C" int32_t  Controller_GetLocalFrameAdvantage()  { return g_localFrameAdvantage; }
extern "C" int32_t  Controller_GetRemoteFrameAdvantage() { return g_remoteFrameAdvantage; }
extern "C" bool    Controller_IsRollbackEnabled()   { return g_rollbackFrames > 0; }

extern "C" void Controller_SetRollbackFrames(int32_t n)
{
    if (n < 0)                  n = 0;
    if (n > ROLLBACK_WINDOW)    n = ROLLBACK_WINDOW;
    g_rollbackFrames = n;
    if (n == 0)
    {
        std::memset(g_predictedInput, 0, sizeof(g_predictedInput));
        std::memset(g_predictedFrame, -1, sizeof(g_predictedFrame));
    }
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

// Override the local team's recorded game input for |frame| to |input|.
// Called during re-simulation from Rollback_ReSimulate before each tick,
// using the value saved in that frame's snapshot slot.
// hkprocessFrameInput reads g_recordedGameInput[localTeamIdx][idx] during
// re-sim, so pre-populating it from the snapshot makes the hook read the
// correct historical input without any shift-buffer dependency.
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
    }
    else
    {
        // Drain any inbound packets that arrived since last check
        InputPacket pkt;
        while (Netplay_GetInboundPacket(pkt))
        {
            int32_t pktMaxFrame = (int32_t)pkt.start_frame + NET_HISTORY_SIZE - 1;
            if (pktMaxFrame <= g_lastRemoteFrame)
                continue;
            for (size_t i = 0; i < NET_HISTORY_SIZE; i++)
            {
                int32_t targetFrame = (int32_t)pkt.start_frame + (int32_t)i;
                if (targetFrame >= 0)
                {
                    g_receivedInput[(uint32_t)targetFrame % RING_SIZE] = pkt.inputs[i];
                    g_confirmedFrame[(uint32_t)targetFrame % RING_SIZE] = targetFrame;
                    if (targetFrame > g_lastRemoteFrame)
                    {
                        g_lastRemoteFrame = targetFrame;
                        g_lastRemoteInput = pkt.inputs[i];
                    }
                }
            }

            LogReceivedPacket(pkt);
        }

        if (IsFrameConfirmed(frame))
        {
            g_predictedInput[idx] = false;
            g_usedRemoteInput[idx] = g_receivedInput[idx];
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
        for (int32_t f = frame - 1; f >= 0 && f >= frame - 250; f--)
        {
            if (IsFrameConfirmed(f))
            {
                prediction = g_receivedInput[f % RING_SIZE];
                found = true;
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

// ---------------------------------------------------------------------------
// hkGetsCurrentInput_withSOCD
// Hook on GetsCurrentInput_withSOCD at 0x1402B3700.
// ---------------------------------------------------------------------------
static uint64_t __fastcall hkGetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex)
{

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
                // Read the physical slot directly. Calling the original getter
                // for player 0 can route through keyboard input because the
                // boot path maps the keyboard to player 0.
                uint32_t padInput = *(uint32_t*)(pad + PAD_SLOT_INPUT);
                uint32_t kbInput = IsGameProcessForeground()
                    ? ControllerDecomp::keyboardToGameMask(pad)
                    : 0;
                raw = ApplySOCD(padInput | kbInput);
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

    // --- Capture raw input for ALL players every live frame (both netplay
    //     and offline/training).  During resim these recorded values are
    //     returned so the history rings inside cChrInput__processFrameInput
    //     receive the correct historical inputs instead of current physical. ---
    if (!g_isResimulating)
    {
        int32_t frame = GetGameFrameCount();
        if (frame >= 0 && playerIndex < 2)
        {
            g_rawLocalInput[playerIndex][(uint32_t)frame % RING_SIZE] = (uint32_t)raw;
        }
    }

    // --- DURING RESIM: return recorded raw per-player input.
    //     Note: For online, this value is immediately overwritten by
    //     sNetPad__readBufferedInputForTeam inside the native
    //     processFrameInput, so this only affects the offline resim
    //     path or the intermediate buttonRemap step.
    //     For online resim the TRUE input source is the netpad ring
    //     (which we must snapshot-restore separately — see
    //     Controller_SaveRollbackState sNetPad cursor save).
    if (g_isResimulating)
    {
        uint32_t idx = (uint32_t)g_resimulateCurrentFrame % RING_SIZE;
        if (is_active)
        {
            bool isLocalPlayer = Netplay_IsHost() ? (playerIndex == 0) : (playerIndex == 1);
            if (isLocalPlayer)
                return g_recordedLocalInput[idx];
            else
                return g_usedRemoteInput[idx];
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

static void __fastcall hkStepIntro(uint64_t param_1)
{
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
// hkprocessFrameInput (per character, inside oMove):
//   LOCAL:  accumulate, overwrite fields with delayed oldInputs value
//   REMOTE: overwrite fields with g_receivedInput[currentFrame % RING_SIZE]
// ---------------------------------------------------------------------------
static void DrainAndProcessPackets()
{
    InputPacket inPkt;
    while (Netplay_GetInboundPacket(inPkt))
    {
        // 1. Process inputs from the packet
        size_t framesToProcess = NET_HISTORY_SIZE;
        for (size_t i = 0; i < framesToProcess; i++)
        {
            int32_t targetFrame = (int32_t)inPkt.start_frame + (int32_t)i;
            if (targetFrame >= 0)
            {
                // History packets may revise a future frame during a delay
                // transition. Accept that newest value until the frame has
                // executed; past frames remain immutable in delay mode.
                if (!IsFrameConfirmed(targetFrame) || targetFrame > g_netFrameCount)
                {
                    g_receivedInput[(uint32_t)targetFrame % RING_SIZE] = inPkt.inputs[i];
                    g_confirmedFrame[(uint32_t)targetFrame % RING_SIZE] = targetFrame;
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

        // 2. Process the piggybacked checksum (always processed from every packet)
        if (g_netFrameCount >= DESYNC_SKIP_FRAMES)
        {
            int32_t csFrame = (int32_t)inPkt.checksum_frame;
            if (csFrame >= 0)
            {
                uint32_t ridx = (uint32_t)csFrame % RING_SIZE;
                g_remoteChecksum[ridx] = inPkt.checksum;

                if (g_localChecksum[ridx] != 0 && csFrame < g_netFrameCount)
                {
                    uint32_t diff = g_localChecksum[ridx] > inPkt.checksum
                                    ? g_localChecksum[ridx] - inPkt.checksum
                                    : inPkt.checksum - g_localChecksum[ridx];
                    g_desyncChecked[ridx]  = csFrame;
                    g_desyncMismatch[ridx] = (diff > DESYNC_THRESHOLD);
                    if (diff > DESYNC_THRESHOLD)
                    {
                        if (!g_desyncSeen && !g_desyncDetected)
                            HookLog("DESYNC MISMATCH at frame %d: diff=%u local=0x%08X remote=0x%08X\n",
                                    csFrame, diff, g_localChecksum[ridx], inPkt.checksum);
                    }
                }
            }
        }

        g_remoteFrameAdvantage = inPkt.frame_advantage;
        LogReceivedPacket(inPkt);
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
        // Only do rollback logic when in-game (aNextBattle step >= 2)
        typedef uint64_t(__fastcall* GetAmaterasu_t)();
        static GetAmaterasu_t getAmaterasu = (GetAmaterasu_t)_addr(0x140001A90);
        uint64_t aNextBattle = getAmaterasu();
        int step = aNextBattle ? *(int*)(aNextBattle + 0xD8) : 0;
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
                        HookLog("RollbackNet: conflict at frame %d  used=0x%08X recv=0x%08X diff=0x%08X re-sim [%d, %d)\n",
                                f, used, recv, used ^ recv,
                                f, g_netFrameCount);
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
// Desync checksum — sum of quantized positions + actionId across all
// characters.  Uses addition (order-independent) and integer quantization
// to tolerate tiny frame-timing float differences that are not visible
// desyncs but would cause order-sensitive XOR to false-positive.
// ---------------------------------------------------------------------------
// Desync checksum — sum of quantized positions + actionId across all
// characters.  Uses addition (order-independent) and integer quantization
// to tolerate tiny frame-timing float differences that are not visible
// desyncs but would cause order-sensitive XOR to false-positive.
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
                cs += (uint32_t)(qx < 0 ? -qx : qx);
                cs += (uint32_t)(qy < 0 ? -qy : qy);
                cs += *(uint32_t*)(character + 0x1310); // actionId
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    return cs;
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
    int32_t packetEndFrame = g_netFrameCount + g_delayFrames;
    for (int32_t frame = g_netFrameCount; frame < g_netFrameCount + MAX_HISTORY; ++frame)
    {
        uint32_t idx = (uint32_t)frame % RING_SIZE;
        if (g_scheduledLocalFrame[idx] == frame && frame > packetEndFrame)
            packetEndFrame = frame;
    }
    outPkt.start_frame = (uint32_t)(packetEndFrame - MAX_HISTORY + 1);
    for (size_t i = 0; i < MAX_HISTORY; i++)
    {
        int32_t frame = (int32_t)outPkt.start_frame + (int32_t)i;
        uint32_t idx = (uint32_t)frame % RING_SIZE;
        outPkt.inputs[i] = g_scheduledLocalFrame[idx] == frame
            ? g_scheduledLocalInput[idx] : 0;
    }

    if (!g_isResimulating && g_netFrameCount >= DESYNC_SKIP_FRAMES)
    {
        uint32_t cs = ComputeChecksum();
        uint32_t idx = (uint32_t)g_netFrameCount % RING_SIZE;
        g_localChecksum[idx] = cs;
        outPkt.checksum_frame = (uint32_t)g_netFrameCount;
        outPkt.checksum       = cs;
    }

    // Fill frame advantage (GGPO-style) — tell the remote peer our local estimate.
    outPkt.frame_advantage = g_localFrameAdvantage;
    outPkt.flags           = 1;  // non-zero = real packet (seed packet has flags=0)

    Netplay_SendPacket(outPkt);

    // --- Desync comparison: scan backward from newest remote checksum. ---
    if (!g_isResimulating && g_netFrameCount >= DESYNC_SKIP_FRAMES)
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
            if (consecutive >= DESYNC_CONSECUTIVE_MIN)
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
// LOCAL:  accumulate native-processed input, then overwrite character fields
//         with the delayed value from g_oldInputs.
// REMOTE: overwrite character fields with g_receivedInput[currentFrame].
//
// Both players experience the same delay: local via the shift buffer,
// remote via the frame-indexed receive array and network transport.
// ---------------------------------------------------------------------------
static void __fastcall hkprocessFrameInput(uint64_t cChrInput)
{
    // 1. Run native translation first so cChrInput fields (+0x14, etc.) populate normally.
    oprocessFrameInput(cChrInput);

    uint64_t character = *(uint64_t*)(cChrInput + 0x8);
    if (!character)
        return;

    typedef uint32_t(__fastcall* getTeamIdx_t)(uint64_t);
    static getTeamIdx_t fnGetTeamIdx = (getTeamIdx_t)_addr(ADDR_GET_TEAM_INDEX);
    uint32_t teamIdx = fnGetTeamIdx(character);

    // Frame resolution: use resim frame or live game frame count
    int32_t lookupFrame = g_isResimulating ? g_resimulateCurrentFrame : GetGameFrameCount();
    if (lookupFrame < 0)
        return;

    uint32_t idx = (uint32_t)lookupFrame % RING_SIZE;

    if (Netplay_IsActive())
    {
        if (g_isResimulating)
            DiagLogPreOverride(cChrInput, teamIdx);

        if (g_isResimulating)
        {
            uint32_t snapLocal = 0;
            uint32_t snapRemote = 0;
            if (Rollback_GetSnapshotInputs(lookupFrame, &snapLocal, &snapRemote))
            {
                bool isLocal = Netplay_IsHost() ? (teamIdx == 0) : (teamIdx == 1);
                uint32_t resolved = isLocal ? snapLocal : snapRemote;

                //HookLog("[ResimInput] the local and remote input for this frame was local=0x%08X remote=0x%08X from the snapshot frame %d\n",
                        //snapLocal, snapRemote, lookupFrame);

                *(uint32_t*)(cChrInput + 0x14)  = resolved; // mHeld
                *(uint32_t*)(cChrInput + 0x560) = resolved; // mRemapped
                *(uint32_t*)(cChrInput + 0x56C) = resolved; // mProcessed
                *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask — clear
            }
            else
            {
                // Fallback to ring buffer arrays if snapshot lookup fails
                if (Netplay_IsHost())
                {
                    uint32_t resolved = (teamIdx == 0) ? g_recordedGameInput[0][idx] : g_usedRemoteInput[idx];
                    *(uint32_t*)(cChrInput + 0x14)  = resolved;
                    *(uint32_t*)(cChrInput + 0x560) = resolved;
                    *(uint32_t*)(cChrInput + 0x56C) = resolved;
                    *(uint32_t*)(cChrInput + 0x568) = 0;
                }
                else
                {
                    uint32_t resolved = (teamIdx == 1) ? g_recordedGameInput[1][idx] : g_usedRemoteInput[idx];
                    *(uint32_t*)(cChrInput + 0x14)  = resolved;
                    *(uint32_t*)(cChrInput + 0x560) = resolved;
                    *(uint32_t*)(cChrInput + 0x56C) = resolved;
                    *(uint32_t*)(cChrInput + 0x568) = 0;
                }
            }
        }
        else
        {
            // Live Path
            if (Netplay_IsHost())
            {
                if (teamIdx == 0)
                {
                    // Host P1 (Local)
                    if (lookupFrame > g_delayFrames + g_rollbackFrames)
                    {
                        g_localPending |= *(uint32_t*)(cChrInput + 0x14);
                        uint32_t delayed = g_scheduledLocalFrame[idx] == lookupFrame
                            ? g_scheduledLocalInput[idx] : 0;
                        g_recordedGameInput[0][idx] = delayed;
                        *(uint32_t*)(cChrInput + 0x14)  = delayed;
                        *(uint32_t*)(cChrInput + 0x560) = delayed;
                        *(uint32_t*)(cChrInput + 0x56C) = delayed;
                        *(uint32_t*)(cChrInput + 0x568) = 0;
                    }
                    else
                    {
                        g_recordedGameInput[0][idx] = 0;
                        *(uint32_t*)(cChrInput + 0x14)  = 0;
                        *(uint32_t*)(cChrInput + 0x560) = 0;
                        *(uint32_t*)(cChrInput + 0x56C) = 0;
                        *(uint32_t*)(cChrInput + 0x568) = 0;
                    }
                }
                else if (teamIdx == 1)
                {
                    // Host P2 (Remote)
                    uint32_t remote = g_usedRemoteInput[idx];
                    *(uint32_t*)(cChrInput + 0x14)  = remote;
                    *(uint32_t*)(cChrInput + 0x560) = remote;
                    *(uint32_t*)(cChrInput + 0x56C) = remote;
                    *(uint32_t*)(cChrInput + 0x568) = 0;
                }
            }
            else // Client
            {
                if (teamIdx == 0)
                {
                    // Client P1 (Remote): read centrally-resolved input
                    uint32_t remote = g_usedRemoteInput[idx];
                    *(uint32_t*)(cChrInput + 0x14)  = remote;
                    *(uint32_t*)(cChrInput + 0x560) = remote;
                    *(uint32_t*)(cChrInput + 0x56C) = remote;
                    *(uint32_t*)(cChrInput + 0x568) = 0;
                }
                else if (teamIdx == 1)
                {
                    // Client P2 (Local)
                    if (lookupFrame > g_delayFrames + g_rollbackFrames)
                    {
                        g_localPending |= *(uint32_t*)(cChrInput + 0x14);
                        uint32_t delayed = g_scheduledLocalFrame[idx] == lookupFrame
                            ? g_scheduledLocalInput[idx] : 0;
                        g_recordedGameInput[1][idx] = delayed;

                        *(uint32_t*)(cChrInput + 0x14)  = delayed;
                        *(uint32_t*)(cChrInput + 0x560) = delayed;
                        *(uint32_t*)(cChrInput + 0x56C) = delayed;
                        *(uint32_t*)(cChrInput + 0x568) = 0;
                    }
                    else
                    {
                        g_recordedGameInput[1][idx] = 0;
                        *(uint32_t*)(cChrInput + 0x14)  = 0;
                        *(uint32_t*)(cChrInput + 0x560) = 0;
                        *(uint32_t*)(cChrInput + 0x56C) = 0;
                        *(uint32_t*)(cChrInput + 0x568) = 0;
                    }
                }
            }
        }

        DiagCaptureLiveRef(cChrInput, teamIdx, g_netFrameCount);

        if (g_isResimulating)
            DiagLogPostOverride(cChrInput, teamIdx);

        if (g_postResimFrame >= 0 && !g_isResimulating)
            DiagLogPostResim(cChrInput, teamIdx, g_netFrameCount);

        DiagLogActDbg(cChrInput, teamIdx, g_isResimulating ? g_resimulateCurrentFrame : g_netFrameCount, g_isResimulating);
    }
    else // Offline / Singleplayer / F7 test
    {
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
    opadPollAllSlots(sPad);

    if (Netplay_IsActive())
        return;

    // Copy P1's processed pad input → P2's pad slot input area.
    *(uint32_t*)(sPad + 1 * PAD_SLOT_STRIDE + PAD_SLOT_INPUT) =
        *(uint32_t*)(sPad + 0 * PAD_SLOT_STRIDE + PAD_SLOT_INPUT);
}

static uint64_t __fastcall hkpadSlotReadDevice(uint64_t slotBase)
{
    return opadSlotReadDevice(slotBase);
}

static void __fastcall hkxinputToGameMask(uint64_t sPad, uint64_t slotBase, const XInputState* pState)
{
    oxinputToGameMask(sPad, slotBase, pState);
}

static void __fastcall hkpadSlotPostReadProcess(uint64_t sPad, uint64_t slotBase, uint32_t slotIdx)
{
    opadSlotPostReadProcess(sPad, slotBase, slotIdx);
}

static void __fastcall hkinputAutoRepeatTimerProcess(uint64_t slotBase)
{
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

    HookLog("Controller: Init complete — hooks installed\n");
}
