#include "umvc3.h"
#include "umvc3menu.h"
#include <iostream>
#include "eNotifManager.h"
#include "eSettingsManager.h"
#include "framestepper.h"
#include <Windows.h>
int64 camera_ptr = 0;

void UMVC3Hooks::HookProcessStuff()
{
	TheMenu->Process();
	Notifications->Update();
	FrameStepper_Process();


	if (TheMenu->m_bFreeCam)
	{
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyXPlus))
			TheMenu->camPos.X += TheMenu->m_nFreeCameraSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyXMinus))
			TheMenu->camPos.X -= TheMenu->m_nFreeCameraSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyYPlus))
			TheMenu->camPos.Y += TheMenu->m_nFreeCameraSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyYMinus))
			TheMenu->camPos.Y -= TheMenu->m_nFreeCameraSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyZPlus))
			TheMenu->camPos.Z += TheMenu->m_nFreeCameraSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyZMinus))
			TheMenu->camPos.Z -= TheMenu->m_nFreeCameraSpeed;

		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyYawMinus))
			TheMenu->camRot.X -= TheMenu->m_nFreeCameraRotationSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyYawPlus))
			TheMenu->camRot.X += TheMenu->m_nFreeCameraRotationSpeed;

		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyRollMinus))
			TheMenu->camRot.Z -= TheMenu->m_nFreeCameraRotationSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyRollPlus))
			TheMenu->camRot.Z += TheMenu->m_nFreeCameraRotationSpeed;

		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyPitchMinus))
			TheMenu->camRot.Y -= TheMenu->m_nFreeCameraRotationSpeed;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyPitchPlus))
			TheMenu->camRot.Y += TheMenu->m_nFreeCameraRotationSpeed;

		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyFOVMinus))
			TheMenu->camFov -= 0.1f;
		if (GetAsyncKeyState(SettingsMgr->iFreeCameraKeyFOVPlus))
			TheMenu->camFov += 0.1f;
	}
}


int64 UMVC3Hooks::HookCamera(int64 camera, int64 a2)
{
	char unk[72];
	camera_ptr = camera;

	if (*(char*)(camera + 1416) == 1)
	{
		((void(__fastcall*)(int64))_addr(0x140015710))(camera + 0x230);
	}
	else
	{
		((void(__fastcall*)(int64*))_addr(0x14000BAA0))((int64*)&unk);
		((void(__fastcall*)(int64*, float, float, float, float))_addr(0x14045F7D0))((int64*)&unk, *(float *)(camera + 0x44), *(float *)(camera + 0x40), *(float *)(camera + 0x4C) * 0.017453292f, *(float *)(camera + 0x48));
		((void(__fastcall*)(int64,int64*))_addr(0x14000BA60))(a2, (int64*)&unk);

	}
	return a2;
}

void SetCamFOV(float value)
{
	if (camera_ptr)
		*(float*)(camera_ptr + 0x4C) = value;
}

float GetCamFOV()
{
	if (camera_ptr)
		return *(float*)(camera_ptr + 0x4C);
	else
		return 0.0f;
}
