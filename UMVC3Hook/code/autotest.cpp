#include "../pch.h"
#include "autotest.h"
#include "controller.h"
#include "mk10utils.h"
#include "rollback.h"
#include "../kiero/minhook/include/MinHook.h"

#include <cstddef>
#include <iterator>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr uintptr_t ADDR_UPDATE_COMMAND_MATCHER = 0x140028D10;

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
            frame, SCRIPTS[g_status.scriptIndex].name, matcherIndex, lineIndex,
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

    const InputScript& script = SCRIPTS[g_status.scriptIndex];
    int32_t scriptFrame = frame - g_status.startFrame;
    if (scriptFrame >= ScriptLength(script)) {
        g_status.active = false;
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
    g_facingLeft = facingLeft;
    HookLog("AutoTest: scheduled %s for P%d at frame %d facing=%s\n",
            SCRIPTS[scriptIndex].name, playerIndex + 1, g_status.startFrame,
            facingLeft ? "left" : "right");
}

extern "C" void AutoTest_Stop()
{
    g_status.active = false;
    g_status.currentInput = 0;
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
