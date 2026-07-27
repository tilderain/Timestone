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
constexpr uintptr_t ADDR_BUTTON_REMAP = 0x14002DA30;
constexpr uintptr_t PTR_SCHARACTER = 0x140D44A70;
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

struct RuntimeCommandNameCache {
    uint64_t resource;
    uint64_t command;
    char name[96];
};

RuntimeCommandNameCache g_runtimeCommandNames[MAX_RUNTIME_COMMAND_NAMES] = {};

using UpdateCommandMatcher_t = void(__fastcall*)(uint64_t commandMatcher);
UpdateCommandMatcher_t oUpdateCommandMatcher = nullptr;

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
    using ButtonRemap_t = uint32_t(__fastcall*)(uint64_t, uint32_t, uint8_t);
    static ButtonRemap_t buttonRemap = (ButtonRemap_t)_addr(ADDR_BUTTON_REMAP);

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
        uint32_t resolved = buttonRemap(1, raw, 0);
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
    if (!successfulLine)
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
    g_status.lastSuccessListId = *(int32_t*)(commandMatcher + 0x188);
    g_status.lastSuccessAnimation = *(int32_t*)(commandMatcher + 0x18C);
    g_status.lastAction = *(uint32_t*)(character + 0x1310);

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
