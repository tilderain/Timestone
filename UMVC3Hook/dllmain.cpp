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
#include "code/netplay.h"
#include "code/controller.h"
#include "code/diag.h"
#include "code/exception.h"
#include <Windows.h>
#include <cstdio>
#include "kiero/minhook/include/MinHook.h"
#include <enet/enet.h>
//#include "code/generated.h"

// Log file for hook debugging
FILE* g_hookLog = nullptr;
static SRWLOCK g_hookLogLock = SRWLOCK_INIT;

void HookLogSetFile(const char* filename)
{
	AcquireSRWLockExclusive(&g_hookLogLock);
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
	// Use append mode so it doesn't truncate what g_hookLog just wrote
	freopen(filename, "a", stderr);
	ReleaseSRWLockExclusive(&g_hookLogLock);
}

void HookLog(const char* fmt, ...)
{
	//return;
	AcquireSRWLockExclusive(&g_hookLogLock);
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
	ReleaseSRWLockExclusive(&g_hookLogLock);
}

using namespace Memory::VP;

// ---------------------------------------------------------------------------
// Boot timing instrumentation
// ---------------------------------------------------------------------------
static struct {
	LARGE_INTEGER start;
	LARGE_INTEGER freq;
	const char*  labels[32];
	LARGE_INTEGER stamps[32];
	int           count;
	int           dumped;
} g_bootTimer;

static void BootTimer_Init()
{
	QueryPerformanceFrequency(&g_bootTimer.freq);
	QueryPerformanceCounter(&g_bootTimer.start);
	g_bootTimer.count = 0;
	g_bootTimer.dumped = 0;
}

void BootTimer_Stamp(const char* label)
{
	int idx = g_bootTimer.count;
	if (idx >= 32) return;
	QueryPerformanceCounter(&g_bootTimer.stamps[idx]);
	g_bootTimer.labels[idx] = label;
	g_bootTimer.count = idx + 1;
}

static void BootTimer_Dump()
{
	if (g_bootTimer.dumped) return;
	g_bootTimer.dumped = 1;
	HookLog("=== BootTiming ===\n");
	double prevMs = 0.0;
	for (int i = 0; i < g_bootTimer.count; i++) {
		double ms = (double)(g_bootTimer.stamps[i].QuadPart - g_bootTimer.start.QuadPart)
			* 1000.0 / (double)g_bootTimer.freq.QuadPart;
		HookLog("  +%7.1f ms  (%+7.1f ms)  %s\n", ms, i == 0 ? 0.0 : ms - prevMs, g_bootTimer.labels[i]);
		prevMs = ms;
	}
	HookLog("=================\n");
}

// Auto-connect target (passed via -connect on command line for spawned 2nd instance)
static char g_autoConnectIp[64] = "";
static int  g_autoConnectPort = 0;

// Window corner positioning for dual training
// 0=none, 1=top-right (host), 2=bottom-left (client)
static int g_windowCorner = 0;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

EndScene oEndScene = NULL;
WNDPROC oWndProc;
static HWND window = NULL;

// Direct-polling input injector that feeds ImGui's backend directly,
// bypassing the Windows message queue entirely.
void PollKeyboardDirectly()
{
    if (!window || !TheMenu->GetActiveState())
        return;

    ImGuiIO& io = ImGui::GetIO();
    static bool prevKeys[256] = { 0 };
    static float repeatTimers[256] = { 0 };
    float deltaTime = io.DeltaTime;

    for (int vk = 8; vk < 256; vk++)
    {
        // Skip keys that aren't useful for typing or editing
        if (vk > 13 && vk < 32 && vk != 27) continue;

        bool isDown = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool wasDown = prevKeys[vk];
        prevKeys[vk] = isDown;

        bool trigger = false;
        if (isDown)
        {
            if (!wasDown)
            {
                trigger = true;
                repeatTimers[vk] = 0.400f; // Delay before key starts repeating (400ms)
            }
            else
            {
                repeatTimers[vk] -= deltaTime;
                if (repeatTimers[vk] <= 0.0f)
                {
                    trigger = true;
                    repeatTimers[vk] = 0.050f; // Key repeat rate (50ms)
                }
            }
        }

        if (trigger)
        {
            // Simulate physical key-down event for navigation and shortcut keys
            LPARAM lParam = (MapVirtualKey(vk, MAPVK_VK_TO_VSC) << 16) | 1;
            ImGui_ImplWin32_WndProcHandler(window, WM_KEYDOWN, vk, lParam);

            // Translate virtual key code to printable character
            BYTE ks[256] = { 0 };
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   ks[VK_SHIFT] = 0x80;
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) ks[VK_CONTROL] = 0x80;
            if (GetAsyncKeyState(VK_MENU) & 0x8000)    ks[VK_MENU] = 0x80;
            if (GetKeyState(VK_CAPITAL) & 0x0001)      ks[VK_CAPITAL] = 0x01;

            WCHAR wchar[4] = { 0 };
            if (ToUnicode(vk, MapVirtualKey(vk, MAPVK_VK_TO_VSC), ks, wchar, 4, 0) > 0)
            {
                for (int i = 0; wchar[i] != 0 && i < 4; i++)
                {
                    if (wchar[i] >= 32 && wchar[i] != 127)
                    {
                        ImGui_ImplWin32_WndProcHandler(window, WM_CHAR, wchar[i], 0);
                    }
                }
            }
        }
        else if (wasDown && !isDown)
        {
            // Simulate physical key-up event
            LPARAM lParam = (MapVirtualKey(vk, MAPVK_VK_TO_VSC) << 16) | 0xC0000001;
            ImGui_ImplWin32_WndProcHandler(window, WM_KEYUP, vk, lParam);
        }
    }
}


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

	// Load a Japanese-capable font so Shift-JIS property names render correctly.
	// Try loading a system font first, fall back to the embedded Shinonome font.
	io.Fonts->AddFontDefault();

	{
		ImFontConfig fontConfig;
		fontConfig.MergeMode = true;
		
fontConfig.OversampleH    = 1;
fontConfig.OversampleV    = 1;
fontConfig.PixelSnapH     = true;
fontConfig.RasterizerMultiply = 1.0f;
fontConfig.SizePixels     = 12.0f; // use exact integer sizes: 9, 10, 12, 14, 16...

		const ImWchar* jpRange = io.Fonts->GetGlyphRangesJapanese();

		ImFont* jpFont = io.Fonts->AddFontFromFileTTF(
			"C:\\Windows\\Fonts\\msgothic.ttc", 12.0f, &fontConfig, jpRange);
		if (jpFont) {
			io.Fonts->Build();
			HookLog("InitImGui: merged msgothic.ttc Japanese glyphs\n");
		} else {
			io.Fonts->Build();
			HookLog("InitImGui: WARNING - no Japanese font available\n");
		}
	}
}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_KILLFOCUS:
        TheMenu->m_bIsFocused = false;
        break;
    case WM_SETFOCUS:
        TheMenu->m_bIsFocused = true;
        break;
    default:
        break;
    }

    if (TheMenu->GetActiveState())
    {
        // Intercept keyboard Raw Input while menu is open so key presses do not trigger game actions
        if (uMsg == WM_INPUT)
        {
            alignas(RAWINPUT) BYTE buffer[256];
            UINT dwSize = sizeof(buffer);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buffer, &dwSize, sizeof(RAWINPUTHEADER)) != (UINT)-1)
            {
                RAWINPUT* raw = (RAWINPUT*)buffer;
                if (raw->header.dwType == RIM_TYPEKEYBOARD)
                {
                    return 0; // Swallow event
                }
            }
        }

        // Suppress legacy keyboard events to ensure menu typing doesn't bleed into game operations
        if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_CHAR ||
            uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP)
        {
            return 0; // Swallow event
        }
    }

    // Pass native mouse events directly through to ImGui
    if (ImGui::GetCurrentContext() != nullptr)
    {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
            return true;
    }

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}
bool init = false;
bool first_msg = true;
long __stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice)
{
	//Exception_UpdateTestHotkey();

	if (!init)
	{
		D3DDEVICE_CREATION_PARAMETERS params;
		pDevice->GetCreationParameters(&params);
		window = params.hFocusWindow;

		if (g_windowCorner) {
			RECT rect;
			GetWindowRect(window, &rect);
			int w = rect.right - rect.left;
			int h = rect.bottom - rect.top;
			int scrW = GetSystemMetrics(SM_CXSCREEN);
			int scrH = GetSystemMetrics(SM_CYSCREEN);
			int x = (g_windowCorner == 1) ? (scrW - w) : 0;
			int y = (g_windowCorner == 1) ? 0 : (scrH - h);
			SetWindowPos(window, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
			g_windowCorner = 0;
		}

		oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
		InitImGui(pDevice);
		FrameStepper_SetDevice(pDevice);
		init = true;
		BootTimer_Stamp("First hkEndScene");
		BootTimer_Dump();
	}

	// Capture the current back buffer if a snapshot was requested (pause / advance)
	FrameStepper_Capture(pDevice);

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();

	// Inject inputs directly from hardware, bypassing the Windows message queue
	PollKeyboardDirectly();

	ImGui::NewFrame();
	ImGui::GetIO().MouseDrawCursor = false;

	// Process menu logic, notifications, frame stepper, and freecam
	// (formerly in a separate HookUpdate thread with Sleep(1))
	UMVC3Hooks::HookProcessStuff();

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
	BootTimer_Stamp("MainThread start");
	Notifications->Init();
	bool attached = false;
	do
	{
		// Reuse the engine's real D3D9 device. Kiero's normal initialization
		// creates a second device, which serializes with native driver startup.
		uint64_t render = *(uint64_t*)_addr(0x140E179A8);
		void* device = render ? *(void**)(render + 0xB0) : nullptr;
		if (device && kiero::initFromD3D9Device(device) == kiero::Status::Success)
		{
			kiero::bind(42, (void**)& oEndScene, (void*)hkEndScene);
			BootTimer_Stamp("Hooked engine D3D9 device");
			attached = true;
		}
		else
		{
			Sleep(1);
		}
	} while (!attached);
	return TRUE;
}

// MinHook trampoline for aTitleLogo__update (0x140217700)
typedef void(__fastcall* TitleLogoUpdate_t)(uintptr_t param_1);
static TitleLogoUpdate_t oTitleLogoUpdate = nullptr;

void __fastcall Hook_titleLogoUpdate(uintptr_t param_1)
{
	static bool firstCall = true;
	if (firstCall) {
		BootTimer_Stamp("aTitleLogo__update first call");
		firstCall = false;
	}

	oTitleLogoUpdate(param_1);

	// State 0 queues the common boot archives needed by battle. Once it advances,
	// configure training immediately; aTitle itself is only menu setup here.
	if (GetLaunchOption() == LAUNCH_TRAINING || GetLaunchOption() == LAUNCH_DUAL_TRAINING) {
		int parentState = *(int*)(param_1 + 0x78);
		if (parentState == 1) {
			BootIntoTraining();
		}
	}
}



// Debug counters exported for the DTI editor tab
int g_dbg_peekCalls = 0;
int g_dbg_peekKeydowns = 0;
int g_dbg_translated = 0;
int g_dbg_lastMsg = 0;

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
			g_autoConnectPort = 7777;
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

	// ---- Netplay pause patches ----
	// Force uBattleUiCreatePauseMenu_maybe (0x1402ba150) to always create the
	// uBattleUiPauseNetGallery (quit-match dialog overlay) and never create the
	// real uBattleUiPause menu (which freezes game logic).
	//
	// Patch 1: At the JZ that skips NetGallery creation when NOT in ranked mode,
	// NOP it so NetGallery is always created.
	//   Original: 0F 84 90 00 00 00  (JZ +0x90)
	//   Patched:  90 90 90 90 90 90  (NOP × 6)
	Memory::VP::Nop(_addr(0x1402BA17E), 6);
	HookLog("UMVC3Hook: Patched uBattleUiCreatePauseMenu JZ→NOP (force NetGallery)\n");

	// Patch 2: Change JNZ (skip real pause if online) to JMP (always skip).
	//   Original: 75 5C  (JNZ +0x5C)
	//   Patched:  EB 5C  (JMP +0x5C)
	Memory::VP::Patch<uint8_t>(_addr(0x1402BA223), 0xEB);
	HookLog("UMVC3Hook: Patched uBattleUiCreatePauseMenu JNZ→JMP (skip real pause)\n");

	// Patch 3: uBattleUiPauseNetGallery_vfn5 (0x1402e4320) checks
	// isGameModeOnlineRanked — when false it calls the parent's vfn78 (no-op)
	// instead of uMenuSubBase_vfn5 (which actually renders the UI).
	// Change the conditional jump to unconditional so the NetGallery always renders.
	//   Original: 75 0C  (JNZ +0x0C)
	//   Patched:  EB 0C  (JMP +0x0C)
	Memory::VP::Patch<uint8_t>(_addr(0x1402E433B), 0xEB);
	HookLog("UMVC3Hook: Patched uBattleUiPauseNetGallery_vfn5 JNZ→JMP (force render)\n");

	if (opt == LAUNCH_DUAL_TRAINING) {
		// Position host window to top-right corner
		g_windowCorner = 1;

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
						// The launch menu ran before this peer existed, so send the
						// selected fighters/stage as the post-connect handshake.
						SendTrainingLaunchPacket();
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
		// Position client window to bottom-left corner
		g_windowCorner = 2;

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
				bool receivedLaunchConfig = false;
				while (waited < timeoutMs) {
					int result = enet_host_service(client, &event, 100);
					if (result > 0) {
						if (event.type == ENET_EVENT_TYPE_CONNECT) {
							peer->data = (void*)"connected";
							SetNetworkHandles(client, peer);
							HookLog("UMVC3Hook: Auto-connect SUCCESS to %s:%d\n",
								g_autoConnectIp, g_autoConnectPort);
						}
						else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
							if (event.packet->dataLength == sizeof(LaunchTrainingPacket) &&
								*(uint32_t*)event.packet->data == PACKET_LAUNCH_TRAINING) {
								receivedLaunchConfig = ApplyTrainingLaunchPacket(
									*(const LaunchTrainingPacket*)event.packet->data);
								if (receivedLaunchConfig)
									HookLog("UMVC3Hook: Auto-connect received dual-training selection\n");
							}
							enet_packet_destroy(event.packet);
						}
						else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
							HookLog("UMVC3Hook: Auto-connect disconnected before launch sync\n");
							break;
						}
					}
					if (receivedLaunchConfig) break;
					waited += 100;
				}
				if (!GetLaunchDialogConfig().enetPeer || !receivedLaunchConfig) {
					HookLog("UMVC3Hook: Auto-connect TIMEOUT waiting for launch sync - destroying client\n");
					SetNetworkHandles(nullptr, nullptr);
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
	//InjectHook(_addr(0x14001B451), tramp->Jump(&UMVC3Camera::HookedSetRotation), PATCH_CALL);
	//InjectHook(_addr(0x14001B471), tramp->Jump(&UMVC3Camera::HookedSetPosition), PATCH_CALL);
	//InjectHook(_addr(0x14001A490), tramp->Jump(UMVC3Hooks::HookCamera), PATCH_JUMP);

	// Training mode: handled via code hooks (aTitleLogo__playLogoMovies patch) and
	// the training boot module which intercepts aTitle__update to transition
	// directly into a battle area with training mode settings.

	if (opt == LAUNCH_TRAINING || opt == LAUNCH_DUAL_TRAINING) {
		// The direct aTitle transition still runs aTitle__load, which creates a
		// purely visual uLoading unit at 0x14021706F. Keep its state countdown and
		// resource readiness checks, but skip constructing the loading screen.
		Memory::VP::Nop(_addr(0x14021706F), 5);
		HookLog("UMVC3Hook: Disabled aTitle loading-screen creation for training boot\n");

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

	// Diagnostic hooks: move_base entry/exit capture, dir history ring analysis
	Diag_InitHooks();

	// Force the global XorShift RNG state to the same initial seed on both
	// host and client instances.  The game normally does this once at startup
	// (MtXorShift__seed(&state, 0x1105) @ 0x140a3d120), but explicit
	// re-seeding here eliminates any risk of divergence caused by hook-init
	// code paths (different menu selections, timing, etc.) running before the
	// game's own RNG init.  Both sides must agree on the seed so that
	// deterministic RNG consumption stays in sync during netplay.
	HookLog("UMVC3Hook: Re-seeding global XorShift state with 0x1105\n");
	typedef void(__fastcall* SeedGlobalXorShift_t)();
	((SeedGlobalXorShift_t)_addr(0x140a3d120))();

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

// ---------------------------------------------------------------------------
// CreateFileW hook to log file loads during boot
// ---------------------------------------------------------------------------
static auto g_hkCreateFileW_orig = CreateFileW;
static HANDLE WINAPI g_hkCreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
	DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
	HANDLE hTemplateFile)
{
	HANDLE h = g_hkCreateFileW_orig(lpFileName, dwDesiredAccess, dwShareMode,
									 lpSecurityAttributes, dwCreationDisposition,
									 dwFlagsAndAttributes, hTemplateFile);
	if (lpFileName) {
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		double ms = (double)(t.QuadPart - g_bootTimer.start.QuadPart)
			* 1000.0 / (double)g_bootTimer.freq.QuadPart;
		HookLog("[FILEw: %+.0fms] %S\n", ms, lpFileName);
	}
	return h;
}

static auto g_hkCreateFileA_orig = CreateFileA;
static HANDLE WINAPI g_hkCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess,
	DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
	DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
	HANDLE hTemplateFile)
{
	HANDLE h = g_hkCreateFileA_orig(lpFileName, dwDesiredAccess, dwShareMode,
									 lpSecurityAttributes, dwCreationDisposition,
									 dwFlagsAndAttributes, hTemplateFile);
	if (lpFileName) {
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		double ms = (double)(t.QuadPart - g_bootTimer.start.QuadPart)
			* 1000.0 / (double)g_bootTimer.freq.QuadPart;
		HookLog("[FILEa: %+.0fms] %s\n", ms, lpFileName);
	}
	return h;
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (CheckGame())
		{
			Exception_InstallHandler();
			BootTimer_Init();
			BootTimer_Stamp("DllMain entry");

			// Install CreateFileA/W hooks to log every file open
			/*{
				MH_STATUS mh = MH_Initialize();
				if (mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED) {
					MH_CreateHookApi(L"kernel32", "CreateFileA",
						(LPVOID)g_hkCreateFileA, (LPVOID*)&g_hkCreateFileA_orig);
					MH_CreateHookApi(L"kernel32", "CreateFileW",
						(LPVOID)g_hkCreateFileW, (LPVOID*)&g_hkCreateFileW_orig);
					MH_EnableHook(MH_ALL_HOOKS);
				}
			}*/

			SettingsMgr->Init();
			DisableThreadLibraryCalls(hMod);

			if (enet_initialize() != 0) {
				HookLog("UMVC3Hook: Failed to initialize enet!\n");
			} else {
				HookLog("UMVC3Hook: enet initialized successfully\n");
			}

			CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
			BootTimer_Stamp("Before OnInitializeHook");
			OnInitializeHook();
			BootTimer_Stamp("After OnInitializeHook");
		}
		break;
	case DLL_PROCESS_DETACH:
		Exception_RemoveHandler();
		CleanupNetworkOnExit();
		enet_deinitialize();
		kiero::shutdown();
		break;
	}
	return TRUE;
}
