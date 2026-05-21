// controller.cpp
// Decompiled pad/input functions from umvc3.exe, hooked via MinHook.
// Primary purpose: intercept GetsCurrentInput_withSOCD for P2 netplay input injection.
//
// Hook architecture:
//   GetsCurrentInput_withSOCD  — FULLY DECOMPILED, with netplay P2 injection
//   All other hooks             — call through to originals (MinHook trampolines)
#include "../pch.h"
#include "controller.h"
#include "mk10utils.h"
#include "netplay.h"
#include "../kiero/minhook/include/MinHook.h"
#include <cstdint>
#include <cstring>

extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// External netplay queries
// ---------------------------------------------------------------------------
extern bool     Netplay_HasRemoteInput();
extern uint32_t Netplay_GetRemoteInput();

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
static constexpr uintptr_t ADDR_GET_CURRENT_INPUT     = 0x1402B3700;
static constexpr uintptr_t ADDR_PAD_SLOT_READ_DEVICE  = 0x140514B90;
static constexpr uintptr_t ADDR_PAD_POLL_ALL_SLOTS    = 0x140514570;
static constexpr uintptr_t ADDR_XINPUT_TO_GAME_MASK   = 0x1405111B0;
static constexpr uintptr_t ADDR_PAD_SLOT_POST_READ    = 0x1405146A0;
static constexpr uintptr_t ADDR_INPUT_AUTOREPEAT      = 0x140514F20;
static constexpr uintptr_t ADDR_PAD_GET_VIB_MOTOR     = 0x140510620;
static constexpr uintptr_t ADDR_PAD_VIB_PROCESS       = 0x140514D90;
static constexpr uintptr_t ADDR_PAD_CHECK_STOP_VIB    = 0x140514ED0;
static constexpr uintptr_t ADDR_KEYBOARD_TO_GAME_MASK = 0x1402B2FA0;
static constexpr uintptr_t ADDR_PAD_GET_INSTANCE      = 0x140001B00;
static constexpr uintptr_t ADDR_MVC3_MGR_GET_INSTANCE = 0x140001AF0;
static constexpr uintptr_t ADDR_MGR_GET_PLAYER_CHAR   = 0x14025AC40;
static constexpr uintptr_t ADDR_METHOD_3D00           = 0x1402B3D00;

// ---------------------------------------------------------------------------
// Original function pointers (MinHook trampolines)
// ---------------------------------------------------------------------------
static GetsCurrentInput_withSOCD_t oGetsCurrentInput_withSOCD = nullptr;
static padSlotReadDevice_t         opadSlotReadDevice         = nullptr;
static padPollAllSlots_t           opadPollAllSlots           = nullptr;
static xinputToGameMask_t          oxinputToGameMask          = nullptr;
static padSlotPostReadProcess_t    opadSlotPostReadProcess    = nullptr;
static inputAutoRepeatTimerProcess_t oinputAutoRepeatTimerProcess = nullptr;

// ===========================================================================
// Decompiled helper functions (inlined from game)
// ===========================================================================
namespace ControllerDecomp {

// ---------------------------------------------------------------------------
// keyboardToGameMask (0x1402B2FA0)
// Maps VK key states → game input bitmask. Called when keyboard is the
// active device for a player slot.
// ---------------------------------------------------------------------------
static uint32_t keyboardToGameMask(uint64_t sPad)
{
    typedef uint64_t(__fastcall* method_3D00_t)(uint64_t, uint32_t, uint32_t);
    static method_3D00_t fn3D00 = (method_3D00_t)_addr(ADDR_METHOD_3D00);

    uint32_t r = 0;

    if (fn3D00(sPad, 0x28, 0)) r |= IN_DOWN;
    if (fn3D00(sPad, 0x26, 0)) r |= IN_UP;
    if (fn3D00(sPad, 0x25, 0)) r |= IN_LEFT;
    if (fn3D00(sPad, 0x27, 0)) r |= IN_RIGHT;

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

    if (fn3D00(sPad, 0x58, 0)) r |= IN_X;       // X
    if (fn3D00(sPad, 0x59, 0)) r |= IN_Y;       // Y
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

// ---------------------------------------------------------------------------
// GetsCurrentInput_withSOCD (0x1402B3700)
// Per-player pad read with SOCD cleaning.
// This is the KEY function for netplay P2 input injection.
// ---------------------------------------------------------------------------
uint32_t GetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex)
{
    if (playerIndex > 3)
        return 0;

    auto getMgr = (uint64_t(__fastcall*)())_addr(ADDR_MVC3_MGR_GET_INSTANCE);
    auto getCharIdx = (uint32_t(__fastcall*)(uint64_t, uint32_t))_addr(ADDR_MGR_GET_PLAYER_CHAR);

    uint64_t manager = getMgr();
    uint32_t kbPlayer = getCharIdx(manager, 0);

    // Keyboard path: if this player matches the keyboard-assigned player
    // AND keyboard isn't disabled (pad+0xC30 == 0)
    if (kbPlayer == playerIndex && *(uint8_t*)(pad + PAD_KEYBOARD_DISABLE) == 0)
    {
        return keyboardToGameMask(pad);
    }

    // Pad slot read (slot = pad + playerIndex * 0x2E0 + 0x1BC)
    uint32_t input = *(uint32_t*)(pad + (uint64_t)playerIndex * PAD_SLOT_STRIDE + PAD_SLOT_INPUT);

    // SOCD cleaning — faithful reproduction of game's original bit ops
    // D-Pad Left(0x80) + Right(0x20) -> clear Right
    if ((input & 0x80020) == 0x80020) input &= 0xFFFFFFDF;
    // D-Pad Left(0x80) + Stick Right(0x20000) -> clear Stick Right
    if ((input & 0x20080) == 0x20080)   input &= 0xFFFDFFFF;
    // D-Pad Left(0x80) + Right(0x20) low-byte only -> clear Right
    if (((uint8_t)input & 0xA0) == 0xA0) input &= 0xFFFFFFDF;
    // Stick Left(0x80000) + Stick Right(0x20000) -> clear Stick Right
    if ((input & 0xA0000) == 0xA0000)   input &= 0xFFFDFFFF;
    // D-Pad Up(0x10) + Stick Down(0x40000) -> clear Stick Down
    if ((input & 0x40010) == 0x40010)   input &= 0xFFFBFFFF;
    // Stick Up(0x10000) + D-Pad Down(0x40) -> clear D-Pad Down
    if ((input & 0x10040) == 0x10040)   input &= 0xFFFFFFBF;
    // D-Pad Up(0x10) + Down(0x40) low-byte -> clear Down
    if (((uint8_t)input & 0x50) == 0x50) input &= 0xFFFFFFBF;
    // Stick Up(0x10000) + Stick Down(0x40000) -> clear Stick Down
    if ((input & 0x50000) == 0x50000)   input &= 0xFFFBFFFF;

    return input;
}

} // namespace ControllerDecomp

// ===========================================================================
// Hooked functions
// ===========================================================================

// ---------------------------------------------------------------------------
// hkGetsCurrentInput_withSOCD
// Hook on GetsCurrentInput_withSOCD at 0x1402B3700.
// - For P2 with netplay remote input available, injects remote input
// - All other cases: call through to original via MinHook trampoline
// - Decompiled reference implementation kept in ControllerDecomp namespace
// ---------------------------------------------------------------------------
static uint64_t __fastcall hkGetsCurrentInput_withSOCD(uint64_t pad, uint32_t playerIndex)
{
    // Netplay: intercept P2 (playerIndex == 1) with remote input
    if (playerIndex == 1 && Netplay_HasRemoteInput())
    {
        uint32_t remote = Netplay_GetRemoteInput();
        // Still apply SOCD cleaning to remote input
        if ((remote & 0x80020) == 0x80020) remote &= 0xFFFFFFDF;
        if ((remote & 0x20080) == 0x20080) remote &= 0xFFFDFFFF;
        if (((uint8_t)remote & 0xA0) == 0xA0) remote &= 0xFFFFFFDF;
        if ((remote & 0xA0000) == 0xA0000) remote &= 0xFFFDFFFF;
        if ((remote & 0x40010) == 0x40010) remote &= 0xFFFBFFFF;
        if ((remote & 0x10040) == 0x10040) remote &= 0xFFFFFFBF;
        if (((uint8_t)remote & 0x50) == 0x50) remote &= 0xFFFFFFBF;
        if ((remote & 0x50000) == 0x50000) remote &= 0xFFFBFFFF;
        return remote;
    }

    // Use original game code via trampoline for reliability
    return oGetsCurrentInput_withSOCD(pad, playerIndex);
}

// ---------------------------------------------------------------------------
// All other hooks: call through to original trampolines.
// These exist so we have injection points for future use.
// ---------------------------------------------------------------------------
static uint64_t __fastcall hkpadSlotReadDevice(uint64_t slotBase)
{
    return opadSlotReadDevice(slotBase);
}

static void __fastcall hkpadPollAllSlots(uint64_t sPad)
{
    opadPollAllSlots(sPad);
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

    // padSlotReadDevice (future DirectInput hook point)
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_PAD_SLOT_READ_DEVICE),
        (LPVOID)&hkpadSlotReadDevice,
        (LPVOID*)&opadSlotReadDevice
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_PAD_SLOT_READ_DEVICE));
    else
        HookLog("Controller: Failed to hook padSlotReadDevice (%d)\n", s);

    // padPollAllSlots (future hook point)
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_PAD_POLL_ALL_SLOTS),
        (LPVOID)&hkpadPollAllSlots,
        (LPVOID*)&opadPollAllSlots
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_PAD_POLL_ALL_SLOTS));
    else
        HookLog("Controller: Failed to hook padPollAllSlots (%d)\n", s);

    // xinputToGameMask (future XInput hook point)
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_XINPUT_TO_GAME_MASK),
        (LPVOID)&hkxinputToGameMask,
        (LPVOID*)&oxinputToGameMask
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_XINPUT_TO_GAME_MASK));
    else
        HookLog("Controller: Failed to hook xinputToGameMask (%d)\n", s);

    // padSlotPostReadProcess (future hook point)
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_PAD_SLOT_POST_READ),
        (LPVOID)&hkpadSlotPostReadProcess,
        (LPVOID*)&opadSlotPostReadProcess
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_PAD_SLOT_POST_READ));
    else
        HookLog("Controller: Failed to hook padSlotPostReadProcess (%d)\n", s);

    // inputAutoRepeatTimerProcess (future hook point)
    s = MH_CreateHook(
        (LPVOID)_addr(ADDR_INPUT_AUTOREPEAT),
        (LPVOID)&hkinputAutoRepeatTimerProcess,
        (LPVOID*)&oinputAutoRepeatTimerProcess
    );
    if (s == MH_OK)
        MH_EnableHook((LPVOID)_addr(ADDR_INPUT_AUTOREPEAT));
    else
        HookLog("Controller: Failed to hook inputAutoRepeatTimerProcess (%d)\n", s);

    HookLog("Controller: Init complete — %d hooks installed\n", 6);
}
