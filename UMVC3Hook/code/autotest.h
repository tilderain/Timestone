#pragma once

#include <cstdint>

struct AutoTestStatus {
    bool active;
    int32_t playerIndex;
    int32_t scriptIndex;
    int32_t startFrame;
    int32_t currentFrame;
    int32_t scriptFrame;
    uint32_t currentInput;
    int32_t lastSuccessFrame;
    int32_t lastSuccessMatcher;
    int32_t lastSuccessLine;
    int32_t lastSuccessListId;
    int32_t lastSuccessAnimation;
    uint32_t lastAction;
};

constexpr int AUTOTEST_MAX_RUNTIME_COMMANDS = 512;

struct AutoTestRuntimeCommand {
    char name[96];
    char commandListName[128];
    char commandListPath[128];
    int32_t commandListTextCount;
    int32_t commandListCategory;
    int32_t commandListAttackCount;
    int32_t playerIndex;
    int32_t matcherIndex;
    int32_t lineIndex;
    int32_t lineCount;
    int32_t cmdStep;
    float time;
    float chargeTime;
    uint32_t padBuf;
    float failTime;
    uint32_t lineFlags;
    uintptr_t character;
    uintptr_t matcher;
    uintptr_t resource;
    int32_t successListId;
    int32_t successAnimation;
    int32_t successThrow;
    int32_t successTdown;
    float successDelay;
    int32_t successWork;
    int32_t commandStepCount;
    int32_t commandState;
    uint32_t commandFlags;
    int32_t commandListVariantToken;
    float lifeRateMin;
    float lifeRateMax;
    float gaugeRateMin;
    float gaugeRateMax;
    int32_t statusRequired;
    int32_t statusForbidden;
    int32_t abilityRequired;
    int32_t abilityForbidden;
    float heightMin;
    float heightMax;
    int32_t jumpCount;
    int32_t airSpecialCount;
    uint32_t limitFlags;
    uint32_t characterFlagsRequired;
    uint32_t characterFlagsForbidden;
    uint32_t animationFlagsRequired;
    uint32_t animationFlagsForbidden;
    uint32_t programFlagsRequired;
    uint32_t programFlagsForbidden;
};

struct AutoTestSuiteStatus {
    bool active;
    bool waitingForCompletion;
    bool waitingForReset;
    bool dealtDamage;
    bool charactersStable;
    bool shotsClear;
    bool cutinClear;
    int32_t playerIndex;
    int32_t currentCommand;
    int32_t commandCount;
    int32_t passedCount;
    int32_t failedCount;
    int32_t currentMatcher;
    int32_t currentLine;
    int32_t commandStartFrame;
    int32_t quietFrames;
    int32_t requiredQuietFrames;
    int32_t shotCount;
    uint32_t attackerAction;
    uint32_t defenderAction;
    char currentName[96];
    char lastResult[160];
};

void AutoTest_InitHooks();

extern "C" bool AutoTest_GetInput(int32_t frame, uint32_t playerIndex,
                                   uint32_t* outInput);
extern "C" void AutoTest_Start(int32_t scriptIndex, int32_t playerIndex,
                                bool facingLeft);
extern "C" void AutoTest_Stop();
extern "C" AutoTestStatus AutoTest_GetStatus();
extern "C" int32_t AutoTest_GetScriptCount();
extern "C" const char* AutoTest_GetScriptName(int32_t scriptIndex);
extern "C" int32_t AutoTest_GetRuntimeCommands(
    AutoTestRuntimeCommand* outCommands, int32_t capacity);
extern "C" bool AutoTest_StartRuntimeCommand(int32_t playerIndex,
                                               int32_t matcherIndex,
                                               int32_t lineIndex,
                                               bool facingLeft,
                                               int32_t variantToken = -1);
extern "C" const char* AutoTest_GetRuntimeExecutionError();
extern "C" bool AutoTest_StartSuite(int32_t playerIndex, bool facingLeft);
extern "C" bool AutoTest_StartVariantSuite(int32_t playerIndex,
                                             bool facingLeft);
extern "C" void AutoTest_Process();
extern "C" AutoTestSuiteStatus AutoTest_GetSuiteStatus();
extern "C" bool AutoTest_IsActive();

// Network suite integration. Only the client returns true from the forced
// replay query; the host remains the live reference timeline.
extern "C" bool AutoTest_GetForcedClientResim(int32_t currentFrame,
                                                int32_t* outStartFrame,
                                                int32_t* outEndFrame);
extern "C" void AutoTest_OnResimFrame(int32_t frame, uint32_t checksum);
extern "C" void AutoTest_OnLiveFrame(int32_t frame, uint32_t checksum);
extern "C" void AutoTest_OnDesyncDetected(int32_t frame);
extern "C" void AutoTest_PrepareForRestart();
extern "C" void AutoTest_OnTrainingRestartExecuted();
