// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <iostream>
#include "code/umvc3.h"
#include "MemoryMgr.h"
#include "includes.h"
#include "code/umvc3menu.h"
#include "code/eNotifManager.h"
#include "utils/Trampoline.h"
#include "code/eSettingsManager.h"
#include "code/UMVC3Camera.h"
#include "code/LaunchMenu.h"
#include "code/framestepper.h"
#include "code/trainingboot.h"
#include "code/controller.h"
#include <Windows.h>
#include <cstdio>
#include "kiero/minhook/include/MinHook.h"
#include <enet/enet.h>
//#include "code/generated.h"

// Log file for hook debugging
FILE* g_hookLog = nullptr;

void HookLogSetFile(const char* filename)
{
	if (g_hookLog) {
		fclose(g_hookLog);
		g_hookLog = nullptr;
	}
	g_hookLog = fopen(filename, "w");
	if (g_hookLog) {
		fprintf(g_hookLog, "=== UMVC3Hook Log Start (switched to %s) ===\n", filename);
		fflush(g_hookLog);
	}
	// Also redirect stderr to the new log file for Wine crash logs
	freopen(filename, "w", stderr);
}

void HookLog(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	// Always print to console/debugger
	vprintf(fmt, args);
	va_end(args);

	// Also write to log file
	if (g_hookLog) {
		va_list args2;
		va_start(args2, fmt);
		vfprintf(g_hookLog, fmt, args2);
		fflush(g_hookLog);
		va_end(args2);
	}
}

using namespace Memory::VP;

// Auto-connect target (passed via -connect on command line for spawned 2nd instance)
static char g_autoConnectIp[64] = "";
static int  g_autoConnectPort = 0;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

EndScene oEndScene = NULL;
WNDPROC oWndProc;
static HWND window = NULL;


void InitImGui(LPDIRECT3DDEVICE9 pDevice)
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
	ImGui_ImplWin32_Init(window);
	ImGui_ImplDX9_Init(pDevice);

	ImGuiStyle* style = &ImGui::GetStyle();
	style->WindowRounding = 6.0f;
	style->ItemSpacing = ImVec2(7, 5.5);
	style->FrameRounding = 2.0f;
	style->FramePadding = ImVec2(6, 4.25);

}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

	switch (uMsg)
	{
	case WM_KILLFOCUS:
		TheMenu->m_bIsFocused = false;
		break;
	case WM_SETFOCUS:
		TheMenu->m_bIsFocused = true;
		break;
		break;
	default:
		break;
	}

	// Always forward window messages to ImGui so overlay controls (e.g.
	// netplay delay [+]/[-] buttons) work even when the debug menu is closed.
	// ImGui_ImplWin32_WndProcHandler returns nonzero only if it handled the
	// message — in that case the game should not process it further.
	if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
		return true;

	return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}



bool init = false;
bool first_msg = true;
long __stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice)
{
	if (!init)
	{
		D3DDEVICE_CREATION_PARAMETERS params;
		pDevice->GetCreationParameters(&params);
		window = params.hFocusWindow;
		oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
		InitImGui(pDevice);
		FrameStepper_SetDevice(pDevice);
		init = true;
	}

	// Capture the current back buffer if a snapshot was requested (pause / advance)
	FrameStepper_Capture(pDevice);

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::GetIO().MouseDrawCursor = false;
	if (first_msg)
	{
		Notifications->SetNotificationTime(5500);
		Notifications->PushNotification("UMVC3Hook %s is running! Press F1 to open the menu.", UMVC3HOOK_VERSION);
		first_msg = false;
		
	}
	Notifications->Draw();

	if (TheMenu->GetActiveState())
	{
		TheMenu->Draw();
	}

	FrameStepper_Draw();

	ImGui::EndFrame();

	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

	return oEndScene(pDevice);
}



DWORD WINAPI MainThread(LPVOID lpReserved)
{
	Sleep(5000);
	Notifications->Init();
	bool attached = false;
	do
	{
		if (kiero::init(kiero::RenderType::D3D9) == kiero::Status::Success)
		{
			kiero::bind(42, (void**)& oEndScene, (void*)hkEndScene);
			attached = true;
		}
	} while (!attached);
	return TRUE;
}

void WINAPI HookUpdate()
{
	while (true)
	{
		UMVC3Hooks::HookProcessStuff();
		Sleep(1);
	}
}

// MinHook trampoline for aTitleLogo__update (0x140217700)
typedef void(__fastcall* TitleLogoUpdate_t)(uintptr_t param_1);
static TitleLogoUpdate_t oTitleLogoUpdate = nullptr;

void __fastcall Hook_titleLogoUpdate(uintptr_t param_1)
{
	oTitleLogoUpdate(param_1);

	// Skip logo movies (state 1) when training mode is active.
	// The caution screen (state 0) is handled by a byte patch in processBootSequence
	// that skips the MtAnim display creation, keeping archive loading intact.
	if (GetLaunchOption() == LAUNCH_TRAINING || GetLaunchOption() == LAUNCH_DUAL_TRAINING) {
		int parentState = *(int*)(param_1 + 0x78);
		if (parentState == 1) {
			*(int*)(param_1 + 0x7c) = 0;
			*(int*)(param_1 + 0x78) = 2;
			HookLog("UMVC3Hook: Skipped logo movies (state 1 -> 2)\n");
		}
	}
}


void OnInitializeHook()
{
	// Open log file for hook debugging (append mode so 2nd instance doesn't clobber)
	HookLogSetFile("UMVC3Hook.log");

	if (SettingsMgr->bEnableConsoleWindow)
	{
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
	}

	HookLog("UMVC3Hook::OnInitializeHook() | Begin!\n");

	// Patch single-instance mutex check: JNZ -> JMP to allow multiple instances
	Memory::VP::Patch<uint8_t>(_addr(0x14051E1CB), 0xEB);

	// Disable P2 auto-pause on pad disconnect in offline versus (battle type 0).
	// FUN_1402b4140 at 0x1402B4140 checks slot+0x60 (device active) and
	// slot+0x63 (ready counter) to determine if a pad slot is "alive".
	// When P2's physical controller is disconnected, this returns false
	// even though the pad-to-player mapping says P2 is assigned (slot 1),
	// causing the game to enter a "Press Start" / wait-for-P2 state.
	// Patch: replace the function body with MOV AL,1; RET (B0 01 C3).
	// Note: single-byte patches used because _MEMORY_NO_CRT disables initializer_list overloads.
	Memory::VP::Patch<uint8_t>(_addr(0x1402B4140), 0xB0);  // MOV AL,
	Memory::VP::Patch<uint8_t>(_addr(0x1402B4141), 0x01);  //   1
	Memory::VP::Patch<uint8_t>(_addr(0x1402B4142), 0xC3);  // RET
	HookLog("UMVC3Hook: Patched FUN_1402b4140 to always return true (slot always alive)\n");

	// Check for command-line args using GetCommandLineA (avoids wchar_t size mismatch
	// between Linux/GCC (32-bit) and Windows/MSVC (16-bit) under Wine/Proton)
	const char* cmdLineA = GetCommandLineA();
	HookLog("UMVC3Hook: Raw Command Line: %s\n", cmdLineA);

	bool trainingArg = false;
	if (strstr(cmdLineA, "-training")) {
		trainingArg = true;
		SetLaunchOption(LAUNCH_TRAINING);
		HookLog("UMVC3Hook: Detected -training argument, entering training mode\n");
	}

	// Check for -connect argument (format: -connect=IP:PORT) from dual training spawn
	const char* connectArg = strstr(cmdLineA, "-connect=");
	if (connectArg) {
		const char* connStr = connectArg + 9; // Skip past "-connect="

		// Isolate the connection string (stop at space, double-quote, or end)
		char tempConn[128] = {};
		int len = 0;
		while (connStr[len] && connStr[len] != ' ' && connStr[len] != '\"' && len < 127) {
			tempConn[len] = connStr[len];
			len++;
		}
		tempConn[len] = '\0';

		// Parse IP:PORT from tempConn
		const char* colon = strchr(tempConn, ':');
		if (colon) {
			size_t ipLen = colon - tempConn;
			if (ipLen < sizeof(g_autoConnectIp)) {
				strncpy(g_autoConnectIp, tempConn, ipLen);
				g_autoConnectIp[ipLen] = '\0';
			}
			g_autoConnectPort = atoi(colon + 1);
		}
		else {
			strncpy(g_autoConnectIp, tempConn, sizeof(g_autoConnectIp) - 1);
			g_autoConnectIp[sizeof(g_autoConnectIp) - 1] = '\0';
			g_autoConnectPort = 10800;
		}

		HookLog("UMVC3Hook: Detected -connect argument, target %s:%d\n",
			g_autoConnectIp, g_autoConnectPort);
	}

	// Show launch menu on first instance only
	LaunchOption opt = GetLaunchOption();
	if (!trainingArg) {
		opt = ShowLaunchMenu();
	}

	//if (opt == LAUNCH_TRAINING || opt == LAUNCH_DUAL_TRAINING) {
		// Patch aTitleLogo__processBootSequence @ 0x1402179ab: change JZ (0x74) to JMP (0xEB).
		// When the caution resource loads, the original code creates a MtAnim display object
		// and attaches it (which shows the epilepsy warning on screen). By jumping past that,
		// we skip the display entirely but still do the archive loading that follows.
		// Since param_1+0x60 stays NULL, sub-state 1 (the wait loop) immediately advances to
		// cleanup -- the caution screen never renders.
		Memory::VP::Patch<uint8_t>(_addr(0x1402179ab), 0xEB);
		HookLog("UMVC3Hook: Training mode - patched caution screen skip\n");
	//}

	// Hook aTitleLogo__update to skip logo movies (epilepsy/Capcom/Marvel) in training mode
	MH_STATUS mhStatus = MH_Initialize();
	if (mhStatus == MH_OK || mhStatus == MH_ERROR_ALREADY_INITIALIZED) {
		if (MH_CreateHook(
			(LPVOID)_addr(0x140217700),
			(LPVOID)Hook_titleLogoUpdate,
			(LPVOID*)&oTitleLogoUpdate
		) == MH_OK) {
			MH_EnableHook((LPVOID)_addr(0x140217700));
			HookLog("UMVC3Hook: Hooked aTitleLogo__update\n");
		} else {
			HookLog("UMVC3Hook: FAILED to hook aTitleLogo__update\n");
		}
	}

	if (opt == LAUNCH_DUAL_TRAINING) {
		// Switch host log to host-specific file so host + client don't clobber each other
		HookLogSetFile("UMVC3Hook_host.log");
		HookLog("UMVC3Hook: Dual training host - switched to host log file\n");

		const LaunchDialogConfig& dlgCfg = GetLaunchDialogConfig();

		// 1. Create enet host FIRST so it's listening before the 2nd instance starts
		ENetAddress addr;
		addr.host = ENET_HOST_ANY;
		addr.port = (enet_uint16)dlgCfg.hostPort;

		ENetHost* server = enet_host_create(&addr, 1, 2, 0, 0);
		if (!server) {
			HookLog("UMVC3Hook: Dual training - FAILED to create enet host on port %d\n",
				dlgCfg.hostPort);
		} else {
			SetNetworkHandles(server, nullptr);
			HookLog("UMVC3Hook: Dual training - hosting on port %d, waiting for 2nd instance...\n",
				dlgCfg.hostPort);
		}

		// 2. Spawn the second instance with -connect so it auto-connects back
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);

		// Use MultiByteToWideChar (locale-independent) for the IP address
		wchar_t wIp[64];
		MultiByteToWideChar(CP_ACP, 0, dlgCfg.hostIp, -1, wIp, 64);

		// FIX: Use %ls for wide strings (GCC/MinGW treats %s as narrow char*, even in swprintf)
		wchar_t spawnCmd[MAX_PATH + 64];
		swprintf(spawnCmd, MAX_PATH + 64, L"\"%ls\" -training -connect=%ls:%d",
			exePath, wIp, dlgCfg.hostPort);

		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi;
		if (CreateProcessW(exePath, spawnCmd, NULL, NULL, FALSE,
			CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			HookLog("UMVC3Hook: Spawned second instance with -training -connect=%s:%d\n",
				dlgCfg.hostIp, dlgCfg.hostPort);
		}
		else {
			HookLog("UMVC3Hook: Failed to spawn second instance (error %lu)\n", GetLastError());
		}

		// 3. Wait for connection (only if host was created)
		if (server) {
			ENetEvent event;
			int timeoutMs = 15000;  // 15s timeout (generous for localhost)
			int waited = 0;
			while (waited < timeoutMs) {
				int result = enet_host_service(server, &event, 100);
				if (result > 0) {
					if (event.type == ENET_EVENT_TYPE_CONNECT) {
						event.peer->data = (void*)"connected";
						SetNetworkHandles(server, event.peer);
						HookLog("UMVC3Hook: Dual training - 2nd instance connected!\n");
						break;
					}
					else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
						enet_packet_destroy(event.packet);
					}
				}
				waited += 100;
			}
			if (!GetLaunchDialogConfig().enetPeer) {
				HookLog("UMVC3Hook: Dual training - TIMEOUT waiting for 2nd instance (15s)\n");
			}
		}
	}

	// Auto-connect: second instance spawned by dual training connects back to host
	if (g_autoConnectPort != 0 && g_autoConnectIp[0] != '\0') {
		// Switch client log to client-specific file so host + client don't clobber each other
		HookLogSetFile("UMVC3Hook_client.log");

		HookLog("UMVC3Hook: Auto-connecting to host %s:%d\n",
			g_autoConnectIp, g_autoConnectPort);

		ENetHost* client = enet_host_create(NULL, 1, 2, 0, 0);
		if (client) {
			ENetAddress targetAddr;
			if (enet_address_set_host_ip(&targetAddr, g_autoConnectIp) != 0) {
				enet_address_set_host(&targetAddr, g_autoConnectIp);
			}
			targetAddr.port = (enet_uint16)g_autoConnectPort;

			ENetPeer* peer = enet_host_connect(client, &targetAddr, 2, 0);
			if (peer) {
				ENetEvent event;
				int timeoutMs = 15000;
				int waited = 0;
				while (waited < timeoutMs) {
					int result = enet_host_service(client, &event, 100);
					if (result > 0) {
						if (event.type == ENET_EVENT_TYPE_CONNECT) {
							peer->data = (void*)"connected";
							SetNetworkHandles(client, peer);
							HookLog("UMVC3Hook: Auto-connect SUCCESS to %s:%d\n",
								g_autoConnectIp, g_autoConnectPort);
							break;
						}
						else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
							enet_packet_destroy(event.packet);
						}
					}
					waited += 100;
				}
				if (!GetLaunchDialogConfig().enetPeer) {
					HookLog("UMVC3Hook: Auto-connect TIMEOUT (15s) - destroying client\n");
					enet_host_destroy(client);
				}
			}
			else {
				HookLog("UMVC3Hook: Auto-connect - enet_host_connect returned NULL\n");
				enet_host_destroy(client);
			}
		}
		else {
			HookLog("UMVC3Hook: Auto-connect - enet_host_create FAILED\n");
		}
	}

	TheMenu->Initialize();


	Trampoline* tramp = Trampoline::MakeTrampoline(GetModuleHandle(nullptr));
	TheMenu->tramp = tramp;
	InjectHook(_addr(0x14001B451), tramp->Jump(&UMVC3Camera::HookedSetRotation), PATCH_CALL);
	InjectHook(_addr(0x14001B471), tramp->Jump(&UMVC3Camera::HookedSetPosition), PATCH_CALL);
	InjectHook(_addr(0x14001A490), tramp->Jump(UMVC3Hooks::HookCamera), PATCH_JUMP);

	// Training mode: handled via code hooks (aTitleLogo__playLogoMovies patch) and
	// the training boot module which intercepts aTitle__update to transition
	// directly into a battle area with training mode settings.

	if (opt == LAUNCH_TRAINING || opt == LAUNCH_DUAL_TRAINING) {
		// Backup patch: in uBattleUiTrainingCheckP2PadAssigned_maybe @ 0x1402E57EE,
		// change JNZ to JMP so the function ALWAYS returns 1 (skip P2 wait state)
		// even if sPadCheckSlotDeviceAlive_maybe somehow returns false.
		// Original: 75 44 (JNZ +0x44) → EB 44 (JMP +0x44)
		Memory::VP::Patch<uint8_t>(_addr(0x1402E57EE), 0xEB);
		HookLog("UMVC3Hook: Patched uBattleUiTrainingCheckP2PadAssigned_maybe JNZ→JMP\n");

		TrainingBoot_InitHooks();
	}

	TheMenu->PostInit();

	// Frame stepper (pause / frame-advance in training mode)
	FrameStepper_Init();

	// Controller hooks: decompiled pad/input functions with netplay P2 injection
	Controller_InitHooks();

	//InjectHook(_addr(0x140650c10), tramp->Jump(InitMainLoop), PATCH_JUMP);//parts of ui & particles
	//InjectHook(_addr(0x14004ad20), tramp->Jump(InitMainLoop), PATCH_JUMP);//parts pyshics
	//InjectHook(_addr(0x14004b9d0), tramp->Jump(InitMainLoop), PATCH_JUMP);//parts pyshics
	//InjectHook(_addr(0x14004bd30), tramp->Jump(InitMainLoop), PATCH_JUMP);//parts fighter freeze
}

bool CheckGame()
{
	char* gameName = (char*)(_addr(0x140B12D10));

	if (strcmp(gameName, "umvc3") == 0)
	{
		return true;
	}
	else
	{
		MessageBoxA(0, "Invalid game version!\nUMVC3Hook only supports latest (or it needs to be updated) Steam executable.", 0, MB_ICONINFORMATION);
		return false;
	}
}

void AntiDebugPatch()
{
	//HookLog("[AntiDebug] Applying anti-debug patches...\n");

	// Patch IsDebuggerPresent to always return FALSE
	/*auto pIsDebuggerPresent = GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsDebuggerPresent");
	if (pIsDebuggerPresent) {
		DWORD oldProtect;
		VirtualProtect((LPVOID)pIsDebuggerPresent, 3, PAGE_EXECUTE_READWRITE, &oldProtect);
		uint8_t patch[] = { 0x31, 0xC0, 0xC3 }; // xor eax,eax; ret
		memcpy(pIsDebuggerPresent, patch, 3);
		VirtualProtect((LPVOID)pIsDebuggerPresent, 3, oldProtect, &oldProtect);
		HookLog("[AntiDebug] Patched IsDebuggerPresent\n");
	}

	// Patch CheckRemoteDebuggerPresent to always return FALSE
	auto pCheckRemoteDebuggerPresent = GetProcAddress(GetModuleHandleA("kernel32.dll"), "CheckRemoteDebuggerPresent");
	if (pCheckRemoteDebuggerPresent) {
		DWORD oldProtect;
		VirtualProtect((LPVOID)pCheckRemoteDebuggerPresent, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
		// xor eax,eax; mov [rdx],eax; ret  -> returns FALSE and writes 0 to the output flag
		uint8_t patch[] = { 0x31, 0xC0, 0x89, 0x02, 0xC3 };
		memcpy(pCheckRemoteDebuggerPresent, patch, 5);
		VirtualProtect((LPVOID)pCheckRemoteDebuggerPresent, 6, oldProtect, &oldProtect);
		HookLog("[AntiDebug] Patched CheckRemoteDebuggerPresent\n");
	}

	// Use NtSetInformationProcess to clear the PEB BeingDebugged flag
	// This also handles direct PEB reads that bypass IsDebuggerPresent
	typedef long (WINAPI *NtSetInformationProcess_t)(HANDLE, ULONG, PVOID, ULONG);
	auto pNtSetInformationProcess = (NtSetInformationProcess_t)
		GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationProcess");
	if (pNtSetInformationProcess) {
		ULONG debugFlags = 1; // ProcessDebugFlags = 1 means "not being debugged"
		long status = pNtSetInformationProcess(GetCurrentProcess(), 0x1F, &debugFlags, sizeof(debugFlags));
		HookLog("[AntiDebug] NtSetInformationProcess(ProcessDebugFlags): 0x%X\n", status);
	}*/

	// Install VEH to catch the packer's crash at umvc3+0x257c58
	// The crash is a page fault from callq *0x50(%r8) with r8=1
	// We catch it and advance RIP past the faulting instruction
	/*AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* ep) -> LONG {
		static uintptr_t crashBase = (uintptr_t)GetModuleHandleA(NULL) + 0x257c58;
		if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
			ep->ExceptionRecord->ExceptionAddress == (void*)crashBase) {
			// Skip past the 4-byte callq instruction
			ep->ContextRecord->Rip += 4;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		return EXCEPTION_CONTINUE_SEARCH;
	});*/
	//HookLog("[AntiDebug] Installed VEH for packer crash at +0x257c58\n");
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (CheckGame())
		{
			//AntiDebugPatch();
			SettingsMgr->Init();
			DisableThreadLibraryCalls(hMod);

			if (enet_initialize() != 0) {
				HookLog("UMVC3Hook: Failed to initialize enet!\n");
			} else {
				HookLog("UMVC3Hook: enet initialized successfully\n");
			}

			CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
			CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)HookUpdate, hMod, 0, nullptr);
			OnInitializeHook();
		}
		break;
	case DLL_PROCESS_DETACH:
		CleanupNetworkOnExit();
		enet_deinitialize();
		kiero::shutdown();
		break;
	}
	return TRUE;
}




