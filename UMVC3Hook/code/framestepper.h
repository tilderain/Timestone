#pragma once

#include <cstdint>
#include <d3d9.h>

// Frame stepper for training mode.
// Pause game execution and advance one frame at a time with keyboard controls.
// Install hooks before opening the launcher; do not start networking here.
void FrameStepper_Init();
void FrameStepper_InitNetplay();
void FrameStepper_Process();
void FrameStepper_Draw();
float FrameStepper_GetStatusHudBottom();
// The frame stepper has the game paused (F3 then advances a frame).
bool FrameStepper_IsPaused();
void FrameStepper_DrawAutoTest();
void FrameStepper_ShowOpponentDisconnected();
// In-game lobby: tear down the current netplay match (rollback state, battle
// handshake, transport) without ending the process.
void FrameStepper_EndNetplaySession();
void FrameStepper_SetDevice(LPDIRECT3DDEVICE9 device);
void FrameStepper_InvalidateDeviceObjects();
void FrameStepper_Capture(LPDIRECT3DDEVICE9 pDevice);
void FrameStepper_SetTrainingFrameLimit(int32_t frames);
// Launch-only option: retain desync diagnostics without automatically pausing.
extern bool g_disableDesyncPause;
void UpdateKeyboardState();
bool CopyKeyboardSnapshot(uint8_t (&state)[256], bool (&pressed)[256]);
bool KeyDown(int vk);
bool KeyPressed(int vk);
extern "C" void FrameStepper_ScheduleAutoTestRestart(uint32_t roundSeed);
extern "C" bool FrameStepper_ScheduleRosterRestart(int32_t targetFrame,
                                                    uint32_t roundSeed);
extern "C" bool FrameStepper_IsBattleSynchronized();
extern "C" bool FrameStepper_IsMatchRestartPending();
extern "C" bool FrameStepper_RequestMatchRestart();
// False in a room match: no mid-match restart or delay change there for now.
extern "C" bool FrameStepper_MidMatchChangesAllowed();
// As above, with the battle giving way to the shared character select.
extern "C" bool FrameStepper_RequestSelectRestart();
extern "C" uint32_t FrameStepper_GetRestartSeed();
extern "C" void FrameStepper_SetRestartSeed(uint32_t seed);
extern "C" uint32_t FrameStepper_GetBattleGeneration();
extern "C" bool FrameStepper_IsDesyncCheckReady();
extern "C" void FrameStepper_NotifyFighting();
