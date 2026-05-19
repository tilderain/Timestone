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
#include <Windows.h>
#include <cstdio>
#include "kiero/minhook/include/MinHook.h"
//#include "code/generated.h"

// Log file for hook debugging
static FILE* g_hookLog = nullptr;

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

	if (TheMenu->GetActiveState())
	{
		ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);
		return true;
	}

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
		init = true;
	}

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


// (Training mode input automation removed - using code hooks instead)

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
	// Open log file for hook debugging
	g_hookLog = fopen("UMVC3Hook.log", "w");
	if (g_hookLog) {
		fprintf(g_hookLog, "=== UMVC3Hook Log Start ===\n");
		fflush(g_hookLog);
	}

	if (SettingsMgr->bEnableConsoleWindow)
	{
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
	}

	// Always redirect stderr to the log file to capture Wine crash logs
	freopen("UMVC3Hook.log", "a", stderr);

	HookLog("UMVC3Hook::OnInitializeHook() | Begin!\n");

	// Patch single-instance mutex check: JNZ -> JMP to allow multiple instances
	Memory::VP::Patch<uint8_t>(_addr(0x14051E1CB), 0xEB);

	// Check for command-line arg from spawned instance
	LPWSTR cmdLine = GetCommandLineW();
	bool trainingArg = false;
	if (wcsstr(cmdLine, L"-training")) {
		trainingArg = true;
		SetLaunchOption(LAUNCH_TRAINING);
		HookLog("UMVC3Hook: Detected -training argument, entering training mode\n");
	}

	// Show launch menu on first instance only
	LaunchOption opt = GetLaunchOption();
	if (!trainingArg) {
		opt = ShowLaunchMenu();
	}

	if (opt == LAUNCH_TRAINING || opt == LAUNCH_DUAL_TRAINING) {
		// Patch aTitleLogo__processBootSequence @ 0x1402179ab: change JZ (0x74) to JMP (0xEB).
		// When the caution resource loads, the original code creates a MtAnim display object
		// and attaches it (which shows the epilepsy warning on screen). By jumping past that,
		// we skip the display entirely but still do the archive loading that follows.
		// Since param_1+0x60 stays NULL, sub-state 1 (the wait loop) immediately advances to
		// cleanup -- the caution screen never renders.
		Memory::VP::Patch<uint8_t>(_addr(0x1402179ab), 0xEB);
		HookLog("UMVC3Hook: Training mode - patched caution screen skip\n");
	}

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
		// Spawn a second game instance with -training arg
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);

		// Build command line for second instance
		wchar_t spawnCmd[MAX_PATH + 32];
		swprintf(spawnCmd, MAX_PATH + 32, L"\"%s\" -training", exePath);

		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi;
		if (CreateProcessW(exePath, spawnCmd, NULL, NULL, FALSE,
			CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			HookLog("UMVC3Hook: Spawned second instance with -training\n");
		}
		else {
			HookLog("UMVC3Hook: Failed to spawn second instance (error %lu)\n", GetLastError());
		}
	}

	TheMenu->Initialize();


	Trampoline* tramp = Trampoline::MakeTrampoline(GetModuleHandle(nullptr));
	TheMenu->tramp = tramp;
	InjectHook(_addr(0x14001B451), tramp->Jump(&UMVC3Camera::HookedSetRotation), PATCH_CALL);
	InjectHook(_addr(0x14001B471), tramp->Jump(&UMVC3Camera::HookedSetPosition), PATCH_CALL);
	InjectHook(_addr(0x14001A490), tramp->Jump(UMVC3Hooks::HookCamera), PATCH_JUMP);

	// Training mode: handled via code hooks (aTitleLogo__playLogoMovies patch) and
	// set the battle type to 5 (~10s after game loads) to trigger training.

	TheMenu->PostInit();
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

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (CheckGame())
		{
			SettingsMgr->Init();
			DisableThreadLibraryCalls(hMod);
			CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
			OnInitializeHook();
		}
		break;
	case DLL_PROCESS_DETACH:
		kiero::shutdown();
		break;
	}
	return TRUE;
}




