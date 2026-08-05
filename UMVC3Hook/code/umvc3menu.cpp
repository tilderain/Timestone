#include "../pch.h"
#include "umvc3menu.h"
#include "../imgui/imgui.h"
#include <iostream>
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include <stdlib.h>
#include "../sigscan.h"
#include <vector>
#include "defaults.h"
#include "generated.h"
#include <XInput.h>
#include "dtieditor.h"      
#include "framestepper.h"
#include <intrin.h>

static int64 timer = GetTickCount64();
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



DWORD GetState(DWORD t, XINPUT_STATE* state) {
	CONTEXT context;
	RtlCaptureContext(&context);
	printf("%x - %x - %x - %x\n", context.Rbx, context.Rbp, context.Rbp + 0x4f4f8, context.Rbp - 0x4f4f8);
	return XInputGetState(t, state);;
}


void UMVC3Menu::Draw()
{
	ImGui::GetIO().MouseDrawCursor = true;
	ImGui::Begin(GetUMVC3HookVersion());
	if (ImGui::BeginTabBar("##tabs"))
	{
		if (ImGui::BeginTabItem("Camera Control"))
		{

			if (!recording) {
				if (ImGui::Button("Record")) {
					recording = true;
					replaying = false;
					recordReplayIndex = 0;
				}
			}
			else if (recordReplayIndex > 90) {
				if (ImGui::Button("Stop record")) {
					recording = false;
					recordedLength = recordReplayIndex - 1;
				}
			}
			if (replayAvailable && !recording) {
				if (!replaying) {

					if (ImGui::Button("Replay")) {
						replaying = true;
						recordReplayIndex = 0;
					}
				}
				else if (recordReplayIndex > 90) {
					if (ImGui::Button("Stop replay")) {
						replaying = false;
					}
				}
			}

			auto sigger = sigscan::get();

			if (!pAddrSet) {
				if (ImGui::Button("p-addresses")) {
					pAddrSet = true;
					auto mysterytable = *(uintptr_t*)_addr(0x140d533e0);
					auto block2 = *(uintptr_t*)_addr(0x140d47e68);
					for (size_t i = 0; i < 6; i++)
					{
						scriptableFighters[i].fighter = *(Fighter**)(mysterytable + 0xaa0 + (0x438 * i));
					}
				}
			}
			else {
				for (size_t i = 0; i < 6; i++)
				{
					if (scriptableFighters[i].name == nullptr) {
						ImGui::Text("p%d:%llx", i, scriptableFighters[i].fighter);
					}
					else {
						ImGui::Text("p%d:%llx %llx %s", i, scriptableFighters[i].fighter, scriptableFighters[i].tickPtr, scriptableFighters[i].name);
					}
					if (scriptableFighters[i].fighter->tagState == ETagState::Active || scriptableFighters[i].fighter->tagState == ETagState::Active2)
					{
						ImGui::Text("  X:%d", (int)scriptableFighters[i].fighter->vector.x);
						ImGui::SameLine();
						ImGui::Text("- Y:%d", (int)scriptableFighters[i].fighter->vector.y);
						ImGui::SameLine();
						ImGui::Text("- Vel:%d", (int)scriptableFighters[i].fighter->vector.xVelocity);

						ImGui::Text("  Anim:%x", scriptableFighters[i].fighter->anim);
						ImGui::Text("  Special:%x", scriptableFighters[i].fighter->specialState);

						ImGui::Text("  Grounded:%s", scriptableFighters[i].GetGroundedState());
						ImGui::Text("  TagState:%s", scriptableFighters[i].GetTagState());
					}
				}

				auto block2 = *(uintptr_t*)_addr(0x140d47e68);
				auto team1 = block2 + 0x350;
				auto team2 = block2 + 0x610;
				ImGui::Text("Team1:%llx ActiveFighter %x", team1, team1 + 0x48);
				ImGui::Text("Team2:%llx ActiveFighter %x", team2, team2 + 0x48);
			}

			//if (testphyics) {
			//	auto iVar3 = *(int*)(baseaddr(0x0e177e8) + 0x98);
			//	auto piVar7 = (int*)(baseaddr(0x0e177e8) + 0x9c);
			//	auto lVar1 = baseaddr(0xe177e8) + 200;
			//	auto iVar4 = *piVar7;
			//	ImGui::Text("p1tag:%x %x %x", iVar3, piVar7, lVar1);
			//	undefined8* puVar8 = nullptr;
			//	while (iVar4 < iVar3) {
			//		iVar4 = *piVar7;
			//		*piVar7 = *piVar7 + 1;
			//		puVar8 = (undefined8*)((longlong)iVar4 * 0x10 + lVar1);
			//		ImGui::Text("ptr:%x", puVar8);
			//		piVar7 = (int*)(baseaddr(0xe177e8) + 0x9c);
			//		iVar4 = *piVar7;
			//	}
			//}


			/*if (ImGui::Button("Attach")) {
				hookCharacterTicks(tramp);
			}*/



			if (ImGui::Button("Attach")) {
				attachHooks(tramp, false);
			}
			if (ImGui::Button("AttachWphys")) {
				attachHooks(tramp, true);
			}
			if (ImGui::Button("Toggle runahead")) {
				runAheadEnabled = !runAheadEnabled;
				if (runAheadEnabled) SaveState();
			}
			if (ImGui::Button("Inputs")) {
				InjectHook(_addr(0x140514bb5), tramp->Jump(GetState), PATCH_CALL);
			}

			ImGui::Text("Paused %d", (int)paused);
			if (ImGui::Button("Pause")) {
				paused = true;
			}
			if (ImGui::Button("Unpause")) {
				paused = false;
			}



			ImGui::Text("RunAhead %d", runaheadcount);
#define runaheadbtn(i) ImGui::SameLine();\
			if (ImGui::Button(#i)) {\
				runaheadcount = i;\
			}

			runaheadbtn(0)
				runaheadbtn(1)
				runaheadbtn(2)
				runaheadbtn(3)
				runaheadbtn(4)
				runaheadbtn(5)
				runaheadbtn(6)

				ImGui::Text("Paused %d", (int)paused);
			ImGui::SameLine();
			if (ImGui::Button("1")) {
				paused = true;
			}
			ImGui::SameLine();
			if (ImGui::Button("0")) {
				paused = false;
			}

			if (ImGui::Button("Save")) {
				SaveState();

			}

			if (ImGui::Button("Load")) {
				LoadState();

			}
			ImGui::EndTabItem();
		}

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
		ImGui::EndTabBar();
	}
	ImGui::End();
}

void UMVC3Menu::UpdateControls()
{
	if (GetAsyncKeyState(VK_F1))
	{
		if (GetTickCount64() - timer <= 150) return;
		timer = GetTickCount64();
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
	sprintf(buffer, "UMVC3Hook by ermaccer (%s)", UMVC3HOOK_VERSION);
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
