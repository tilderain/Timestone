#include "../pch.h"
#include "autotest.h"
#include "controller.h"
#include "mk10utils.h"
#include "rollback.h"
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
constexpr uintptr_t SMVC3_MANAGER_ACTION_OFFSET = 0x48;
constexpr uintptr_t ADDR_EXECUTE_MATCH_PHASE = 0x140004B80;
constexpr uintptr_t ADDR_SET_RESTART_MODE = 0x14000AF90;
constexpr uintptr_t ADDR_SET_CHARACTER_POSITION = 0x140002AF0;
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

constexpr InputScript SCRIPTS[] = {
    {"QCF + Light", QCF_LIGHT, std::size(QCF_LIGHT)},
    {"QCB + Light", QCB_LIGHT, std::size(QCB_LIGHT)},
    {"DP + Light", DP_LIGHT, std::size(DP_LIGHT)},
    {"Forward Dash", FORWARD_DASH, std::size(FORWARD_DASH)},
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
float g_suiteDefenderLife = 0.0f;
int32_t g_suiteDeadline = -1;
uint32_t g_suiteIdleActions[2] = {};
constexpr int32_t SUITE_COMMAND_TIMEOUT = 600;
constexpr int32_t SUITE_COMPLETION_TIMEOUT = 3600;
constexpr int32_t SUITE_QUIET_FRAMES = 20;

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
        *(float*)(attacker + 0x50) + (g_suiteFacingLeft ? -2.0f : 2.0f),
        *(float*)(attacker + 0x54),
        *(float*)(attacker + 0x58),
        0.0f,
    };
    using SetPosition_t = void(__fastcall*)(uint64_t, const float*);
    ((SetPosition_t)_addr(ADDR_SET_CHARACTER_POSITION))(defender, position);
}

void RequestTrainingReset()
{
    uint64_t action = GetAction();
    if (!action)
        return;

    using SetRestartMode_t = void(__fastcall*)(uint64_t, int32_t);
    ((SetRestartMode_t)_addr(ADDR_SET_RESTART_MODE))(action, 3);
    using ExecuteMatchPhase_t = void(__fastcall*)(uint64_t, int32_t,
                                                   uint64_t, uint64_t);
    ((ExecuteMatchPhase_t)_addr(ADDR_EXECUTE_MATCH_PHASE))(action, 12, 0, 0);
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
    RequestTrainingReset();
    g_suitePhase = SuitePhase::WaitingForReset;
    g_suiteStatus.waitingForCompletion = false;
    g_suiteStatus.waitingForReset = true;
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
    return g_runningRuntimeCommand ? g_runtimeScript
                                   : SCRIPTS[g_status.scriptIndex];
}

const char* ActiveScriptName()
{
    return g_runningRuntimeCommand ? g_runtimeScript.name
                                   : SCRIPTS[g_status.scriptIndex].name;
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
}

bool GetRuntimeCommandNotation(uint64_t resource, uint64_t command,
                               CommandNotation& notation)
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
                           const CommandNotation& display)
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
        if (attackCount != display.attackCount)
            return false;
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
        uint32_t flags = command ? *(uint32_t*)(command + 0x20) : 0;
        HookLog("AutoTestCommandList: COMMAND matcher=%d line=%d command=0x%llX steps=%d flags=0x%08X generated=\"%s\"\n",
                matcherIndex, lineIndex, (unsigned long long)command,
                stepCount, flags, generated);
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
                               char* name, size_t capacity)
{
    name[0] = '\0';
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
    if (!GetRuntimeCommandNotation(commandResource, command, runtimeNotation))
        return;

    int32_t matchedRow = -1;
    for (int32_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
        CommandNotation displayNotation = {};
        if (!GetDisplayRowNotation(cached.rowNotation, rowIndex,
                                   displayNotation) ||
            !CommandNotationsMatch(runtimeNotation, displayNotation)) {
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
}

bool AppendRuntimeInput(uint32_t mask, uint32_t expected, int frames)
{
    if (g_runtimeScript.stepCount + 2 > MAX_RUNTIME_INPUT_STEPS)
        return false;
    uint32_t raw = FindRawInput(mask, expected);
    if (raw == UINT32_MAX)
        return false;
    g_runtimeInputSteps[g_runtimeScript.stepCount++] = {raw, frames};
    g_runtimeInputSteps[g_runtimeScript.stepCount++] = {0, 1};
    return true;
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
    g_suiteStatus.active = false;
    g_suiteStatus.waitingForCompletion = false;
    g_suiteStatus.waitingForReset = false;
    g_suitePhase = SuitePhase::Idle;
}

extern "C" AutoTestStatus AutoTest_GetStatus()
{
    return g_status;
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
                out.commandFlags = command ? *(uint32_t*)(command + 0x20) : 0;
                GetRuntimeCommandName(cacheIndex, resource, command,
                                      matcherIndex, lineIndex, out.name,
                                      sizeof(out.name));
                GetInGameCommandListName(player, character, resource, command,
                                          out.commandListName,
                                          sizeof(out.commandListName));
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

    g_runtimeScript.stepCount = 0;
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

        if (!AppendRuntimeInput(mask, expected, 2)) {
            snprintf(g_runtimeExecutionError, sizeof(g_runtimeExecutionError),
                     "Could not map command step %d (mask %08X = %08X)",
                     i, mask, expected);
            g_runtimeScript.stepCount = 0;
            return false;
        }
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
    HookLog("AutoTest: scheduled runtime command matcher=%d line=%d for P%d at frame %d\n",
            matcherIndex, lineIndex, playerIndex + 1, g_status.startFrame);
    return true;
}

extern "C" const char* AutoTest_GetRuntimeExecutionError()
{
    return g_runtimeExecutionError;
}

extern "C" bool AutoTest_StartSuite(int32_t playerIndex, bool facingLeft)
{
    if (playerIndex < 0 || playerIndex > 1)
        return false;

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
            command.resource, runtimeCommand, notation);
        bool duplicate = false;
        if (hasNotation) {
            for (int32_t existing = 0; existing < commandCount; ++existing) {
                if (g_suiteCommandHasNotation[existing] &&
                    CommandNotationsEqual(
                        notation, g_suiteCommandNotations[existing])) {
                    duplicate = true;
                    break;
                }
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
    g_suiteStatus.commandCount = commandCount;
    g_suiteStatus.currentMatcher = -1;
    g_suiteStatus.currentLine = -1;
    g_suiteStatus.requiredQuietFrames = SUITE_QUIET_FRAMES;
    g_suiteFacingLeft = facingLeft;
    g_suiteMatchedCommand = false;
    g_suitePhase = SuitePhase::StartCommand;
    HookLog("AutoTestSuite: started %d unique commands for P%d (%d duplicate notations removed)\n",
            commandCount, playerIndex + 1, duplicateCount);
    return true;
}

extern "C" void AutoTest_Process()
{
    if (!g_suiteStatus.active || g_isResimulating)
        return;

    int32_t frame = Controller_GetNetFrameCount();
    uint64_t action = GetAction();
    int32_t actionStep = action ? *(int32_t*)(action + 0xD8) : -1;

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

        const AutoTestRuntimeCommand& command =
            g_suiteCommands[g_suiteStatus.currentCommand];
        g_suiteStatus.currentMatcher = command.matcherIndex;
        g_suiteStatus.currentLine = command.lineIndex;
        snprintf(g_suiteStatus.currentName, sizeof(g_suiteStatus.currentName),
                 "%s", command.commandListName[0]
                     ? command.commandListName : command.name);
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
        if (!AutoTest_StartRuntimeCommand(g_suiteStatus.playerIndex,
                                          command.matcherIndex,
                                          command.lineIndex,
                                          g_suiteFacingLeft)) {
            FinishSuiteCommand(false, AutoTest_GetRuntimeExecutionError());
            return;
        }
        g_suiteStatus.active = true;
        g_suiteStatus.commandStartFrame = frame;
        g_suiteDeadline = frame + SUITE_COMMAND_TIMEOUT;
        g_suitePhase = SuitePhase::RunningCommand;
        return;
    }
    case SuitePhase::RunningCommand:
        if (g_suiteMatchedCommand) {
            g_suiteStatus.waitingForCompletion = true;
            g_suiteDeadline = frame + SUITE_COMPLETION_TIMEOUT;
            g_suitePhase = SuitePhase::WaitingForCompletion;
        } else if (!g_status.active && frame >= g_status.startFrame) {
            FinishSuiteCommand(false, "input ended without matching command");
        } else if (frame > g_suiteDeadline) {
            FinishSuiteCommand(false, "command matcher timed out");
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

        bool quiet = g_suiteStatus.charactersStable &&
                     g_suiteStatus.shotsClear && g_suiteStatus.cutinClear;
        g_suiteStatus.quietFrames = quiet ? g_suiteStatus.quietFrames + 1 : 0;
        if (g_suiteStatus.quietFrames >= SUITE_QUIET_FRAMES) {
            FinishSuiteCommand(true, g_suiteStatus.dealtDamage
                ? "matched, dealt damage, and returned to idle"
                : "matched and returned to idle");
        } else if (frame > g_suiteDeadline) {
            FinishSuiteCommand(false, "matched but did not return to idle");
        }
        return;
    }
    case SuitePhase::WaitingForReset:
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
