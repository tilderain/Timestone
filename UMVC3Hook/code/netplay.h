#pragma once
#include <cstddef>
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
    PACKET_BATTLE_READY = 5,  // BattleReadyPacket
    PACKET_LAUNCH_TRAINING = 6, // LaunchTrainingPacket
    PACKET_DESYNC = 7,  // DesyncPacket
    PACKET_RESTART_MATCH = 8, // RestartMatchPacket
    PACKET_MULTI_RESIM = 9, // MultiResimPacket
    PACKET_SNAPDUMP = 10, // SnapDumpPacket
    PACKET_AUTOTEST_START = 11, // AutoTestStartPacket
    PACKET_AUTOTEST_FRAME = 12, // AutoTestFramePacket
    PACKET_AUTOTEST_COMPLETE = 13, // AutoTestCompletePacket
    PACKET_LAUNCH_P2_SELECTION = 14, // LaunchP2SelectionPacket
    PACKET_LAUNCH_P1_SELECTION = 15, // LaunchP1SelectionPacket
    PACKET_EXTENSION = 16, // Plugin feature packet routed through generic reliable queue
    PACKET_PLAYER_DISCONNECT = 17, // PlayerDisconnectPacket
    PACKET_LAUNCH_CHAT = 18, // LaunchChatPacket
};

#pragma pack(push, 1)
constexpr size_t NET_HISTORY_SIZE = 16;
constexpr size_t NET_MAX_PACKET_SIZE = 2 * 1024;
constexpr size_t NET_MAX_WAITING_DATA = 256 * 1024;

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
    int32_t  effective_frame;    // absolute frame for the included setting change
};

struct BattleReadyPacket {
    uint32_t type;               // PACKET_BATTLE_READY
    uint32_t generation;         // distinguishes later rounds/rematches
    uint32_t flags;              // BattleSyncFlags advertised by the sender
    uint32_t round_seed;         // host-authoritative gameplay RNG seed
    uint32_t opening_member;     // owned team's synchronized opening member (0..2)
};

struct LaunchTrainingPacket {
    uint32_t type;               // PACKET_LAUNCH_TRAINING
    int32_t  p1Chars[3];
    int32_t  p2Chars[3];
    int32_t  p1Colors[3];        // zero-based archive variant IDs
    int32_t  p2Colors[3];
    int32_t  p1Assists[3];       // 0=Alpha, 1=Beta, 2=Gamma
    int32_t  p2Assists[3];
    uint32_t simpleMode[2];
    uint32_t autoJump[2];
    uint32_t autoJumpValidMask;   // bit 0=P1 value valid, bit 1=P2 value valid
    uint8_t  voiceLanguage[2][3]; // 0=Japanese, 1=English
    uint8_t  voiceValidMask;      // bit 0=P1 values valid, bit 1=P2 values valid
    int32_t  stageId;
    uint32_t flags;              // LaunchTrainingFlags
};

struct LaunchP2SelectionPacket {
    uint32_t type;               // PACKET_LAUNCH_P2_SELECTION
    char protocol[32];           // UMVC3HOOK_VERSION
    int32_t  chars[3];
    int32_t  colors[3];          // zero-based archive variant IDs
    int32_t  assists[3];         // 0=Alpha, 1=Beta, 2=Gamma
    uint32_t simpleMode;
    uint32_t ready;
};

struct LaunchP1SelectionPacket {
    uint32_t type;               // PACKET_LAUNCH_P1_SELECTION
    char protocol[32];           // UMVC3HOOK_VERSION
    int32_t  chars[3];
    int32_t  colors[3];          // zero-based archive variant IDs
    int32_t  assists[3];         // 0=Alpha, 1=Beta, 2=Gamma
    uint32_t simpleMode;
    int32_t  stageId;
};

constexpr size_t LAUNCH_CHAT_MAX_MESSAGE = 256;

struct LaunchChatPacket {
    uint32_t type;               // PACKET_LAUNCH_CHAT
    uint16_t length;             // bytes used in message; no terminator required
    uint16_t reserved;           // must be zero
    char message[LAUNCH_CHAT_MAX_MESSAGE];
};
static_assert(sizeof(LaunchChatPacket) == 264);

enum LaunchTrainingFlags : uint32_t {
    LAUNCH_TRAINING_BOTH_CPUS = 1 << 0,
    LAUNCH_TRAINING_P2_CPU = 1 << 1,
    LAUNCH_TRAINING_FIVE_BARS = 1 << 2,
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
    uint32_t battle_generation;  // match epoch that requested this restart
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

struct SnapDumpPacket {
    uint32_t type;               // PACKET_SNAPDUMP
    int32_t  frame;              // final frame included in the rolling dump
    char timestamp[16];          // YYYYMMDD_HHMMSS from the requesting peer
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
    uint32_t fill_hyper_gauge;
    float    setup_positions[2][4]; // host-authoritative fighter positions
    uint8_t  shot_id_pool[0x404];   // host-authoritative sShot +0x38 state
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

struct PlayerDisconnectPacket {
    uint32_t type;               // PACKET_PLAYER_DISCONNECT
};

enum BattleSyncFlags : uint32_t {
    BATTLE_SYNC_READY   = 1u << 0,
    BATTLE_SYNC_RELEASE = 1u << 1,
    BATTLE_SYNC_SEED    = 1u << 2,
    BATTLE_SYNC_FIGHTING = 1u << 3,
    BATTLE_SYNC_OPENING_READY = 1u << 4,
    BATTLE_SYNC_OPENING_RELEASE = 1u << 5,
};

#pragma pack(pop)

static_assert(sizeof(BattleReadyPacket) == 20);
static_assert(sizeof(AutoTestStartPacket) <= NET_MAX_PACKET_SIZE,
              "largest netplay packet exceeds ENet packet limit");

// Restrict ENet's allocation limits to the small payloads used by this protocol.
void Netplay_ConfigureHostLimits(ENetHost* host);

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

// Notify the peer before a user-requested shutdown. Waits only until the
// service thread has handed the reliable packet to ENet, not for a peer ACK.
bool Netplay_SendPlayerDisconnect(uint32_t timeoutMs = 100);
bool Netplay_TakePlayerDisconnectNotice();

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
void Netplay_SendRollback(int32_t rollbackFrames, int32_t effectiveFrame = -1);

// Retrieve the last delay value received from the remote peer.  Returns true
// if a new value was available (it is consumed by this call).
bool Netplay_GetInboundDelay(int32_t& outDelay, int32_t& outEffectiveFrame);

// Retrieve the last rollback value received from the remote peer.  Returns true
// if a new value was available (it is consumed by this call).
bool Netplay_GetInboundRollback(int32_t& outRollback, int32_t& outEffectiveFrame);

// Startup rendezvous for opening-member selection and post-init intro readiness.
void Netplay_SendBattleReady(uint32_t generation, uint32_t flags, uint32_t roundSeed,
                             uint32_t openingMember);
bool Netplay_GetInboundBattleReady(uint32_t& outGeneration, uint32_t& outFlags,
                                   uint32_t& outRoundSeed, uint32_t& outOpeningMember);

// Exchange post-savedata team preferences as part of the launch settings.
void Netplay_SendLaunchTrainingSettings(const LaunchTrainingPacket& packet);
bool Netplay_GetInboundLaunchTrainingSettings(LaunchTrainingPacket& outPacket);

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
bool Netplay_HasInboundRestartMatch();

// Generic reliable control channel for self-contained plugin features. Payloads
// must begin with PACKET_EXTENSION and are limited to 256 bytes.
bool Netplay_SendExtensionPacket(const void* data, size_t size);
bool Netplay_GetExtensionPacket(void* data, size_t capacity, size_t& outSize);

// Synchronize the multi-resimulation mode with the remote peer.
void Netplay_SendMultiResim(bool enabled);
bool Netplay_GetInboundMultiResim(bool& outEnabled);

// In dual training only, request that the peer write snapdump history using the
// requester's timestamp.
void Netplay_SendSnapDump(int32_t frame, const char* timestamp);
bool Netplay_GetInboundSnapDump(int32_t& outFrame, char* timestamp,
                                size_t timestampSize);

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
