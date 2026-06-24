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
#include "../kiero/minhook/include/MinHook.h"
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

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

// Remote inputs indexed by local frame number (mod RING_SIZE).
// All offsets are relative to the receiver's own frame counter.
static uint32_t  g_receivedInput[RING_SIZE];

// Tracks the exact absolute frame number currently occupying each slot in the ring buffer.
// Prevents reading stale inputs from frame - 256.
static int32_t   g_confirmedFrame[RING_SIZE];

// Raw GetsCurrentInput_withSOCD result per frame x player (before delay).
// Used during resim to return the correct historical input from
// hkGetsCurrentInput_withSOCD so cChrInput__processFrameInput
// populates the history rings correctly.
// [playerIndex][frame % RING_SIZE]
static uint32_t  g_rawLocalInput[2][RING_SIZE];

static bool IsFrameConfirmed(int32_t frame)
{
    if (frame < 0) return false;
    return g_confirmedFrame[frame % RING_SIZE] == frame;
}

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
struct LiveFrameRecord {
    uint32_t mHeld;
    uint32_t m18;
    uint32_t m1C;
    uint32_t mRemapped;
    uint32_t mMask;
    uint32_t mProcessed;
    uint64_t frameCnt;
    uint32_t h590;
    uint32_t h594;
    uint64_t heldR0;
    uint32_t relHead;
    uint64_t relR0;
    uint64_t prsR0;
    uint32_t dir0;
    uint32_t dir4;
    uint64_t dirRing0;
    uint64_t dirRingSlot;
    uint64_t dirRing1;
    uint64_t dirRing2;
    uint32_t actionId;
    uint32_t flags14F0;
    float    posX;
    float    posY;
    float    posZ;
};
static LiveFrameRecord g_liveRef[2][RING_SIZE];

// Frame counter — read from the game's own sReplay singleton each frame.
// g_psReplay pointer   = *(uint64_t*)_addr(0x140d4d9d8)  (Ghidra confirmation)
// Frame counter uint32 = *(sReplay + 0x54)                (INC dword [RCX+0x54] at 0x1401cefdd)
static int32_t   g_netFrameCount   = -1;
static int32_t   g_delayFrames     = 2;
static int32_t   g_rollbackFrames  = 8;
static uint32_t  g_localPending    = 0;   // accumulator, ORed across team chars

// Tracks the latest frame for which remote input has been received.
// Used to hold-last-value when no new input has arrived for the current frame.
static int32_t   g_lastRemoteFrame = -1;
static uint32_t  g_lastRemoteInput = 0;

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
static constexpr int32_t  DESYNC_CONSECUTIVE_MIN   = 30;

// ---------------------------------------------------------------------------
// Timesync (Slippi-style) — QPC-based frame timing
// ---------------------------------------------------------------------------
uint64_t     g_lastRemoteTimestampUs  = 0;
uint64_t     g_avgRttUs              = 50000;   // start with 50ms default
int64_t      g_clockOffsetUs         = 0;
int64_t      g_timesyncAdvantage     = 0;

static uint64_t g_localFrameQPC[RING_SIZE];       // QPC when each local frame was processed
static int32_t  g_localFrameQPC_Frame[RING_SIZE]; // frame number for each QPC entry (-1 = unused)
static uint64_t g_lastSendQPC = 0;                // QPC when we last sent a packet

// Helper: current time in microseconds (QPC-based)
static inline uint64_t GetUs()
{
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return (uint64_t)((qpc.QuadPart * 1000000LL) / freq.QuadPart);
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

static int32_t GetGameFrameCount()
{
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (!sReplay) return -1;
    return *(int32_t*)(sReplay + 0x54);
}

// ---------------------------------------------------------------------------
// Reset buffers — called from Netplay_Init
// ---------------------------------------------------------------------------
extern "C" void Controller_ResetBuffers()
{
    std::memset(g_oldInputs,          0, sizeof(g_oldInputs));
    std::memset(g_receivedInput,      0, sizeof(g_receivedInput));
    std::memset(g_confirmedFrame,    -1, sizeof(g_confirmedFrame));
    std::memset(g_usedRemoteInput,    0, sizeof(g_usedRemoteInput));
    std::memset(g_recordedLocalInput, 0, sizeof(g_recordedLocalInput));
    std::memset(g_rawLocalInput,      0, sizeof(g_rawLocalInput));  // 2D [2][RING_SIZE]
    std::memset(g_liveRef,            0, sizeof(g_liveRef));
    // g_netFrameCount is read from sReplay each frame; no reset needed.
    g_localPending    = 0;
    g_lastRemoteFrame = -1;
    g_lastRemoteInput = 0;
    g_desyncDetected   = false;
    g_desyncSeen       = false;
    g_desyncFrame      = -1;
    g_desyncLocalCS    = 0;
    g_desyncRemoteCS   = 0;
    std::memset(g_localChecksum,    0, sizeof(g_localChecksum));
    std::memset(g_remoteChecksum,   0, sizeof(g_remoteChecksum));
    std::memset(g_desyncChecked,  0xFF, sizeof(g_desyncChecked));   // -1 per int32_t
    std::memset(g_desyncMismatch, 0,    sizeof(g_desyncMismatch));
    g_lastRemoteTimestampUs = 0;
    g_avgRttUs              = 50000;
    g_clockOffsetUs         = 0;
    g_timesyncAdvantage     = 0;
    std::memset(g_localFrameQPC, 0, sizeof(g_localFrameQPC));
    std::memset(g_localFrameQPC_Frame, -1, sizeof(g_localFrameQPC_Frame));
    g_lastSendQPC = 0;
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
extern "C" int64_t  Controller_GetTimesyncAdvantage() { return g_timesyncAdvantage; }
extern "C" uint64_t Controller_GetAvgRttUs()          { return g_avgRttUs; }
extern "C" bool    Controller_IsRollbackEnabled()   { return g_rollbackFrames > 0; }

extern "C" void Controller_SetRollbackFrames(int32_t n)
{
    if (n < 0)                  n = 0;
    if (n > ROLLBACK_WINDOW)    n = ROLLBACK_WINDOW;
    g_rollbackFrames = n;
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

// Save/restore the local input shift buffer around re-sim.  The re-sim loop
// calls SilentFrameEnd which shifts g_oldInputs with accumulated re-sim local
// input (which is 0 during re-sim since we bypass with g_recordedLocalInput).
// Restoring it prevents the next outbound packet from carrying a corrupted
// timeline that would cause spurious conflicts on the remote side.
static uint32_t g_savedInputHistory[MAX_HISTORY];
static uint32_t g_savedLocalPendingReSim;

extern "C" void Controller_SaveInputHistory()
{
    memcpy(g_savedInputHistory, g_oldInputs, sizeof(g_oldInputs));
    g_savedLocalPendingReSim = g_localPending;
}

extern "C" void Controller_RestoreInputHistory()
{
    memcpy(g_oldInputs, g_savedInputHistory, sizeof(g_oldInputs));
    g_localPending = g_savedLocalPendingReSim;
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
        g_usedRemoteInput[idx] = g_receivedInput[idx];
    }
    else
    {
        // Predict using the latest confirmed frame's input
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
        g_usedRemoteInput[idx] = prediction;
    }
}

// Log the inputs used during a re-simmed frame for debugging.
extern "C" void Controller_LogReSimFrame(int32_t frame)
{
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    HookLog("[%s] ReSim frame %d: local=0x%08X remote=0x%08X recv=0x%08X\n",
            Netplay_IsHost() ? "host" : "client",
            frame,
            g_recordedLocalInput[idx],
            g_usedRemoteInput[idx],
            IsFrameConfirmed(frame) ? g_receivedInput[idx] : 0);
}

static uint32_t ComputeChecksum();

// Recompute and store local checksum for a given frame after re-simulation
// so that subsequent desync comparisons use the corrected state, not the
// stale pre-rollback checksum.
extern "C" void Controller_StoreChecksum(int32_t frame)
{
    if (frame >= DESYNC_SKIP_FRAMES)
    {
        uint32_t cs = ComputeChecksum();
        g_localChecksum[(uint32_t)frame % RING_SIZE] = cs;
    }
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
    g_delayFrames = delay;
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
                uint32_t padInput = (uint32_t)oGetsCurrentInput_withSOCD(pad, 0);
                uint32_t kbInput  = ControllerDecomp::keyboardToGameMask(pad);
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
        if (frame >= 0)
        {
            uint32_t pIdx = (uint32_t)playerIndex & 1;
            g_rawLocalInput[pIdx][(uint32_t)frame % RING_SIZE] = (uint32_t)raw;
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
                return g_rawLocalInput[playerIndex & 1][idx];
            else
                return 0; // remote: match live behavior, sNetPad supplies the real value
        }
        else
        {
            return g_rawLocalInput[playerIndex & 1][idx];
        }
    }

    return raw;
}

// ---------------------------------------------------------------------------
// hkStepIntro
// Hook on sAction__stepIntro at 0x140007D90.
// When netplay is active, immediately transitions to the 3-second countdown
// phase (same path as training mode's auto-skip), avoiding desync from
// unsynchronized skip-button checks.
// ---------------------------------------------------------------------------
static void(__fastcall* oStepIntro)(uint64_t param_1) = nullptr;

static void __fastcall hkStepIntro(uint64_t param_1)
{
    if (Netplay_IsActive()) {
        // Same as training mode skip: jump directly to countdown
        auto setCountdown = (void(__fastcall*)(uint64_t, int))_addr(0x14000AE00);
        setCountdown(param_1, 3);
        return;
    }
    oStepIntro(param_1);
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
extern "C" void Controller_OnFrameStart()
{
    if (!Netplay_IsActive())
        return;

    g_netFrameCount = GetGameFrameCount();
    // If sReplay isn't ready yet, skip packet processing this frame.
    if (g_netFrameCount < 0) return;

    g_localPending = 0;

    // --- Drain every received packet.
    //     By using the sender's transmitted start_frame, we align both games
    //     to the same absolute timeline and prevent coordinate/frame drift.
    InputPacket inPkt;
    while (Netplay_GetInboundPacket(inPkt))
    {
        // Out-of-order protection: discard the packet entirely if its maximum
        // frame coverage is older than or equal to our already known remote frame.
        int32_t pktMaxFrame = (int32_t)inPkt.start_frame + NET_HISTORY_SIZE - 1;
        if (pktMaxFrame <= g_lastRemoteFrame)
        {
            continue;
        }

        for (size_t i = 0; i < NET_HISTORY_SIZE; i++)
        {
            // Cast to signed int32_t to handle negative/underflowed frame
            // numbers gracefully during initial startup frames.
            int32_t targetFrame = (int32_t)inPkt.start_frame + (int32_t)i;
            if (targetFrame >= 0)
            {
                g_receivedInput[(uint32_t)targetFrame % RING_SIZE] = inPkt.inputs[i];
                g_confirmedFrame[(uint32_t)targetFrame % RING_SIZE] = targetFrame;

                // Track the latest known remote frame
                // Guard: seed packet (timestamp_us == 0) still confirms frames
                // to break the deadlock, but must NOT inflate g_lastRemoteFrame
                // or real packets get discarded by the out-of-order check.
                if (targetFrame > g_lastRemoteFrame && inPkt.timestamp_us != 0)
                {
                    g_lastRemoteFrame = targetFrame;
                    g_lastRemoteInput = inPkt.inputs[i];
                }
            }
        }

        // Piggybacked checksum (Slippi-style): extract and compare.
        // checksum == 0 is a sentinel meaning "not yet confirmed" — skip it.
        if (g_netFrameCount >= DESYNC_SKIP_FRAMES)
        {
            int32_t csFrame = (int32_t)inPkt.checksum_frame;
            if (csFrame >= 0 && inPkt.checksum != 0)
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

        // --- Timesync: RTT, clock offset, and frame advantage ---
        uint64_t now = GetUs();
        uint64_t ts = inPkt.timestamp_us;

        // Save remote timestamp for echo in our next outbound packet
        g_lastRemoteTimestampUs = ts;

        // Skip timesync on zero-timestamp packets (e.g. the init seed packet)
        if (ts != 0)
        {
            // RTT measurement: remote echoed our timestamp back
            if (inPkt.echo_timestamp_us != 0)
            {
                uint64_t rtt = now - inPkt.echo_timestamp_us;
                if (rtt >= 1000 && rtt <= 500000)
                    g_avgRttUs = (uint64_t)(g_avgRttUs * 0.9 + rtt * 0.1);
            }

            // Clock offset: remote_timestamp - (local_recv - one_way)
            uint64_t one_way = g_avgRttUs / 2;
            int64_t offset_this = (int64_t)ts
                                - ((int64_t)now - (int64_t)one_way);
            g_clockOffsetUs = (int64_t)(g_clockOffsetUs * 0.95 + offset_this * 0.05);

        // Frame timing advantage:
        //   The sender recorded timestamp_us at its frame N (in OnFrameEnd).
        //   Packet start_frame = N + g_delayFrames - MAX_HISTORY + 1, so
        //   N = start_frame + MAX_HISTORY - 1 - g_delayFrames.
        //   Look up our local QPC for the same frame N.

            int32_t senderFrame = (int32_t)inPkt.start_frame + MAX_HISTORY - 1 - g_delayFrames;
            uint32_t remoteIdx = (uint32_t)senderFrame % RING_SIZE;
            if (g_localFrameQPC_Frame[remoteIdx] == senderFrame)
            {
                int64_t T_local         = (int64_t)g_localFrameQPC[remoteIdx];
                int64_t T_remote_local  = (int64_t)inPkt.timestamp_us - g_clockOffsetUs;
                int64_t adv_this        = T_local - T_remote_local;
                // Filter: only update if this sample is within ±200ms of current estimate
                if (adv_this > g_timesyncAdvantage - 200000 &&
                    adv_this < g_timesyncAdvantage + 200000)
                    g_timesyncAdvantage = (int64_t)(g_timesyncAdvantage * 0.9 + adv_this * 0.1);
            }
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
            // --- Bounded spin-wait: if remote is more than (delay + rollback_frames)
            //     behind, we can't roll back (snapshot ring buffer wraps).  Block
            //     until the gap shrinks back within the window.
            int32_t spinWaitThreshold = g_delayFrames + g_rollbackFrames;
            if (g_netFrameCount > spinWaitThreshold)
            {
                int32_t minNeeded = g_netFrameCount - spinWaitThreshold;
                while (!IsFrameConfirmed(minNeeded) && Netplay_IsActive())
                {
                    Sleep(1);
                    InputPacket waitPkt;
                    while (Netplay_GetInboundPacket(waitPkt))
                    {
                        int32_t waitPktMaxFrame = (int32_t)waitPkt.start_frame + NET_HISTORY_SIZE - 1;
                        if (waitPktMaxFrame <= g_lastRemoteFrame)
                        {
                            continue;
                        }

                        for (size_t i = 0; i < NET_HISTORY_SIZE; i++)
                        {
                            int32_t targetFrame = (int32_t)waitPkt.start_frame + (int32_t)i;
                            if (targetFrame >= 0)
                            {
                                g_receivedInput[(uint32_t)targetFrame % RING_SIZE] = waitPkt.inputs[i];
                                g_confirmedFrame[(uint32_t)targetFrame % RING_SIZE] = targetFrame;
                                if (targetFrame > g_lastRemoteFrame)
                                {
                                    g_lastRemoteFrame = targetFrame;
                                    g_lastRemoteInput = waitPkt.inputs[i];
                                }
                            }
                        }

                        int32_t wcsFrame = (int32_t)waitPkt.checksum_frame;
                        if (wcsFrame >= 0 && waitPkt.checksum != 0)
                        {
                            uint32_t wridx = (uint32_t)wcsFrame % RING_SIZE;
                            g_remoteChecksum[wridx] = waitPkt.checksum;
                            if (g_localChecksum[wridx] != 0 && wcsFrame < g_netFrameCount)
                            {
                                uint32_t diff = g_localChecksum[wridx] > waitPkt.checksum
                                                ? g_localChecksum[wridx] - waitPkt.checksum
                                                : waitPkt.checksum - g_localChecksum[wridx];
                                g_desyncChecked[wridx]  = wcsFrame;
                                g_desyncMismatch[wridx] = (diff > DESYNC_THRESHOLD);
                                if (diff > DESYNC_THRESHOLD)
                                {
                                    if (!g_desyncSeen && !g_desyncDetected)
                                        HookLog("DESYNC MISMATCH at frame %d: diff=%u local=0x%08X remote=0x%08X\n",
                                                wcsFrame, diff, g_localChecksum[wridx], waitPkt.checksum);
                                }
                            }
                        }
                    }
                }
            }

            // --- Rollback detection: check if any newly received input for a frame
            //     we already simulated differs from what we predicted/used.
            //     If so, request a re-sim from the earliest conflicting frame.
            if (g_netFrameCount > 0)
            {
                int32_t checkStart = g_netFrameCount - g_rollbackFrames;
                if (checkStart < 0) checkStart = 0;
                int32_t earliestConflict = -1;
                for (int32_t f = checkStart; f < g_netFrameCount; f++)
                {
                    if (IsFrameConfirmed(f))
                    {
                        uint32_t recv = g_receivedInput[(uint32_t)f % RING_SIZE];
                        uint32_t used = g_usedRemoteInput[(uint32_t)f % RING_SIZE];
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
                    Rollback_RequestReSimulate(earliestConflict, g_netFrameCount);
                }
            }
        }
    }
    else
    {
        // --- Legacy delay-based fallback: spin-wait for current frame's input.
        int32_t neededFrame = g_netFrameCount;
        while (!IsFrameConfirmed(neededFrame) && Netplay_IsActive())
            {
                Sleep(1);
                InputPacket waitPkt;
                while (Netplay_GetInboundPacket(waitPkt))
                {
                    int32_t waitPktMaxFrame = (int32_t)waitPkt.start_frame + NET_HISTORY_SIZE - 1;
                    if (waitPktMaxFrame <= g_lastRemoteFrame)
                    {
                        continue;
                    }

                    for (size_t i = 0; i < NET_HISTORY_SIZE; i++)
                    {
                        int32_t targetFrame = (int32_t)waitPkt.start_frame + (int32_t)i;
                        if (targetFrame >= 0)
                        {
                            g_receivedInput[(uint32_t)targetFrame % RING_SIZE] = waitPkt.inputs[i];
                            g_confirmedFrame[(uint32_t)targetFrame % RING_SIZE] = targetFrame;
                            if (targetFrame > g_lastRemoteFrame)
                            {
                                g_lastRemoteFrame = targetFrame;
                                g_lastRemoteInput = waitPkt.inputs[i];
                            }
                        }
                    }
                }
            }
    }

    // --- Centrally resolve this frame's remote input so hkprocessFrameInput
    //     just reads from g_usedRemoteInput (no per-character resolution logic).
    //     Must run for both rollback and delay-based paths.
    Controller_ResolveRemoteInput(g_netFrameCount);

    // --- Timesync: gently slow down if we are ahead of the remote.
    //     g_timesyncAdvantage < 0 means we processed this frame before the
    //     remote did (in wall-clock terms) → we're ahead → add a small sleep
    //     so both peers converge.
    if (g_netFrameCount >= DESYNC_SKIP_FRAMES && g_lastRemoteTimestampUs != 0)
    {
        if (g_timesyncAdvantage < -16666)
            Sleep(8);       // more than 1 frame ahead
        else if (g_timesyncAdvantage < -5000)
            Sleep(3);       // ~1/3 frame ahead
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

    // Don't send anything on frame 0 — let the first frame be neutral so both
    // sides have time to establish the connection and fill initial delay slots.
    if (completedFrame < 1)
        return;

    // --- 1. Resolve remote input for the completed frame index so the rendering
    //         thread doesn't read stale/unresolved data from the previous ring buffer loop.
    Controller_ResolveRemoteInput(completedFrame);

    // --- 2. Record the executed delayed local input for display before shifting.
    //         Done at the frame boundary rather than inside hkprocessFrameInput
    //         because per-character processFrameInput is not called during
    //         cinematic freezes, hitstun, or transition screens, which would
    //         leave gaps in g_recordedLocalInput and cause input display flicker.
    {
        uint32_t idx = (uint32_t)completedFrame % RING_SIZE;
        g_recordedLocalInput[idx] = g_oldInputs[MAX_HISTORY - g_delayFrames];
    }

    // --- 3. Shift history buffer: discard oldest, append current.
    memmove(&g_oldInputs[0], &g_oldInputs[1],
            (MAX_HISTORY - 1) * sizeof(uint32_t));
    g_oldInputs[MAX_HISTORY - 1] = g_localPending;

    // --- 4. Build outbound packet — inputs[] are absolute-frame-indexed.
    //     start_frame = (currentFrame + delay) - MAX_HISTORY + 1
    //     inputs[0]                    → frame start_frame          (oldest)
    //     inputs[MAX_HISTORY-1]        → frame start_frame+MAX_HISTORY-1 (newest)
    //     checksum is piggybacked (Slippi-style) for desync detection.
    InputPacket outPkt = {};
    outPkt.start_frame = (uint32_t)(g_netFrameCount + g_delayFrames
                                     - MAX_HISTORY + 1);
    for (size_t i = 0; i < MAX_HISTORY; i++)
        outPkt.inputs[i] = g_oldInputs[i];

    if (g_netFrameCount >= DESYNC_SKIP_FRAMES)
    {
        uint32_t cs = ComputeChecksum();
        uint32_t idx = (uint32_t)g_netFrameCount % RING_SIZE;
        g_localChecksum[idx] = cs;

        if (!g_isResimulating)
        {
            outPkt.checksum_frame = (uint32_t)g_netFrameCount;
            if (IsFrameConfirmed(g_netFrameCount))
                outPkt.checksum = cs;
            else
                outPkt.checksum = 0; // sentinel — unconfirmed, may change after rollback
        }
    }

    // Fill timesync timestamps — record local QPC at the same point in the
    // frame cycle as the remote will when receiving this packet (symmetric).
    uint64_t now = GetUs();
    uint32_t qpcIdx = (uint32_t)g_netFrameCount % RING_SIZE;
    g_localFrameQPC[qpcIdx] = now;
    g_localFrameQPC_Frame[qpcIdx] = g_netFrameCount;
    outPkt.timestamp_us      = now;
    outPkt.echo_timestamp_us = g_lastRemoteTimestampUs;

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
                    HookLog("DESYNC TRIGGER at frame %d: %d consecutive mismatches\n",
                            newestChecked, consecutive);
                }
            }
        }
    }

    // Log this frame's local input and what was sent.
    uint32_t localFrame = (uint32_t)g_netFrameCount % RING_SIZE;
    int32_t thisIdx = g_netFrameCount - (int32_t)outPkt.start_frame;
    /*HookLog("Frame %d: local=0x%08X sent[start=%u idx=%d val=0x%08X]\n",
            g_netFrameCount,
            g_recordedLocalInput[localFrame],
            outPkt.start_frame,
            thisIdx,
            (thisIdx >= 0 && thisIdx < (int32_t)MAX_HISTORY) ? outPkt.inputs[thisIdx] : 0);*/
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

    if (!Netplay_IsActive())
        return;

    if (g_netFrameCount < 0)
        return;

    uint64_t character = *(uint64_t*)(cChrInput + 0x8);
    if (!character)
        return;

    typedef uint32_t(__fastcall* getTeamIdx_t)(uint64_t);
    static getTeamIdx_t fnGetTeamIdx = (getTeamIdx_t)_addr(ADDR_GET_TEAM_INDEX);
    uint32_t teamIdx = fnGetTeamIdx(character);

    // Diagnostic logging during resim: capture state BEFORE override
    if (g_isResimulating) {
        uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
        uint32_t actionId = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
        uint32_t flags14F0 = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
        int32_t f = g_resimulateCurrentFrame;
        uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
        uint32_t head590 = *(uint32_t*)(cChrInput + 0x590);
        uint32_t head594 = *(uint32_t*)(cChrInput + 0x594);
        uint32_t slotIdx = frameCnt & 0x3F; // ring of 64 entries
        uint32_t dir0 = *(uint32_t*)(cChrInput + 0x1B0);
        uint32_t dir1 = *(uint32_t*)(cChrInput + 0x1B4);
        float px = uChar ? *(float*)(uChar + 0x50) : 0.0f;
        float py = uChar ? *(float*)(uChar + 0x54) : 0.0f;
        float pz = uChar ? *(float*)(uChar + 0x58) : 0.0f;
        HookLog("ResimDiag frame=%d team=%d pre: mOld=0x%08X mHeld=0x%08X m18=0x%08X m1C=0x%08X mRemapped=0x%08X mMask=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
                f, teamIdx,
                *(uint32_t*)(cChrInput + 0x10),
                *(uint32_t*)(cChrInput + 0x14),
                *(uint32_t*)(cChrInput + 0x18),
                *(uint32_t*)(cChrInput + 0x1C),
                *(uint32_t*)(cChrInput + 0x560),
                *(uint32_t*)(cChrInput + 0x568),
                *(uint32_t*)(cChrInput + 0x56C),
                frameCnt, head590, head594,
                *(uint64_t*)(cChrInput + 0x28),
                *(uint32_t*)(cChrInput + 0x0A8),
                *(uint64_t*)(cChrInput + 0x0AC),
                *(uint64_t*)(cChrInput + 0x130),
                dir0, dir1,
                *(uint64_t*)(cChrInput + 0x360),
                *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8),
                *(uint64_t*)(cChrInput + 0x368),
                *(uint64_t*)(cChrInput + 0x370),
                actionId, flags14F0, px, py, pz);
    }

    int32_t lookupFrame = g_isResimulating ? g_resimulateCurrentFrame : g_netFrameCount;

    if (Netplay_IsHost())
    {
        if (teamIdx == 0)
        {
            // Host P1 (Local)
            uint32_t delayed;
            if (g_isResimulating)
            {
                delayed = g_recordedLocalInput[(uint32_t)lookupFrame % RING_SIZE];
            // Resim: snapshot restore + oprocessFrameInput's mOldHeld copy at
            // cChrInput+0x10 already provides the correct prev-frame values
            // for +0x10, +0x18, +0x1C, +0x20.  DO NOT override them here —
            // setting all four to the same value destroys the XOR edge
            // detection in the direction processor (FUN_14002E5A0).
            }
            else
            {
                // Live: accumulate and read from delay buffer
                g_localPending |= *(uint32_t*)(cChrInput + 0x14);
                delayed = g_oldInputs[MAX_HISTORY - g_delayFrames];
                // Live: native engine already set aux fields from physical input history
            }
            *(uint32_t*)(cChrInput + 0x14)  = delayed; // mHeld
            *(uint32_t*)(cChrInput + 0x560) = delayed; // mRemapped
            *(uint32_t*)(cChrInput + 0x56C) = delayed; // mProcessed
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask — clear, next processFrameInput re-accumulates
        }
        else if (teamIdx == 1)
        {
            uint32_t remote = g_usedRemoteInput[(uint32_t)lookupFrame % RING_SIZE];
            *(uint32_t*)(cChrInput + 0x14)  = remote; // mHeld
            *(uint32_t*)(cChrInput + 0x560) = remote; // mRemapped
            *(uint32_t*)(cChrInput + 0x56C) = remote; // mProcessed
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask — clear
        }
    }
    else // Client
    {
        if (teamIdx == 0)
        {
            // Client P1 (Remote): read centrally-resolved input
            uint32_t remote = g_usedRemoteInput[(uint32_t)lookupFrame % RING_SIZE];
            *(uint32_t*)(cChrInput + 0x14)  = remote; // mHeld
            *(uint32_t*)(cChrInput + 0x560) = remote; // mRemapped
            *(uint32_t*)(cChrInput + 0x56C) = remote; // mProcessed
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask — clear
            // NOTE: Do NOT override +0x10/+0x18/+0x1C/+0x20 here during resim —
            // snapshot restore + oprocessFrameInput's mOldHeld copy provide
            // correct prev-frame values; overriding breaks XOR edge detection.
        }
        else if (teamIdx == 1)
        {
            uint32_t delayed;
            if (g_isResimulating)
            {
                delayed = g_recordedLocalInput[(uint32_t)lookupFrame % RING_SIZE];
            }
            else
            {
                g_localPending |= *(uint32_t*)(cChrInput + 0x14);
                delayed = g_oldInputs[MAX_HISTORY - g_delayFrames];
            }

            *(uint32_t*)(cChrInput + 0x14)  = delayed; // mHeld
            *(uint32_t*)(cChrInput + 0x560) = delayed; // mRemapped
            *(uint32_t*)(cChrInput + 0x56C) = delayed; // mProcessed
            *(uint32_t*)(cChrInput + 0x568) = 0;        // mMask — clear
        }
    }
    
    // Save post-override state as live reference for future resim comparison
    if (!g_isResimulating && g_netFrameCount >= 0) {
        uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
        uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
        uint32_t liveIdx = (uint32_t)g_netFrameCount % RING_SIZE;
        auto& rec = g_liveRef[teamIdx][liveIdx];
        rec.mHeld       = *(uint32_t*)(cChrInput + 0x14);
        rec.m18         = *(uint32_t*)(cChrInput + 0x18);
        rec.m1C         = *(uint32_t*)(cChrInput + 0x1C);
        rec.mRemapped   = *(uint32_t*)(cChrInput + 0x560);
        rec.mMask       = *(uint32_t*)(cChrInput + 0x568);
        rec.mProcessed  = *(uint32_t*)(cChrInput + 0x56C);
        rec.frameCnt    = frameCnt;
        rec.h590        = *(uint32_t*)(cChrInput + 0x590);
        rec.h594        = *(uint32_t*)(cChrInput + 0x594);
        rec.heldR0      = *(uint64_t*)(cChrInput + 0x28);
        rec.relHead     = *(uint32_t*)(cChrInput + 0x0A8);
        rec.relR0       = *(uint64_t*)(cChrInput + 0x0AC);
        rec.prsR0       = *(uint64_t*)(cChrInput + 0x130);
        rec.dir0        = *(uint32_t*)(cChrInput + 0x1B0);
        rec.dir4        = *(uint32_t*)(cChrInput + 0x1B4);
        rec.dirRing0    = *(uint64_t*)(cChrInput + 0x360);
        rec.dirRingSlot = *(uint64_t*)(cChrInput + 0x360 + (frameCnt & 0x3F) * 8);
        rec.dirRing1    = *(uint64_t*)(cChrInput + 0x368);
        rec.dirRing2    = *(uint64_t*)(cChrInput + 0x370);
        rec.actionId    = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
        rec.flags14F0   = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
        rec.posX        = uChar ? *(float*)(uChar + 0x50) : 0.0f;
        rec.posY        = uChar ? *(float*)(uChar + 0x54) : 0.0f;
        rec.posZ        = uChar ? *(float*)(uChar + 0x58) : 0.0f;
    }

    // Diagnostic logging during resim: capture state AFTER override
    if (g_isResimulating) {
        uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
        uint32_t actionId = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
        uint32_t flags14F0 = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
        int32_t f = g_resimulateCurrentFrame;
        uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
        uint32_t head590 = *(uint32_t*)(cChrInput + 0x590);
        uint32_t head594 = *(uint32_t*)(cChrInput + 0x594);
        uint32_t slotIdx = frameCnt & 0x3F;
        uint32_t dir0_p = *(uint32_t*)(cChrInput + 0x1B0);
        uint32_t dir1_p = *(uint32_t*)(cChrInput + 0x1B4);
        float px = uChar ? *(float*)(uChar + 0x50) : 0.0f;
        float py = uChar ? *(float*)(uChar + 0x54) : 0.0f;
        float pz = uChar ? *(float*)(uChar + 0x58) : 0.0f;
        HookLog("ResimDiag frame=%d team=%d post: mOld=0x%08X mHeld=0x%08X m18=0x%08X m1C=0x%08X mMask=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
                f, teamIdx,
                *(uint32_t*)(cChrInput + 0x10),
                *(uint32_t*)(cChrInput + 0x14),
                *(uint32_t*)(cChrInput + 0x18),
                *(uint32_t*)(cChrInput + 0x1C),
                *(uint32_t*)(cChrInput + 0x568),
                *(uint32_t*)(cChrInput + 0x56C),
                frameCnt, head590, head594,
                *(uint64_t*)(cChrInput + 0x28),
                *(uint32_t*)(cChrInput + 0x0A8),
                *(uint64_t*)(cChrInput + 0x0AC),
                *(uint64_t*)(cChrInput + 0x130),
                dir0_p, dir1_p,
                *(uint64_t*)(cChrInput + 0x360),
                *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8),
                *(uint64_t*)(cChrInput + 0x368),
                *(uint64_t*)(cChrInput + 0x370),
                actionId, flags14F0, px, py, pz);
        // Dump saved live reference for comparison
        uint32_t liveIdx = (uint32_t)f % RING_SIZE;
        auto& lr = g_liveRef[teamIdx][liveIdx];
        HookLog("ResimDiag frame=%d team=%d live: mHeld=0x%08X m18=0x%08X m1C=0x%08X mMask=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
                f, teamIdx,
                lr.mHeld, lr.m18, lr.m1C, lr.mMask, lr.mProcessed,
                lr.frameCnt, lr.h590, lr.h594,
                lr.heldR0, lr.relHead, lr.relR0, lr.prsR0,
                lr.dir0, lr.dir4,
                lr.dirRing0, lr.dirRingSlot, lr.dirRing1, lr.dirRing2,
                lr.actionId, lr.flags14F0,
                lr.posX, lr.posY, lr.posZ);
        // Highlight when resim actionId diverges from the live reference
        if (actionId != lr.actionId) {
            HookLog("ActDiv frame=%d team=%d resim_act=0x%08X live_act=0x%08X mHeld_post=0x%08X mHeld_live=0x%08X fl=0x%08X\n",
                f, teamIdx, actionId, lr.actionId,
                *(uint32_t*)(cChrInput + 0x14), lr.mHeld, flags14F0);
        }
    }

    // Post-resim diagnostic: log the very first live tick after a resim
    // to check whether actionId/state survives the resim→live boundary.
    // Flag is cleared at the next hkMvc3Main_move boundary, so ALL characters
    // in this frame will log.
    if (g_postResimFrame >= 0 && !g_isResimulating)
    {
        uint64_t uChar = *(uint64_t*)(cChrInput + 0x08);
        uint32_t actionId = uChar ? *(uint32_t*)(uChar + 0x1310) : 0xDEAD;
        uint32_t flags14F0 = uChar ? *(uint32_t*)(uChar + 0x14F0) : 0;
        uint64_t frameCnt = *(uint64_t*)(cChrInput + 0x18);
        uint32_t head590 = *(uint32_t*)(cChrInput + 0x590);
        uint32_t head594 = *(uint32_t*)(cChrInput + 0x594);
        uint32_t slotIdx = frameCnt & 0x3F;
        uint32_t dir0_pr = *(uint32_t*)(cChrInput + 0x1B0);
        uint32_t dir1_pr = *(uint32_t*)(cChrInput + 0x1B4);
        float px = uChar ? *(float*)(uChar + 0x50) : 0.0f;
        float py = uChar ? *(float*)(uChar + 0x54) : 0.0f;
        float pz = uChar ? *(float*)(uChar + 0x58) : 0.0f;
        HookLog("PostResim frame=%d team=%d mHeld=0x%08X m18=0x%08X m1C=0x%08X mProcessed=0x%08X cntr=%llu h590=0x%08X h594=0x%08X heldR0=0x%016llX relHead=0x%08X relR0=0x%016llX prsR0=0x%016llX dir[0]=0x%08X dir[4]=0x%08X d[0]=0x%016llX d[slot]=0x%016llX d[1]=0x%016llX d[2]=0x%016llX act=0x%08X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f)\n",
                g_netFrameCount, teamIdx,
                *(uint32_t*)(cChrInput + 0x14),
                *(uint32_t*)(cChrInput + 0x18),
                *(uint32_t*)(cChrInput + 0x1C),
                *(uint32_t*)(cChrInput + 0x56C),
                frameCnt, head590, head594,
                *(uint64_t*)(cChrInput + 0x28),
                *(uint32_t*)(cChrInput + 0x0A8),
                *(uint64_t*)(cChrInput + 0x0AC),
                *(uint64_t*)(cChrInput + 0x130),
                dir0_pr, dir1_pr,
                *(uint64_t*)(cChrInput + 0x360),
                *(uint64_t*)(cChrInput + 0x360 + slotIdx * 8),
                *(uint64_t*)(cChrInput + 0x368),
                *(uint64_t*)(cChrInput + 0x370),
                actionId, flags14F0, px, py, pz);
    }

    // Log non-idle states during both live and resim for debugging
    if (character) {
        uint64_t uc = *(uint64_t*)(cChrInput + 0x08);
        if (uc) {
            uint32_t act = *(uint32_t*)(uc + 0x1310);
            if (act != 0) {
                int32_t curF = g_isResimulating ? g_resimulateCurrentFrame : g_netFrameCount;
                uint64_t fcnt = *(uint64_t*)(cChrInput + 0x18);
                uint32_t slot = fcnt & 0x3F;
                uint32_t cBtn = *(uint16_t*)(cChrInput + 0x22);
                float px = *(float*)(uc + 0x50);
                float py = *(float*)(uc + 0x54);
                float pz = *(float*)(uc + 0x58);
                HookLog("ActDbg frame=%d team=%d act=0x%08X mHeld=0x%08X m18=0x%08X m1C=0x%08X cntr=%llu d[%u]=0x%016llX dir0=0x%08X dir4=0x%08X cBtn=0x%04X fl=0x%08X pos=(%+.4f,%+.4f,%+.4f) %s\n",
                    curF, teamIdx, act,
                    *(uint32_t*)(cChrInput + 0x14),
                    *(uint32_t*)(cChrInput + 0x18),
                    *(uint32_t*)(cChrInput + 0x1C),
                    fcnt, slot,
                    *(uint64_t*)(cChrInput + 0x360 + slot * 8),
                    *(uint32_t*)(cChrInput + 0x1B0),
                    *(uint32_t*)(cChrInput + 0x1B4),
                    cBtn,
                    *(uint32_t*)(uc + 0x14F0),
                    px, py, pz,
                    g_isResimulating ? "RESIM" : "LIVE");
            }
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
