#pragma once
#include <cstddef>
#include <cstdint>
#include <enet/enet.h>

// Netplay packet formats and the game-thread/service-thread interface.
// Input history carries button-remapped masks on a shared absolute-frame timeline.
// controller.cpp schedules delayed input, predicts missing input, and requests rollback.
// Confirmed state checksums ride on input packets; control packets synchronize alerts.
// The service thread handles ENet I/O through lock-protected packet exchange.

// First uint32_t of each protocol packet.
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
    PACKET_EXTENSION = 16, // Generic plugin feature payload
    PACKET_PLAYER_DISCONNECT = 17, // PlayerDisconnectPacket
    PACKET_LAUNCH_CHAT = 18, // LaunchChatPacket
    PACKET_BENCHMARK = 19, // Benchmark payload on the extension channel
};

#pragma pack(push, 1)
constexpr size_t NET_HISTORY_SIZE = 16;
constexpr size_t NET_MAX_PACKET_SIZE = 2 * 1024;
constexpr size_t NET_MAX_WAITING_DATA = 256 * 1024;

struct InputPacket {
    uint32_t type;               // PACKET_INPUT
    uint32_t battle_generation;  // match epoch; reject stale packets
    uint32_t start_frame;        // inputs[i] executes at start_frame + i
    uint32_t checksum_frame;     // confirmed frame, UINT32_MAX if absent
    uint32_t checksum;           // character/gameplay-RNG checksum for checksum_frame
    uint32_t inputs[NET_HISTORY_SIZE];
    int32_t  frame_advantage;    // sender's estimate; negative means ahead
    uint32_t flags;              // 0 = seed packet, non-zero = real packet
};

struct DelayPacket {
    uint32_t type;               // PACKET_DELAY
    int32_t  delay;              // input delay in frames, -1 if unchanged
    int32_t  rollbackFrames;     // prediction window in frames, -1 if unchanged
    int32_t  effective_frame;    // absolute change frame, -1 if unspecified
};

struct BattleReadyPacket {
    uint32_t type;               // PACKET_BATTLE_READY
    uint32_t generation;         // battle synchronization epoch
    uint32_t flags;              // BattleSyncFlags
    uint32_t round_seed;         // host-authoritative gameplay RNG seed
    uint32_t opening_member;     // sender's starting team member (0..2)
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
    uint32_t autoJumpValidMask;   // valid teams: bit 0=P1, bit 1=P2
    uint8_t  voiceLanguage[2][3]; // 0=Japanese, 1=English
    uint8_t  voiceValidMask;      // valid teams: bit 0=P1, bit 1=P2
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
    uint32_t battle_generation;  // match epoch; reject stale alerts
    uint32_t action;             // DesyncAction
    int32_t  frame;
    uint32_t localChecksum;
    uint32_t remoteChecksum;
};

struct RestartMatchPacket {
    uint32_t type;               // PACKET_RESTART_MATCH
    uint32_t battle_generation;  // match epoch; reject stale restart requests
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
    uint32_t move_id;            // increasing suite case ID
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

// Cap ENet packet size and queued receive data for this protocol.
void Netplay_ConfigureHostLimits(ENetHost* host);

// Take ownership of the ENet host/peer and start the service thread.
void Netplay_Init(ENetHost* host, ENetPeer* peer);

// Legacy wrapper: queue a packet with rawInput in inputs[0] at frame 0.
void Netplay_SendInput(uint32_t rawInput);

// Replace the pending outbound history; the service thread sends and resends it.
void Netplay_SendPacket(const InputPacket& pkt);

// Consume the oldest queued input packet; return false if the queue is empty.
bool Netplay_GetInboundPacket(InputPacket& outPkt);

// Whether the netplay service loop is running.
bool Netplay_IsActive();
// Consume a transport-disconnect notification.
bool Netplay_TakePeerDisconnected();

// Queue a reliable disconnect notice; wait for the send attempt, not a peer ACK.
bool Netplay_SendPlayerDisconnect(uint32_t timeoutMs = 100);
// Consume a user-requested disconnect notice from the peer.
bool Netplay_TakePlayerDisconnectNotice();

// Whether this instance is the host rather than the client.
bool Netplay_IsHost();

// Transport RTT estimate in milliseconds, or 0 if inactive.
uint32_t Netplay_GetRTT();

// Mean of up to 40 RTT samples taken at least 16 ms apart, or 0 if inactive.
uint32_t Netplay_GetAverageRTT();

// Transport loss estimate in percent, or -1 if unavailable. Steam includes reordering.
float Netplay_GetPacketLoss();

// Steam peer-reported loss/reordering in percent, or -1 if unavailable.
float Netplay_GetRemotePacketLoss();

// Peak packet jitter over a 500 ms window, in milliseconds, or -1 if unavailable.
int32_t Netplay_GetJitter();

// Request a peer timeout change on the ENet service thread.
void Netplay_SetTimeout(uint32_t timeoutMs);

// Legacy accessors: test for queued input and peek at its oldest packet's inputs[0].
bool     Netplay_HasRemoteInput();
uint32_t Netplay_GetRemoteInput();

// Stop the service thread, disconnect the peer, and destroy the host.
void Netplay_Shutdown();

// Queue an input-delay change for the peer, optionally at a shared frame.
void Netplay_SendDelay(int32_t delay, int32_t effectiveFrame = -1);

// Queue a prediction-window change for the peer, optionally at a shared frame.
void Netplay_SendRollback(int32_t rollbackFrames, int32_t effectiveFrame = -1);

// Consume the latest received delay change; return false if none is pending.
bool Netplay_GetInboundDelay(int32_t& outDelay, int32_t& outEffectiveFrame);

// Consume the latest received rollback change; return false if none is pending.
bool Netplay_GetInboundRollback(int32_t& outRollback, int32_t& outEffectiveFrame);

// Exchange battle readiness, RNG seed, and opening-member selection.
void Netplay_SendBattleReady(uint32_t generation, uint32_t flags, uint32_t roundSeed,
                             uint32_t openingMember);
bool Netplay_GetInboundBattleReady(uint32_t& outGeneration, uint32_t& outFlags,
                                   uint32_t& outRoundSeed, uint32_t& outOpeningMember);

// Exchange launch settings and saved team preferences.
void Netplay_SendLaunchTrainingSettings(const LaunchTrainingPacket& packet);
bool Netplay_GetInboundLaunchTrainingSettings(LaunchTrainingPacket& outPacket);

// Synchronize desync alerts and their dismissal.
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

// Queue feature payloads up to 256 bytes, reliable by default.
// The first uint32_t must be PACKET_EXTENSION or PACKET_BENCHMARK.
bool Netplay_SendExtensionPacket(const void* data, size_t size, bool reliable = true);
bool Netplay_GetExtensionPacket(void* data, size_t capacity, size_t& outSize);

// Synchronize the multi-resimulation mode with the remote peer.
void Netplay_SendMultiResim(bool enabled);
bool Netplay_GetInboundMultiResim(bool& outEnabled);

// Request a dual-training history dump using the requester's timestamp.
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

// Clear inbound/outbound input and desync messages at a battle boundary.
void Netplay_ClearInputQueues();
