#pragma once
#include <cstdint>
#include <enet/enet.h>

// ---------------------------------------------------------------------------
// Netplay: rollback netcode input transport over ENet.
//
// Architecture:
//   Local physical input is accumulated per-character in hkprocessFrameInput.
//   Each frame's raw accumulated input is written into a delay ring buffer at
//   (currentFrame + delayFrames), and sent as a sliding-history window
//   (NET_HISTORY_SIZE entries) over the wire.
//
//   On receipt, every history slot is written into the OTHER player's delay
//   buffer at a relative offset from the receiver's current frame, providing
//   resilience against packet loss.  Both sides read at currentFrame during
//   hkprocessFrameInput — symmetric input delay for both players.
//
//   If remote input for the current frame hasn't arrived, we predict using
//   the last known value.  The rollback engine (rollback.cpp) detects
//   late-arriving corrections via g_usedRemoteInput vs g_receivedInput
//   comparison and automatically restores snapshots + re-simulates with
//   render disabled.
//
//   A dedicated background thread owns ALL ENet I/O. The game thread
//   communicates via Win32 critical sections for packet exchange.
//
//   A desync-check packet is also sent every frame: it carries a simple
//   XOR checksum of all character position/state.  If the remote side's
//   checksum for a frame doesn't match the local checksum, desync is
//   declared and the game pauses with an on-screen warning.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Packet type identifiers (first uint32_t of every ENet payload).
// ---------------------------------------------------------------------------
enum PacketType : uint32_t {
    PACKET_INPUT = 0,  // InputPacket
    PACKET_DELAY = 1,  // DelayPacket
    PACKET_RESIM_NOTIFY = 2,  // ResimNotifyPacket
    PACKET_GROUND_TRUTH = 4,  // GroundTruthPacket
    PACKET_BATTLE_READY = 5,  // BattleReadyPacket
    PACKET_LAUNCH_TRAINING = 6, // LaunchTrainingPacket
    PACKET_DESYNC = 7,  // DesyncPacket
    PACKET_RESTART_MATCH = 8, // RestartMatchPacket
    PACKET_MULTI_RESIM = 9, // MultiResimPacket
    PACKET_DUMP_NDJSON = 10, // DumpNdjsonPacket
    PACKET_AUTOTEST_START = 11, // AutoTestStartPacket
    PACKET_AUTOTEST_FRAME = 12, // AutoTestFramePacket
    PACKET_AUTOTEST_COMPLETE = 13, // AutoTestCompletePacket
};

#pragma pack(push, 1)
constexpr size_t NET_HISTORY_SIZE = 16;

struct InputPacket {
    uint32_t type;               // PACKET_INPUT
    uint32_t battle_generation;  // rejects in-flight packets from prior matches
    uint32_t start_frame;        // inputs[0] corresponds to this frame
    uint32_t checksum_frame;     // confirmed game frame, UINT32_MAX when absent
    uint32_t checksum;           // corrected state at checksum_frame
    uint32_t inputs[NET_HISTORY_SIZE];
    int32_t  frame_advantage;    // sender's local frame advantage (negative = sender is ahead)
    uint32_t flags;              // 0 = seed packet, non-zero = real packet
};

struct DelayPacket {
    uint32_t type;               // PACKET_DELAY
    int32_t  delay;              // delay frame count
    int32_t  rollbackFrames;     // rollback frame count (both sides stay in sync)
    int32_t  effective_frame;    // absolute frame for the delay transition
};

struct BattleReadyPacket {
    uint32_t type;               // PACKET_BATTLE_READY
    uint32_t generation;         // distinguishes later rounds/rematches
    uint32_t flags;              // BattleSyncFlags advertised by the sender
    uint32_t round_seed;         // host-authoritative gameplay RNG seed
};

struct LaunchTrainingPacket {
    uint32_t type;               // PACKET_LAUNCH_TRAINING
    int32_t  p1Chars[3];
    int32_t  p2Chars[3];
    int32_t  stageId;
    uint32_t flags;              // LaunchTrainingFlags
};

enum LaunchTrainingFlags : uint32_t {
    LAUNCH_TRAINING_BOTH_CPUS = 1 << 0,
    LAUNCH_TRAINING_P2_CPU = 1 << 1,
};

enum DesyncAction : uint32_t {
    DESYNC_TRIGGER = 1,
    DESYNC_UNPAUSE = 2,
};

struct DesyncPacket {
    uint32_t type;               // PACKET_DESYNC
    uint32_t battle_generation;  // match epoch that produced this alert
    uint32_t action;             // DesyncAction
    int32_t  frame;
    uint32_t localChecksum;
    uint32_t remoteChecksum;
};

struct RestartMatchPacket {
    uint32_t type;               // PACKET_RESTART_MATCH
    int32_t  target_frame;       // synchronized net frame to restart on
    uint32_t round_seed;         // deterministic gameplay RNG seed
    uint32_t kind;               // RestartMatchKind
    int32_t  button_restart;     // native sAction button-restart value
};

enum RestartMatchKind : uint32_t {
    RESTART_FULL_MATCH = 0,
    RESTART_TRAINING = 1,
};

struct MultiResimPacket {
    uint32_t type;               // PACKET_MULTI_RESIM
    uint32_t enabled;            // 0 = disabled, 1 = enabled
};

struct DumpNdjsonPacket {
    uint32_t type;               // PACKET_DUMP_NDJSON
    int32_t  frame;              // final frame included in the rolling dump
};

enum AutoTestCaseType : uint32_t {
    AUTOTEST_CASE_MANUAL = 0,
    AUTOTEST_CASE_RUNTIME = 1,
};

enum AutoTestFrameFlags : uint32_t {
    AUTOTEST_FRAME_CHARACTERS_IDLE = 1u << 0,
};

struct AutoTestStartPacket {
    uint32_t type;               // PACKET_AUTOTEST_START
    uint32_t move_id;            // monotonically increasing suite case id
    uint32_t case_type;          // AutoTestCaseType
    int32_t  player_index;
    int32_t  matcher_index;
    int32_t  line_index;
    int32_t  variant_token;
    int32_t  manual_index;
    int32_t  setup_frame;
    int32_t  start_frame;
    uint32_t attacker_idle_action;
    uint32_t defender_idle_action;
    uint32_t facing_left;
    uint32_t round_seed;
    float    setup_position[4];     // host-authoritative defender position
    char     move_name[96];
};

struct AutoTestFramePacket {
    uint32_t type;               // PACKET_AUTOTEST_FRAME
    uint32_t move_id;
    int32_t  frame;
    uint32_t checksum;           // post-resimulation checksum
    uint32_t flags;              // AutoTestFrameFlags
};

struct AutoTestCompletePacket {
    uint32_t type;               // PACKET_AUTOTEST_COMPLETE
};

enum BattleSyncFlags : uint32_t {
    BATTLE_SYNC_READY   = 1u << 0,
    BATTLE_SYNC_RELEASE = 1u << 1,
    BATTLE_SYNC_SEED    = 1u << 2,
    BATTLE_SYNC_FIGHTING = 1u << 3,
};

static constexpr int MAX_RESIM_FRAMES = 16;

struct ResimCharState {
    uint32_t actionId;
    float    posX, posY, posZ;
    uint32_t flags;
    uint32_t mHeld;
    uint64_t frameCnt;
    uint32_t rng[4];
};

struct ResimFrameResult {
    int32_t  frame;
    ResimCharState p1Pre;     // P1 (team 0) point character pre-resim state
    ResimCharState p1Post;    // P1 point character post-resim state
    ResimCharState p2Pre;     // P2 (team 1) point character pre-resim state
    ResimCharState p2Post;    // P2 point character post-resim state
    uint32_t usedInput;
    uint32_t recvInput;
};

struct ResimNotifyPacket {
    uint32_t type;               // PACKET_RESIM_NOTIFY
    int32_t  resimStartFrame;    // first frame of re-simulation
    int32_t  resimEndFrame;      // end frame (exclusive)
    uint32_t correctedTeam;      // which team (0 or 1) had its input corrected
    uint32_t frameCount;         // number of valid entries in frames[]
    ResimFrameResult frames[MAX_RESIM_FRAMES];
};

struct GroundTruthFrame {
    int32_t  frame;
    ResimCharState p1Live;
    ResimCharState p2Live;
};

struct GroundTruthPacket {
    uint32_t         type;        // PACKET_GROUND_TRUTH
    int32_t          startFrame;
    int32_t          endFrame;
    uint32_t         frameCount;
    GroundTruthFrame frames[MAX_RESIM_FRAMES];
};
#pragma pack(pop)

// Hand over the ENet host/peer and start the service thread.
void Netplay_Init(ENetHost* host, ENetPeer* peer);

// Send a single raw uint32 input value over the wire (wraps into InputPacket).
void Netplay_SendInput(uint32_t rawInput);

// Queue a full history packet for the service thread to send.
void Netplay_SendPacket(const InputPacket& pkt);

// Retrieve the most recently received inbound packet. Returns true if data
// was available (the packet is consumed / cleared by this call).
bool Netplay_GetInboundPacket(InputPacket& outPkt);

// Returns true if netplay is active and the peer is still connected.
bool Netplay_IsActive();
bool Netplay_TakePeerDisconnected();

// Returns true if this instance is the netplay host (server), false if client.
bool Netplay_IsHost();

// Returns the peer's mean round-trip time (ping) in milliseconds, or 0 if inactive.
uint32_t Netplay_GetRTT();

// Request a peer timeout change on the ENet service thread.
void Netplay_SetTimeout(uint32_t timeoutMs);

// Compatibility queries (return first slot of latest inbound packet)
bool     Netplay_HasRemoteInput();
uint32_t Netplay_GetRemoteInput();

// Shut down the service thread, disconnect peer, destroy host.
void Netplay_Shutdown();

// Send a delay-control value to the remote peer.
void Netplay_SendDelay(int32_t delay, int32_t effectiveFrame = -1);

// Send a rollback-frame control value to the remote peer.
void Netplay_SendRollback(int32_t rollbackFrames);

// Send a resim-notify packet to the remote peer — signals which frames were
// re-simulated due to a late-arriving input correction.  The caller should
// fill ringIdx fields (correctedTeam, frameCount, frames[]) before sending.
void Netplay_SendResimNotify(const ResimNotifyPacket& pkt);

// Retrieve the last resim-notify received from the remote peer.  Returns true
// if a new notification was available (consumed by this call).
bool Netplay_GetInboundResimNotify(ResimNotifyPacket& outNotify);

// Retrieve the last delay value received from the remote peer.  Returns true
// if a new value was available (it is consumed by this call).
bool Netplay_GetInboundDelay(int32_t& outDelay, int32_t& outEffectiveFrame);

// Send a ground-truth packet to the remote peer — provides the non-simulating
// side's exact live state for the requested frame range.
void Netplay_SendGroundTruth(const GroundTruthPacket& pkt);

// Retrieve the last ground-truth packet received from the remote peer.
// Returns true if a new packet was available (consumed by this call).
bool Netplay_GetInboundGroundTruth(GroundTruthPacket& outGT);

// Retrieve the last rollback value received from the remote peer.  Returns true
// if a new value was available (it is consumed by this call).
bool Netplay_GetInboundRollback(int32_t& outRollback);

// Startup rendezvous used after local intro initialization has completed.
void Netplay_SendBattleReady(uint32_t generation, uint32_t flags, uint32_t roundSeed);
bool Netplay_GetInboundBattleReady(uint32_t& outGeneration, uint32_t& outFlags,
                                   uint32_t& outRoundSeed);

// Synchronize the desync warning and its P-key dismissal with the peer.
void Netplay_SendDesyncTrigger(int32_t frame, uint32_t localChecksum,
                               uint32_t remoteChecksum);
void Netplay_SendDesyncUnpause();
bool Netplay_GetInboundDesync(DesyncPacket& outPacket);

void Netplay_SendRestartMatch(int32_t targetFrame, uint32_t roundSeed);
void Netplay_SendTrainingRestart(int32_t targetFrame, uint32_t roundSeed,
                                 int32_t buttonRestart);
bool Netplay_GetInboundRestartMatch(int32_t& outTargetFrame, uint32_t& outRoundSeed,
                                    uint32_t& outKind, int32_t& outButtonRestart);

// Synchronize the multi-resimulation mode with the remote peer.
void Netplay_SendMultiResim(bool enabled);
bool Netplay_GetInboundMultiResim(bool& outEnabled);

// Request that both peers dump their current rolling NDJSON history.
void Netplay_SendDumpNDJSON(int32_t frame);
bool Netplay_GetInboundDumpNDJSON(int32_t& outFrame);

// Host starts each suite case; the client reports every post-resim frame.
void Netplay_SendAutoTestStart(const AutoTestStartPacket& packet);
bool Netplay_GetInboundAutoTestStart(AutoTestStartPacket& outPacket);
void Netplay_SendAutoTestFrame(const AutoTestFramePacket& packet);
bool Netplay_GetInboundAutoTestFrame(AutoTestFramePacket& outPacket);
void Netplay_SendAutoTestComplete();
bool Netplay_GetInboundAutoTestComplete();

// Discard input packets produced before the synchronized battle boundary.
void Netplay_ClearInputQueues();

// --- Desync-check: checksum is piggybacked on InputPacket (Slippi-style) ---
