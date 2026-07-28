#include "../pch.h"
#include "autotest.h"
#include "controller.h"
#include "mk10utils.h"
#include "rollback.h"
#include "netplay.h"
#include "framestepper.h"
#include "../kiero/minhook/include/MinHook.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr uintptr_t ADDR_UPDATE_COMMAND_MATCHER = 0x140028D10;
constexpr uintptr_t ADDR_GET_COMMAND_BY_LINE = 0x14005B630;
constexpr uintptr_t ADDR_GET_COMMAND_STEP = 0x14005AD40;
constexpr uintptr_t ADDR_GET_CHARACTER_NAME = 0x140058F90;
constexpr uintptr_t ADDR_GET_CHARACTER_ID_FROM_DTI = 0x140059250;
constexpr uintptr_t ADDR_GET_RESOURCE_MANAGER = 0x140001B10;
constexpr uintptr_t ADDR_GET_MVC3_MANAGER = 0x140001AF0;
constexpr uintptr_t ADDR_LOAD_GLOBAL_ARCHIVE = 0x14025AD30;
constexpr uintptr_t DTI_RMSGDATA = 0x140D52E30;
constexpr uintptr_t PTR_SCHARACTER = 0x140D44A70;
constexpr uintptr_t PTR_COMMAND_LIST_ROW_TABLES = 0x140B16960;
constexpr uintptr_t PTR_SMVC3_MANAGER = 0x140D510A0;
constexpr uintptr_t PTR_SREPLAY = 0x140D4D9D8;
constexpr uintptr_t SMVC3_MANAGER_ACTION_OFFSET = 0x48;
constexpr uintptr_t ADDR_SET_RESTART_MODE = 0x14000AF90;
constexpr uintptr_t ADDR_SET_CHARACTER_POSITION = 0x140002AF0;
constexpr uintptr_t ADDR_REPLAY_ROUND_START = 0x1401CFA90;
constexpr uintptr_t PTR_SHOT_LISTS = 0x140D47F90;
constexpr uintptr_t MVC3_MANAGER_CUTIN_OFFSET = 0xB50;
constexpr uintptr_t TEAM_OFFSETS[] = {0x40, 0x300};
constexpr uintptr_t TEAM_ACTIVE_CHARACTER_OFFSET = 0x48;
constexpr uintptr_t COMMAND_MATCHER_OFFSETS[] = {0x2738, 0x2950};

struct InputStep {
    uint32_t mask;
    int32_t frames;
};

struct InputScript {
    const char* name;
    const InputStep* steps;
    size_t stepCount;
};

// Directions are authored facing right. AutoTest_GetInput mirrors left/right
// when the test starts with the character facing left.
constexpr InputStep QCF_LIGHT[] = {
    {IN_DOWN, 2},
    {IN_DOWN | IN_RIGHT, 2},
    {IN_RIGHT | IN_X, 1},
    {0, 45},
};

constexpr InputStep QCB_LIGHT[] = {
    {IN_DOWN, 2},
    {IN_DOWN | IN_LEFT, 2},
    {IN_LEFT | IN_X, 1},
    {0, 45},
};

constexpr InputStep DP_LIGHT[] = {
    {IN_RIGHT, 2},
    {IN_DOWN, 2},
    {IN_DOWN | IN_RIGHT | IN_X, 1},
    {0, 45},
};

constexpr InputStep FORWARD_DASH[] = {
    {IN_RIGHT, 1},
    {0, 1},
    {IN_RIGHT, 1},
    {0, 35},
};

constexpr InputStep FORWARD_THROW[] = {
    {IN_RIGHT, 6},
    {IN_RIGHT | IN_B, 2},
    {0, 45},
};

constexpr InputStep BACK_THROW[] = {
    {IN_RIGHT, 12},
    {IN_LEFT | IN_B, 2},
    {0, 45},
};

constexpr InputScript SCRIPTS[] = {
    {"QCF + Light", QCF_LIGHT, std::size(QCF_LIGHT)},
    {"QCB + Light", QCB_LIGHT, std::size(QCB_LIGHT)},
    {"DP + Light", DP_LIGHT, std::size(DP_LIGHT)},
    {"Forward Dash", FORWARD_DASH, std::size(FORWARD_DASH)},
};

constexpr InputScript SUITE_MANUAL_SCRIPTS[] = {
    {"Forward Throw", FORWARD_THROW, std::size(FORWARD_THROW)},
    {"Back Throw", BACK_THROW, std::size(BACK_THROW)},
};

AutoTestStatus g_status = {
    false, 0, 0, -1, -1, -1, 0, -1, -1, -1, -1, -1, 0,
};
bool g_facingLeft = false;
constexpr int32_t MAX_RUNTIME_INPUT_STEPS = 256;
constexpr int32_t MAX_RUNTIME_COMMAND_NAMES = 512;
InputStep g_runtimeInputSteps[MAX_RUNTIME_INPUT_STEPS] = {};
InputScript g_runtimeScript = {"Runtime command", g_runtimeInputSteps, 0};
bool g_runningRuntimeCommand = false;
bool g_runningRuntimeHyper = false;
int32_t g_runningManualCommand = -1;
char g_runtimeExecutionError[160] = {};

enum class SuitePhase {
    Idle,
    StartCommand,
    RunningCommand,
    WaitingForCompletion,
    WaitingForReset,
};

AutoTestRuntimeCommand g_suiteCommands[AUTOTEST_MAX_RUNTIME_COMMANDS] = {};
AutoTestSuiteStatus g_suiteStatus = {};
SuitePhase g_suitePhase = SuitePhase::Idle;
bool g_suiteFacingLeft = false;
bool g_suiteMatchedCommand = false;
bool g_suiteManualCase = false;
float g_suiteDefenderLife = 0.0f;
int32_t g_suiteDeadline = -1;
uint32_t g_suiteIdleActions[2] = {};
uint32_t g_suiteRngSeed = 0;
char g_suiteResultPath[MAX_PATH] = {};
char g_suiteTestDate[32] = {};
char g_suiteCharacterName[64] = {};
char g_suiteCharacterStem[64] = {};
const char* g_suiteType = "commands";
constexpr int32_t SUITE_COMMAND_TIMEOUT = 600;
constexpr int32_t SUITE_COMPLETION_TIMEOUT = 3600;
constexpr int32_t SUITE_QUIET_FRAMES = 20;
constexpr int32_t NET_SUITE_STABLE_FRAMES = 3;
constexpr int32_t NET_SUITE_DESYNC_FRAMES = 1;
constexpr int32_t NET_SUITE_HISTORY = 512;

struct NetSuiteFrame {
    int32_t frame;
    uint32_t checksum;
    bool charactersIdle;
};

bool g_netSuiteActive = false;
bool g_netSuiteClient = false;
bool g_netSuiteRestartPending = false;
bool g_netSuiteRestartExecuted = false;
bool g_netSuiteRestartLeftFighting = false;
uint32_t g_netSuiteMoveId = 0;
int32_t g_netSuiteStartFrame = -1;
int32_t g_netSuiteSetupFrame = -1;
bool g_netSuiteSetupApplied = false;
int32_t g_netSuiteLastForcedFrame = -1;
int32_t g_netSuiteLastComparedFrame = -1;
int32_t g_netSuiteStableFrames = 0;
int32_t g_netSuiteMismatchFrames = 0;
int32_t g_netSuiteComparedFrames = 0;
int32_t g_netSuiteFirstMismatchFrame = -1;
uint32_t g_netSuiteFirstHostChecksum = 0;
uint32_t g_netSuiteFirstClientChecksum = 0;
NetSuiteFrame g_netSuiteLiveFrames[NET_SUITE_HISTORY] = {};
AutoTestFramePacket g_netSuiteRemoteFrames[NET_SUITE_HISTORY] = {};

struct RuntimeCommandNameCache {
    uint64_t resource;
    uint64_t command;
    char name[96];
};

RuntimeCommandNameCache g_runtimeCommandNames[MAX_RUNTIME_COMMAND_NAMES] = {};

struct CommandListTextCache {
    int32_t characterId;
    uint64_t resource;
    int32_t rowCount;
    uint64_t rowMetadata;
    uint64_t rowNotation;
    bool textDumped;
    uint64_t dumpedCommandResources[2];
    char path[128];
};

CommandListTextCache g_commandListText[2] = {
    {-1, 0, 0, 0, 0, false, {}, {}},
    {-1, 0, 0, 0, 0, false, {}, {}},
};

struct CommandNotation {
    int32_t directions[16];
    int32_t directionCount;
    uint32_t buttons;
    int32_t attackCount;
};

CommandNotation g_suiteCommandNotations[AUTOTEST_MAX_RUNTIME_COMMANDS] = {};
bool g_suiteCommandHasNotation[AUTOTEST_MAX_RUNTIME_COMMANDS] = {};

using UpdateCommandMatcher_t = void(__fastcall*)(uint64_t commandMatcher);
UpdateCommandMatcher_t oUpdateCommandMatcher = nullptr;

uint64_t GetActiveCharacter(int32_t playerIndex)
{
    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    return characters
        ? *(uint64_t*)(characters + TEAM_OFFSETS[playerIndex] +
                      TEAM_ACTIVE_CHARACTER_OFFSET)
        : 0;
}

uint64_t GetAction()
{
    uint64_t manager = *(uint64_t*)_addr(PTR_SMVC3_MANAGER);
    return manager ? *(uint64_t*)(manager + SMVC3_MANAGER_ACTION_OFFSET) : 0;
}

float GetCharacterLife(uint64_t character)
{
    return character ? *(float*)(character + 0x459) : 0.0f;
}

int32_t CountActiveShots()
{
    constexpr int32_t SHOT_LIST_COUNT = 10;
    constexpr int32_t SHOT_LIST_STRIDE = 0x28;
    constexpr uintptr_t SHOT_NODE_NEXT = 0x10;
    int32_t count = 0;
    for (int32_t list = 0; list < SHOT_LIST_COUNT; ++list) {
        uint64_t sentinel = _addr(PTR_SHOT_LISTS) + list * SHOT_LIST_STRIDE;
        uint64_t sentinelRef = sentinel + 0x08;
        uint64_t node = *(uint64_t*)(sentinel + SHOT_NODE_NEXT);
        for (int32_t safety = 128;
             node && node != sentinelRef && safety-- > 0;
             node = *(uint64_t*)(node + SHOT_NODE_NEXT))
            ++count;
    }
    return count;
}

bool HasActiveCutin()
{
    uint64_t manager = *(uint64_t*)_addr(PTR_SMVC3_MANAGER);
    return manager && *(uint64_t*)(manager + MVC3_MANAGER_CUTIN_OFFSET);
}

bool CharacterActionsAreIdle(uint64_t attacker, uint64_t defender)
{
    if (!attacker || !defender)
        return false;

    uint32_t actions[2] = {*(uint32_t*)(attacker + 0x1310),
                           *(uint32_t*)(defender + 0x1310)};
    g_suiteStatus.attackerAction = actions[0];
    g_suiteStatus.defenderAction = actions[1];
    return actions[0] == g_suiteIdleActions[0] &&
           actions[1] == g_suiteIdleActions[1];
}

void PlaceDefenderNextToAttacker(int32_t attackerIndex)
{
    uint64_t attacker = GetActiveCharacter(attackerIndex);
    uint64_t defender = GetActiveCharacter(attackerIndex ^ 1);
    if (!attacker || !defender)
        return;

    float position[4] = {
        *(float*)(attacker + 0x50) + (g_suiteFacingLeft ? -0.25f : 0.25f),
        *(float*)(attacker + 0x54),
        *(float*)(attacker + 0x58),
        0.0f,
    };
    using SetPosition_t = void(__fastcall*)(uint64_t, const float*);
    ((SetPosition_t)_addr(ADDR_SET_CHARACTER_POSITION))(defender, position);
}

void FillAttackerHyperGauge(int32_t attackerIndex)
{
    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (characters)
        *(float*)(characters + TEAM_OFFSETS[attackerIndex] + 0x78) = 50000.0f;
}

void RequestTrainingReset()
{
    uint64_t action = GetAction();
    if (!action)
        return;

    using SetRestartMode_t = void(__fastcall*)(uint64_t, int32_t);
    ((SetRestartMode_t)_addr(ADDR_SET_RESTART_MODE))(action, 3);
}

bool ResetSuiteRng()
{
    uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
    if (!replay)
        return false;

    *(uint32_t*)(replay + 0x23424) = g_suiteRngSeed;
    *(uint32_t*)(replay + 0x58) = g_suiteRngSeed;
    using ReplayRoundStart_t = void(__fastcall*)(uint64_t);
    ((ReplayRoundStart_t)_addr(ADDR_REPLAY_ROUND_START))(replay);
    return true;
}

void InitializeSuiteResultFile(int32_t playerIndex, bool deduplicate)
{
    g_suiteResultPath[0] = '\0';
    g_suiteTestDate[0] = '\0';
    g_suiteCharacterName[0] = '\0';
    g_suiteCharacterStem[0] = '\0';
    g_suiteType = deduplicate ? "commands" : "variants";

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    snprintf(g_suiteTestDate, sizeof(g_suiteTestDate),
             "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds);

    int32_t characterId = playerIndex >= 0 && playerIndex <= 1
        ? g_commandListText[playerIndex].characterId : -1;
    using GetCharacterName_t = const char*(__fastcall*)(int32_t);
    static GetCharacterName_t getCharacterName =
        (GetCharacterName_t)_addr(ADDR_GET_CHARACTER_NAME);
    const char* characterName = characterId > 0
        ? getCharacterName(characterId) : nullptr;
    snprintf(g_suiteCharacterName, sizeof(g_suiteCharacterName), "%s",
             characterName && characterName[0] ? characterName : "Unknown");

    char safeCharacter[64] = {};
    size_t safeLength = 0;
    for (const char* c = g_suiteCharacterName; *c &&
         safeLength + 1 < sizeof(safeCharacter); ++c) {
        char value = *c;
        bool valid = (value >= 'a' && value <= 'z') ||
                     (value >= 'A' && value <= 'Z') ||
                     (value >= '0' && value <= '9') || value == '-' ||
                     value == '_';
        if (value >= 'A' && value <= 'Z')
            value = (char)(value - 'A' + 'a');
        safeCharacter[safeLength++] = valid ? value : '_';
    }
    if (!safeCharacter[0])
        snprintf(safeCharacter, sizeof(safeCharacter), "Unknown");
    snprintf(g_suiteCharacterStem, sizeof(g_suiteCharacterStem), "%s",
             safeCharacter);

    if (!CreateDirectoryA("autotest", nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        HookLog("AutoTestSuite: could not create ./autotest (error=%lu)\n",
                GetLastError());
        return;
    }

    snprintf(g_suiteResultPath, sizeof(g_suiteResultPath),
             "autotest/%04u%02u%02u_%02u%02u%02u_%03u_%s_%s.tsv",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds, safeCharacter, g_suiteType);
    FILE* file = fopen(g_suiteResultPath, "w");
    if (!file) {
        HookLog("AutoTestSuite: could not create %s\n", g_suiteResultPath);
        g_suiteResultPath[0] = '\0';
        return;
    }
    fprintf(file, "test_date\tcharacter\tsuite_type\tmove_id\tmove\tresult\tcompared_frames\tmismatch_frame\thost_checksum\tclient_checksum\treason\n");
    fclose(file);
    HookLog("AutoTestSuite: results -> %s (date=%s character=%s)\n",
            g_suiteResultPath, g_suiteTestDate, g_suiteCharacterName);
}

void WriteSuiteResult(bool passed, const char* reason)
{
    if (!g_suiteResultPath[0])
        return;

    bool network = g_netSuiteActive && Netplay_IsActive() && Netplay_IsHost();
    const char* outcome = network
        ? (passed ? "SYNC"
                  : (g_netSuiteFirstMismatchFrame >= 0 ? "DESYNC" : "ERROR"))
        : (passed ? "PASS" : "FAIL");
    uint32_t moveId = network ? g_netSuiteMoveId
                              : (uint32_t)g_suiteStatus.currentCommand + 1;
    FILE* file = fopen(g_suiteResultPath, "a");
    if (!file) {
        HookLog("AutoTestSuite: could not append %s\n", g_suiteResultPath);
        return;
    }
    fprintf(file, "%s\t%s\t%s\t%u\t%s\t%s\t%d\t%d\t0x%08X\t0x%08X\t%s\n",
            g_suiteTestDate, g_suiteCharacterName, g_suiteType, moveId,
            g_suiteStatus.currentName, outcome,
            network ? g_netSuiteComparedFrames : 0,
            network ? g_netSuiteFirstMismatchFrame : -1,
            network ? g_netSuiteFirstHostChecksum : 0,
            network ? g_netSuiteFirstClientChecksum : 0, reason);
    fclose(file);
}

void FinishSuiteCommand(bool passed, const char* reason)
{
    if (passed)
        ++g_suiteStatus.passedCount;
    else
        ++g_suiteStatus.failedCount;
    snprintf(g_suiteStatus.lastResult, sizeof(g_suiteStatus.lastResult),
             "%s: %s", g_suiteStatus.currentName, reason);
    HookLog("AutoTestSuite: command=%d/%d matcher=%d line=%d %s\n",
            g_suiteStatus.currentCommand + 1, g_suiteStatus.commandCount,
            g_suiteStatus.currentMatcher, g_suiteStatus.currentLine,
            g_suiteStatus.lastResult);

    g_status.active = false;
    g_status.currentInput = 0;
    g_runningRuntimeCommand = false;
    g_runningRuntimeHyper = false;
    g_runningManualCommand = -1;
    WriteSuiteResult(passed, reason);
    if (g_netSuiteActive && Netplay_IsActive() && Netplay_IsHost()) {
        if (!passed && g_netSuiteFirstMismatchFrame >= 0) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "autotest/%s_%u_host.ndjson",
                     g_suiteCharacterStem, g_netSuiteMoveId);
            Rollback_ExportNetplayHistory(g_netSuiteFirstMismatchFrame, path);
        }
        g_netSuiteRestartPending = true;
        AutoTest_PrepareForRestart();
        FrameStepper_ScheduleAutoTestRestart(g_suiteRngSeed);
    } else {
        RequestTrainingReset();
    }
    g_suitePhase = SuitePhase::WaitingForReset;
    g_suiteStatus.waitingForCompletion = false;
    g_suiteStatus.waitingForReset = true;
}

void ResetNetSuiteMoveState(uint32_t moveId, int32_t startFrame)
{
    g_netSuiteMoveId = moveId;
    g_netSuiteStartFrame = startFrame;
    g_netSuiteLastForcedFrame = startFrame - 1;
    g_netSuiteLastComparedFrame = startFrame - 1;
    g_netSuiteStableFrames = 0;
    g_netSuiteMismatchFrames = 0;
    g_netSuiteComparedFrames = 0;
    g_netSuiteFirstMismatchFrame = -1;
    g_netSuiteFirstHostChecksum = 0;
    g_netSuiteFirstClientChecksum = 0;
    g_netSuiteRestartExecuted = false;
    g_netSuiteRestartLeftFighting = false;
    for (NetSuiteFrame& entry : g_netSuiteLiveFrames)
        entry.frame = -1;
    for (AutoTestFramePacket& entry : g_netSuiteRemoteFrames)
        entry.frame = -1;
}

void ProcessNetSuiteSetup(int32_t frame)
{
    if (!g_netSuiteActive || g_netSuiteSetupApplied ||
        g_netSuiteSetupFrame < 0 || frame < g_netSuiteSetupFrame)
        return;
    if (!ResetSuiteRng()) {
        HookLog("AutoTestNet: move %u setup could not reset RNG\n",
                g_netSuiteMoveId);
        return;
    }
    PlaceDefenderNextToAttacker(g_suiteStatus.playerIndex);
    if (g_runningRuntimeHyper)
        FillAttackerHyperGauge(g_suiteStatus.playerIndex);
    g_netSuiteSetupApplied = true;
    HookLog("AutoTestNet: move %u setup applied at frame %d seed=0x%08X\n",
            g_netSuiteMoveId, frame, g_suiteRngSeed);
}

void ConfigureManualScript(int32_t manualIndex, int32_t playerIndex,
                           bool facingLeft, int32_t startFrame)
{
    g_status = {
        true, playerIndex, -1, startFrame,
        -1, -1, 0, -1, -1, -1, -1, -1, 0,
    };
    g_facingLeft = facingLeft;
    g_runningRuntimeCommand = false;
    g_runningManualCommand = manualIndex;
}

bool BothCharactersIdle()
{
    return CharacterActionsAreIdle(GetActiveCharacter(g_suiteStatus.playerIndex),
                                   GetActiveCharacter(g_suiteStatus.playerIndex ^ 1));
}

void StartInboundNetSuiteMove(const AutoTestStartPacket& packet)
{
    if (Netplay_IsHost() || packet.player_index != 0 ||
        packet.start_frame <= Controller_GetNetFrameCount()) {
        HookLog("AutoTestNet: rejected move %u start=%d current=%d player=%d\n",
                packet.move_id, packet.start_frame,
                Controller_GetNetFrameCount(), packet.player_index);
        return;
    }

    g_netSuiteActive = true;
    g_netSuiteClient = true;
    g_netSuiteRestartPending = false;
    g_suiteStatus = {};
    g_suiteStatus.active = true;
    g_suiteStatus.playerIndex = packet.player_index;
    g_suiteIdleActions[0] = packet.attacker_idle_action;
    g_suiteIdleActions[1] = packet.defender_idle_action;
    snprintf(g_suiteStatus.currentName, sizeof(g_suiteStatus.currentName),
             "%s", packet.move_name);
    g_suiteFacingLeft = packet.facing_left != 0;
    g_suiteRngSeed = packet.round_seed;
    bool configured = false;
    if (packet.case_type == AUTOTEST_CASE_MANUAL &&
        packet.manual_index >= 0 &&
        packet.manual_index < (int32_t)std::size(SUITE_MANUAL_SCRIPTS)) {
        ConfigureManualScript(packet.manual_index, packet.player_index,
                              g_suiteFacingLeft, packet.start_frame);
        configured = true;
    } else if (packet.case_type == AUTOTEST_CASE_RUNTIME) {
        configured = AutoTest_StartRuntimeCommand(packet.player_index,
                                                   packet.matcher_index,
                                                   packet.line_index,
                                                   g_suiteFacingLeft);
        if (configured)
            g_status.startFrame = packet.start_frame;
    }
    if (!configured) {
        HookLog("AutoTestNet: could not configure move %u: %s\n",
                packet.move_id, AutoTest_GetRuntimeExecutionError());
        g_netSuiteActive = false;
        g_suiteStatus.active = false;
        return;
    }

    // Inputs arrive through the normal host input stream. Keeping the script
    // configured gives diagnostics a move identity without injecting client input.
    g_status.active = false;
    ResetNetSuiteMoveState(packet.move_id, packet.start_frame);
    g_netSuiteSetupFrame = packet.setup_frame;
    g_netSuiteSetupApplied = false;
    HookLog("AutoTestNet: client armed move %u %s at frame %d\n",
            packet.move_id, packet.move_name, packet.start_frame);
}

void ProcessNetSuitePackets()
{
    AutoTestStartPacket start = {};
    while (Netplay_GetInboundAutoTestStart(start))
        StartInboundNetSuiteMove(start);

    if (!g_netSuiteActive || !Netplay_IsHost())
        return;

    AutoTestFramePacket report = {};
    while (Netplay_GetInboundAutoTestFrame(report)) {
        if (report.move_id != g_netSuiteMoveId || report.frame < g_netSuiteStartFrame)
            continue;
        g_netSuiteRemoteFrames[(uint32_t)report.frame % NET_SUITE_HISTORY] = report;
    }

    for (;;) {
        int32_t frame = g_netSuiteLastComparedFrame + 1;
        uint32_t idx = (uint32_t)frame % NET_SUITE_HISTORY;
        NetSuiteFrame& host = g_netSuiteLiveFrames[idx];
        const AutoTestFramePacket& client = g_netSuiteRemoteFrames[idx];
        if (host.frame != frame || client.frame != frame ||
            client.move_id != g_netSuiteMoveId ||
            !Controller_IsFrameConfirmed(frame))
            break;

        // The live callback may have cached speculative host state. Once input
        // is confirmed, read the checksum ring again so ordinary correction
        // rollback can replace it before comparison, matching the normal
        // confirmed-checksum desync path.
        uint32_t confirmedHostChecksum = 0;
        if (!Controller_GetFrameChecksum(frame, &confirmedHostChecksum))
            break;
        host.checksum = confirmedHostChecksum;

        ++g_netSuiteComparedFrames;
        g_netSuiteLastComparedFrame = frame;
        bool checksumMatch = host.checksum == client.checksum;
        g_netSuiteMismatchFrames = checksumMatch
            ? 0 : g_netSuiteMismatchFrames + 1;
        if (!checksumMatch && g_netSuiteFirstMismatchFrame < 0) {
            g_netSuiteFirstMismatchFrame = frame;
            g_netSuiteFirstHostChecksum = host.checksum;
            g_netSuiteFirstClientChecksum = client.checksum;
            HookLog("AutoTestNet: move %u mismatch frame=%d host=0x%08X client=0x%08X\n",
                    client.move_id, frame, host.checksum, client.checksum);
        }

        if (g_netSuiteMismatchFrames >= NET_SUITE_DESYNC_FRAMES &&
            (g_suitePhase == SuitePhase::RunningCommand ||
             g_suitePhase == SuitePhase::WaitingForCompletion)) {
            g_desyncDetected = true;
            g_desyncSeen = false;
            g_desyncFrame = frame;
            g_desyncLocalCS = host.checksum;
            g_desyncRemoteCS = client.checksum;
            Netplay_SendDesyncTrigger(frame, host.checksum, client.checksum);
            Rollback_NotifyDesyncTriggered(frame);
            HookLog("AutoTestNet: move %u persistent desync at frame %d after %d mismatches\n",
                    client.move_id, frame, g_netSuiteMismatchFrames);
            FinishSuiteCommand(false, "persistent desync detected");
            return;
        }

        bool bothIdle = host.charactersIdle &&
                        (client.flags & AUTOTEST_FRAME_CHARACTERS_IDLE) != 0;
        g_netSuiteStableFrames = checksumMatch && bothIdle
            ? g_netSuiteStableFrames + 1 : 0;
        g_suiteStatus.quietFrames = g_netSuiteStableFrames;
        if (g_netSuiteStableFrames >= NET_SUITE_STABLE_FRAMES &&
            g_suitePhase == SuitePhase::WaitingForCompletion) {
            char reason[96];
            snprintf(reason, sizeof(reason),
                     "checksums stable for %d idle frames",
                     NET_SUITE_STABLE_FRAMES);
            FinishSuiteCommand(true, reason);
            return;
        }
    }
}

uint32_t MirrorDirections(uint32_t input)
{
    if (!g_facingLeft)
        return input;

    const bool left = (input & IN_LEFT) != 0;
    const bool right = (input & IN_RIGHT) != 0;
    input &= ~(IN_LEFT | IN_RIGHT);
    if (left) input |= IN_RIGHT;
    if (right) input |= IN_LEFT;
    return input;
}

int32_t ScriptLength(const InputScript& script)
{
    int32_t length = 0;
    for (size_t i = 0; i < script.stepCount; ++i)
        length += script.steps[i].frames;
    return length;
}

uint32_t ScriptInputAt(const InputScript& script, int32_t scriptFrame)
{
    for (size_t i = 0; i < script.stepCount; ++i) {
        if (scriptFrame < script.steps[i].frames)
            return MirrorDirections(script.steps[i].mask);
        scriptFrame -= script.steps[i].frames;
    }
    return 0;
}

const InputScript& ActiveScript()
{
    if (g_runningRuntimeCommand)
        return g_runtimeScript;
    if (g_runningManualCommand >= 0)
        return SUITE_MANUAL_SCRIPTS[g_runningManualCommand];
    return SCRIPTS[g_status.scriptIndex];
}

const char* ActiveScriptName()
{
    return ActiveScript().name;
}

uint32_t FindRawInput(uint32_t requiredMask, uint32_t expected)
{
    // Directions have a fixed mapping and must not depend on the configurable
    // button table. Invert them directly, then search only physical buttons.
    uint32_t rawDirection = 0;
    if ((expected & requiredMask & 0x01) != 0) rawDirection |= IN_RIGHT;
    if ((expected & requiredMask & 0x02) != 0) rawDirection |= IN_LEFT;
    if ((expected & requiredMask & 0x04) != 0) rawDirection |= IN_UP;
    if ((expected & requiredMask & 0x08) != 0) rawDirection |= IN_DOWN;

    constexpr uint32_t rawButtonBits[] = {
        IN_X, IN_Y, IN_B, IN_A, IN_LB, IN_RB, IN_LT, IN_RT,
        IN_L3, IN_R3, IN_BACK,
    };
    constexpr uint32_t combinationCount = 1u << std::size(rawButtonBits);
    for (uint32_t combination = 0; combination < combinationCount;
         ++combination) {
        uint32_t raw = rawDirection;
        for (size_t bit = 0; bit < std::size(rawButtonBits); ++bit) {
            if ((combination & (1u << bit)) != 0)
                raw |= rawButtonBits[bit];
        }
        uint32_t resolved = 0;
        if (!Controller_RemapSyntheticInput(raw, &resolved))
            return UINT32_MAX;
        if ((resolved & requiredMask) == expected)
            return raw;
    }
    return UINT32_MAX;
}

void AppendNameToken(char* name, size_t capacity, const char* token)
{
    size_t length = strlen(name);
    if (length && length + 3 < capacity) {
        snprintf(name + length, capacity - length, " > ");
        length = strlen(name);
    }
    if (length + 1 < capacity)
        snprintf(name + length, capacity - length, "%s", token);
}

void DescribeResolvedInput(uint32_t input, char* token, size_t capacity)
{
    token[0] = '\0';
    const char* direction = nullptr;
    switch (input & 0x0F) {
    case 0x01: direction = "6"; break;
    case 0x02: direction = "4"; break;
    case 0x04: direction = "8"; break;
    case 0x05: direction = "9"; break;
    case 0x06: direction = "7"; break;
    case 0x08: direction = "2"; break;
    case 0x09: direction = "3"; break;
    case 0x0A: direction = "1"; break;
    default: break;
    }
    if (direction)
        snprintf(token, capacity, "%s", direction);

    struct ButtonName { uint32_t bit; const char* name; };
    static constexpr ButtonName buttons[] = {
        {0x10, "L"}, {0x20, "M"}, {0x40, "H"}, {0x80, "S"},
        {0x100, "A1"}, {0x200, "A2"}, {0x400, "L+M"},
        {0x800, "L+H"}, {0x1000, "M+H"}, {0x2000, "L+M+H"},
    };
    for (const ButtonName& button : buttons) {
        if ((input & button.bit) == 0)
            continue;
        size_t length = strlen(token);
        snprintf(token + length, capacity - length, "%s%s",
                 length ? "+" : "", button.name);
    }
    if (!token[0])
        snprintf(token, capacity, "neutral");
}

void BuildRuntimeCommandName(uint64_t resource, uint64_t command,
                             int32_t matcherIndex, int32_t lineIndex,
                             char* name, size_t capacity)
{
    using GetCommandStep_t = uint64_t(__fastcall*)(uint64_t, uint64_t, uint32_t);
    static GetCommandStep_t getCommandStep =
        (GetCommandStep_t)_addr(ADDR_GET_COMMAND_STEP);
    name[0] = '\0';
    int32_t count = command ? *(int32_t*)(command + 0x04) : 0;
    for (int32_t i = 0; i < count && i < 256; ++i) {
        uint64_t step = getCommandStep(resource, command, (uint32_t)i);
        if (!step)
            continue;
        int32_t type = *(int32_t*)(step + 0x00);
        uint32_t expected = 0;
        switch (type) {
        case 2:
        case 3:
            expected = *(uint32_t*)(step + 0x18);
            break;
        case 4: {
            int32_t simultaneous = *(int32_t*)(step + 0x0C);
            expected = simultaneous == 0 ? 0x10 : 0x30;
            break;
        }
        case 5:
        case 6:
            expected = *(uint32_t*)(step + 0x0C) |
                       *(uint32_t*)(step + 0x10);
            break;
        case 7:
            expected = *(uint32_t*)(step + 0x10);
            break;
        default:
            continue;
        }
        char token[48];
        DescribeResolvedInput(expected, token, sizeof(token));
        AppendNameToken(name, capacity, token);
    }
    if (!name[0])
        snprintf(name, capacity, "Matcher %d Line %d", matcherIndex, lineIndex);
    if (matcherIndex == 0 && command && *(int32_t*)(command + 0x18) == 1) {
        char notation[128];
        snprintf(notation, sizeof(notation), "%s", name);
        snprintf(name, capacity, "(AIR) %s", notation);
    }
}

bool GetRuntimeCommandNotation(uint64_t resource, uint64_t command,
                               CommandNotation& notation,
                               uint32_t commandFlags = 0)
{
    notation = {};
    if (!resource || !command)
        return false;

    using GetCommandStep_t = uint64_t(__fastcall*)(uint64_t, uint64_t, uint32_t);
    static GetCommandStep_t getCommandStep =
        (GetCommandStep_t)_addr(ADDR_GET_COMMAND_STEP);
    int32_t count = *(int32_t*)(command + 0x04);
    for (int32_t i = 0; i < count && i < 256; ++i) {
        uint64_t step = getCommandStep(resource, command, (uint32_t)i);
        if (!step)
            continue;

        uint32_t expected = 0;
        switch (*(int32_t*)(step + 0x00)) {
        case 2:
        case 3:
            expected = *(uint32_t*)(step + 0x18);
            break;
        case 4:
            expected = *(int32_t*)(step + 0x0C) == 0 ? 0x10 : 0x30;
            break;
        case 5:
        case 6:
            expected = *(uint32_t*)(step + 0x0C) |
                       *(uint32_t*)(step + 0x10);
            break;
        case 7:
            expected = *(uint32_t*)(step + 0x10);
            break;
        default:
            continue;
        }

        uint32_t direction = expected & 0x0F;
        if (direction && notation.directionCount <
                             (int32_t)std::size(notation.directions)) {
            int32_t numpad = 0;
            switch (direction) {
            case 0x01: numpad = 6; break;
            case 0x02: numpad = 4; break;
            case 0x04: numpad = 8; break;
            case 0x05: numpad = 9; break;
            case 0x06: numpad = 7; break;
            case 0x08: numpad = 2; break;
            case 0x09: numpad = 3; break;
            case 0x0A: numpad = 1; break;
            default: break;
            }
            if (numpad)
                notation.directions[notation.directionCount++] = numpad;
        }
        uint32_t buttons = expected & 0xF0;
        if (expected & 0x400) buttons |= 0x30;
        if (expected & 0x800) buttons |= 0x50;
        if (expected & 0x1000) buttons |= 0x60;
        if (expected & 0x2000) buttons |= 0x70;
        notation.buttons |= buttons;
    }
    if (commandFlags & 1)
        notation.buttons |= 0x80000000;
    return notation.directionCount > 0 || notation.buttons != 0;
}

bool CommandNotationsEqual(const CommandNotation& left,
                           const CommandNotation& right)
{
    if (left.directionCount != right.directionCount ||
        left.buttons != right.buttons ||
        left.attackCount != right.attackCount)
        return false;
    for (int32_t i = 0; i < left.directionCount; ++i) {
        if (left.directions[i] != right.directions[i])
            return false;
    }
    return true;
}

bool GetDisplayRowNotation(uint64_t rowNotation, int32_t rowIndex,
                           CommandNotation& notation)
{
    notation = {};
    if (!rowNotation || rowIndex < 0)
        return false;

    // Token IDs 0-7 are the eight directional sprites, 8 is the plus sign,
    // and 14-18 are L, M, H, S, and the generic attack-button sprite.
    static constexpr int32_t directions[] = {1, 2, 3, 4, 6, 7, 8, 9};
    const uint32_t* tokens =
        (const uint32_t*)(rowNotation + (uint64_t)rowIndex * 15 * 4);
    for (int32_t i = 0; i < 15; ++i) {
        uint32_t token = tokens[i];
        if (token == UINT32_MAX)
            break;
        if (token < std::size(directions) &&
            notation.directionCount < (int32_t)std::size(notation.directions)) {
            notation.directions[notation.directionCount++] = directions[token];
        } else if (token >= 14 && token <= 17) {
            notation.buttons |= 0x10u << (token - 14);
        } else if (token == 18) {
            ++notation.attackCount;
        }
    }
    return notation.directionCount > 0 || notation.buttons != 0 ||
           notation.attackCount > 0;
}

bool CommandNotationsMatch(const CommandNotation& runtime,
                           const CommandNotation& display,
                           uint32_t commandFlags)
{
    if (runtime.directionCount != display.directionCount)
        return false;
    for (int32_t i = 0; i < runtime.directionCount; ++i) {
        if (runtime.directions[i] != display.directions[i])
            return false;
    }

    uint32_t runtimeButtons = runtime.buttons & 0xF0;
    if (display.buttons && (runtimeButtons & display.buttons) != display.buttons)
        return false;
    if (display.attackCount > 0) {
        uint32_t attacks = runtimeButtons & 0x70;
        int32_t attackCount = ((attacks & 0x10) != 0) +
                              ((attacks & 0x20) != 0) +
                              ((attacks & 0x40) != 0);
        // Hyper commands are stored as a primary attack in the runtime table;
        // the command-list XX sprite is the actual two-button pad input.
        if (display.attackCount > 1) {
            if ((commandFlags & 1) == 0 || attackCount != 1)
                return false;
        } else if ((commandFlags & 1) != 0 ||
                   attackCount != display.attackCount) {
            return false;
        }
    }
    return display.attackCount > 0 || display.buttons || runtimeButtons == 0;
}

void DecodeCommandListText(uint64_t record, char* name, size_t capacity)
{
    name[0] = '\0';
    uint32_t length = *(uint32_t*)record;
    const wchar_t* text = *(const wchar_t**)(record + 0x08);
    if (!text || length == 0 || length > 0x1000 || capacity == 0)
        return;

    wchar_t decoded[128];
    uint32_t decodedLength = length;
    if (decodedLength >= std::size(decoded))
        decodedLength = (uint32_t)std::size(decoded) - 1;
    for (uint32_t i = 0; i < decodedLength; ++i) {
        wchar_t glyph = text[i];
        if (glyph == 0)
            decoded[i] = L' ';
        else if (glyph >= 0x21 && glyph <= 0x3A)
            decoded[i] = (wchar_t)(glyph + 0x20);
        else if (glyph == 0xFFFE)
            decoded[i] = L'|';
        else if (glyph == 0xFFFF) {
            decodedLength = i;
            break;
        } else
            decoded[i] = glyph;
    }
    decoded[decodedLength] = L'\0';

    int written = WideCharToMultiByte(CP_UTF8, 0, decoded,
                                      (int)decodedLength, name,
                                      (int)capacity - 1, nullptr, nullptr);
    if (written > 0)
        name[written] = '\0';
}

void DumpCommandListArrays(int32_t playerIndex)
{
    CommandListTextCache& cached = g_commandListText[playerIndex];
    if (cached.textDumped || !cached.resource || !cached.rowMetadata ||
        !cached.rowNotation || cached.rowCount <= 0)
        return;

    uint32_t count = *(uint32_t*)(cached.resource + 0x70);
    uint64_t records = *(uint64_t*)(cached.resource + 0x78);
    if (!records || count == 0 || count > 0x10000)
        return;

    HookLog("AutoTestCommandList: BEGIN player=%d characterId=%d path=%s textCount=%u rowCount=%d metadata=0x%llX notation=0x%llX\n",
            playerIndex + 1, cached.characterId, cached.path, count,
            cached.rowCount, (unsigned long long)cached.rowMetadata,
            (unsigned long long)cached.rowNotation);
    for (uint32_t textIndex = 0; textIndex < count; ++textIndex) {
        uint64_t record = records + (uint64_t)textIndex * 0x10;
        char text[256] = {};
        DecodeCommandListText(record, text, sizeof(text));
        HookLog("AutoTestCommandList: TEXT index=%u length=%u value=\"%s\"\n",
                textIndex, *(uint32_t*)record, text);
    }
    for (int32_t rowIndex = 0; rowIndex < cached.rowCount; ++rowIndex) {
        const int32_t* metadata = (const int32_t*)(cached.rowMetadata +
                                                  (uint64_t)rowIndex * 0x10);
        const uint32_t* tokens = (const uint32_t*)(cached.rowNotation +
                                                   (uint64_t)rowIndex * 0x3C);
        HookLog("AutoTestCommandList: ROW index=%d metadata=[%d,%d,%d,0x%08X] tokens=[",
                rowIndex, metadata[0], metadata[1], metadata[2], metadata[3]);
        for (int32_t tokenIndex = 0; tokenIndex < 15; ++tokenIndex) {
            HookLog("%s%d", tokenIndex ? "," : "", (int32_t)tokens[tokenIndex]);
            if (tokens[tokenIndex] == UINT32_MAX)
                break;
        }
        HookLog("]\n");
    }
    HookLog("AutoTestCommandList: END player=%d characterId=%d\n",
            playerIndex + 1, cached.characterId);
    cached.textDumped = true;
}

void DumpRuntimeCommandArray(int32_t playerIndex, int32_t matcherIndex,
                             uint64_t resource, int32_t lineCount)
{
    if (playerIndex < 0 || playerIndex > 1 || matcherIndex < 0 ||
        matcherIndex > 1 || !resource || lineCount <= 0 || lineCount > 0x1000)
        return;

    CommandListTextCache& cached = g_commandListText[playerIndex];
    if (cached.dumpedCommandResources[matcherIndex] == resource)
        return;

    using GetCommandByLine_t = uint64_t(__fastcall*)(uint64_t, uint32_t);
    using GetCommandStep_t = uint64_t(__fastcall*)(uint64_t, uint64_t, uint32_t);
    static GetCommandByLine_t getCommandByLine =
        (GetCommandByLine_t)_addr(ADDR_GET_COMMAND_BY_LINE);
    static GetCommandStep_t getCommandStep =
        (GetCommandStep_t)_addr(ADDR_GET_COMMAND_STEP);

    HookLog("AutoTestCommandList: RUNTIME_BEGIN player=%d characterId=%d matcher=%d resource=0x%llX lineCount=%d\n",
            playerIndex + 1, cached.characterId, matcherIndex,
            (unsigned long long)resource, lineCount);
    for (int32_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        uint64_t command = getCommandByLine(resource, (uint32_t)lineIndex);
        char generated[128] = {};
        BuildRuntimeCommandName(resource, command, matcherIndex, lineIndex,
                                generated, sizeof(generated));
        int32_t stepCount = command ? *(int32_t*)(command + 0x04) : -1;
        int32_t commandState = command ? *(int32_t*)(command + 0x18) : -1;
        uint32_t flags = command ? *(uint32_t*)(command + 0x20) : 0;
        HookLog("AutoTestCommandList: COMMAND matcher=%d line=%d command=0x%llX steps=%d state=%d flags=0x%08X generated=\"%s\"\n",
                 matcherIndex, lineIndex, (unsigned long long)command,
                 stepCount, commandState, flags, generated);
        for (int32_t stepIndex = 0; command && stepIndex < stepCount &&
                                      stepIndex < 256; ++stepIndex) {
            uint64_t step = getCommandStep(resource, command,
                                           (uint32_t)stepIndex);
            if (!step)
                continue;
            const uint32_t* words = (const uint32_t*)step;
            HookLog("AutoTestCommandList: STEP matcher=%d line=%d index=%d raw=[%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X]\n",
                    matcherIndex, lineIndex, stepIndex, words[0], words[1],
                    words[2], words[3], words[4], words[5], words[6], words[7]);
        }
    }
    HookLog("AutoTestCommandList: RUNTIME_END player=%d characterId=%d matcher=%d\n",
            playerIndex + 1, cached.characterId, matcherIndex);
    cached.dumpedCommandResources[matcherIndex] = resource;
}

void GetRuntimeCommandName(int32_t cacheIndex, uint64_t resource,
                           uint64_t command, int32_t matcherIndex,
                           int32_t lineIndex, char* name, size_t capacity)
{
    if (cacheIndex < 0 || cacheIndex >= MAX_RUNTIME_COMMAND_NAMES) {
        BuildRuntimeCommandName(resource, command, matcherIndex, lineIndex,
                                name, capacity);
        return;
    }

    RuntimeCommandNameCache& cached = g_runtimeCommandNames[cacheIndex];
    if (cached.resource != resource || cached.command != command) {
        cached.resource = resource;
        cached.command = command;
        BuildRuntimeCommandName(resource, command, matcherIndex, lineIndex,
                                cached.name, sizeof(cached.name));
    }
    snprintf(name, capacity, "%s", cached.name);
}

uint64_t GetCommandListTextResource(int32_t playerIndex, uint64_t character)
{
    if (playerIndex < 0 || playerIndex > 1 || !character)
        return 0;

    using GetObjectDti_t = uint64_t(__fastcall*)(uint64_t);
    using GetCharacterIdFromDti_t = uint32_t(__fastcall*)(uint64_t);
    GetObjectDti_t getObjectDti =
        *(GetObjectDti_t*)(*(uint64_t*)character + 0x20);
    static GetCharacterIdFromDti_t getCharacterIdFromDti =
        (GetCharacterIdFromDti_t)_addr(ADDR_GET_CHARACTER_ID_FROM_DTI);
    uint64_t characterDti = getObjectDti ? getObjectDti(character) : 0;
    int32_t characterId = characterDti
        ? (int32_t)getCharacterIdFromDti(characterDti)
        : -1;
    if (characterId <= 0 || characterId >= 0x3C)
        return 0;

    CommandListTextCache& cached = g_commandListText[playerIndex];
    if (cached.characterId == characterId && cached.resource)
        return cached.resource;

    if (cached.characterId != characterId) {
        cached = {characterId, 0, 0, 0, 0, false, {}, {}};
        // The game indexes this table with selectedCharacterId - 1. Character
        // IDs are one-based, while the static command-list table is zero-based.
        uint64_t tables = _addr(PTR_COMMAND_LIST_ROW_TABLES) +
                          (uint64_t)(characterId - 1) * 0x10;
        uint64_t rowMetadata = *(uint64_t*)(tables + 0x00);
        uint64_t rowNotation = *(uint64_t*)(tables + 0x08);
        if (rowMetadata && rowNotation > rowMetadata) {
            uint64_t size = rowNotation - rowMetadata;
            if (size % 0x10 == 0 && size / 0x10 <= 0x100) {
                cached.rowCount = (int32_t)(size / 0x10);
                cached.rowMetadata = rowMetadata;
                cached.rowNotation = rowNotation;
            }
        }
    }
    using GetCharacterName_t = const char*(__fastcall*)(int32_t);
    using GetResourceManager_t = uint64_t(__fastcall*)();
    static GetCharacterName_t getCharacterName =
        (GetCharacterName_t)_addr(ADDR_GET_CHARACTER_NAME);
    static GetResourceManager_t getResourceManager =
        (GetResourceManager_t)_addr(ADDR_GET_RESOURCE_MANAGER);

    // Follow the native boot path: mount ui/game_msg.arc before requesting an
    // rMsgData child from ui/commandlist inside that archive.
    using GetMvc3Manager_t = uint64_t(__fastcall*)();
    using LoadGlobalArchive_t = void(__fastcall*)(uint64_t, uint32_t,
                                                   uint64_t, uint64_t);
    static GetMvc3Manager_t getMvc3Manager =
        (GetMvc3Manager_t)_addr(ADDR_GET_MVC3_MANAGER);
    static LoadGlobalArchive_t loadGlobalArchive =
        (LoadGlobalArchive_t)_addr(ADDR_LOAD_GLOBAL_ARCHIVE);
    uint64_t mvc3Manager = getMvc3Manager();
    if (mvc3Manager)
        loadGlobalArchive(mvc3Manager, 8, 0, 0);

    const char* characterName = getCharacterName(characterId);
    uint64_t manager = getResourceManager();
    if (!characterName || !characterName[0] || !manager)
        return 0;

    snprintf(cached.path, sizeof(cached.path),
             "ui\\commandlist\\msg_commandlist_%s_en", characterName);
    using LoadResource_t = uint64_t(__fastcall*)(uint64_t, void*, const char*,
                                                  uint32_t);
    LoadResource_t loadResource =
        *(LoadResource_t*)(*(uint64_t*)manager + 0x60);
    if (loadResource)
        cached.resource = loadResource(manager, (void*)_addr(DTI_RMSGDATA),
                                       cached.path, 1);
    DumpCommandListArrays(playerIndex);
    return cached.resource;
}

void GetInGameCommandListName(int32_t playerIndex, uint64_t character,
                               uint64_t commandResource, uint64_t command,
                               char* name, size_t capacity,
                               int32_t* category = nullptr,
                               int32_t* attackCount = nullptr)
{
    name[0] = '\0';
    if (category) *category = -1;
    if (attackCount) *attackCount = 0;
    uint64_t resource = GetCommandListTextResource(playerIndex, character);
    if (!resource || !commandResource || !command)
        return;

    uint32_t count = *(uint32_t*)(resource + 0x70);
    uint64_t records = *(uint64_t*)(resource + 0x78);
    CommandListTextCache& cached = g_commandListText[playerIndex];
    int32_t rowCount = cached.rowCount;
    if (!records || count == 0 || count > 0x10000 || rowCount <= 0 ||
        !cached.rowMetadata || !cached.rowNotation)
        return;

    CommandNotation runtimeNotation = {};
    uint32_t commandFlags = *(uint32_t*)(command + 0x20);
    if (!GetRuntimeCommandNotation(commandResource, command, runtimeNotation,
                                   commandFlags))
        return;

    int32_t matchedRow = -1;
    for (int32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        CommandNotation displayNotation = {};
        if (!GetDisplayRowNotation(cached.rowNotation, rowIndex,
                                   displayNotation) ||
            !CommandNotationsMatch(runtimeNotation, displayNotation,
                                   commandFlags)) {
            continue;
        }
        if (matchedRow >= 0)
            return;
        matchedRow = rowIndex;
    }
    if (matchedRow < 0)
        return;

    int32_t textIndex = *(int32_t*)(cached.rowMetadata +
                                    (uint64_t)matchedRow * 0x10);
    if (textIndex < 0 || (uint32_t)textIndex >= count)
        return;

    DecodeCommandListText(records + (uint64_t)textIndex * 0x10,
                          name, capacity);
    const int32_t* metadata = (const int32_t*)(cached.rowMetadata +
                                               (uint64_t)matchedRow * 0x10);
    if (category) *category = metadata[1];
    if (attackCount) {
        CommandNotation displayNotation = {};
        GetDisplayRowNotation(cached.rowNotation, matchedRow, displayNotation);
        *attackCount = displayNotation.attackCount;
    }
}

bool AppendRuntimeInput(uint32_t mask, uint32_t expected, int frames)
{
    if (g_runtimeScript.stepCount + 1 > MAX_RUNTIME_INPUT_STEPS)
        return false;
    uint32_t raw = FindRawInput(mask, expected);
    if (raw == UINT32_MAX)
        return false;
    g_runtimeInputSteps[g_runtimeScript.stepCount++] = {raw, frames};
    return true;
}

bool GetStagedHyperCommand(int32_t playerIndex, uint64_t resource,
                           uint64_t command, char* name, size_t capacity,
                           uint32_t* outFollowupButtons)
{
    if (playerIndex < 0 || playerIndex > 1 ||
        !resource || !command || !name || !outFollowupButtons)
        return false;

    using GetCommandStep_t = uint64_t(__fastcall*)(uint64_t, uint64_t, uint32_t);
    static GetCommandStep_t getCommandStep =
        (GetCommandStep_t)_addr(ADDR_GET_COMMAND_STEP);
    bool hasGenericTwoAttackInput = false;
    int32_t stepCount = *(int32_t*)(command + 0x04);
    for (int32_t i = 0; i < stepCount && i < 256; ++i) {
        uint64_t step = getCommandStep(resource, command, (uint32_t)i);
        if (!step)
            continue;
        int32_t type = *(int32_t*)(step + 0x00);
        if ((type == 2 || type == 3) &&
            (*(uint32_t*)(step + 0x18) & 0x300) == 0x300)
            hasGenericTwoAttackInput = true;
    }
    if (!hasGenericTwoAttackInput)
        return false;

    CommandListTextCache& cached = g_commandListText[playerIndex];
    uint64_t textResource = GetCommandListTextResource(
        playerIndex, GetActiveCharacter(playerIndex));
    if (!textResource || !cached.rowMetadata || !cached.rowNotation)
        return false;

    CommandNotation runtimeNotation = {};
    if (!GetRuntimeCommandNotation(resource, command, runtimeNotation,
                                   *(uint32_t*)(command + 0x20)))
        return false;

    int32_t matchedRow = -1;
    CommandNotation matchedNotation = {};
    for (int32_t row = 0; row < cached.rowCount; ++row) {
        const int32_t* metadata = (const int32_t*)(cached.rowMetadata +
                                                   (uint64_t)row * 0x10);
        CommandNotation display = {};
        if (metadata[1] != 2 || !GetDisplayRowNotation(
                cached.rowNotation, row, display) || display.attackCount < 2 ||
            display.buttons == 0 ||
            runtimeNotation.directionCount != display.directionCount)
            continue;

        bool directionsMatch = true;
        for (int32_t i = 0; i < runtimeNotation.directionCount; ++i) {
            if (runtimeNotation.directions[i] != display.directions[i]) {
                directionsMatch = false;
                break;
            }
        }
        if (!directionsMatch)
            continue;
        if (matchedRow >= 0)
            return false;
        matchedRow = row;
        matchedNotation = display;
    }
    if (matchedRow < 0)
        return false;

    uint32_t textCount = *(uint32_t*)(textResource + 0x70);
    uint64_t records = *(uint64_t*)(textResource + 0x78);
    int32_t textIndex = *(int32_t*)(cached.rowMetadata +
                                    (uint64_t)matchedRow * 0x10);
    if (!records || textIndex < 0 || (uint32_t)textIndex >= textCount)
        return false;
    DecodeCommandListText(records + (uint64_t)textIndex * 0x10,
                          name, capacity);
    *outFollowupButtons = matchedNotation.buttons;
    return name[0] != '\0';
}

void __fastcall hkUpdateCommandMatcher(uint64_t commandMatcher)
{
    oUpdateCommandMatcher(commandMatcher);

    if (!g_status.active || g_isResimulating || !commandMatcher)
        return;

    uint64_t character = *(uint64_t*)(commandMatcher + 0x160);
    if (!character)
        return;

    using GetTeamIndex_t = uint32_t(__fastcall*)(uint64_t character);
    static GetTeamIndex_t getTeamIndex =
        (GetTeamIndex_t)_addr(0x140004AC0);
    if ((int32_t)getTeamIndex(character) != g_status.playerIndex)
        return;

    uint64_t successfulLine = *(uint64_t*)(commandMatcher + 0x180);
    int32_t successListId = *(int32_t*)(commandMatcher + 0x188);
    if (!successfulLine && successListId < 0)
        return;

    int32_t frame = Controller_GetNetFrameCount();
    uint64_t lines = *(uint64_t*)(commandMatcher + 0x178);
    int32_t lineCount = *(int32_t*)(commandMatcher + 0x170);
    int32_t lineIndex = -1;
    if (lines && successfulLine >= lines && lineCount > 0) {
        uint64_t offset = successfulLine - lines;
        if (offset % 0x20 == 0 && offset / 0x20 < (uint64_t)lineCount)
            lineIndex = (int32_t)(offset / 0x20);
    }
    // Some matcher implementations publish a successful command object rather
    // than an address inside the live Line array. The native selected-line
    // output remains the authoritative index in those cases.
    if (lineIndex < 0 && successListId >= 0 && successListId < lineCount)
        lineIndex = successListId;

    int32_t matcherIndex = -1;
    if (commandMatcher == character + 0x2738) matcherIndex = 0;
    if (commandMatcher == character + 0x2950) matcherIndex = 1;

    if (g_status.lastSuccessFrame == frame &&
        g_status.lastSuccessMatcher == matcherIndex &&
        g_status.lastSuccessLine == lineIndex)
        return;

    g_status.lastSuccessFrame = frame;
    g_status.lastSuccessMatcher = matcherIndex;
    g_status.lastSuccessLine = lineIndex;
    g_status.lastSuccessListId = successListId;
    g_status.lastSuccessAnimation = *(int32_t*)(commandMatcher + 0x18C);
    g_status.lastAction = *(uint32_t*)(character + 0x1310);

    if (g_suiteStatus.active &&
        matcherIndex == g_suiteStatus.currentMatcher &&
        lineIndex == g_suiteStatus.currentLine)
        g_suiteMatchedCommand = true;

    HookLog("AutoTest: frame=%d script=%s matcher=%d line=%d list=%d anm=%d action=0x%08X\n",
            frame, ActiveScriptName(), matcherIndex, lineIndex,
            g_status.lastSuccessListId, g_status.lastSuccessAnimation,
            g_status.lastAction);
}

} // namespace

void AutoTest_InitHooks()
{
    MH_STATUS status = MH_CreateHook(
        (LPVOID)_addr(ADDR_UPDATE_COMMAND_MATCHER),
        (LPVOID)&hkUpdateCommandMatcher,
        (LPVOID*)&oUpdateCommandMatcher);
    if (status == MH_OK)
        status = MH_EnableHook((LPVOID)_addr(ADDR_UPDATE_COMMAND_MATCHER));

    if (status != MH_OK)
        HookLog("AutoTest: failed to hook command matcher (%d)\n", status);
    else
        HookLog("AutoTest: command matcher instrumentation installed\n");
}

extern "C" bool AutoTest_GetInput(int32_t frame, uint32_t playerIndex,
                                   uint32_t* outInput)
{
    if (!g_status.active || !outInput || frame < g_status.startFrame ||
        (int32_t)playerIndex != g_status.playerIndex)
        return false;

    const InputScript& script = ActiveScript();
    int32_t scriptFrame = frame - g_status.startFrame;
    if (scriptFrame >= ScriptLength(script)) {
        g_status.active = false;
        g_runningRuntimeCommand = false;
        g_runningRuntimeHyper = false;
        g_status.currentInput = 0;
        HookLog("AutoTest: completed %s at frame %d\n", script.name, frame);
        return false;
    }

    uint32_t input = ScriptInputAt(script, scriptFrame);
    g_status.currentFrame = frame;
    g_status.scriptFrame = scriptFrame;
    g_status.currentInput = input;
    *outInput = input;
    return true;
}

extern "C" void AutoTest_Start(int32_t scriptIndex, int32_t playerIndex,
                                bool facingLeft)
{
    if (scriptIndex < 0 || scriptIndex >= (int32_t)std::size(SCRIPTS))
        return;
    if (playerIndex < 0 || playerIndex > 1)
        return;

    g_status = {
        true, playerIndex, scriptIndex, Controller_GetNetFrameCount() + 5,
        -1, -1, 0, -1, -1, -1, -1, -1, 0,
    };
    g_runningRuntimeCommand = false;
    g_runningRuntimeHyper = false;
    g_runningManualCommand = -1;
    g_facingLeft = facingLeft;
    HookLog("AutoTest: scheduled %s for P%d at frame %d facing=%s\n",
            SCRIPTS[scriptIndex].name, playerIndex + 1, g_status.startFrame,
            facingLeft ? "left" : "right");
}

extern "C" void AutoTest_Stop()
{
    g_status.active = false;
    g_status.currentInput = 0;
    g_runningRuntimeCommand = false;
    g_runningRuntimeHyper = false;
    g_runningManualCommand = -1;
    g_suiteStatus.active = false;
    g_suiteStatus.waitingForCompletion = false;
    g_suiteStatus.waitingForReset = false;
    g_suitePhase = SuitePhase::Idle;
    g_netSuiteActive = false;
    g_netSuiteClient = false;
    g_netSuiteRestartPending = false;
}

extern "C" AutoTestStatus AutoTest_GetStatus()
{
    return g_status;
}

extern "C" bool AutoTest_IsActive()
{
    return g_status.active || g_suiteStatus.active || g_netSuiteActive;
}

extern "C" int32_t AutoTest_GetScriptCount()
{
    return (int32_t)std::size(SCRIPTS);
}

extern "C" const char* AutoTest_GetScriptName(int32_t scriptIndex)
{
    if (scriptIndex < 0 || scriptIndex >= (int32_t)std::size(SCRIPTS))
        return "Unknown";
    return SCRIPTS[scriptIndex].name;
}

extern "C" int32_t AutoTest_GetRuntimeCommands(
    AutoTestRuntimeCommand* outCommands, int32_t capacity)
{
    if (!outCommands || capacity <= 0)
        return 0;

    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    if (!characters)
        return 0;

    using GetCommandByLine_t = uint64_t(__fastcall*)(uint64_t resource,
                                                      uint32_t lineIndex);
    static GetCommandByLine_t getCommandByLine =
        (GetCommandByLine_t)_addr(ADDR_GET_COMMAND_BY_LINE);

    int32_t written = 0;
    for (int32_t player = 0; player < 2 && written < capacity; ++player) {
        uint64_t character = *(uint64_t*)(characters + TEAM_OFFSETS[player] +
                                         TEAM_ACTIVE_CHARACTER_OFFSET);
        if (!character)
            continue;

        for (int32_t matcherIndex = 0;
             matcherIndex < 2 && written < capacity; ++matcherIndex) {
            uint64_t matcher = character + COMMAND_MATCHER_OFFSETS[matcherIndex];
            int32_t lineCount = *(int32_t*)(matcher + 0x170);
            uint64_t lines = *(uint64_t*)(matcher + 0x178);
            uint64_t resource = *(uint64_t*)(matcher + 0x168);
            if (!lines || lineCount <= 0 || lineCount > 0x1000)
                continue;

            GetCommandListTextResource(player, character);
            DumpRuntimeCommandArray(player, matcherIndex, resource, lineCount);

            for (int32_t lineIndex = 0;
                 lineIndex < lineCount && written < capacity; ++lineIndex) {
                uint64_t line = lines + (uint64_t)lineIndex * 0x20;
                uint64_t command = resource
                    ? getCommandByLine(resource, (uint32_t)lineIndex)
                    : 0;
                int32_t cacheIndex = written;
                AutoTestRuntimeCommand& out = outCommands[written++];
                out = {};
                out.playerIndex = player;
                out.matcherIndex = matcherIndex;
                out.lineIndex = lineIndex;
                out.lineCount = lineCount;
                out.cmdStep = *(int32_t*)(line + 0x08);
                out.time = *(float*)(line + 0x0C);
                out.chargeTime = *(float*)(line + 0x10);
                out.padBuf = *(uint32_t*)(line + 0x14);
                out.lineFlags = *(uint32_t*)(line + 0x18);
                out.failTime = *(float*)(line + 0x1C);
                out.character = character;
                out.matcher = matcher;
                out.resource = resource;
                out.successListId = *(int32_t*)(matcher + 0x188);
                out.successAnimation = *(int32_t*)(matcher + 0x18C);
                out.successThrow = *(int32_t*)(matcher + 0x190);
                out.successTdown = *(int32_t*)(matcher + 0x194);
                out.successDelay = *(float*)(matcher + 0x198);
                out.successWork = *(int32_t*)(matcher + 0x19C);
                out.commandStepCount = command ? *(int32_t*)(command + 0x04) : -1;
                out.commandState = command ? *(int32_t*)(command + 0x18) : -1;
                out.commandFlags = command ? *(uint32_t*)(command + 0x20) : 0;
                GetRuntimeCommandName(cacheIndex, resource, command,
                                      matcherIndex, lineIndex, out.name,
                                      sizeof(out.name));
                GetInGameCommandListName(player, character, resource, command,
                                          out.commandListName,
                                          sizeof(out.commandListName),
                                          &out.commandListCategory,
                                          &out.commandListAttackCount);
                uint32_t stagedFollowup = 0;
                if (GetStagedHyperCommand(player, resource, command,
                                          out.commandListName,
                                          sizeof(out.commandListName),
                                          &stagedFollowup)) {
                    out.commandListCategory = 2;
                    out.commandListAttackCount = 2;
                }
                CommandListTextCache& textCache = g_commandListText[player];
                snprintf(out.commandListPath, sizeof(out.commandListPath),
                         "%s", textCache.path);
                out.commandListTextCount = textCache.resource
                    ? *(int32_t*)(textCache.resource + 0x70)
                    : -1;
                out.lifeRateMin = *(float*)(matcher + 0x10);
                out.lifeRateMax = *(float*)(matcher + 0x14);
                out.gaugeRateMin = *(float*)(matcher + 0x18);
                out.gaugeRateMax = *(float*)(matcher + 0x1C);
                out.statusRequired = *(int32_t*)(matcher + 0x24);
                out.statusForbidden = *(int32_t*)(matcher + 0x28);
                out.abilityRequired = *(int32_t*)(matcher + 0x2C);
                out.abilityForbidden = *(int32_t*)(matcher + 0x30);
                out.heightMin = *(float*)(matcher + 0x44);
                out.heightMax = *(float*)(matcher + 0x48);
                out.jumpCount = *(int32_t*)(matcher + 0x4C);
                out.airSpecialCount = *(int32_t*)(matcher + 0x50);
                out.limitFlags = *(uint32_t*)(matcher + 0x60);
                out.characterFlagsRequired = *(uint32_t*)(matcher + 0x64);
                out.characterFlagsForbidden = *(uint32_t*)(matcher + 0x68);
                out.animationFlagsRequired = *(uint32_t*)(matcher + 0x70);
                out.animationFlagsForbidden = *(uint32_t*)(matcher + 0x74);
                out.programFlagsRequired = *(uint32_t*)(matcher + 0x7C);
                out.programFlagsForbidden = *(uint32_t*)(matcher + 0x80);
            }
        }
    }
    return written;
}

extern "C" bool AutoTest_StartRuntimeCommand(int32_t playerIndex,
                                               int32_t matcherIndex,
                                               int32_t lineIndex,
                                               bool facingLeft)
{
    g_runtimeExecutionError[0] = '\0';
    if (playerIndex < 0 || playerIndex > 1 || matcherIndex < 0 ||
        matcherIndex > 1 || lineIndex < 0) {
        snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                 "Invalid runtime command selection");
        return false;
    }

    uint64_t characters = *(uint64_t*)_addr(PTR_SCHARACTER);
    uint64_t character = characters
        ? *(uint64_t*)(characters + TEAM_OFFSETS[playerIndex] +
                      TEAM_ACTIVE_CHARACTER_OFFSET)
        : 0;
    uint64_t matcher = character
        ? character + COMMAND_MATCHER_OFFSETS[matcherIndex]
        : 0;
    uint64_t resource = matcher ? *(uint64_t*)(matcher + 0x168) : 0;
    int32_t lineCount = matcher ? *(int32_t*)(matcher + 0x170) : 0;
    if (!resource || lineIndex >= lineCount) {
        snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                 "The selected command is no longer available");
        return false;
    }

    using GetCommandByLine_t = uint64_t(__fastcall*)(uint64_t, uint32_t);
    using GetCommandStep_t = uint64_t(__fastcall*)(uint64_t, uint64_t, uint32_t);
    static GetCommandByLine_t getCommandByLine =
        (GetCommandByLine_t)_addr(ADDR_GET_COMMAND_BY_LINE);
    static GetCommandStep_t getCommandStep =
        (GetCommandStep_t)_addr(ADDR_GET_COMMAND_STEP);
    uint64_t command = getCommandByLine(resource, (uint32_t)lineIndex);
    int32_t stepCount = command ? *(int32_t*)(command + 0x04) : 0;
    if (!command || stepCount <= 0 || stepCount > 256) {
        snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                 "The selected command has no readable input steps");
        return false;
    }

    char commandListName[128] = {};
    int32_t commandListCategory = -1;
    int32_t commandListAttackCount = 0;
    GetInGameCommandListName(playerIndex, character, resource, command,
                             commandListName, sizeof(commandListName),
                             &commandListCategory, &commandListAttackCount);
    uint32_t stagedHyperFollowup = 0;
    const bool isStagedHyper = GetStagedHyperCommand(
        playerIndex, resource, command, commandListName,
        sizeof(commandListName), &stagedHyperFollowup);
    const bool isHyper = isStagedHyper ||
                         (commandListCategory == 2 &&
                          commandListAttackCount >= 2);
    if (isHyper)
        FillAttackerHyperGauge(playerIndex);

    g_runtimeScript.stepCount = 0;
    const int32_t commandState = *(int32_t*)(command + 0x18);
    const uint32_t commandFlags = *(uint32_t*)(command + 0x20);
    // cChrCmdSpatk treats 0 as grounded, 1 as airborne, and other values as
    // unrestricted. cChrCmdCombo uses this field for different restrictions.
    const bool requiresAir = matcherIndex == 0 && commandState == 1;
    if (requiresAir) {
        if (!AppendRuntimeInput(0x0F, 0x04, 3)) {
            snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                     "Could not map jump input for air-only command");
            return false;
        }
        g_runtimeInputSteps[g_runtimeScript.stepCount++] = {0, 6};
    }
    for (int32_t i = 0; i < stepCount; ++i) {
        uint64_t step = getCommandStep(resource, command, (uint32_t)i);
        if (!step)
            continue;
        int32_t type = *(int32_t*)(step + 0x00);
        uint32_t mask = 0;
        uint32_t expected = 0;
        switch (type) {
        case 1: // End/commit marker.
        case 9: // Action-state setup, not an input requirement.
        case 10:
            continue;
        case 2:
        case 3:
            mask = *(uint32_t*)(step + 0x14);
            expected = *(uint32_t*)(step + 0x18);
            break;
        case 4: {
            int32_t simultaneous = *(int32_t*)(step + 0x0C);
            mask = 0xF0;
            expected = simultaneous == 0 ? 0x10 : 0x30;
            break;
        }
        case 5:
        case 6:
            mask = *(uint32_t*)(step + 0x08) | *(uint32_t*)(step + 0x10);
            expected = *(uint32_t*)(step + 0x0C) |
                       *(uint32_t*)(step + 0x10);
            break;
        case 7:
            mask = *(uint32_t*)(step + 0x0C);
            expected = *(uint32_t*)(step + 0x10);
            break;
        case 8:
            // Direction-transition validation uses the history established by
            // adjacent input steps and does not describe another pad state.
            continue;
        default:
            if (type >= 0x0B && type <= 0x5D) {
                // These are native command predicates (character/action state,
                // resources, mode rules, and character-specific restrictions),
                // not inputs. The game still evaluates them when the generated
                // input reaches this command line.
                continue;
            }
            snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                     "Command step %d uses unknown matcher type %d",
                     i, type);
            g_runtimeScript.stepCount = 0;
            return false;
        }

        if (isStagedHyper && (expected & 0x300) == 0x300) {
            // The native matcher encodes the generic XX requirement in the
            // assist-bit range. Drive two attacks, not literal A1+A2.
            mask = (mask & ~0x300u) | 0x70u;
            expected = (expected & ~0x300u) | 0x30u;
        } else if (isHyper && (expected & 0x70) != 0) {
            // The native hyper command checks one representative attack and a
            // separate hyper predicate. Drive the command-list's XX literally.
            mask = (mask & ~0x70u) | 0x70u;
            expected = (expected & ~0x70u) | 0x30u;
        }

        if (!AppendRuntimeInput(mask, expected, 2)) {
            snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                     "Could not map command step %d (mask %08X = %08X)",
                     i, mask, expected);
            g_runtimeScript.stepCount = 0;
            return false;
        }
    }

    if (isStagedHyper && stagedHyperFollowup != 0 &&
        !AppendRuntimeInput(stagedHyperFollowup, stagedHyperFollowup, 2)) {
        snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                 "Could not append staged hyper activation input");
        g_runtimeScript.stepCount = 0;
        return false;
    }

    if (g_runtimeScript.stepCount == 0 ||
        g_runtimeScript.stepCount >= MAX_RUNTIME_INPUT_STEPS) {
        snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                 "No executable input checks were found");
        return false;
    }
    g_runtimeInputSteps[g_runtimeScript.stepCount++] = {0, 45};
    g_status = {
        true, playerIndex, -1, Controller_GetNetFrameCount() + 5,
        -1, -1, 0, -1, -1, -1, -1, -1, 0,
    };
    g_facingLeft = facingLeft;
    g_runningRuntimeCommand = true;
    g_runningRuntimeHyper = isHyper;
    g_runningManualCommand = -1;
    HookLog("AutoTest: scheduled runtime command matcher=%d line=%d state=%d flags=0x%08X for P%d at frame %d%s\n",
            matcherIndex, lineIndex, commandState, commandFlags,
            playerIndex + 1, g_status.startFrame,
            requiresAir ? " with jump setup" : "");
    return true;
}

extern "C" const char* AutoTest_GetRuntimeExecutionError()
{
    return g_runtimeExecutionError;
}

bool StartSuite(int32_t playerIndex, bool facingLeft, bool deduplicate)
{
    if (playerIndex < 0 || playerIndex > 1)
        return false;
    if (Netplay_IsActive() && (!Netplay_IsHost() || playerIndex != 0)) {
        HookLog("AutoTestNet: start the network suite on host P1\n");
        return false;
    }

    uint64_t replay = *(uint64_t*)_addr(PTR_SREPLAY);
    if (!replay)
        return false;
    g_suiteRngSeed = *(uint32_t*)(replay + 0x58);

    int32_t allCount = AutoTest_GetRuntimeCommands(
        g_suiteCommands, AUTOTEST_MAX_RUNTIME_COMMANDS);
    using GetCommandByLine_t = uint64_t(__fastcall*)(uint64_t, uint32_t);
    static GetCommandByLine_t getCommandByLine =
        (GetCommandByLine_t)_addr(ADDR_GET_COMMAND_BY_LINE);
    int32_t commandCount = 0;
    int32_t duplicateCount = 0;
    for (int32_t i = 0; i < allCount; ++i) {
        const AutoTestRuntimeCommand command = g_suiteCommands[i];
        if (command.playerIndex != playerIndex)
            continue;

        uint64_t runtimeCommand = command.resource
            ? getCommandByLine(command.resource, (uint32_t)command.lineIndex)
            : 0;
        CommandNotation notation = {};
        bool hasNotation = GetRuntimeCommandNotation(
            command.resource, runtimeCommand, notation,
            command.commandFlags);
        bool duplicate = false;
        const char* commandName = command.commandListName[0]
            ? command.commandListName : command.name;
        for (int32_t existing = 0; deduplicate && existing < commandCount;
             ++existing) {
            const AutoTestRuntimeCommand& existingCommand =
                g_suiteCommands[existing];
            const char* existingName = existingCommand.commandListName[0]
                ? existingCommand.commandListName : existingCommand.name;
            if (strcmp(commandName, existingName) == 0 ||
                (hasNotation && g_suiteCommandHasNotation[existing] &&
                 CommandNotationsEqual(
                     notation, g_suiteCommandNotations[existing]))) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            ++duplicateCount;
            continue;
        }

        g_suiteCommands[commandCount] = command;
        g_suiteCommandNotations[commandCount] = notation;
        g_suiteCommandHasNotation[commandCount] = hasNotation;
        ++commandCount;
    }
    if (commandCount == 0)
        return false;

    InitializeSuiteResultFile(playerIndex, deduplicate);

    for (int32_t i = 0; i < commandCount; ++i) {
        const AutoTestRuntimeCommand& command = g_suiteCommands[i];
        HookLog("AutoTestSuite: list=%d matcher=%d line=%d notation=\"%s\" command=\"%s\"\n",
                i + 1, command.matcherIndex, command.lineIndex,
                command.name, command.commandListName[0]
                    ? command.commandListName : command.name);
    }

    g_suiteStatus = {};
    g_suiteStatus.active = true;
    g_suiteStatus.playerIndex = playerIndex;
    g_suiteStatus.currentCommand = 0;
    g_suiteStatus.commandCount = commandCount + (int32_t)std::size(SUITE_MANUAL_SCRIPTS);
    g_suiteStatus.currentMatcher = -1;
    g_suiteStatus.currentLine = -1;
    g_suiteStatus.requiredQuietFrames = Netplay_IsActive()
        ? NET_SUITE_STABLE_FRAMES : SUITE_QUIET_FRAMES;
    g_suiteFacingLeft = facingLeft;
    g_suiteMatchedCommand = false;
    g_suitePhase = SuitePhase::StartCommand;
    g_netSuiteActive = Netplay_IsActive();
    g_netSuiteClient = false;
    g_netSuiteRestartPending = false;
    g_netSuiteMoveId = 0;
    HookLog("AutoTestSuite: started %d manual cases and %d %scommands for P%d (%d duplicate moves removed)\n",
            (int32_t)std::size(SUITE_MANUAL_SCRIPTS), commandCount,
            deduplicate ? "unique " : "variant ", playerIndex + 1,
            duplicateCount);
    return true;
}

extern "C" bool AutoTest_StartSuite(int32_t playerIndex, bool facingLeft)
{
    return StartSuite(playerIndex, facingLeft, true);
}

extern "C" bool AutoTest_StartVariantSuite(int32_t playerIndex,
                                             bool facingLeft)
{
    return StartSuite(playerIndex, facingLeft, false);
}

extern "C" void AutoTest_Process()
{
    ProcessNetSuitePackets();
    ProcessNetSuiteSetup(Controller_GetNetFrameCount());
    if (g_netSuiteActive && g_netSuiteClient)
        return;
    if (!g_suiteStatus.active || g_isResimulating)
        return;

    int32_t frame = Controller_GetNetFrameCount();
    uint64_t action = GetAction();
    int32_t actionStep = action ? *(int32_t*)(action + 0xD8) : -1;

    if (g_netSuiteActive && !g_netSuiteClient && !g_netSuiteRestartPending &&
        g_desyncDetected &&
        (g_suitePhase == SuitePhase::RunningCommand ||
         g_suitePhase == SuitePhase::WaitingForCompletion)) {
        if (g_netSuiteFirstMismatchFrame < 0) {
            g_netSuiteFirstMismatchFrame = g_desyncFrame;
            g_netSuiteFirstHostChecksum = g_desyncLocalCS;
            g_netSuiteFirstClientChecksum = g_desyncRemoteCS;
        }
        FinishSuiteCommand(false, "persistent desync detected");
        return;
    }

    switch (g_suitePhase) {
    case SuitePhase::StartCommand: {
        if (actionStep != 5)
            return;
        if (g_suiteStatus.currentCommand >= g_suiteStatus.commandCount) {
            snprintf(g_suiteStatus.lastResult, sizeof(g_suiteStatus.lastResult),
                     "Suite complete: %d passed, %d failed",
                     g_suiteStatus.passedCount, g_suiteStatus.failedCount);
            HookLog("AutoTestSuite: %s\n", g_suiteStatus.lastResult);
            g_suiteStatus.active = false;
            g_suitePhase = SuitePhase::Idle;
            return;
        }

        if (!g_netSuiteActive && !ResetSuiteRng()) {
            snprintf(g_suiteStatus.lastResult, sizeof(g_suiteStatus.lastResult),
                     "Suite stopped: sReplay unavailable for RNG reset");
            HookLog("AutoTestSuite: %s\n", g_suiteStatus.lastResult);
            g_suiteStatus.active = false;
            g_suitePhase = SuitePhase::Idle;
            return;
        }

        int32_t manualCount = (int32_t)std::size(SUITE_MANUAL_SCRIPTS);
        g_suiteManualCase = g_suiteStatus.currentCommand < manualCount;
        const AutoTestRuntimeCommand* command = g_suiteManualCase
            ? nullptr
            : &g_suiteCommands[g_suiteStatus.currentCommand - manualCount];
        g_suiteStatus.currentMatcher = command ? command->matcherIndex : -1;
        g_suiteStatus.currentLine = command ? command->lineIndex : -1;
        if (g_suiteManualCase) {
            snprintf(g_suiteStatus.currentName,
                     sizeof(g_suiteStatus.currentName), "%s",
                     SUITE_MANUAL_SCRIPTS[g_suiteStatus.currentCommand].name);
        } else if (command->commandListName[0] && command->name[0] &&
                   strcmp(command->commandListName, command->name) != 0) {
            snprintf(g_suiteStatus.currentName,
                     sizeof(g_suiteStatus.currentName), "%s [%s]",
                     command->commandListName, command->name);
        } else {
            snprintf(g_suiteStatus.currentName,
                     sizeof(g_suiteStatus.currentName), "%s",
                     command->commandListName[0]
                         ? command->commandListName : command->name);
        }
        if (!g_netSuiteActive)
            PlaceDefenderNextToAttacker(g_suiteStatus.playerIndex);
        uint64_t attacker = GetActiveCharacter(g_suiteStatus.playerIndex);
        uint64_t defender = GetActiveCharacter(g_suiteStatus.playerIndex ^ 1);
        g_suiteDefenderLife = GetCharacterLife(defender);
        g_suiteStatus.dealtDamage = false;
        g_suiteStatus.charactersStable = false;
        g_suiteStatus.shotsClear = false;
        g_suiteStatus.cutinClear = false;
        g_suiteStatus.quietFrames = 0;
        g_suiteStatus.shotCount = CountActiveShots();
        g_suiteIdleActions[0] = attacker
            ? *(uint32_t*)(attacker + 0x1310) : UINT32_MAX;
        g_suiteIdleActions[1] = defender
            ? *(uint32_t*)(defender + 0x1310) : UINT32_MAX;
        g_suiteStatus.attackerAction = g_suiteIdleActions[0];
        g_suiteStatus.defenderAction = g_suiteIdleActions[1];
        g_suiteMatchedCommand = false;
        if (g_suiteManualCase) {
            ConfigureManualScript(g_suiteStatus.currentCommand,
                                  g_suiteStatus.playerIndex,
                                  g_suiteFacingLeft, frame + 5);
        } else {
            if (!AutoTest_StartRuntimeCommand(g_suiteStatus.playerIndex,
                                              command->matcherIndex,
                                              command->lineIndex,
                                              g_suiteFacingLeft)) {
                FinishSuiteCommand(false, AutoTest_GetRuntimeExecutionError());
                return;
            }
            g_suiteStatus.active = true;
        }
        if (g_netSuiteActive) {
            int32_t networkLead = (int32_t)(Netplay_GetRTT() * 60 / 1000) + 30;
            g_status.startFrame = frame + networkLead;
            AutoTestStartPacket packet = {};
            packet.move_id = ++g_netSuiteMoveId;
            packet.case_type = g_suiteManualCase
                ? AUTOTEST_CASE_MANUAL : AUTOTEST_CASE_RUNTIME;
            packet.player_index = g_suiteStatus.playerIndex;
            packet.matcher_index = command ? command->matcherIndex : -1;
            packet.line_index = command ? command->lineIndex : -1;
            packet.manual_index = g_suiteManualCase
                ? g_suiteStatus.currentCommand : -1;
            packet.setup_frame = g_status.startFrame - 10;
            packet.start_frame = g_status.startFrame;
            packet.attacker_idle_action = g_suiteIdleActions[0];
            packet.defender_idle_action = g_suiteIdleActions[1];
            packet.facing_left = g_suiteFacingLeft ? 1u : 0u;
            packet.round_seed = g_suiteRngSeed;
            snprintf(packet.move_name, sizeof(packet.move_name), "%s",
                     g_suiteStatus.currentName);
            ResetNetSuiteMoveState(packet.move_id, packet.start_frame);
            g_netSuiteSetupFrame = packet.setup_frame;
            g_netSuiteSetupApplied = false;
            g_netSuiteRestartPending = false;
            Netplay_SendAutoTestStart(packet);
            HookLog("AutoTestNet: host started move %u %s at frame %d\n",
                    packet.move_id, packet.move_name, packet.start_frame);
        }
        g_suiteStatus.commandStartFrame = frame;
        g_suiteDeadline = frame + SUITE_COMMAND_TIMEOUT;
        g_suitePhase = SuitePhase::RunningCommand;
        return;
    }
    case SuitePhase::RunningCommand:
        if (!g_status.active && frame >= g_status.startFrame) {
            g_suiteStatus.waitingForCompletion = true;
            g_suiteDeadline = frame + SUITE_COMPLETION_TIMEOUT;
            g_suitePhase = SuitePhase::WaitingForCompletion;
        } else if (frame > g_suiteDeadline) {
            FinishSuiteCommand(false, "input timed out");
        }
        return;
    case SuitePhase::WaitingForCompletion: {
        uint64_t attacker = GetActiveCharacter(g_suiteStatus.playerIndex);
        uint64_t defender = GetActiveCharacter(g_suiteStatus.playerIndex ^ 1);
        float life = GetCharacterLife(defender);
        if (life < g_suiteDefenderLife)
            g_suiteStatus.dealtDamage = true;

        g_suiteStatus.charactersStable =
            CharacterActionsAreIdle(attacker, defender);
        g_suiteStatus.shotCount = CountActiveShots();
        g_suiteStatus.shotsClear = g_suiteStatus.shotCount == 0;
        g_suiteStatus.cutinClear = !HasActiveCutin();

        if (g_netSuiteActive) {
            if (frame > g_suiteDeadline)
                FinishSuiteCommand(false, "network checksum stabilization timed out");
            return;
        }

        bool quiet = g_suiteStatus.charactersStable;
        g_suiteStatus.quietFrames = quiet ? g_suiteStatus.quietFrames + 1 : 0;
        if (g_suiteStatus.quietFrames >= SUITE_QUIET_FRAMES) {
            FinishSuiteCommand(true, "input completed and returned to idle");
        } else if (frame > g_suiteDeadline) {
            FinishSuiteCommand(false, "did not return to idle");
        }
        return;
    }
    case SuitePhase::WaitingForReset:
        if (g_netSuiteActive) {
            if (!g_netSuiteRestartExecuted)
                return;
            if (actionStep != 5) {
                g_netSuiteRestartLeftFighting = true;
                return;
            }
            if (!g_netSuiteRestartLeftFighting)
                return;
        }
        if (actionStep == 5) {
            ++g_suiteStatus.currentCommand;
            g_suiteStatus.waitingForReset = false;
            g_suitePhase = SuitePhase::StartCommand;
        }
        return;
    case SuitePhase::Idle:
        return;
    }
}

extern "C" AutoTestSuiteStatus AutoTest_GetSuiteStatus()
{
    return g_suiteStatus;
}

extern "C" bool AutoTest_GetForcedClientResim(int32_t currentFrame,
                                                int32_t* outStartFrame,
                                                int32_t* outEndFrame)
{
    if (!g_netSuiteActive || !g_netSuiteClient || g_netSuiteRestartPending ||
        !Netplay_IsActive() || Netplay_IsHost() || !outStartFrame || !outEndFrame)
        return false;

    int32_t frame = g_netSuiteLastForcedFrame + 1;
    if (frame < g_netSuiteStartFrame)
        frame = g_netSuiteStartFrame;
    if (frame >= currentFrame || !Controller_IsFrameConfirmed(frame))
        return false;
    g_netSuiteLastForcedFrame = frame;
    *outStartFrame = frame;
    *outEndFrame = frame + 1;
    return true;
}

extern "C" void AutoTest_OnResimFrame(int32_t frame, uint32_t checksum)
{
    if (!g_netSuiteActive || !g_netSuiteClient || g_netSuiteRestartPending ||
        frame < g_netSuiteStartFrame || !Controller_IsFrameConfirmed(frame))
        return;
    AutoTestFramePacket packet = {};
    packet.move_id = g_netSuiteMoveId;
    packet.frame = frame;
    packet.checksum = checksum;
    if (BothCharactersIdle())
        packet.flags |= AUTOTEST_FRAME_CHARACTERS_IDLE;
    Netplay_SendAutoTestFrame(packet);
}

extern "C" void AutoTest_OnLiveFrame(int32_t frame, uint32_t checksum)
{
    if (!g_netSuiteActive || g_netSuiteClient || g_netSuiteRestartPending ||
        frame < g_netSuiteStartFrame)
        return;
    uint32_t idx = (uint32_t)frame % NET_SUITE_HISTORY;
    g_netSuiteLiveFrames[idx] = {frame, checksum, BothCharactersIdle()};
}

extern "C" void AutoTest_OnDesyncDetected(int32_t frame)
{
    if (!g_netSuiteActive || !g_netSuiteClient || g_netSuiteMoveId == 0)
        return;

    if (!g_suiteCharacterStem[0]) {
        GetCommandListTextResource(g_suiteStatus.playerIndex,
                                   GetActiveCharacter(g_suiteStatus.playerIndex));
        int32_t characterId = g_commandListText[g_suiteStatus.playerIndex].characterId;
        using GetCharacterName_t = const char*(__fastcall*)(int32_t);
        static GetCharacterName_t getCharacterName =
            (GetCharacterName_t)_addr(ADDR_GET_CHARACTER_NAME);
        const char* name = characterId > 0 ? getCharacterName(characterId) : nullptr;
        snprintf(g_suiteCharacterStem, sizeof(g_suiteCharacterStem), "%s",
                 name && name[0] ? name : "unknown");
        for (char* c = g_suiteCharacterStem; *c; ++c) {
            if (*c >= 'A' && *c <= 'Z')
                *c = (char)(*c - 'A' + 'a');
            else if (!((*c >= 'a' && *c <= 'z') ||
                       (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
                *c = '_';
        }
    }

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "autotest/%s_%u_client.ndjson",
             g_suiteCharacterStem, g_netSuiteMoveId);
    Rollback_ExportNetplayHistory(frame, path);
}

extern "C" void AutoTest_PrepareForRestart()
{
    if (!g_netSuiteActive)
        return;
    g_netSuiteRestartPending = true;
    g_status.active = false;
    g_status.currentInput = 0;
}

extern "C" void AutoTest_OnTrainingRestartExecuted()
{
    if (!g_netSuiteActive)
        return;
    g_netSuiteRestartExecuted = true;
    g_netSuiteRestartLeftFighting = true;
}
