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
extern "C" void Controller_ClearDesyncDetection();
extern "C" void Controller_ResetFrameEpoch();
extern "C" void Controller_OnFrameStart();
extern "C" void Controller_OnFrameEnd();

// Resolve and install the synchronized assist-button opening team order.
extern "C" uint32_t Controller_CaptureOpeningMember();
extern "C" void Controller_SetOpeningMembers(uint32_t team0Member,
                                              uint32_t team1Member);
extern "C" void Controller_ClearOpeningMembers();

// Return the canonical logical input scheduled for the current frame. It is
// stored in the snapshot so resimulation does not depend on live device state.
extern "C" uint32_t Controller_GetCurrentFrameInput();

// Translate a synthetic raw pad mask without the netplay player-index rewrite.
// This matches the path used when autotest input is injected.
extern "C" bool Controller_RemapSyntheticInput(uint32_t rawInput,
                                                uint32_t* outInput);

// Override offline resimulation's recorded game input for a specific frame.
// Netplay consumes canonical local input directly from the snapshot.
extern "C" void Controller_InjectLocalInput(int32_t frame, uint32_t input);

// Override the remote timeline for a specific frame. Netplay's scoped button
// remap hook uses the snapshot first and this ring as its fallback.
extern "C" void Controller_InjectRemoteInput(int32_t frame, uint32_t input);

// Save/restore the local input shift buffer separately, so Rollback_ReSimulate
// can preserve the original timeline for outbound packets.
extern "C" void Controller_SaveInputHistory();
extern "C" void Controller_RestoreInputHistory();

// Centrally resolve remote input for a given frame into g_usedRemoteInput.
extern "C" void Controller_ResolveRemoteInput(int32_t frame);

// Capture the resolved remote input immediately before simulating this frame.
extern "C" void Controller_MarkFrameInputProcessed(int32_t frame);

// Log the inputs used during a re-simmed frame for debugging.
extern "C" void Controller_LogReSimFrame(int32_t frame);

// Check and log whether received remote input exists for each frame in [startFrame, endFrame).
// Prints all results as a single contiguous block before resimulation begins.
// Debug: dump g_oldInputs buffer contents to HookLog
extern "C" void Controller_DumpOldInputs(const char* label);

// Silent frame-boundary helpers for re-sim — maintain the local input shift
// buffer without draining packets or sending outbound data.
extern "C" void Controller_SilentFrameStart();
extern "C" void Controller_SilentFrameEnd();

// Replace the checksum for a historical frame after its corrected re-sim tick.
extern "C" void Controller_RecordFrameChecksum(int32_t frame);
extern "C" bool Controller_GetFrameChecksum(int32_t frame, uint32_t* outChecksum);

// Netplay debug info — read from framestepper draw
extern "C" int32_t  Controller_GetDelayFrames();
extern "C" bool     Controller_IsDelayChangePending();
extern "C" int32_t  Controller_GetNetFrameCount();
extern "C" int32_t  Controller_GetLastRemoteFrame();
extern "C" int32_t  Controller_GetRollbackFrames();
extern "C" bool     Controller_IsRollbackChangePending();
extern "C" bool     Controller_IsRollbackEnabled();
extern "C" bool     Controller_IsPredictionBudgetExhausted();
extern "C" bool     Controller_ShouldShowPredictionBudgetExhausted();

// Frame confirmation check — returns true if remote input has arrived for this frame
extern "C" bool     Controller_IsFrameConfirmed(int32_t frame);

// Executed input accessors — return what the game actually used for each player
extern "C" uint32_t Controller_GetRecordedLocalInput(int32_t frame);
extern "C" uint32_t Controller_GetUsedRemoteInput(int32_t frame);
extern "C" uint32_t Controller_GetReceivedRemoteInput(int32_t frame);

// Team-aware expected input for diagnostic comparison during re-simulation.
// Returns the correct expected input for a given team index and frame,
// accounting for host/client and local/remote role.
extern "C" uint32_t Controller_GetExpectedInput(uint32_t teamIdx, int32_t frame);

// Frame advantage info for overlay display (GGPO-style)
extern "C" int32_t  Controller_GetLocalFrameAdvantage();
extern "C" int32_t  Controller_GetRemoteFrameAdvantage();

// Runtime delay adjustment — called from framestepper UI or netplay inbound
extern "C" void     Controller_SetDelayFrames(int32_t delay);
extern "C" void     Controller_ScheduleDelayFrames(int32_t delay, int32_t effectiveFrame);
extern "C" void     Controller_SetRollbackFrames(int32_t n);
extern "C" void     Controller_ScheduleRollbackFrames(int32_t n, int32_t effectiveFrame);
extern "C" void     Controller_ResetFightTimer(uint64_t sAction);

// Desync detection flags — set by controller when checksums diverge.
extern bool         g_desyncDetected;   // first-time alert (triggers pause)
extern bool         g_desyncSeen;       // user dismissed with P; suppresses re-pause
extern int32_t      g_desyncFrame;
extern uint32_t     g_desyncLocalCS;
extern uint32_t     g_desyncRemoteCS;
extern "C" void     Controller_ClearDesyncDetection();

// Frame advantage (GGPO-style) — frame-count-based timesync
extern int32_t      g_localFrameAdvantage;    // negative = we're ahead of remote
extern int32_t      g_remoteFrameAdvantage;   // received from peer (their local advantage)
