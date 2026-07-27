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

void AutoTest_InitHooks();

extern "C" bool AutoTest_GetInput(int32_t frame, uint32_t playerIndex,
                                   uint32_t* outInput);
extern "C" void AutoTest_Start(int32_t scriptIndex, int32_t playerIndex,
                                bool facingLeft);
extern "C" void AutoTest_Stop();
extern "C" AutoTestStatus AutoTest_GetStatus();
extern "C" int32_t AutoTest_GetScriptCount();
extern "C" const char* AutoTest_GetScriptName(int32_t scriptIndex);
