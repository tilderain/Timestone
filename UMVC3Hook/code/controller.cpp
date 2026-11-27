// Native input hooks and frame-indexed netplay input scheduling.
// Exchange button-remapped input before character-specific processing;
// predict missing remote input and replay frames when confirmation differs.
#include "../pch.h"
#include "controller.h"
#include "escmenu_imgui.h"
#include "lobby_flow.h"
#include "lag_diagnostics.h"
#include "mk10utils.h"
#include "netplay.h"
#include "rollback.h"
#include "rollback_diagnostics.h"
#include "rollback_profile.h"
#include "autotest.h"
#include "framestepper.h"
#include "gameplay_hooks.h"
#include "spectate.h"
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <dinput.h>
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
static int32_t   g_rollbackFrames  = kNetplayRollbackFrames;
static int32_t   g_pendingRollbackFrames = -1;
static int32_t   g_pendingRollbackEffectiveFrame = -1;

static bool IsFrameConfirmed(int32_t frame)
{
    if (frame < 0) return false;
    // A spectator only ever simulates frames whose inputs are final.
    if (Spectate_IsViewing()) return true;
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
// Last frame the intro ran on in this game, -1 before it (see IsIntroQuietFrame).
static int32_t   g_introLastFrame = -1;

// Shared logical frame, read from the native sReplay cursor.
static int32_t   g_netFrameCount   = -1;
static uint32_t  g_localPending    = 0;   // accumulator, ORed across team chars
static bool g_preRenderInputReady = false;

// Newest received remote input; fallback when prediction finds no earlier confirmation.
static int32_t   g_lastRemoteFrame = -1;
static uint32_t  g_lastRemoteInput = 0;
static int32_t   g_lastNonMultiCheckedFrame = -1;

bool g_multiResimEnabled = true; // Multi-resimulation toggle

// ---------------------------------------------------------------------------
// Per-frame character checksums and desync status.
// ---------------------------------------------------------------------------
bool     g_desyncDetected     = false;
bool     g_desyncSeen         = false;
int32_t  g_desyncFrame        = -1;
uint32_t g_desyncLocalCS      = 0;
uint32_t g_desyncRemoteCS     = 0;

static uint32_t  g_localChecksum[RING_SIZE];
static uint32_t  g_localChecksumParts[RING_SIZE][4];
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
// Time sync: keeps the two games at the same frame at the same moment.
//
// A game that runs ahead of the other receives every remote input late and
// rolls back for it, while the game behind receives input early and never
// rolls back: the one-sided rollback. Nothing brings the two back together by
// itself; every hitch one side has stays as an offset.
//
// Each input packet carries the sender's frame clock at the instant it left
// (frame + fraction, netplay.cpp). On arrival the receiver compares it with
// its own clock at that instant; smoothed, that is its "lead": by how much
// its clock is past the frames it is being sent. The lead also holds the
// network delay, which neither side can tell from a clock offset, but both
// sides see that same delay. So each side sends its lead to the other, and
// half the difference of the two is the frame advantage: the clock offset,
// with the network delay (and any error in measuring it) cancelled out. It
// is zero exactly when remote input reaches both games equally late, which
// is when both roll back equally.
//
// The side that is ahead then lengthens its frames by a few percent at most
// (rollback_hooks.cpp, hkMainMove) until the advantage is gone. No frame is
// dropped or held, so there is nothing to see or feel.
// ---------------------------------------------------------------------------
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" bool FrameStepper_IsMatchRestartPending();

static constexpr float TIMESYNC_SMOOTHING   = 1.0f / 32.0f; // per packet (two a frame)
static constexpr float TIMESYNC_MAX_STEP    = 1.5f;   // frames one late packet may count for
static constexpr int   TIMESYNC_MIN_SAMPLES = 30;
static constexpr float TIMESYNC_DEADBAND    = 0.25f;  // frames of advantage left alone
static constexpr float TIMESYNC_GAIN        = 0.012f; // stretch per frame of advantage
static constexpr float TIMESYNC_MAX_STRETCH = 0.05f;  // 5%: 0.8 ms on a frame
static constexpr uint64_t TIMESYNC_STALE_MS = 500;    // no packets: no correction

static int64_t  g_frameStartQpc = 0;     // when g_netFrameCount began
static int64_t  g_qpcFrequency = 0;
static float    g_localLead = 0.0f;      // smoothed, frames
static int      g_localLeadSamples = 0;
static float    g_remoteLead = 0.0f;     // the peer's own, from its packets
static bool     g_remoteLeadKnown = false;
static uint64_t g_lastLeadTick = 0;
// Read by the render thread (performance window) and by pacing.
static std::atomic<float> g_frameAdvantage{0.0f};
static std::atomic<bool>  g_frameAdvantageKnown{false};
static std::atomic<float> g_frameStretch{0.0f};

static void ResetTimeSync()
{
    g_localLead = 0.0f;
    g_localLeadSamples = 0;
    g_remoteLead = 0.0f;
    g_remoteLeadKnown = false;
    g_lastLeadTick = 0;
    g_frameAdvantage.store(0.0f, std::memory_order_relaxed);
    g_frameAdvantageKnown.store(false, std::memory_order_relaxed);
    g_frameStretch.store(0.0f, std::memory_order_relaxed);
}

// One arriving input packet: the sender's clock against ours.
static void SampleTimeSync(const InputPacket& packet, int64_t arrivalQpc)
{
    if (!(packet.flags & INPUT_FLAG_CLOCK) || g_netFrameCount < 0 || !g_qpcFrequency ||
        !g_frameStartQpc)
        return;
    // The sender's frame, from its low bits, as the one nearest to ours.
    const uint32_t frameMask = (1u << INPUT_CLOCK_FRAME_BITS) - 1;
    int32_t frameDelta = static_cast<int32_t>(
        ((packet.flags >> INPUT_CLOCK_FRAME_SHIFT) - static_cast<uint32_t>(g_netFrameCount)) &
        frameMask);
    if (frameDelta & (1 << (INPUT_CLOCK_FRAME_BITS - 1))) frameDelta -= 1 << INPUT_CLOCK_FRAME_BITS;
    const float remotePhase =
        static_cast<float>((packet.flags >> INPUT_CLOCK_PHASE_SHIFT) & 0x7F) /
        static_cast<float>(INPUT_CLOCK_PHASE_ONE);
    // Our clock when it arrived. It may have waited since before this frame
    // began; and a game held up for input is no further than its frame's end.
    float localPhase = static_cast<float>(
        static_cast<double>(arrivalQpc - g_frameStartQpc) * 60.0 /
        static_cast<double>(g_qpcFrequency));
    if (localPhase < -4.0f) localPhase = -4.0f;
    if (localPhase > 1.0f) localPhase = 1.0f;

    const float sample = localPhase - static_cast<float>(frameDelta) - remotePhase;
    if (g_localLeadSamples < TIMESYNC_MIN_SAMPLES) {
        // Plain average until there is enough to smooth.
        g_localLead += (sample - g_localLead) / static_cast<float>(g_localLeadSamples + 1);
    } else {
        // A packet the network held back says "ahead" by as much as it was
        // held: let no single one move the estimate far.
        float step = sample - g_localLead;
        if (step > TIMESYNC_MAX_STEP) step = TIMESYNC_MAX_STEP;
        if (step < -TIMESYNC_MAX_STEP) step = -TIMESYNC_MAX_STEP;
        g_localLead += step * TIMESYNC_SMOOTHING;
    }
    if (g_localLeadSamples < TIMESYNC_MIN_SAMPLES) ++g_localLeadSamples;
    g_remoteLead = static_cast<float>(packet.frame_advantage) / 256.0f;
    g_remoteLeadKnown = true;
    g_lastLeadTick = GetTickCount64();
}

// Once a frame: the advantage, and the stretch that answers it.
static void UpdateTimeSync()
{
    const bool fresh = g_lastLeadTick && GetTickCount64() - g_lastLeadTick < TIMESYNC_STALE_MS;
    if (!fresh || !g_remoteLeadKnown || g_localLeadSamples < TIMESYNC_MIN_SAMPLES ||
        !FrameStepper_IsBattleSynchronized() || FrameStepper_IsMatchRestartPending()) {
        g_frameStretch.store(0.0f, std::memory_order_relaxed);
        if (!fresh) g_frameAdvantageKnown.store(false, std::memory_order_relaxed);
        return;
    }
    const float advantage = (g_localLead - g_remoteLead) * 0.5f;
    float stretch = (advantage - TIMESYNC_DEADBAND) * TIMESYNC_GAIN;
    if (stretch < 0.0f) stretch = 0.0f;
    if (stretch > TIMESYNC_MAX_STRETCH) stretch = TIMESYNC_MAX_STRETCH;
    g_frameAdvantage.store(advantage, std::memory_order_relaxed);
    g_frameAdvantageKnown.store(true, std::memory_order_relaxed);
    g_frameStretch.store(stretch, std::memory_order_relaxed);
}

extern "C" bool Controller_GetFrameAdvantage(float* frames)
{
    if (!Netplay_IsActive() || !g_frameAdvantageKnown.load(std::memory_order_relaxed))
        return false;
    if (frames) *frames = g_frameAdvantage.load(std::memory_order_relaxed);
    return true;
}

extern "C" float Controller_GetFrameStretch()
{
    return Netplay_IsActive() ? g_frameStretch.load(std::memory_order_relaxed) : 0.0f;
}

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

static void LogGameNetSummary();

extern "C" void Controller_ResetFrameEpoch()
{
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    // Reset both native cursors at the battle barrier so frame labels match.
    if (sReplay)
        *(int32_t*)(sReplay + 0x54) = 0;
    LogGameNetSummary();
    g_gameFrameBase = 0;
    g_netFrameCount = 0;
    g_fightFrameBase = -1;
    g_introLastFrame = -1;
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
    g_preRenderInputReady = false;
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
    ResetTimeSync();
}

// ---------------------------------------------------------------------------
// Debug accessors — called from framestepper draw
// ---------------------------------------------------------------------------
extern "C" int32_t Controller_GetDelayFrames()      { return g_delayFrames; }
extern "C" bool Controller_IsDelayChangePending()   { return g_pendingDelayFrames >= 0; }
extern "C" int32_t Controller_GetNetFrameCount()    { return GetGameFrameCount(); }
extern "C" int32_t Controller_GetExecutingFrame()   { return g_netFrameCount; }
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

// Rollbacks of the last second, for the status display: when each happened
// and how many frames it replayed.
static constexpr int ROLLBACK_STAT_SIZE = 64;
static uint64_t g_rollbackStatTick[ROLLBACK_STAT_SIZE] = {};
static int32_t  g_rollbackStatDepth[ROLLBACK_STAT_SIZE] = {};
static uint32_t g_rollbackStatHead = 0;
static std::atomic<float> g_averageRollbackFrames{0.0f};

// One game's netcode in numbers, for the line written when it ends. Game
// thread only; a few integer additions per rollback or input wait.
static struct GameNetStats {
    uint32_t frames;            // played under netplay
    uint32_t rollbacks, resimulatedFrames, deepest, atLimit;
    uint32_t waits, longWaits;  // the game held for the opponent's input
    int64_t waitQpc, longestWaitQpc;
    int32_t lowestDelay = INT32_MAX, highestDelay = INT32_MIN;
} g_gameNetStats;

// A delay the running game was played at: both sides of a change made
// during it, and the one it ended on.
static void NoteGameDelay(int32_t delay)
{
    GameNetStats& s = g_gameNetStats;
    if (delay < s.lowestDelay) s.lowestDelay = delay;
    if (delay > s.highestDelay) s.highestDelay = delay;
}

static void NoteInputWait(int64_t startQpc)
{
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    const int64_t waited = end.QuadPart - startQpc;
    GameNetStats& s = g_gameNetStats;
    ++s.waits;
    s.waitQpc += waited;
    if (waited > s.longestWaitQpc) s.longestWaitQpc = waited;
    if (waited * 60 > g_qpcFrequency) ++s.longWaits;
}

// Where every game ends or begins (Controller_ResetFrameEpoch).
static void LogGameNetSummary()
{
    NoteGameDelay(g_delayFrames);
    const GameNetStats s = g_gameNetStats;
    g_gameNetStats = GameNetStats{};
    if (s.frames < 60 || !g_qpcFrequency || Netplay_IsSpectator()) return;
    const double toMs = 1000.0 / static_cast<double>(g_qpcFrequency);
    char delay[24];
    if (s.lowestDelay == s.highestDelay) snprintf(delay, sizeof(delay), "%d", s.highestDelay);
    else snprintf(delay, sizeof(delay), "%d-%d", s.lowestDelay, s.highestDelay);
    HookLog("Netplay: game summary: %u frames, delay %s | rollbacks %u (avg %.1f, deepest %u, "
            "%u at the %d-frame limit, %.1f%% of frames resimulated) | input waits %u "
            "(%u over a frame, %.0f ms in all, longest %.0f ms)\n",
            s.frames, delay, s.rollbacks,
            s.rollbacks ? static_cast<double>(s.resimulatedFrames) / s.rollbacks : 0.0,
            s.deepest, s.atLimit, g_rollbackFrames,
            100.0 * s.resimulatedFrames / s.frames, s.waits, s.longWaits,
            s.waitQpc * toMs, s.longestWaitQpc * toMs);
}

static void RecordRollbackDepth(int32_t depth)
{
    GameNetStats& s = g_gameNetStats;
    ++s.rollbacks;
    s.resimulatedFrames += static_cast<uint32_t>(depth);
    if (static_cast<uint32_t>(depth) > s.deepest) s.deepest = static_cast<uint32_t>(depth);
    if (depth >= g_rollbackFrames) ++s.atLimit;
    g_rollbackStatTick[g_rollbackStatHead % ROLLBACK_STAT_SIZE] = GetTickCount64();
    g_rollbackStatDepth[g_rollbackStatHead % ROLLBACK_STAT_SIZE] = depth;
    ++g_rollbackStatHead;
}

// Game thread, once a frame.
static void UpdateAverageRollbackFrames()
{
    const uint64_t now = GetTickCount64();
    int32_t total = 0, count = 0;
    for (int i = 0; i < ROLLBACK_STAT_SIZE; ++i) {
        if (!g_rollbackStatTick[i] || now - g_rollbackStatTick[i] >= 1000) continue;
        total += g_rollbackStatDepth[i];
        ++count;
    }
    g_averageRollbackFrames.store(count ? static_cast<float>(total) / count : 0.0f,
                                  std::memory_order_relaxed);
}

extern "C" float Controller_GetAverageRollbackFrames()
{
    return g_averageRollbackFrames.load(std::memory_order_relaxed);
}

// The window is fixed (kNetplayRollbackFrames); the argument only says
// whether there is one. Only a spectator runs without.
extern "C" void Controller_SetRollbackFrames(int32_t n)
{
    n = n <= 0 && Netplay_IsSpectator() ? 0 : kNetplayRollbackFrames;
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

    if (g_gameNetStats.frames) NoteGameDelay(g_delayFrames);
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

    if (g_gameNetStats.frames) NoteGameDelay(g_delayFrames);
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

// The intro under rollback. Its camera (sAction+0x1D8, a uMayaMotionCamera
// that also takes viewport 0) is a unit a rollback neither restores nor
// replays. It lives until the game enters the countdown: sAction::move runs
// sAction__executeMatchPhase(3) at the top of the tick after the intro
// reports it is over, which kills the camera, clears the pointer and gives
// the viewport back. A snapshot taken while sAction is still in step 2, that
// tick's included, holds those pointers and cannot be gone back to (crash of
// 2026-11-24: sCamera::move called through a viewport camera that had become
// a uBattleUiHit). Nothing in those ticks needs a rollback either:
//   - the characters are given neutral input in them, so a mispredicted
//     input changes nothing (the skip is read from confirmed frames, see
//     hkStepIntro);
//   - no replay starts in them (Controller_FirstRollbackFrame, used where a
//     replay is begun).
// The tick after the last of them is the countdown's second, with live
// input: the players can move through "READY... FIGHT" as in the game itself.
// (Until 2026-11-25 twenty more frames were kept neutral as a margin, which
// took a third of a second of movement off the start of every game.)
//
// The step is read at the top of the tick, before anything in it runs, so
// the frame noted is the one this tick's snapshot and inputs are filed under.
static bool IsIntroQuietFrame(int32_t frame)
{
    return g_introLastFrame >= 0 && frame <= g_introLastFrame;
}

extern "C" int32_t Controller_FirstRollbackFrame()
{
    return g_introLastFrame >= 0 ? g_introLastFrame + 1 : 0;
}

extern "C" bool FrameStepper_IsBattleSynchronized();

extern "C" void Controller_NoteTickStart(int32_t frame)
{
    if (frame < 0 || !Netplay_IsActive() || !FrameStepper_IsBattleSynchronized())
        return;
    using GetActionFn = uint64_t(__fastcall*)();
    static GetActionFn getAction = (GetActionFn)_addr(0x140001A90);
    const uint64_t action = getAction();
    // mStep: 2 is the intro, 3 and 4 the countdown, 5 the fight.
    if (action && *(int32_t*)(action + 0xD8) <= 2)
        g_introLastFrame = frame;
}

// A bit of the timeline's input word that no logical action uses: "an attack
// is held", for the intro and outro skip. It travels with the inputs (packets,
// snapshots, the spectator log) and is stripped before a character sees them.
static constexpr uint32_t TIMELINE_SKIP_FLAG = 0x80000000u;

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

    if (Spectate_IsViewing()) {
        // Both teams are played from the spectate log, drawn or not.
        resolved = Spectate_GetInput(inputTeam, context->frame);
    } else if (g_isResimulating) {
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

    // Not an input: the intro / outro skip's own bit (see TimelineSkipPressed).
    resolved &= ~TIMELINE_SKIP_FLAG;
    // Through the intro the characters are given nothing (see above); what
    // was pressed is still captured and sent, which is what the skip reads.
    if (IsIntroQuietFrame(context->frame))
        resolved = 0;

    context->timelineInput = resolved;
    context->timelineResolved = true;
    return resolved;
}

// sMvc3Pad+0xC30: 0 while the game takes player 1 from the keyboard, 1 from
// the pad. sMvc3SyncPad (0x1402B42C0) sets it on a press on the hardware slot
// player 1 is mapped to and clears it on any key or mouse button. While it is
// 0 the native getters and cChrInput__buttonRemap give player 1 the keyboard
// alone: a pad on another slot, or one that never flips the byte, does nothing.
static constexpr uint32_t PAD_INPUT_SOURCE = 0xC30;

// A netplay player owns one team, on whichever hardware slot their controller
// is and whichever source the game believes player 1 is using. (Until
// 2026-11-26 only the client read every slot; the host read slot 0 through
// the native getter, and a player whose pad was elsewhere could play as P2
// and not as P1.) Reads what the native poll already decoded.
static uint32_t ReadAllPadSlots(uint64_t pad)
{
    static uint32_t loggedSlots = 0;
    uint32_t mergedInput = 0;
    for (uint32_t slot = 0; slot < PAD_MAX_SLOTS; ++slot) {
        const uint64_t slotBase = pad + slot * PAD_SLOT_STRIDE;
        if (!*(const uint8_t*)(slotBase + PAD_SLOT_ACTIVE) ||
            !*(const uint8_t*)(slotBase + PAD_SLOT_DEVICE_KIND))
            continue;
        const uint32_t input = *(const uint32_t*)(slotBase + PAD_SLOT_INPUT);
        if (input && !(loggedSlots & (1u << slot))) {
            loggedSlots |= 1u << slot;
            HookLog("Input: match input from the controller on pad slot %u (the game's "
                    "player 1 source is %s)\n", slot,
                    *(const uint8_t*)(pad + PAD_INPUT_SOURCE) ? "the pad" : "the keyboard");
        }
        mergedInput |= input;
    }
    return ApplySOCD(mergedInput);
}

// The local player's pad through their own bindings, with the keyboard added.
// Player 1's remap is the one that gives way to the keyboard, so the source
// byte is held on "pad" for it; a remap of nothing as player 1 is then the
// keyboard's part (nothing while the pad is the source). The client's team
// carries a copy of the player's own bindings (TrainingBoot).
static uint32_t RemapLocalInput(uint32_t team, uint32_t rawInput, uint8_t alternateMap)
{
    uint32_t logical;
    using GetInstanceFn = uint64_t(__fastcall*)();
    const uint64_t pad = ((GetInstanceFn)_addr(ADDR_PAD_GET_INSTANCE))();
    if (team == 0 && pad) {
        uint8_t* source = (uint8_t*)(pad + PAD_INPUT_SOURCE);
        const uint8_t saved = *source;
        *source = 1;
        logical = obuttonRemap(0, rawInput, alternateMap);
        *source = saved;
    } else {
        logical = obuttonRemap(team, rawInput, alternateMap);
    }
    // Keys typed into another window are not the player's.
    if (IsGameProcessForeground())
        logical |= obuttonRemap(0, 0, alternateMap);
    return logical;
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
    if (Spectate_IsViewing()) {
        // Nothing local reaches the characters of a match being watched.
        uint32_t watched = ResolveTimelineInput(context, inputTeam, 0, false);
        context->injectedAtButtonRemap = true;
        return watched;
    }
    uint32_t currentInput = 0;
    uint32_t rawTestInput = 0;
    if (g_isResimulating) {
        // A resimulated frame takes its input from the snapshot or the
        // recorded history (ResolveTimelineInput); what is pressed now is
        // not read at all.
    } else if (AutoTest_GetInput(context->frame, inputTeam, &rawTestInput)) {
        // Remap test input without P1's focus/menu shortcut.
        currentInput = obuttonRemap(1, rawTestInput, alternateMap);
    } else {
        const uint32_t localTeam = Netplay_IsHost() ? 0u : 1u;
        if (context->ownerTeam == localTeam && playerIndex == localTeam)
            currentInput = RemapLocalInput(localTeam, rawInput, alternateMap);
        else
            currentInput = obuttonRemap(playerIndex, rawInput, alternateMap);
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
    if (is_active && Spectate_IsViewing())
        return 0;

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
            else if (playerIndex == 0)
            {
                // The keyboard is merged after the remap (RemapLocalInput).
                raw = ReadAllPadSlots(pad);
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
                raw = ReadAllPadSlots(pad);
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

// The local player's buttons as the characters would be given them, read
// outside any character's input processing.
static uint32_t ReadLocalLogicalInput()
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
    return RemapLocalInput(team, rawInput, alternateMap);
}

extern "C" uint32_t Controller_CaptureOpeningMember()
{
    const uint32_t logicalInput = ReadLocalLogicalInput();
    if (logicalInput & IN_RB)
        return 2;
    if (logicalInput & IN_LB)
        return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Skipping the intro and the winner's outro
// ---------------------------------------------------------------------------
// Natively both end early on a button read straight from a pad. In netplay
// that read is outside the synchronized inputs: each machine would leave the
// intro or the outro on a frame of its own. The read is answered from the
// input timeline instead, the one both players (and their spectators)
// simulate from: a face button from either player.
//
// The frame looked at is SKIP_INPUT_LAG behind the current one. That is at
// least the rollback window, so both players' inputs for it are confirmed by
// the time it is read: the answer is the same live and in any replay, and no
// rollback can move the frame the intro or the outro ends on. Only a press
// counts, not a button held since the loading.
// What is read there is a bit of its own (TIMELINE_SKIP_FLAG, declared with
// the timeline), set while the local player holds an attack and never given
// to a character. The attacks themselves must not be added to the timeline
// here: with no input delay the local characters are simulated from what
// their own input hook read, before anything added here, while the other
// peer and every replay would get the added bits (the outro desync of
// 2026-11-24, where the winner acted on them on one side only).
// The four attacks are 0xF0 in the logical layout cChrInput__buttonRemap
// produces. The pad read the native skip makes is answered in the pad's
// layout, where it tests the face buttons, 0xF000.
static constexpr uint32_t SKIP_ACTIONS = 0xF0;
static constexpr uint32_t SKIP_PAD_BUTTONS = IN_X | IN_Y | IN_A | IN_B;
static constexpr int32_t SKIP_INPUT_LAG = 16; // the largest rollback window there is

static uint32_t ConfirmedInputs(int32_t frame)
{
    if (frame < 0)
        return 0;
    if (Spectate_IsViewing())
        return Spectate_GetInput(0, frame) | Spectate_GetInput(1, frame);
    // Both sides start on neutral input.
    if (frame <= g_delayFrames + g_rollbackFrames)
        return 0;
    const uint32_t idx = (uint32_t)frame % RING_SIZE;
    const uint32_t remote = IsFrameConfirmed(frame) ? g_receivedInput[idx] : 0;
    return g_recordedLocalInput[idx] | remote;
}

static bool TimelineSkipPressed()
{
    const int32_t frame =
        (g_isResimulating ? g_resimulateCurrentFrame : GetGameFrameCount()) - SKIP_INPUT_LAG;
    if (frame < 1)
        return false;
    return (ConfirmedInputs(frame) & TIMELINE_SKIP_FLAG) != 0 &&
           (ConfirmedInputs(frame - 1) & TIMELINE_SKIP_FLAG) == 0;
}

// Characters do not necessarily read their pads during the intro or the
// outro, and the local side of the timeline is captured where they do. The
// local buttons are added here so a press reaches the timeline regardless.
static void CaptureLocalSkipInput()
{
    if (g_isResimulating || Spectate_IsViewing())
        return;
    if (ReadLocalLogicalInput() & SKIP_ACTIONS)
        g_localPending |= TIMELINE_SKIP_FLAG;
}

// The same two for the result screen (native_result.cpp), whose own skips
// (the winner's quote, the score board's count-up) are taken from the
// timeline as well so that both players' screens move on together.
extern "C" bool Controller_TimelineSkipPressed()
{
    return Netplay_IsActive() && FrameStepper_IsBattleSynchronized() && TimelineSkipPressed();
}

extern "C" void Controller_CaptureSkipInput()
{
    if (Netplay_IsActive() && FrameStepper_IsBattleSynchronized())
        CaptureLocalSkipInput();
}

// While set, the raw pad read answers with this in place of any pad: the
// native skip test sees the timeline's press or nothing.
static bool g_padReadOverridden = false;
static uint32_t g_padReadValue = 0;

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
        if (AutoTest_IsActive()) {
            setNextStep(param_1, 3);
            return;
        }
        // The intro plays, and the game's own skip is taken on a face button
        // from either player, read from confirmed frames of the timeline.
        CaptureLocalSkipInput();
        g_padReadOverridden = true;
        g_padReadValue = TimelineSkipPressed() ? SKIP_PAD_BUTTONS : 0;
        oStepIntro(param_1);
        g_padReadOverridden = false;
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
    if (Netplay_IsActive() && (LobbyFlow_InRoomMatch() || Spectate_IsViewing())) {
        // Room matches are sets: the lobby counts this game, and what follows
        // it is chosen by both players on the game's own result menu
        // (native_result.h). Nothing restarts from a button here. A spectator
        // runs the same outro from the same inputs.
        if (!Spectate_IsViewing()) {
            LobbyFlow_OnResultPhase(*(int32_t*)(sAction + 0x114));
            if (FrameStepper_IsMatchRestartPending())
                return;
        }
        // The outro's own skip, on a face button from either player read
        // from the input timeline (the game tests its confirm mask).
        CaptureLocalSkipInput();
        g_padReadOverridden = true;
        g_padReadValue = 0;
        if (FrameStepper_IsBattleSynchronized() && TimelineSkipPressed()) {
            using GetInstanceFn = uint64_t(__fastcall*)();
            using GetMaskFn = uint32_t(__fastcall*)(uint64_t);
            const uint64_t pad = ((GetInstanceFn)_addr(ADDR_PAD_GET_INSTANCE))();
            g_padReadValue = pad ? ((GetMaskFn)_addr(ADDR_PAD_UNIT_FLAGS))(pad) : 0;
        }
        oStepResult8(sAction);
        g_padReadOverridden = false;
        return;
    }
    if (Netplay_IsActive()) {
        LobbyFlow_OnResultPhase(*(int32_t*)(sAction + 0x114));
        if (!FrameStepper_IsMatchRestartPending() && LobbyFlow_AllowResultRestart() &&
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
    // A spectator has no say on the result screen: its menu never runs, and
    // the restart or the end of the match arrives with the feed.
    if (Netplay_IsActive() && Spectate_IsViewing())
        return;
    if (Netplay_IsActive() && LobbyFlow_InRoomMatch()) {
        // The result menu is up: the rematch waits for both players' choice
        // there (LobbyFlow_GameTick acts on it) and is then the synchronized
        // restart. The native handler is never run: in versus all it does is
        // restart the match by itself (restart mode 1, step 12) once it is
        // let through, and what holds it back while the menu is up is not
        // part of a rollback. A replay of these frames ran it, and that peer
        // alone went back into the intro (desync of 2026-11-24).
        LobbyFlow_OnResultPhase(*(int32_t*)(sAction + 0x114));
        return;
    } else if (Netplay_IsActive()) {
        LobbyFlow_OnResultPhase(*(int32_t*)(sAction + 0x114));
        if (!FrameStepper_IsMatchRestartPending() && LobbyFlow_AllowResultRestart() &&
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
    int64_t arrivalQpc = 0;
    while (Netplay_GetInboundPacket(inPkt, &arrivalQpc))
    {
        if (inPkt.battle_generation != FrameStepper_GetBattleGeneration())
            continue;
        SampleTimeSync(inPkt, arrivalQpc);

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

        LogReceivedPacket(inPkt);
        TraceInputPacket("IPRX", "frame-start", g_netFrameCount, inPkt, 0,
                         acceptedMask, changedMask, priorConfirmedMask);
    }
}

// ---------------------------------------------------------------------------
// Input repair. An input packet carries the sender's newest NET_HISTORY_SIZE
// frames. A game held up for a while (a hitch, a dragged window) can come back
// to find the peer's packets have moved on past frames it never saw: the peer
// ran on to the end of its rollback budget, and only the newest packets were
// kept while nobody was reading them. Those frames were never sent again, so
// this game could not go on, the peer was waiting for this game, and both sat
// in "waiting for opponent" until one left.
//
// Now the game that finds such a hole asks for it, and the peer answers with
// the frames from there on.
// ---------------------------------------------------------------------------
static constexpr uint64_t INPUT_REQUEST_INTERVAL_MS = 50;

// `frame` is unconfirmed and holding the game up. A hole only if the peer has
// already sent input for a later frame; otherwise it just has not got there.
static void RequestMissingInput(int32_t frame)
{
    static uint64_t lastRequest = 0;
    if (frame < 0 || g_lastRemoteFrame <= frame) return;
    const uint64_t now = GetTickCount64();
    if (now - lastRequest < INPUT_REQUEST_INTERVAL_MS) return;
    lastRequest = now;
    static int32_t lastLogged = -1;
    if (lastLogged != frame) {
        lastLogged = frame;
        HookLog("Netplay: asking the peer to resend input from frame %d (newest seen %d, local %d)\n",
                frame, g_lastRemoteFrame, g_netFrameCount);
    }
    Netplay_SendInputRequest(FrameStepper_GetBattleGeneration(), frame);
}

// The peer asked for our input from a frame on: send that span once.
static void AnswerInputRequest()
{
    uint32_t generation = 0;
    int32_t first = -1;
    if (!Netplay_TakeInputRequest(&generation, &first)) return;
    if (generation != FrameStepper_GetBattleGeneration() || first < 0 || g_netFrameCount < 0)
        return;
    // Every frame of a packet is taken as real input by the receiver, so the
    // span must hold only frames that have been scheduled. If it would run
    // past the newest one, the ordinary packets already cover the request.
    int32_t newest = -1;
    for (int32_t frame = g_netFrameCount + MAX_HISTORY - 1; frame >= first; --frame) {
        if (g_scheduledLocalFrame[(uint32_t)frame % RING_SIZE] == frame) {
            newest = frame;
            break;
        }
    }
    if (newest < 0 || first + MAX_HISTORY - 1 > newest) return;

    InputPacket packet = {};
    packet.battle_generation = generation;
    packet.checksum_frame = 0xFFFFFFFFu;
    packet.start_frame = (uint32_t)first;
    packet.flags = INPUT_FLAG_REAL;
    for (int i = 0; i < MAX_HISTORY; ++i) {
        const int32_t frame = first + i;
        const uint32_t idx = (uint32_t)frame % RING_SIZE;
        if (g_scheduledLocalFrame[idx] != frame) return; // older than we still hold
        packet.inputs[i] = g_scheduledLocalInput[idx];
    }
    static int32_t lastLogged = -1;
    if (lastLogged != first) {
        lastLogged = first;
        HookLog("Netplay: resending input frames [%d-%d] at the peer's request (local %d)\n",
                first, first + MAX_HISTORY - 1, g_netFrameCount);
    }
    Netplay_SendRepairPacket(packet);
}

static bool WaitForConfirmedFrame(int32_t frame, const char* reason)
{
    if (IsFrameConfirmed(frame) || !Netplay_IsActive())
        return true;

    LagDiagnosticScope diagnostic(LagMetric::InputWait);
    const int32_t remoteBefore = g_lastRemoteFrame;
    LARGE_INTEGER waitStart;
    QueryPerformanceCounter(&waitStart);
    Rollback_FreezeWatchdogHeartbeat(reason, g_netFrameCount);
    //HookLog("Netplay: waiting for remote input frame %d (%s, local=%d, lastRemote=%d)\n",
    //        frame, reason, g_netFrameCount, g_lastRemoteFrame);
    while (!IsFrameConfirmed(frame) && Netplay_IsActive() &&
           !Netplay_HasInboundRestartMatch())
    {
        SwitchToThread();
        DrainAndProcessPackets();
        AnswerInputRequest();
        RequestMissingInput(frame);
    }
    LagDiagnostics_RecordWait(LagMetric::InputWait, frame, remoteBefore, g_lastRemoteFrame,
                              g_delayFrames, g_rollbackFrames);
    NoteInputWait(waitStart.QuadPart);
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
    LagDiagnosticScope diagnostic(LagMetric::PredictionWait);
    const int32_t remoteBefore = g_lastRemoteFrame;
    LARGE_INTEGER waitStart;
    QueryPerformanceCounter(&waitStart);
    SetPredictionBudgetExhausted();
    Rollback_FreezeWatchdogHeartbeat("prediction budget exhausted",
                                     g_netFrameCount);
    // Asked for before the loop as well: once the popup is up the loop is not
    // entered any more, and this is then called once a frame.
    RequestMissingInput(frame);
    while (!IsFrameConfirmed(frame) && Netplay_IsActive() &&
           !Netplay_HasInboundRestartMatch() &&
           !Controller_ShouldShowPredictionBudgetExhausted())
    {
        SwitchToThread();
        DrainAndProcessPackets();
        AnswerInputRequest();
        RequestMissingInput(frame);
    }

    LagDiagnostics_RecordWait(LagMetric::PredictionWait, frame, remoteBefore, g_lastRemoteFrame,
                              g_delayFrames, g_rollbackFrames);
    NoteInputWait(waitStart.QuadPart);
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
    g_preRenderInputReady = false;
    LagDiagnosticScope diagnostic(LagMetric::ControllerStart);
    if (!Netplay_IsActive()) {
        ClearPredictionBudgetExhausted();
        return;
    }

    g_netFrameCount = GetGameFrameCount();
    // If sReplay isn't ready yet, skip packet processing this frame.
    if (g_netFrameCount < 0) return;
    ++g_gameNetStats.frames;

    // Keep the native replay cursor on the shared frame epoch.
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (sReplay) {
        *(int32_t*)(sReplay + 0x54) = g_netFrameCount;
        g_gameFrameBase = 0;
    }

    // The frame clock: this frame begins now. Outgoing packets are stamped
    // from it and arriving ones are measured against it (time sync).
    {
        LARGE_INTEGER qpc;
        if (!g_qpcFrequency) {
            QueryPerformanceFrequency(&qpc);
            g_qpcFrequency = qpc.QuadPart;
        }
        QueryPerformanceCounter(&qpc);
        g_frameStartQpc = qpc.QuadPart;
        Netplay_SetFrameClock(FrameStepper_IsBattleSynchronized() ? g_netFrameCount : -1,
                              g_frameStartQpc);
    }

    ApplyScheduledDelayIfReady();

    g_localPending = 0;

    // Use the same packet acceptance rules here and inside waits.
    {
        LagDiagnosticScope packetDiagnostic(LagMetric::PacketDrain);
        DrainAndProcessPackets();
        AnswerInputRequest();
    }

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

                RecordRollbackDepth(g_netFrameCount - earliestConflict);
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

    UpdateTimeSync();
    UpdateAverageRollbackFrames();
    g_preRenderInputReady = true;
}

// ---------------------------------------------------------------------------
// Sum integer-truncated positions/HP, action IDs, and per-character RNG hashes.
// Character order does not affect the sum.
// ---------------------------------------------------------------------------
// cChr::mChrF bit 0 closes cChr::isMoveActive (0x14F0); mParentChrInfo's
// character pointer is at 0x6690 + 8.
static constexpr uint64_t CHR_FLAGS_OFFSET = 0x14F0;
static constexpr uint32_t CHR_FLAG_MOVE_STOPPED = 1;
static constexpr uint64_t CHR_PARENT_OFFSET = 0x6698;

// The checksum is the sum of four parts, kept apart for diagnosis: positions,
// life, action ids and the per-character RNG hashes.
static uint32_t g_checksumParts[4] = {};
// ...and the RNG hash of each team member on its own (team * 3 + member).
static uint32_t g_checksumRng[6] = {};

static uint32_t ComputeChecksum()
{
    std::memset(g_checksumParts, 0, sizeof(g_checksumParts));
    std::memset(g_checksumRng, 0, sizeof(g_checksumRng));
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
            // A helper character (Frank West's zombies) sits in the team list
            // with its move gate closed until its parent uses it. The game
            // parks it while the match loads, before the peers synchronize, so
            // its frozen action and clock depend on local load timing. Nothing
            // simulates it in that state; it rejoins the sum when it wakes.
            const bool dormantHelper = character &&
                (*(uint32_t*)(character + CHR_FLAGS_OFFSET) & CHR_FLAG_MOVE_STOPPED) &&
                *(uint64_t*)(character + CHR_PARENT_OFFSET);
            if (character && !dormantHelper)
            {
                int32_t qx = (int32_t)*(float*)(character + 0x50);
                int32_t qy = (int32_t)*(float*)(character + 0x54);
                int32_t hp = (int32_t)*(float*)(character + 0x1550);
                cs += (uint32_t)(qx < 0 ? -qx : qx);
                cs += (uint32_t)(qy < 0 ? -qy : qy);
                cs += (uint32_t)(hp < 0 ? -hp : hp);
                cs += *(uint32_t*)(character + 0x1310); // actionId
                g_checksumParts[0] += (uint32_t)(qx < 0 ? -qx : qx) + (uint32_t)(qy < 0 ? -qy : qy);
                g_checksumParts[1] += (uint32_t)(hp < 0 ? -hp : hp);
                g_checksumParts[2] += *(uint32_t*)(character + 0x1310);

                // Hash each character's RNG separately to keep the sum order-independent.
                const uint32_t* characterRng =
                    (const uint32_t*)(character + 0x2D80);
                uint32_t rngHash = 2166136261u;
                for (int i = 0; i < 4; ++i)
                    rngHash = (rngHash ^ characterRng[i]) * 16777619u;
                cs += rngHash;
                g_checksumParts[3] += rngHash;
                const int32_t member = *(int32_t*)(character + 0x2DAC); // mDefMemberId
                if (member >= 0 && member < 3 && !*(uint64_t*)(character + CHR_PARENT_OFFSET))
                    g_checksumRng[ti * 3 + member] = rngHash;
            }
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }

    return cs;
}

extern "C" void Controller_RecordFrameChecksum(int32_t frame)
{
    LagDiagnosticScope diagnostic(LagMetric::Checksum);
    if (!FrameStepper_IsDesyncCheckReady() ||
        FrameStepper_IsMatchRestartPending() ||
        frame < DESYNC_SKIP_FRAMES)
        return;
    uint32_t idx = (uint32_t)frame % RING_SIZE;
    g_localChecksum[idx] = ComputeChecksum();
    std::memcpy(g_localChecksumParts[idx], g_checksumParts, sizeof(g_checksumParts));
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

// ---------------------------------------------------------------------------
// Spectators
// ---------------------------------------------------------------------------
static constexpr int32_t SPECTATE_CHECKSUM_INTERVAL = 60;
static int32_t g_spectateChecksumNext = SPECTATE_CHECKSUM_INTERVAL;
static int32_t g_spectateLoggedLastTick = 0;

// Appends every frame whose inputs are now final to the log spectators play
// from: one pair per frame, in order. A frame is final once the remote input
// for it has arrived; the local one was fixed when it was scheduled.
static void RecordSpectateFrames()
{
    int32_t next = Spectate_RecFrameCount();
    if (next < 0)
        return;
    if (next == 0) {
        g_spectateChecksumNext = SPECTATE_CHECKSUM_INTERVAL;
        g_spectateLoggedLastTick = 0;
    }
    const bool localIsP1 = Netplay_IsHost();
    const int32_t loggedBefore = next;
    for (; next <= g_netFrameCount && IsFrameConfirmed(next); ++next) {
        const uint32_t idx = (uint32_t)next % RING_SIZE;
        uint32_t local = 0;
        uint32_t remote = 0;
        // The neutral start-up frames run on zero input on both peers, and
        // anything older than the rings has been overwritten (it never is
        // while the prediction budget holds).
        if (next > g_delayFrames + g_rollbackFrames && g_netFrameCount - next < RING_SIZE) {
            local = g_recordedLocalInput[idx];
            remote = g_confirmedFrame[idx] == next ? g_receivedInput[idx] : 0;
        }
        Spectate_RecFrame(localIsP1 ? local : remote, localIsP1 ? remote : local);
    }

    // A checksum for spectators, taken straight from the state: only when it
    // is certain to be final. That is when this tick's frame is the one frame
    // that became final now (so everything before it was final a tick ago,
    // and any correction of it ran at the start of this tick) and it was
    // simulated with the input that arrived.
    const int32_t frame = g_netFrameCount;
    const uint32_t frameIdx = (uint32_t)frame % RING_SIZE;
    if (frame > 0 && frame % SPECTATE_CHECKSUM_INTERVAL == 0 && loggedBefore == frame &&
        next == frame + 1 && !g_isResimulating &&
        (frame <= g_delayFrames + g_rollbackFrames ||
         g_usedRemoteInput[frameIdx] == g_receivedInput[frameIdx])) {
        const uint32_t checksum = ComputeChecksum();
        Spectate_RecChecksum(frame, checksum, g_checksumParts, g_checksumRng);
    }
    g_spectateLoggedLastTick = loggedBefore;
}


// A spectator's frame boundaries: no packets, no prediction, no schedule.
extern "C" int32_t Controller_SpectatorFrameStart()
{
    g_preRenderInputReady = false;
    ClearPredictionBudgetExhausted();
    g_netFrameCount = GetGameFrameCount();
    if (g_netFrameCount < 0)
        return -1;
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (sReplay) {
        *(int32_t*)(sReplay + 0x54) = g_netFrameCount;
        g_gameFrameBase = 0;
    }
    g_localPending = 0;
    return g_netFrameCount;
}

extern "C" void Controller_SpectatorFrameEnd(int32_t frame, int32_t fastForwarded)
{
    if (Spectate_WantsChecksum(frame)) {
        const uint32_t checksum = ComputeChecksum();
        Spectate_OnChecksum(frame, checksum, g_checksumParts, g_checksumRng,
                            fastForwarded != 0);
    }
}

// Moves the native frame cursor past frames simulated without it.
extern "C" void Controller_AdvanceFrameCounter(int32_t frames)
{
    uint64_t sReplay = *(uint64_t*)_addr(G_PSREPLAY_PTR);
    if (sReplay && frames > 0)
        *(int32_t*)(sReplay + 0x54) += frames;
}

static void PublishFrameInput(bool frameComplete)
{
    if (!Netplay_IsActive() || g_isResimulating || g_netFrameCount < 0)
        return;

    // Send history even during neutral startup so delay-only peers can progress.

    // Record every live frame, even when no character-input hook ran.
    if (frameComplete) {
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
    if (frameComplete) {
        memmove(&g_oldInputs[0], &g_oldInputs[1],
                (MAX_HISTORY - 1) * sizeof(uint32_t));
        g_oldInputs[MAX_HISTORY - 1] = g_localPending;
    }

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

    if (frameComplete && FrameStepper_IsDesyncCheckReady() &&
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

    // Our lead, for the peer's time sync. The frame clock is added to the flags as the packet leaves (netplay.cpp).
    outPkt.frame_advantage = static_cast<int32_t>(g_localLead * 256.0f);
    outPkt.flags           = INPUT_FLAG_REAL; // seed packets have flags = 0

    if (INPUT_PROVENANCE_TRACE) {
        uint32_t executedIdx = (uint32_t)g_netFrameCount % RING_SIZE;
        HookLog("IPSCHEDULE frame=%d delay=%d captured=%08X scheduledFrame=%d "
                "scheduledValue=%08X executed=%08X\n",
                g_netFrameCount, g_delayFrames, g_localPending, scheduledFrame,
                g_scheduledLocalInput[scheduledIdx],
                g_recordedLocalInput[executedIdx]);
        TraceInputPacket("IPTX", frameComplete ? "frame-end" : "pre-render", g_netFrameCount, outPkt,
                         validMask, 0, 0, 0);
    }

    Netplay_SendPacket(outPkt);
}

extern "C" void Controller_PublishInputBeforeRender()
{
    if (!g_preRenderInputReady || g_isResimulating || !Netplay_IsActive() ||
        !FrameStepper_IsBattleSynchronized() || FrameStepper_IsMatchRestartPending())
        return;
    g_preRenderInputReady = false;
    PublishFrameInput(false);
}

extern "C" void Controller_OnFrameEnd()
{
    LagDiagnosticScope diagnostic(LagMetric::ControllerEnd);
    g_preRenderInputReady = false;
    if (!Netplay_IsActive() || g_isResimulating || g_netFrameCount < 0)
        return;
    // Also serves as the fallback for frames that never entered rendering.
    PublishFrameInput(true);
    RecordSpectateFrames();

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

    // A helper or clone (mChrF bit 0x1000: Felicia's Kitty Helper, shadows) has
    // no pad. The game builds its input from its parent through virtual +0x328
    // and then runs the shared facing swap. Injecting the player's timeline
    // input there handed the helper the player's own buttons: it jumped and
    // could X-Factor on any press, and lost the forward input it leaves with.
    static constexpr uint32_t CCHR_FLAGS_OFFSET = 0x14F0;
    static constexpr uint32_t CHR_FLAG_PARENT_DRIVEN_INPUT = 0x1000;
    const bool parentDrivenInput = character &&
        (*(uint32_t*)(character + CCHR_FLAGS_OFFSET) & CHR_FLAG_PARENT_DRIVEN_INPUT);

    FrameInputRemapContext context = {
        teamIdx, teamIdx, lookupFrame, 0, false, false
    };
    FrameInputRemapContext* previousContext = g_frameInputRemapContext;
    // Cleared, not inherited, for a parent-driven object: no outer context may
    // inject into it either.
    g_frameInputRemapContext =
        Netplay_IsActive() && character && lookupFrame >= 0 && !parentDrivenInput
            ? &context : (parentDrivenInput ? nullptr : previousContext);
    oprocessFrameInput(cChrInput);
    g_frameInputRemapContext = previousContext;

    // Its input is derived state, not a player's: nothing to record or replay.
    if (!character || lookupFrame < 0 || parentDrivenInput)
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
    LagDiagnosticScope diagnostic(LagMetric::PadPoll);
    RollbackHookProfileScope profile(RollbackHookProfileId::PadPollAll);
    opadPollAllSlots(sPad);
    // The netplay Esc menu reads this frame's decoded pad (Start toggles it)
    // and, while open, clears it before anything else consumes it.
    EscMenuImGui_OnPadPoll(sPad);
}

static uint64_t __fastcall hkpadSlotReadDevice(uint64_t slotBase)
{
    LagDiagnosticScope diagnostic(LagMetric::PadRead);
    if constexpr (LAG_BACKEND_TIMING_ENABLED) {
        if (*(uint8_t*)(slotBase + 0x0B) == 2) {
            LagDiagnosticScope backend(LagMetric::DirectInputRead);
            return opadSlotReadDevice(slotBase);
        }
    }
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

// sMain focus test (0x140259470): true when the window is foreground and not
// iconified. sMvc3Pad__stepFocusGated (0x1402B41B0) suppresses pad input while
// it is false, plus a 30-frame lockout after focus returns. Keep input live out
// of focus, except in dual training where the foreground instance owns the pads.
static constexpr uintptr_t ADDR_MAIN_HAS_INPUT_FOCUS = 0x140259470;
using MainHasInputFocus_t = bool(__fastcall*)(uint64_t);
static MainHasInputFocus_t oMainHasInputFocus = nullptr;

static bool __fastcall hkMainHasInputFocus(uint64_t sMain)
{
    if (GetLaunchOption() != LAUNCH_DUAL_TRAINING)
        return true;
    return oMainHasInputFocus(sMain);
}

// sPad__getRawStickInput_noSOCD: what the result screen and other native
// menus read. A spectator's pad must not reach the match being watched.
using PadRawInput_t = uint32_t(__fastcall*)(uint64_t, uint32_t);
static PadRawInput_t oPadRawInput = nullptr;

static uint32_t __fastcall hkPadRawInput(uint64_t sPad, uint32_t padIndex)
{
    // The intro's and the outro's skip test, answered from the timeline (for
    // a spectator too: it must leave them on the frame the players did).
    if (g_padReadOverridden)
        return g_padReadValue;
    if (Spectate_IsViewing() && Netplay_IsActive())
        return 0;
    return oPadRawInput(sPad, padIndex);
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

    // Pad focus gate
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_MAIN_HAS_INPUT_FOCUS),
        (LPVOID)&hkMainHasInputFocus,
        (LPVOID*)&oMainHasInputFocus
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_MAIN_HAS_INPUT_FOCUS));
    else
        HookLog("Controller: Failed to hook input focus gate (%d)\n", s);

    // Raw pad reads, silenced while spectating
    s = GameplayHooks_Create(
        (LPVOID)_addr(ADDR_PAD_RAW_INPUT),
        (LPVOID)&hkPadRawInput,
        (LPVOID*)&oPadRawInput
    );
    if (s == MH_OK)
        GameplayHooks_QueueEnable((LPVOID)_addr(ADDR_PAD_RAW_INPUT));
    else
        HookLog("Controller: Failed to hook raw pad input (%d)\n", s);

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
}
