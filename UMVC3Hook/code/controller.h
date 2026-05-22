#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Input bit definitions (game's internal bitmask layout)
// ---------------------------------------------------------------------------
static constexpr uint32_t IN_UP       = 0x0010;
static constexpr uint32_t IN_DOWN     = 0x0040;
static constexpr uint32_t IN_LEFT     = 0x0080;
static constexpr uint32_t IN_RIGHT    = 0x0020;
static constexpr uint32_t IN_LB       = 0x0100;
static constexpr uint32_t IN_RB       = 0x0200;
static constexpr uint32_t IN_LT       = 0x0400;
static constexpr uint32_t IN_RT       = 0x0800;
static constexpr uint32_t IN_Y        = 0x1000;
static constexpr uint32_t IN_B        = 0x2000;
static constexpr uint32_t IN_A        = 0x4000;
static constexpr uint32_t IN_X        = 0x8000;
static constexpr uint32_t IN_BACK     = 0x0001;
static constexpr uint32_t IN_L3       = 0x0002;
static constexpr uint32_t IN_R3       = 0x0004;
static constexpr uint32_t IN_START    = 0x0008;
static constexpr uint32_t IN_STICK_UP    = 0x10000;
static constexpr uint32_t IN_STICK_DOWN  = 0x40000;
static constexpr uint32_t IN_STICK_LEFT  = 0x80000;
static constexpr uint32_t IN_STICK_RIGHT = 0x20000;

// ---------------------------------------------------------------------------
// Pad structure constants
// ---------------------------------------------------------------------------
static constexpr uint32_t PAD_SLOT_STRIDE = 0x2E0;
static constexpr uint32_t PAD_SLOT_INPUT  = 0x1BC;
static constexpr uint32_t PAD_MAX_SLOTS   = 4;

// Pad global offsets
static constexpr uint32_t PAD_GLOBAL_VIB_RATE    = 0x48;
static constexpr uint32_t PAD_GLOBAL_FRAME_TIME  = 0x40;
static constexpr uint32_t PAD_KEYBOARD_DISABLE   = 0xC30;
static constexpr uint32_t PAD_KB_MAPPED_PLAYER   = 0xBE2;

// Per-slot offsets (slotBase = pad + slotIdx * 0x2E0)
static constexpr uint32_t SLOT_RAW_XINPUT  = 0x164; // raw input bits from XInput conversion
static constexpr uint32_t SLOT_PREV_XINPUT = 0x168; // previous frame's raw bits
static constexpr uint32_t SLOT_RISING      = 0x16C; // newly pressed this frame
static constexpr uint32_t SLOT_FALLING     = 0x170; // released this frame
static constexpr uint32_t SLOT_DELTA       = 0x174; // XOR of prev and current
static constexpr uint32_t SLOT_AUTOREPEAT  = 0x178; // auto-repeat active flags

// ---------------------------------------------------------------------------
// XINPUT_STATE layout (pGamepad param points into this)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct XInputState {
    uint32_t dwPacketNumber; // +0x00
    uint16_t wButtons;       // +0x04
    uint8_t  bLeftTrigger;   // +0x06
    uint8_t  bRightTrigger;  // +0x07
    int16_t  sThumbLX;       // +0x08
    int16_t  sThumbLY;       // +0x0A
    int16_t  sThumbRX;       // +0x0C
    int16_t  sThumbRY;       // +0x0E
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Hook function type definitions
// ---------------------------------------------------------------------------
typedef uint64_t(__fastcall* GetsCurrentInput_withSOCD_t)(uint64_t pad, uint32_t playerIndex);
typedef uint64_t(__fastcall* padSlotReadDevice_t)(uint64_t slotBase);
typedef void(__fastcall* padPollAllSlots_t)(uint64_t sPad);
typedef void(__fastcall* xinputToGameMask_t)(uint64_t sPad, uint64_t slotBase, const XInputState* pState);
typedef void(__fastcall* padSlotPostReadProcess_t)(uint64_t sPad, uint64_t slotBase, uint32_t slotIdx);
typedef void(__fastcall* inputAutoRepeatTimerProcess_t)(uint64_t slotBase);

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------
void Controller_InitHooks();

// Netplay ring buffer lifecycle — called from framestepper.cpp / netplay.cpp
extern "C" void Controller_ResetBuffers();
extern "C" void Controller_OnFrameStart();
extern "C" void Controller_OnFrameEnd();

// Netplay debug info — read from framestepper draw
extern "C" int32_t  Controller_GetDelayFrames();
extern "C" int32_t  Controller_GetNetFrameCount();
extern "C" int32_t  Controller_GetLastRemoteFrame();
