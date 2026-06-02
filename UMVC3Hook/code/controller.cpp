// controller.cpp
// Decompiled pad/input functions from umvc3.exe, hooked via MinHook.
//
// GGPO netplay integration:
//   Local physical input is accumulated per-character in hkprocessFrameInput.
//   At each frame boundary, GGPO provides synchronized inputs via
//   g_ggpoSyncInputs[]. The local player reads from this array (with frame
//   delay applied by GGPO's local input caching), and the remote player reads
//   the network-provided value.  During rollback re-simulation the same
//   GGPO_SyncInputs() call returns the corrected historical inputs.
#include "../pch.h"
#include "controller.h"
#include "mk10utils.h"
#include "ggpo_netplay.h"
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
static constexpr uintptr_t ADDR_PROCESS_FRAME_INPUT       = 0x14002DD60;
static constexpr uintptr_t ADDR_GET_TEAM_INDEX            = 0x140004AC0;
static constexpr uintptr_t ADDR_STEP_INTRO                 = 0x140007D90;
static constexpr uintptr_t ADDR_METHOD_3D00              = 0x1402B3D00;

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

    // VK_RETURN and VK_A -> unit flags (Start/Select in menus)
    if (fn3D00(sPad, 0x0D, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A70))(sPad);
    if (fn3D00(sPad, 0x41, 0))
        r |= (uint32_t)((uint64_t(__fastcall*)(uint64_t))_addr(0x140001A70))(sPad);

    // VK_BACK and VK_B -> UI anm flags
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
// Controller state save/restore for rollback snapshots
// ===========================================================================
static uint32_t g_savedLocalPending = 0;
static uint32_t g_localPending = 0;

extern "C" void Controller_ResetBuffers()
{
    g_localPending = 0;
    g_savedLocalPending = 0;
}

extern "C" void Controller_SaveRollbackState()
{
    g_savedLocalPending = g_localPending;
}

extern "C" void Controller_RestoreRollbackState()
{
    g_localPending = g_savedLocalPending;
}

// ---------------------------------------------------------------------------
// Debug accessors
// ---------------------------------------------------------------------------
extern "C" int32_t Controller_GetNetFrameCount()
{
    return GetGameFrameCount();
}

// ===========================================================================
// Hooked functions
// ===========================================================================

// ---------------------------------------------------------------------------
// hkGetsCurrentInput_withSOCD
// Hook on GetsCurrentInput_withSOCD at 0x1402B3700.
// Forward declaration needed for Controller_ReadPhysicalInput.
// ---------------------------------------------------------------------------
static uint64_t __fastcall hkGetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex);

// Controller_ReadPhysicalInput — bypasses the GGPO hook to read physical pad
// input directly, used by GGPO's local input capture.
uint32_t Controller_ReadPhysicalInput(uint64_t pad, uint32_t playerIndex)
{
    return (uint32_t)hkGetsCurrentInput_withSOCD(pad, playerIndex);
}

static uint64_t __fastcall hkGetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex)
{
    if (g_ggpoRunning)
    {
        if (GGPO_IsHost())
        {
            // Host P1 is local physical. Host P2 is remote. Return 0 raw input for P2
            // because we inject the remote GGPO-synchronized bits directly in hkprocessFrameInput.
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
    if (g_ggpoRunning) {
        // Same as training mode skip: jump directly to countdown
        auto setCountdown = (void(__fastcall*)(uint64_t, int))_addr(0x14000AE00);
        setCountdown(param_1, 3);
        return;
    }
    oStepIntro(param_1);
}

// ===========================================================================
// hkprocessFrameInput
// Hook on processFrameInput at 0x14002DD60.
//
// With GGPO, the synchronized inputs are in g_ggpoSyncInputs[], populated
// by the GGPO_SyncInputs() call.  During normal play:
//   Local player: reads g_ggpoSyncInputs[localIndex] (GGPO applies frame delay)
//   Remote player: reads g_ggpoSyncInputs[remoteIndex] (network input)
// During re-simulation (advance_frame callback):
//   Same flow — GGPO returns the corrected historical inputs.
// ===========================================================================

static void __fastcall hkprocessFrameInput(uint64_t cChrInput)
{
    // 1. Run native translation first so cChrInput fields (+0x14, etc.) populate normally.
    oprocessFrameInput(cChrInput);

    if (!g_ggpoRunning)
        return;

    int32_t frameCount = GetGameFrameCount();
    if (frameCount < 0)
        return;

    uint64_t character = *(uint64_t*)(cChrInput + 0x8);
    if (!character)
        return;

    typedef uint32_t(__fastcall* getTeamIdx_t)(uint64_t);
    static getTeamIdx_t fnGetTeamIdx = (getTeamIdx_t)_addr(ADDR_GET_TEAM_INDEX);
    uint32_t teamIdx = fnGetTeamIdx(character);

    uint32_t syncInput;
    if (GGPO_IsHost())
    {
        syncInput = (teamIdx == 0) ? g_ggpoSyncInputs[0] : g_ggpoSyncInputs[1];
    }
    else
    {
        syncInput = (teamIdx == 0) ? g_ggpoSyncInputs[0] : g_ggpoSyncInputs[1];
    }

    uint32_t local = *(uint32_t*)(cChrInput + 0x14);
    if (teamIdx == (GGPO_IsHost() ? 0 : 1))
        g_localPending |= local;

    *(uint32_t*)(cChrInput + 0x14)  = syncInput;
    *(uint32_t*)(cChrInput + 0x560) = syncInput;
    *(uint32_t*)(cChrInput + 0x56C) = syncInput;
}

// ===========================================================================
// Offline Mirroring & Standard Hook Implementations
// ===========================================================================

static void __fastcall hkpadPollAllSlots(uint64_t sPad)
{
    opadPollAllSlots(sPad);
    if (g_ggpoRunning)
        return;

    // Copy P1's processed pad input -> P2's pad slot input area.
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

    // GetsCurrentInput_withSOCD — primary hook for GGPO input routing
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

    // cChrInput__processFrameInput — GGPO input synchronization
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

    HookLog("Controller: Init complete -- hooks installed\n");
}
