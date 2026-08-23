#pragma once

#include <cstdint>
#include <d3d9.h>

// Frame stepper for training mode.
// Pause game execution and advance one frame at a time with keyboard controls.
void FrameStepper_Init();
void FrameStepper_InitNetplay();
void FrameStepper_Process();
void FrameStepper_Draw();
void FrameStepper_DrawAutoTest();
void FrameStepper_SetDevice(LPDIRECT3DDEVICE9 device);
void FrameStepper_InvalidateDeviceObjects();
void FrameStepper_Capture(LPDIRECT3DDEVICE9 pDevice);
void FrameStepper_SetTrainingFrameLimit(int32_t frames);
void UpdateKeyboardState();
bool KeyDown(int vk);
bool KeyPressed(int vk);
extern "C" void FrameStepper_ScheduleAutoTestRestart(uint32_t roundSeed);
extern "C" bool FrameStepper_ScheduleRosterRestart(int32_t targetFrame,
                                                    uint32_t roundSeed);
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" bool FrameStepper_IsMatchRestartPending();
extern "C" bool FrameStepper_RequestMatchRestart();
extern "C" uint32_t FrameStepper_GetBattleGeneration();
extern "C" bool FrameStepper_IsDesyncCheckReady();
extern "C" void FrameStepper_NotifyFighting();
