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

// Frame counter — read from the game's own sReplay singleton each frame.
// g_psReplay pointer   = *(uint64_t*)_addr(0x140d4d9d8)  (Ghidra confirmation)
// Frame counter uint32 = *(sReplay + 0x54)                (INC dword [RCX+0x54] at 0x1401cefdd)
static int32_t   g_netFrameCount   = -1;
static int32_t   g_delayFrames     = 4;
static int32_t   g_rollbackFrames  = 4;
static uint32_t  g_localPending    = 0;   // accumulator, ORed across team chars

// Tracks the latest frame for which remote input has been received.
// Used to hold-last-value when no new input has arrived for the current frame.
static int32_t   g_lastRemoteFrame = -1;
static uint32_t  g_lastRemoteInput = 0;

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
    // g_netFrameCount is read from sReplay each frame; no reset needed.
    g_localPending    = 0;
    g_lastRemoteFrame = -1;
    g_lastRemoteInput = 0;
}

// ---------------------------------------------------------------------------
// Debug accessors — called from framestepper draw
// ---------------------------------------------------------------------------
extern "C" int32_t Controller_GetDelayFrames()      { return g_delayFrames; }
extern "C" int32_t Controller_GetNetFrameCount()    { return GetGameFrameCount(); }
extern "C" int32_t Controller_GetLastRemoteFrame()  { return g_lastRemoteFrame; }
extern "C" int32_t Controller_GetRollbackFrames()   { return g_rollbackFrames; }
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
    if (Netplay_IsActive())
    {
        if (Netplay_IsHost())
        {
            // Host P1 is local physical. Host P2 is remote. Return 0 raw input for P2
            // because we inject the remote character-processed bits directly in hkprocessFrameInput.
            if (playerIndex == 1)
            {
                return 0;
            }
        }
        else
        {
            // Client P1 is remote. Return 0 raw input for P1 to prevent raw bit contamination.
            if (playerIndex == 0)
            {
                return 0;
            }
            // Client P2 is local physical. Route physical slot 0 combined with keyboard.
            else if (playerIndex == 1)
            {
                uint32_t padInput = (uint32_t)oGetsCurrentInput_withSOCD(pad, 0);
                uint32_t kbInput  = ControllerDecomp::keyboardToGameMask(pad);
                return ApplySOCD(padInput | kbInput);
            }
        }
    }

    if (playerIndex == 1)
        return oGetsCurrentInput_withSOCD(pad, 1);

    return oGetsCurrentInput_withSOCD(pad, playerIndex);
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
                if (targetFrame > g_lastRemoteFrame)
                {
                    g_lastRemoteFrame = targetFrame;
                    g_lastRemoteInput = inPkt.inputs[i];
                }
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
                    }
                }
            }

            // --- Centrally resolve this frame's remote input so hkprocessFrameInput
            //     just reads from g_usedRemoteInput (no per-character resolution logic).
            Controller_ResolveRemoteInput(g_netFrameCount);

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
}

extern "C" void Controller_OnFrameEnd()
{
    if (!Netplay_IsActive())
        return;

    // Don't send anything on frame 0 — let the first frame be neutral so both
    // sides have time to establish the connection and fill initial delay slots.
    if (g_netFrameCount < 1)
        return;

    // --- 1. Read delayed local input BEFORE shifting (unused here; the
    //         actual read that overwrites the character fields happens in
    //         hkprocessFrameInput which also reads from g_oldInputs before
    //         this shift).

    // --- 2. Shift history buffer: discard oldest, append current.
    memmove(&g_oldInputs[0], &g_oldInputs[1],
            (MAX_HISTORY - 1) * sizeof(uint32_t));
    g_oldInputs[MAX_HISTORY - 1] = g_localPending;

    // --- 3. Build outbound packet — inputs[] are absolute-frame-indexed.
    //     start_frame = (currentFrame + delay) - MAX_HISTORY + 1
    //     inputs[0]                    → frame start_frame          (oldest)
    //     inputs[MAX_HISTORY-1]        → frame start_frame+MAX_HISTORY-1 (newest)
    InputPacket outPkt = {};
    outPkt.start_frame = (uint32_t)(g_netFrameCount + g_delayFrames
                                     - MAX_HISTORY + 1);
    for (size_t i = 0; i < MAX_HISTORY; i++)
        outPkt.inputs[i] = g_oldInputs[i];

    Netplay_SendPacket(outPkt);

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

    int32_t lookupFrame = g_isResimulating ? g_resimulateCurrentFrame : g_netFrameCount;

    if (Netplay_IsHost())
    {
        if (teamIdx == 0)
        {
            // Host P1 (Local)
            uint32_t delayed;
            if (g_isResimulating)
            {
                // Replay the exact input that was recorded during live playback,
                // preventing real-time physical input from polluting the re-sim.
                delayed = g_recordedLocalInput[(uint32_t)lookupFrame % RING_SIZE];
            }
            else
            {
                // Live: accumulate, read from delay buffer, and record
                g_localPending |= *(uint32_t*)(cChrInput + 0x14);
                delayed = g_oldInputs[MAX_HISTORY - g_delayFrames];
                g_recordedLocalInput[(uint32_t)lookupFrame % RING_SIZE] = delayed;
            }
            *(uint32_t*)(cChrInput + 0x14)  = delayed;
            *(uint32_t*)(cChrInput + 0x560) = delayed;
            *(uint32_t*)(cChrInput + 0x56C) = delayed;
        }
        else if (teamIdx == 1)
        {
            // Host P2 (Remote): read centrally-resolved input
            uint32_t remote = g_usedRemoteInput[(uint32_t)lookupFrame % RING_SIZE];

            *(uint32_t*)(cChrInput + 0x14)  = remote;
            *(uint32_t*)(cChrInput + 0x560) = remote;
            *(uint32_t*)(cChrInput + 0x56C) = remote;
        }
    }
    else // Client
    {
        if (teamIdx == 0)
        {
            // Client P1 (Remote): read centrally-resolved input
            uint32_t remote = g_usedRemoteInput[(uint32_t)lookupFrame % RING_SIZE];

            *(uint32_t*)(cChrInput + 0x14)  = remote;
            *(uint32_t*)(cChrInput + 0x560) = remote;
            *(uint32_t*)(cChrInput + 0x56C) = remote;
        }
        else if (teamIdx == 1)
        {
            // Client P2 (Local)
            uint32_t delayed;
            if (g_isResimulating)
            {
                delayed = g_recordedLocalInput[(uint32_t)lookupFrame % RING_SIZE];
            }
            else
            {
                g_localPending |= *(uint32_t*)(cChrInput + 0x14);
                delayed = g_oldInputs[MAX_HISTORY - g_delayFrames];
                g_recordedLocalInput[(uint32_t)lookupFrame % RING_SIZE] = delayed;
            }
            *(uint32_t*)(cChrInput + 0x14)  = delayed;
            *(uint32_t*)(cChrInput + 0x560) = delayed;
            *(uint32_t*)(cChrInput + 0x56C) = delayed;
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
