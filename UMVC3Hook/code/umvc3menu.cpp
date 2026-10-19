#include "../pch.h"
#include "umvc3menu.h"
#include "../imgui/imgui.h"
#include <iostream>
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include <stdlib.h>
#include "../sigscan.h"
#include <vector>
#include "dtieditor.h"      
#include "framestepper.h"
#include "d3d9reset.h"
#include "LaunchMenu_settings.h"
#include <intrin.h>

bool runAheadEnabled = false;



#define undefined4 int




UMVC3Menu* TheMenu = new UMVC3Menu();



static void ShowHelpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted(desc);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}

}

void UMVC3Menu::Initialize()
{
	m_bIsActive = false;
	m_bIsFocused = false;
	m_bCustomCameraFOV = false;
	m_bCustomCameraPos = false;
	m_bCustomCameraRot = false;

	m_nFreeCameraSpeed = 1;
	m_nFreeCameraRotationSpeed = 1;


	camFov = 0;
	camPos = { 0,0,0 };
	camRot = { 0,0,0 };
}

void UMVC3Menu::PostInit()
{
	//hookCtrs(tramp);
	DtiEditor_Init();
	DtiEditor_InitHooks(tramp);
}

void UMVC3Menu::Process()
{
	UpdateControls();
}



void UMVC3Menu::Draw()
{
	ImGui::GetIO().MouseDrawCursor = true;
	ImGui::Begin(GetUMVC3HookVersion());
	if (ImGui::BeginTabBar("##tabs"))
	{
		if (ImGui::BeginTabItem("DTI Inspector"))
		{
			DtiEditor_Draw();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Auto Test"))
		{
			FrameStepper_DrawAutoTest();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Settings"))
		{
			int mode = D3D9Reset_GetRenderingThreadOverride() + 1;
			static bool saveFailed = false;
			if (ImGui::Combo("Rendering thread", &mode, "Game default\0Force OFF\0Force ON\0"))
				saveFailed = !D3D9Reset_SetRenderingThreadOverride(mode - 1);
			ImGui::TextWrapped("Applies between frames without restarting. OFF renders inline; "
				"ON uses the render worker and adds one tick of display latency.");
			bool singleThreaded = IsGameSingleThreaded();
			if (ImGui::Checkbox("Run game single-threaded", &singleThreaded))
				saveFailed = !SetGameSingleThreaded(singleThreaded);
			ImGui::TextWrapped("Sets game worker count to 1. May massively improve performance by 100% on pcs with bad thread work, but may decrease performance by about 10% on higher end pcs.");
			if (saveFailed) ImGui::TextUnformatted("Could not save setting; previous selection retained.");
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}

void UMVC3Menu::UpdateControls()
{
	if (KeyPressed(VK_F1))
	{
		m_bIsActive ^= 1;
	}

}

bool UMVC3Menu::GetActiveState()
{
	return m_bIsActive;
}

char* GetUMVC3HookVersion()
{
	static char buffer[512] = {};
	sprintf(buffer, "UMVC3Hook by ermaccer (0.1)");
	return buffer;
}






//SOME usefull code I had that I have to refactor into the toolset


//if (ImGui::Button("SaveFile")) {
//	auto sig = sigscan::get();
//	auto ptr = (uint8_t*)sig.start;
//	if (PostMemory == nullptr) {
//		PreMemory = new uint8_t[sig.end - sig.start];
//		PostMemory = new uint8_t[sig.end - sig.start];
//	}
//	FILE* file = fopen("./myfile222.bin", "wb");
//	fwrite(PreMemory, 1, sig.end - sig.start, file);
//	fclose(file);
//}
//if (ImGui::Button("StartBackup")) {
//	auto sig = sigscan::get();
//	auto ptr = (uint8_t*)sig.start;
//	if (PostMemory == nullptr) {
//		PreMemory = new uint8_t[sig.end - sig.start];
//		PostMemory = new uint8_t[sig.end - sig.start];
//	}

//	memcpy(PreMemory, ptr, sig.end - sig.start - 1);
//	BlacklistPtrs.clear();

//}
//if (ImGui::Button("Blacklist")) {
//	startmemorybackup = false;
//	auto sig = sigscan::get();
//	auto ptr = (uint8_t*)sig.start;
//	memcpy(PostMemory, ptr, sig.end - sig.start - 1);
//	printf("backlist\n");
//	for (uintptr_t i = 0; i < (sig.end - sig.start - 2); i++)
//	{
//		if (PreMemory[i] != PostMemory[i]) {
//			printf("%llu blacklisted\n", i);
//			if (std::find(BlacklistPtrs.begin(), BlacklistPtrs.end(), i) == BlacklistPtrs.end()) {
//				// someName not in name, add it
//				BlacklistPtrs.push_back(i);
//			}
//		}
//	}
//}

//if (ImGui::Button("Diff")) {
//	startmemorybackup = false;
//	auto sig = sigscan::get();
//	auto ptr = (uint8_t*)sig.start;
//	memcpy(PostMemory, ptr, sig.end - sig.start - 1);
//	printf("checkmem\n");
//	for (uintptr_t i = 0; i < (sig.end - sig.start - 2); i++)
//	{
//		if (PreMemory[i] != PostMemory[i]) {

//			if (std::find(ChangedPtrs.begin(), ChangedPtrs.end(), i) == ChangedPtrs.end() &&
//				std::find(BlacklistPtrs.begin(), BlacklistPtrs.end(), i) == BlacklistPtrs.end() &&
//				std::find(savedptrs.begin(), savedptrs.end(), i) == savedptrs.end()
//				) {
//				printf("%x changed:", i + 0x140000000);

//				for (size_t x = 0; x < 8; x++)
//				{
//					printf("%x ", PreMemory[i + x]);
//				}
//				printf("\n");
//			}
//		}
//		savedptrs = ChangedPtrs;
//	}
//}

//if (ImGui::Button("StopBackup")) {
//	startmemorybackup = false;
//	auto sig = sigscan::get();
//	auto ptr = (uint8_t*)sig.start;
//	memcpy(PostMemory, ptr, sig.end - sig.start - 1);
//	printf("checkmem");
//	for (uintptr_t i = 0; i < (sig.end - sig.start - 2); i++)
//	{
//		if (PreMemory[i] != PostMemory[i]) {
//			printf("%llu changed", i);
//			if (std::find(ChangedPtrs.begin(), ChangedPtrs.end(), i) == ChangedPtrs.end() && 
//				std::find(BlacklistPtrs.begin(), BlacklistPtrs.end(), i) == BlacklistPtrs.end() && 
//				std::find(savedptrs.begin(), savedptrs.end(), i) == savedptrs.end()
//				) {
//				// someName not in name, add it
//				ChangedPtrs.push_back(i);
//				savedptrs.push_back(i);
//			}
//		}
//		//savedptrs = ChangedPtrs;
//	}
//}
//if (ImGui::Button("Backup")) {
//	printf("Backup\n");
//	for (auto ptr : ChangedPtrs) {
//		printf("%x\n", ptr);
//	}
//	printf("sleeping");
//	//Sleep(5000);
//}

//{
//	//if (ImGui::Button("Scan")) {
//	//	mgrAddr = sigscan::get().scan("\x61\x6E\x61\x67\x65\x72\x2D\x31", "xxxxx") - 107;
//	//	printf("scanned %llu\n",(uint64_t)mgrAddr);
//	//		Sleep(2000);

//	//}
//	//if (ImGui::Button("Save5")) {
//	//	if(mgrAddr == 0)
//	//		mgrAddr = sigscan::get().scan("\x61\x6E\x61\x67\x65\x72\x2D\x31", "xxxxx") - 107;
//	//	printf("scanned %llu\n", (uint64_t)mgrAddr);
//	//	Sleep(2000);
//	//	for (size_t i = 0; i < 9800; i++)
//	//	{
//	//		//Sleep(100);
//	//		//printf("%d\n", i);
//	//		mgState[i] = *((uint8_t*)(mgrAddr + i));
//	//	}
//	//}

			// Add the test macro back
			/*		Test(0x14004bd30,FUN_14004bd30)
						Test(0x1400240f0, FUN_1400240f0)
						Test(0x140001b80, FUN_140001b80)
						Test(0x140004a50, FUN_140004a50)
						Test(0x140003aa0, FUN_140003aa0)
						Test(0x1402aa5e0, FUN_1402aa5e0)*/
						//Test(0x14000cee0, FUN_14000cee0)//thing
						//Test(0x140001b50, FUN_140001b50)
						//Test(0x140455550, FUN_140455550)
			/*			Test(0x140001030, FUN_140001030)
						Test(0x140004a40, FUN_140004a40)
						Test(0x1402aab10, FUN_1402aab10)*/
