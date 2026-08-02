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
void FrameStepper_Capture(LPDIRECT3DDEVICE9 pDevice);
extern "C" void FrameStepper_ScheduleAutoTestRestart(uint32_t roundSeed);
