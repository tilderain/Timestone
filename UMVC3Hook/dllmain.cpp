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
#include "code/escmenu_imgui.h"
#include "code/trainingboot.h"
#include "code/netplay.h"
#include "code/controller.h"
#include "code/autotest.h"
#include "code/diag.h"
#include "code/exception.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>
#include "kiero/minhook/include/MinHook.h"
#include <enet/enet.h>
//#include "code/generated.h"

// Log file for hook debugging
FILE* g_hookLog = nullptr;
static SRWLOCK g_hookLogLock = SRWLOCK_INIT;
static char g_hookLogFilename[MAX_PATH] = {};
static HMODULE g_asiModule = nullptr;

void HookLogSetFile(const char* filename)
{
	AcquireSRWLockExclusive(&g_hookLogLock);
	if (strcmp(g_hookLogFilename, filename) == 0) {
		ReleaseSRWLockExclusive(&g_hookLogLock);
		return;
	}
	fflush(stderr);
	if (g_hookLog) {
		fflush(g_hookLog);
		fclose(g_hookLog);
		g_hookLog = nullptr;
	}
	// Unlink a previous run's root filename so its nplive hard links keep the
	// old file instead of being truncated when this instance starts logging.
	DeleteFileA(filename);
	const bool copiedHistory = g_hookLogFilename[0] &&
		CopyFileA(g_hookLogFilename, filename, FALSE);
	g_hookLog = fopen(filename, copiedHistory ? "a" : "w");
	if (g_hookLog) {
		fprintf(g_hookLog, "=== UMVC3Hook Log Start (switched to %s) ===\n", filename);
		fflush(g_hookLog);
	}
	// Also redirect stderr to the new log file for Wine crash logs
	// Use append mode so it doesn't truncate what g_hookLog just wrote
	freopen(filename, "a", stderr);
	snprintf(g_hookLogFilename, sizeof(g_hookLogFilename), "%s", filename);
	ReleaseSRWLockExclusive(&g_hookLogLock);
}

bool HookLogCreateAlias(const char* filename)
{
	AcquireSRWLockExclusive(&g_hookLogLock);
	fflush(stderr);
	if (g_hookLog)
		fflush(g_hookLog);
	const bool created = g_hookLogFilename[0] &&
		CreateHardLinkA(filename, g_hookLogFilename, nullptr);
	ReleaseSRWLockExclusive(&g_hookLogLock);
	return created;
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

static void CollectInstalledAsiFiles(const char* directory, bool recursive,
	const char* selfPath, std::vector<std::string>& files)
{
	char pattern[MAX_PATH] = {};
	if (snprintf(pattern, sizeof(pattern), "%s\\*", directory) >=
		(int)sizeof(pattern))
		return;

	WIN32_FIND_DATAA entry = {};
	HANDLE search = FindFirstFileA(pattern, &entry);
	if (search == INVALID_HANDLE_VALUE)
		return;

	do {
		if (strcmp(entry.cFileName, ".") == 0 ||
			strcmp(entry.cFileName, "..") == 0)
			continue;

		char path[MAX_PATH] = {};
		if (snprintf(path, sizeof(path), "%s\\%s", directory,
				entry.cFileName) >= (int)sizeof(path))
			continue;

		if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (recursive &&
				!(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				CollectInstalledAsiFiles(path, true, selfPath, files);
			continue;
		}

		const char* extension = strrchr(entry.cFileName, '.');
		if (extension && _stricmp(extension, ".asi") == 0 &&
			_stricmp(path, selfPath) != 0)
			files.emplace_back(path);
	} while (FindNextFileA(search, &entry));
	FindClose(search);
}

static void WarnAboutOtherInstalledAsiModules()
{
	char gameDirectory[MAX_PATH] = {};
	DWORD gamePathLength = GetModuleFileNameA(
		nullptr, gameDirectory, sizeof(gameDirectory));
	if (!gamePathLength || gamePathLength >= sizeof(gameDirectory))
		return;
	char* filename = strrchr(gameDirectory, '\\');
	if (!filename)
		filename = strrchr(gameDirectory, '/');
	if (!filename)
		return;
	*filename = '\0';

	char selfPath[MAX_PATH] = {};
	if (!GetModuleFileNameA(g_asiModule, selfPath, sizeof(selfPath)))
		return;

	std::vector<std::string> files;
	CollectInstalledAsiFiles(gameDirectory, false, selfPath, files);
	char pluginDirectory[MAX_PATH] = {};
	snprintf(pluginDirectory, sizeof(pluginDirectory), "%s\\scripts", gameDirectory);
	CollectInstalledAsiFiles(pluginDirectory, true, selfPath, files);
	snprintf(pluginDirectory, sizeof(pluginDirectory), "%s\\plugins", gameDirectory);
	CollectInstalledAsiFiles(pluginDirectory, true, selfPath, files);
	if (files.empty())
		return;

	std::string message =
		"YOU HAVE OTHER MODS INSTALLED.\r\n"
		"THIS WILL CRASH AND BREAK YOUR GAME.\r\n"
		"PLEASE REMOVE THEM:";
	const size_t gameDirectoryLength = strlen(gameDirectory);
	for (const std::string& path : files) {
		const char* displayPath = path.c_str();
		if (_strnicmp(displayPath, gameDirectory, gameDirectoryLength) == 0 &&
			(displayPath[gameDirectoryLength] == '\\' ||
			 displayPath[gameDirectoryLength] == '/'))
			displayPath += gameDirectoryLength + 1;
		message += "\r\n";
		message += displayPath;
		HookLog("UMVC3Hook: Detected incompatible installed ASI: %s\n",
			displayPath);
	}
	MessageBoxA(nullptr, message.c_str(), "why",
		MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void HookLogWriteRecent(FILE* file)
{
	if (!file)
		return;

	fprintf(file, "---- recent HookLog lines (up to 25) ----\n");
	FILE* source = fopen(g_hookLogFilename, "rb");
	if (!source || fseek(source, 0, SEEK_END) != 0) {
		if (source)
			fclose(source);
		fprintf(file, "  <unavailable>\n");
		return;
	}

	const long end = ftell(source);
	if (end <= 0) {
		fclose(source);
		fprintf(file, end == 0 ? "  <none>\n" : "  <unavailable>\n");
		return;
	}
	long start = end;
	int lineCount = 0;
	while (start > 0 && lineCount < 25) {
		--start;
		if (fseek(source, start, SEEK_SET) != 0)
			break;
		const int c = fgetc(source);
		if (c == '\n' && start + 1 < end) {
			++lineCount;
			if (lineCount == 25)
				++start;
		}
	}
	if (fseek(source, start, SEEK_SET) == 0) {
		int c = 0;
		int lastWritten = '\n';
		while ((c = fgetc(source)) != EOF) {
			if (c == '\r') {
				const int next = fgetc(source);
				if (next != '\n') {
					fputc(c, file);
					lastWritten = c;
				}
				c = next;
				if (c == EOF)
					break;
			}
			fputc(c, file);
			lastWritten = c;
		}
		if (lastWritten != '\n')
			fputc('\n', file);
	}
	fclose(source);
	fprintf(file, "---- end recent HookLog lines ----\n");
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
static int  g_trainingFrameLimit = 0;

// Window corner positioning for dual training
// 0=none, 1=top-right (host), 2=bottom-left (client)
static int g_windowCorner = 0;

// D3D9 device creation is not safe to race across the host and spawned client
// under Wine/Proton.  Keep the child launch behind the host's real device and
// behind the rest of the startup patches.
static std::atomic<bool> g_launchSetupComplete { false };
static HANDLE g_launchSetupCompleteEvent = nullptr;
static std::atomic<bool> g_dualSpawnPending { false };
static std::atomic<bool> g_dualClientSpawned { false };
static std::atomic<bool> g_d3d9CreateDeviceHookReady { false };
static HANDLE g_dualHostDeviceReadyEvent = nullptr;

static bool CharacterNamesEqual(const char* lhs, const char* rhs)
{
	while (*lhs || *rhs) {
		while (*lhs == ' ' || *lhs == '-' || *lhs == '_' || *lhs == '.') ++lhs;
		while (*rhs == ' ' || *rhs == '-' || *rhs == '_' || *rhs == '.') ++rhs;
		char left = *lhs;
		char right = *rhs;
		if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
		if (right >= 'A' && right <= 'Z') right = (char)(right - 'A' + 'a');
		if (left != right) return false;
		if (left) ++lhs;
		if (right) ++rhs;
	}
	return true;
}

static int ParseAutoTestCharacter(const char* cmdLine)
{
	const char* arg = strstr(cmdLine, "-autotest=");
	if (!arg) return -1;

	const bool quotedArgument = arg > cmdLine && arg[-1] == '"';
	const char* value = arg + 10;
	bool quotedValue = false;
	if (*value == '"') {
		quotedValue = true;
		++value;
	}
	char name[64] = {};
	int length = 0;
	while (value[length] && length < (int)sizeof(name) - 1) {
		if (value[length] == '"') break;
		if (!quotedArgument && !quotedValue && value[length] == ' ') break;
		name[length] = value[length];
		++length;
	}
	name[length] = '\0';
	if (!name[0]) return 0;

	char* end = nullptr;
	long id = strtol(name, &end, 10);
	if (end && *end == '\0' && id >= 1 && id < kCharNameCount)
		return (int)id;
	for (int i = 1; i < kCharNameCount; ++i) {
		if (CharacterNamesEqual(name, kCharNames[i]))
			return i;
	}
	return 0;
}

static int ParseAutoTestPort(const char* cmdLine)
{
	const char* arg = strstr(cmdLine, "-autotest-port=");
	if (!arg) return 0;

	const char* value = arg + strlen("-autotest-port=");
	char* end = nullptr;
	long port = strtol(value, &end, 10);
	if (end == value || (*end != '\0' && *end != ' ' && *end != '"') ||
		port < 1 || port > 65535)
		return -1;
	return (int)port;
}

static int ParseTrainingFrameLimit(const char* cmdLine)
{
	const char* arg = strstr(cmdLine, "-training-frame-limit=");
	if (!arg) return 0;

	const char* value = arg + strlen("-training-frame-limit=");
	char* end = nullptr;
	long frames = strtol(value, &end, 10);
	if (end == value || (*end != '\0' && *end != ' ' && *end != '"') ||
		frames < 1 || frames > 10000000)
		return -1;
	return (int)frames;
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

EndScene oEndScene = NULL;
using ResetDevice = HRESULT(__stdcall*)(LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS*);
static ResetDevice oResetDevice = nullptr;
WNDPROC oWndProc;
static HWND window = NULL;
extern bool ChrSelImGui_IsOpen();

using AppD3DResultHandler = void(__fastcall*)(void*, int32_t, const char*, uintptr_t);
static AppD3DResultHandler g_originalAppD3DResultHandler = nullptr;

static void __fastcall HookAppD3DResultHandler(
	void* app, int32_t result, const char* operation, uintptr_t context)
{
	if (result != 0) {
		HookLog("D3D9: failure HRESULT=0x%08X operation=%s caller=%p thread=%lu context=%p\n",
			(uint32_t)result, operation ? operation : "<null>",
			__builtin_return_address(0), GetCurrentThreadId(), (void*)context);
	}
	g_originalAppD3DResultHandler(app, result, operation, context);
}

static void InstallAppD3DResultLogging()
{
	void* target = (void*)_addr(0x1405371F0);
	MH_STATUS status = MH_CreateHook(
		target, (LPVOID)&HookAppD3DResultHandler,
		(LPVOID*)&g_originalAppD3DResultHandler);
	if (status == MH_OK)
		status = MH_EnableHook(target);
	if (status == MH_OK || status == MH_ERROR_ENABLED)
		HookLog("D3D9: installed fatal HRESULT context logging\n");
	else
		HookLog("D3D9: failed to install fatal HRESULT context logging (%d)\n", status);
}

// The game uses Raw Input and may suppress legacy WM_KEYDOWN messages. Mirror
// physical keyboard state into ImGui every render frame, as a platform backend
// normally would, and let ImGui own key-repeat timing.
void PollKeyboardDirectly()
{
    if (!window || ImGui::GetCurrentContext() == nullptr)
        return;

    ImGuiIO& io = ImGui::GetIO();
    static bool prevKeys[256] = { 0 };
    const bool menuOpen = TheMenu->GetActiveState();
    const bool rosterOpen = ChrSelImGui_IsOpen();
    const bool escMenuOpen = EscMenuImGui_IsOpen();
    if (!menuOpen && !rosterOpen && !escMenuOpen) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        memset(io.KeysDown, 0, sizeof(io.KeysDown));
        memset(prevKeys, 0, sizeof(prevKeys));
        io.KeyCtrl = io.KeyShift = io.KeyAlt = false;
        return;
    }

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static const int navigationKeys[] = {
        VK_TAB, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_PRIOR, VK_NEXT,
        VK_HOME, VK_END, VK_INSERT, VK_DELETE, VK_BACK, VK_SPACE,
        VK_RETURN, VK_ESCAPE
    };
    if (!menuOpen)
        memset(io.KeysDown, 0, sizeof(io.KeysDown));
    BYTE keyboardState[256] = { 0 };
    const bool haveKeyboardState = menuOpen && GetKeyboardState(keyboardState);
    const int lastKey = menuOpen ? 256 : (int)IM_ARRAYSIZE(navigationKeys);
    for (int keyIndex = 0; keyIndex < lastKey; ++keyIndex)
    {
        const int vk = menuOpen ? keyIndex : navigationKeys[keyIndex];
        const bool isDown = haveKeyboardState
            ? (keyboardState[vk] & 0x80) != 0
            : KeyDown(vk);
        const bool wasDown = prevKeys[vk];
        prevKeys[vk] = isDown;
        io.KeysDown[vk] = isDown;

        if (menuOpen && isDown && !wasDown)
        {
            WCHAR wchar[4] = { 0 };
            if (ToUnicode(vk, MapVirtualKey(vk, MAPVK_VK_TO_VSC),
                          keyboardState, wchar, 4, 0) > 0)
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
    }
    io.KeyCtrl = io.KeysDown[VK_CONTROL];
    io.KeyShift = io.KeysDown[VK_SHIFT];
    io.KeyAlt = io.KeysDown[VK_MENU];
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

    const bool keyboardMessage = uMsg == WM_KEYDOWN || uMsg == WM_KEYUP ||
        uMsg == WM_CHAR || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP;
    const bool keyboardOverlayOpen = TheMenu->GetActiveState() ||
        ChrSelImGui_IsOpen() || EscMenuImGui_IsOpen();
    // Mouse input is always useful; keyboard input is scoped to interactive UI.
    if (ImGui::GetCurrentContext() != nullptr &&
        (!keyboardMessage || keyboardOverlayOpen))
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);

    if (TheMenu->GetActiveState() || EscMenuImGui_IsOpen())
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

    return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}
bool init = false;
bool first_msg = true;
static volatile LONG g_endSceneOverlayInFlight = 0;
static volatile LONG g_initialD3D9ResetComplete = 0;
static SRWLOCK g_d3d9DeviceLock = SRWLOCK_INIT;
static unsigned int g_overlayInitDeferredFrames = 0;

static HRESULT __stdcall hkResetDevice(
	LPDIRECT3DDEVICE9 pDevice, D3DPRESENT_PARAMETERS* presentationParameters)
{
	AcquireSRWLockExclusive(&g_d3d9DeviceLock);

	if (presentationParameters) {
		/// TRACE LOG
		HookLog("D3D9: Reset params device=%p bb=%ux%u format=%u count=%u "
			"ms=%u quality=%u swap=%u hwnd=%p windowed=%d autoDepth=%d "
			"depthFormat=%u flags=0x%08X refresh=%u interval=0x%08X\n",
			pDevice,
			presentationParameters->BackBufferWidth,
			presentationParameters->BackBufferHeight,
			(UINT)presentationParameters->BackBufferFormat,
			presentationParameters->BackBufferCount,
			(UINT)presentationParameters->MultiSampleType,
			presentationParameters->MultiSampleQuality,
			(UINT)presentationParameters->SwapEffect,
			presentationParameters->hDeviceWindow,
			presentationParameters->Windowed,
			presentationParameters->EnableAutoDepthStencil,
			(UINT)presentationParameters->AutoDepthStencilFormat,
			presentationParameters->Flags,
			presentationParameters->FullScreen_RefreshRateInHz,
			presentationParameters->PresentationInterval);
	} else {
		HookLog("D3D9: Reset params device=%p presentationParameters=<null>\n",
			pDevice);
	}

	if (init && ImGui::GetCurrentContext()) {
		FrameStepper_InvalidateDeviceObjects();
		ImGui_ImplDX9_InvalidateDeviceObjects();
	}

	HRESULT result = oResetDevice(pDevice, presentationParameters);
	HookLog("D3D9: Reset returned 0x%08X\n", (uint32_t)result);
	if (FAILED(result)) {
		/// TRACE LOG
		D3DDEVICE_CREATION_PARAMETERS creation = {};
		D3DDISPLAYMODE displayMode = {};
		const HRESULT creationResult = pDevice->GetCreationParameters(&creation);
		const HRESULT displayResult = pDevice->GetDisplayMode(0, &displayMode);
		const HRESULT cooperativeResult = pDevice->TestCooperativeLevel();
		HookLog("D3D9: Reset failure state cooperative=0x%08X "
			"creation=0x%08X adapter=%u deviceType=%u focusWindow=%p behavior=0x%08X "
			"display=0x%08X mode=%ux%u@%u format=%u swapChains=%u\n",
			(uint32_t)cooperativeResult,
			(uint32_t)creationResult,
			creation.AdapterOrdinal,
			(UINT)creation.DeviceType,
			creation.hFocusWindow,
			creation.BehaviorFlags,
			(uint32_t)displayResult,
			displayMode.Width,
			displayMode.Height,
			displayMode.RefreshRate,
			(UINT)displayMode.Format,
			pDevice->GetNumberOfSwapChains());
	}

	if (SUCCEEDED(result) && init && ImGui::GetCurrentContext()) {
		FrameStepper_SetDevice(pDevice);
		ImGui_ImplDX9_CreateDeviceObjects();
	}
	if (SUCCEEDED(result))
		InterlockedExchange(&g_initialD3D9ResetComplete, 1);

	ReleaseSRWLockExclusive(&g_d3d9DeviceLock);
	return result;
}

long __stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice)
{
	// Dear ImGui owns one process-global context and cannot tolerate concurrent
	// or reentrant EndScene callbacks mutating its current-window stack.
	if (InterlockedCompareExchange(&g_endSceneOverlayInFlight, 1, 0) != 0)
		return oEndScene(pDevice);
	AcquireSRWLockExclusive(&g_d3d9DeviceLock);

	//Exception_UpdateTestHotkey();

	if (!init)
	{
		// The engine performs an initial resize shortly after its first EndScene.
		// Keep plugin-owned default-pool resources out of that first Reset. Fall
		// back after roughly five seconds for launch paths that never reset.
		const bool resetComplete =
			InterlockedCompareExchange(&g_initialD3D9ResetComplete, 0, 0) != 0;
		if (!resetComplete && ++g_overlayInitDeferredFrames < 300) {
			const long result = oEndScene(pDevice);
			ReleaseSRWLockExclusive(&g_d3d9DeviceLock);
			InterlockedExchange(&g_endSceneOverlayInFlight, 0);
			return result;
		}
		HookLog("D3D9: initializing overlay after %s\n",
			resetComplete ? "successful initial Reset" : "Reset wait timeout");

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
		BootTimer_Stamp("Initialized ImGui overlay");
		BootTimer_Dump();
	}

	// Capture the current back buffer if a snapshot was requested (pause / advance)
	FrameStepper_Capture(pDevice);
	UpdateKeyboardState();

	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();

	// Inject inputs directly from hardware, bypassing the Windows message queue
	PollKeyboardDirectly();

	ImGui::NewFrame();
	ImGui::GetIO().MouseDrawCursor = false;
	EscMenuImGui_Update();

	// Process menu logic, notifications, frame stepper, and freecam
	// (formerly in a separate HookUpdate thread with Sleep(1))
	UMVC3Hooks::HookProcessStuff();

	if (first_msg)
	{
		Notifications->SetNotificationTime(5500);
		Notifications->PushNotification("Timestone %s is running!", UMVC3HOOK_VERSION);
		first_msg = false;
		
	}
	if (Netplay_TakePlayerDisconnectNotice())
	{
		Notifications->SetNotificationTime(5500);
		Notifications->PushNotification("The other player disconnected.");
	}
	extern void ChrSelImGui_Draw();
	ChrSelImGui_Draw();
	EscMenuImGui_Draw();

	if (TheMenu->GetActiveState())
	{
		TheMenu->Draw();
	}

	FrameStepper_Draw();
	Notifications->Draw();

	ImGui::EndFrame();

	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

	const long result = oEndScene(pDevice);
	ReleaseSRWLockExclusive(&g_d3d9DeviceLock);
	InterlockedExchange(&g_endSceneOverlayInFlight, 0);
	return result;
}

static bool SpawnDualTrainingClient()
{
	if (!g_dualSpawnPending.exchange(false, std::memory_order_acq_rel))
		return false;

	const LaunchDialogConfig& dlgCfg = GetLaunchDialogConfig();
	ENetHost* server = dlgCfg.enetHost;
	if (!server) {
		HookLog("UMVC3Hook: Dual training - host disappeared before client spawn\n");
		return false;
	}

	// Process initialization can overlap. The child's hooked CreateDevice waits
	// for the host's real device before entering the serialized driver call.
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);

	wchar_t wIp[64];
	MultiByteToWideChar(CP_ACP, 0, dlgCfg.hostIp, -1, wIp, 64);

	const wchar_t* cpuArg = TrainingBoot_GetCpuMode() == TRAINING_CPU_BOTH
		? L" -training-cpus"
		: TrainingBoot_GetCpuMode() == TRAINING_CPU_P2_ONLY
			? L" -training-p2-cpu" : L"";
	wchar_t frameLimitArg[64] = {};
	if (g_trainingFrameLimit > 0) {
		swprintf(frameLimitArg, std::size(frameLimitArg),
			L" -training-frame-limit=%d", g_trainingFrameLimit);
	}
	wchar_t autoTestArgs[768] = {};
	if (IsAutoTestLaunchEnabled()) {
		wcscpy(autoTestArgs, L" -autotest-client");
		const wchar_t* commandLine = GetCommandLineW();
		const struct { const wchar_t* source; const wchar_t* forwarded; } options[] = {
			{L"-autotest=", L"-autotest-case-character="},
			{L"-autotest-moves=", L"-autotest-case-moves="},
		};
		for (const auto& option : options) {
			const wchar_t* match = wcsstr(commandLine, option.source);
			if (!match)
				continue;
			const bool quotedArgument = match > commandLine && match[-1] == L'"';
			const wchar_t* value = match + wcslen(option.source);
			const bool quotedValue = *value == L'"';
			if (quotedValue)
				++value;
			const wchar_t* end = value;
			while (*end && *end != L'"' &&
				(quotedArgument || quotedValue || *end != L' '))
				++end;
			size_t used = wcslen(autoTestArgs);
			swprintf(autoTestArgs + used,
				std::size(autoTestArgs) - used, L" %ls\"%.*ls\"",
				option.forwarded, (int)(end - value), value);
		}
	}
	wchar_t spawnCmd[MAX_PATH + 896];
	swprintf(spawnCmd, std::size(spawnCmd),
		L"\"%ls\" -training%ls%ls%ls -connect=%ls:%d",
		exePath, cpuArg, frameLimitArg, autoTestArgs, wIp, dlgCfg.hostPort);

	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	if (CreateProcessW(exePath, spawnCmd, NULL, NULL, FALSE,
		CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		HookLog("UMVC3Hook: Spawned second instance after host D3D9 initialization "
			"(cpuMode=%d frameLimit=%d connect=%s:%d)\n",
			(int)TrainingBoot_GetCpuMode(), g_trainingFrameLimit,
			dlgCfg.hostIp, dlgCfg.hostPort);
	}
	else {
		HookLog("UMVC3Hook: Failed to spawn second instance (error %lu)\n", GetLastError());
		return false;
	}
	g_dualClientSpawned.store(true, std::memory_order_release);
	return true;
}

static void WaitForDualTrainingClient()
{
	if (!g_dualClientSpawned.load(std::memory_order_acquire))
		return;

	const LaunchDialogConfig& dlgCfg = GetLaunchDialogConfig();
	ENetHost* server = dlgCfg.enetHost;
	if (!server)
		return;

	// Keep the existing launch handshake, but run it after the graphics race
	// has been eliminated and outside DllMain.
	ENetEvent event;
	int timeoutMs = 15000;
	int waited = 0;
	while (waited < timeoutMs) {
		int result = enet_host_service(server, &event, 100);
		if (result > 0) {
			if (event.type == ENET_EVENT_TYPE_CONNECT) {
				event.peer->data = (void*)"connected";
				SetNetworkHandles(server, event.peer);
				SendTrainingLaunchPacket();
				FrameStepper_InitNetplay();
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

// Native D3D9 device creation is a process-wide driver operation under
// Wine/Proton.  Serialize only that operation across the host and child so
// dual training can start immediately without reintroducing the rare startup
// D3DERR_NOTAVAILABLE race.
using Direct3D9CreateDeviceFn = HRESULT (WINAPI*)(
	IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
	D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Direct3D9GetAdapterModeCountFn = UINT (WINAPI*)(
	IDirect3D9*, UINT, D3DFORMAT);
using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT);

static Direct3D9CreateDeviceFn g_originalD3D9CreateDevice = nullptr;
static Direct3D9GetAdapterModeCountFn g_originalD3D9GetAdapterModeCount = nullptr;
static Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;
static HANDLE g_d3d9CreateDeviceMutex = nullptr;
static bool g_serializeD3D9CreateDevice = false;

static UINT WINAPI HookD3D9GetAdapterModeCount(
	IDirect3D9* d3d9, UINT adapter, D3DFORMAT format)
{
	const UINT count = g_originalD3D9GetAdapterModeCount
		? g_originalD3D9GetAdapterModeCount(d3d9, adapter, format) : 0;
	HookLog("D3D9: GetAdapterModeCount adapter=%u format=%u reported=%u\n",
		adapter, (UINT)format, count);
	constexpr UINT kGameModeCapacity = 256;
	if (count > kGameModeCapacity) {
		static volatile LONG logged = 0;
		if (InterlockedCompareExchange(&logged, 1, 0) == 0) {
			HookLog("D3D9: clamped adapter mode count from %u to %u "
				"(prevented sRender mode-list overflow)\n",
				count, kGameModeCapacity);
		}
		return kGameModeCapacity;
	}
	return count;
}

static HANDLE GetD3D9CreateDeviceMutex()
{
	HANDLE mutex = g_d3d9CreateDeviceMutex;
	if (mutex)
		return mutex;

	HANDLE candidate = CreateMutexA(nullptr, FALSE,
		"Local\\Timestone_D3D9CreateDevice");
	if (!candidate)
		return nullptr;

	PVOID previous = InterlockedCompareExchangePointer(
		reinterpret_cast<PVOID volatile*>(&g_d3d9CreateDeviceMutex),
		candidate, nullptr);
	if (previous) {
		CloseHandle(candidate);
		return reinterpret_cast<HANDLE>(previous);
	}
	return candidate;
}

static HANDLE GetDualHostDeviceReadyEvent()
{
	HANDLE event = g_dualHostDeviceReadyEvent;
	if (event)
		return event;

	const int port = g_autoConnectPort > 0
		? g_autoConnectPort : GetLaunchDialogConfig().hostPort;
	char name[64] = {};
	snprintf(name, sizeof(name), "Local\\Timestone_D3D9HostReady_%d", port);
	HANDLE candidate = CreateEventA(nullptr, TRUE, FALSE, name);
	if (!candidate)
		return nullptr;

	PVOID previous = InterlockedCompareExchangePointer(
		reinterpret_cast<PVOID volatile*>(&g_dualHostDeviceReadyEvent),
		candidate, nullptr);
	if (previous) {
		CloseHandle(candidate);
		return reinterpret_cast<HANDLE>(previous);
	}
	return candidate;
}

static HRESULT WINAPI HookD3D9CreateDevice(
	IDirect3D9* d3d9,
	UINT adapter,
	D3DDEVTYPE deviceType,
	HWND focusWindow,
	DWORD behaviorFlags,
	D3DPRESENT_PARAMETERS* presentationParameters,
	IDirect3DDevice9** returnedDevice)
{
	if (!g_originalD3D9CreateDevice)
		return D3DERR_DRIVERINTERNALERROR;
	if (presentationParameters) {
		/// TRACE LOG
		HookLog("D3D9: CreateDevice params adapter=%u deviceType=%u focusWindow=%p "
			"behavior=0x%08X bb=%ux%u format=%u count=%u ms=%u quality=%u "
			"swap=%u hwnd=%p windowed=%d autoDepth=%d depthFormat=%u "
			"flags=0x%08X refresh=%u interval=0x%08X\n",
			adapter, (UINT)deviceType, focusWindow, behaviorFlags,
			presentationParameters->BackBufferWidth,
			presentationParameters->BackBufferHeight,
			(UINT)presentationParameters->BackBufferFormat,
			presentationParameters->BackBufferCount,
			(UINT)presentationParameters->MultiSampleType,
			presentationParameters->MultiSampleQuality,
			(UINT)presentationParameters->SwapEffect,
			presentationParameters->hDeviceWindow,
			presentationParameters->Windowed,
			presentationParameters->EnableAutoDepthStencil,
			(UINT)presentationParameters->AutoDepthStencilFormat,
			presentationParameters->Flags,
			presentationParameters->FullScreen_RefreshRateInHz,
			presentationParameters->PresentationInterval);
	} else {
		HookLog("D3D9: CreateDevice params adapter=%u presentationParameters=<null>\n",
			adapter);
	}

	if (!g_serializeD3D9CreateDevice) {
		const HRESULT result = g_originalD3D9CreateDevice(
			d3d9, adapter, deviceType, focusWindow, behaviorFlags,
			presentationParameters, returnedDevice);
		HookLog("D3D9: CreateDevice returned 0x%08X device=%p\n",
			(uint32_t)result, returnedDevice ? *returnedDevice : nullptr);
		return result;
	}

	// Let both processes initialize in parallel, but never let the spawned
	// client win the actual driver-device race against its host.
	if (g_autoConnectPort != 0) {
		HANDLE hostReady = GetDualHostDeviceReadyEvent();
		if (hostReady && WaitForSingleObject(hostReady, 15000) != WAIT_OBJECT_0)
			HookLog("D3D9: timed out waiting for dual-training host device\n");
	}

	HANDLE mutex = GetD3D9CreateDeviceMutex();
	DWORD waitResult = mutex ? WaitForSingleObject(mutex, 5000) : WAIT_FAILED;
	const bool ownsMutex = waitResult == WAIT_OBJECT_0 ||
		waitResult == WAIT_ABANDONED;

	HRESULT result = E_FAIL;
	for (int attempt = 0; attempt < 8; ++attempt) {
		result = g_originalD3D9CreateDevice(
			d3d9, adapter, deviceType, focusWindow, behaviorFlags,
			presentationParameters, returnedDevice);
		if (result != D3DERR_NOTAVAILABLE || attempt == 7)
			break;
		Sleep(25);
	}

	if (ownsMutex)
		ReleaseMutex(mutex);

	if (result == D3DERR_NOTAVAILABLE) {
		HookLog("D3D9: CreateDevice remained unavailable after retries\n");
	}
	HookLog("D3D9: CreateDevice returned 0x%08X device=%p\n",
		(uint32_t)result, returnedDevice ? *returnedDevice : nullptr);
	return result;
}

static void InstallD3D9CreateDeviceHook(IDirect3D9* d3d9)
{
	if (!d3d9)
		return;

	void** vtable = *(void***)d3d9;
	if (!vtable)
		return;

	if (!g_originalD3D9GetAdapterModeCount && vtable[6]) {
		void* modeCountTarget = vtable[6];
		MH_STATUS modeCountStatus = MH_CreateHook(
			modeCountTarget, (LPVOID)&HookD3D9GetAdapterModeCount,
			(LPVOID*)&g_originalD3D9GetAdapterModeCount);
		if (modeCountStatus == MH_OK)
			modeCountStatus = MH_EnableHook(modeCountTarget);
		if (modeCountStatus == MH_OK || modeCountStatus == MH_ERROR_ENABLED) {
			HookLog("D3D9: protected fixed-capacity adapter mode list\n");
		} else {
			HookLog("D3D9: failed to hook IDirect3D9::GetAdapterModeCount (%d)\n",
				modeCountStatus);
		}
	}

	if (g_originalD3D9CreateDevice || !vtable[16])
		return;

	void* target = vtable[16];
	MH_STATUS status = MH_CreateHook(
		target, (LPVOID)&HookD3D9CreateDevice,
		(LPVOID*)&g_originalD3D9CreateDevice);
	if (status != MH_OK) {
		HookLog("D3D9: failed to hook IDirect3D9::CreateDevice (%d)\n", status);
		return;
	}

	status = MH_EnableHook(target);
	if (status != MH_OK && status != MH_ERROR_ENABLED) {
		HookLog("D3D9: failed to enable IDirect3D9::CreateDevice (%d)\n", status);
		return;
	}

	g_d3d9CreateDeviceHookReady.store(true, std::memory_order_release);
	HookLog("D3D9: serialized IDirect3D9::CreateDevice\n");
}

static IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
{
	IDirect3D9* d3d9 = g_originalDirect3DCreate9
		? g_originalDirect3DCreate9(sdkVersion) : nullptr;
	InstallD3D9CreateDeviceHook(d3d9);
	return d3d9;
}

static void InstallD3D9Hooks()
{
	HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
	if (!d3d9Module)
		d3d9Module = LoadLibraryA("d3d9.dll");
	if (!d3d9Module) {
		HookLog("D3D9: d3d9.dll is not available for startup hook\n");
		return;
	}

	void* create9Address = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9");
	if (!create9Address) {
		HookLog("D3D9: Direct3DCreate9 export is unavailable\n");
		return;
	}

	MH_STATUS status = MH_CreateHook(
		create9Address, (LPVOID)&HookDirect3DCreate9,
		(LPVOID*)&g_originalDirect3DCreate9);
	if (status == MH_OK) {
		status = MH_EnableHook(create9Address);
	}
	if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED &&
		status != MH_ERROR_ENABLED) {
		HookLog("D3D9: failed to hook Direct3DCreate9 (%d)\n", status);
		return;
	}

	// Install the COM method hook before the game creates its first device.
	// This creates only the D3D9 interface and never a second device.
	Direct3DCreate9Fn create9 = g_originalDirect3DCreate9
		? g_originalDirect3DCreate9
		: (Direct3DCreate9Fn)create9Address;
	IDirect3D9* probe = create9(D3D_SDK_VERSION);
	InstallD3D9CreateDeviceHook(probe);
	if (probe)
		probe->Release();
}



DWORD WINAPI MainThread(LPVOID lpReserved)
{
	BootTimer_Stamp("MainThread start");
	Notifications->Init();

	bool attached = false;
	bool clientSpawned = false;
	if (g_launchSetupCompleteEvent) {
		WaitForSingleObject(g_launchSetupCompleteEvent, INFINITE);
	} else {
		while (!g_launchSetupComplete.load(std::memory_order_acquire))
			Sleep(1);
	}

	do
	{
		// Reuse the engine's real D3D9 device. Kiero's normal initialization
		// creates a second device, which serializes with native driver startup.
		uint64_t render = *(uint64_t*)_addr(0x140E179A8);
		void* device = render ? *(void**)(render + 0xB0) : nullptr;
		if (device && GetLaunchOption() == LAUNCH_DUAL_TRAINING) {
			HANDLE hostReady = GetDualHostDeviceReadyEvent();
			if (hostReady)
				SetEvent(hostReady);
		}
		if (!clientSpawned &&
			g_dualSpawnPending.load(std::memory_order_acquire) &&
			(g_d3d9CreateDeviceHookReady.load(std::memory_order_acquire) || device)) {
			clientSpawned = SpawnDualTrainingClient();
		}
		if (device) {
			// EndScene may call into TheMenu and FrameStepper, so keep the hook
			// itself behind the normal startup initialization barrier.
			if (!attached && kiero::initFromD3D9Device(device) == kiero::Status::Success)
			{
				const kiero::Status::Enum resetStatus =
					kiero::bind(16, (void**)&oResetDevice, (void*)hkResetDevice);
				if (resetStatus != kiero::Status::Success)
					HookLog("D3D9: failed to hook Reset (%d)\n", (int)resetStatus);
				const kiero::Status::Enum endSceneStatus =
					kiero::bind(42, (void**)&oEndScene, (void*)hkEndScene);
				if (endSceneStatus != kiero::Status::Success) {
					HookLog("D3D9: failed to hook EndScene (%d); dual-training child not started\n",
						(int)endSceneStatus);
					return FALSE;
				}
				BootTimer_Stamp("Hooked engine D3D9 device");
				attached = true;
			}
		}
		if (!attached)
		{
			Sleep(1);
		}
	} while (!attached);
	WaitForDualTrainingClient();
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
	WarnAboutOtherInstalledAsiModules();

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
	if (strstr(cmdLineA, "-training-cpus")) {
		TrainingBoot_SetCpuMode(TRAINING_CPU_BOTH);
		ConfigureCommandLineDualTraining();
		trainingArg = true;
		HookLog("UMVC3Hook: Dual training CPU control enabled for both teams\n");
	}
	else if (strstr(cmdLineA, "-training-p2-cpu")) {
		TrainingBoot_SetCpuMode(TRAINING_CPU_P2_ONLY);
		HookLog("UMVC3Hook: Training CPU control enabled for P2\n");
	}
	if (strstr(cmdLineA, "-five-bars")) {
		TrainingBoot_SetFiveBarStart(true);
		HookLog("UMVC3Hook: Five-bar starting meter enabled for both teams\n");
	}

	const int autoTestCharacter = ParseAutoTestCharacter(cmdLineA);
	const int autoTestPort = ParseAutoTestPort(cmdLineA);
	g_trainingFrameLimit = ParseTrainingFrameLimit(cmdLineA);
	if (g_trainingFrameLimit < 0) {
		HookLog("UMVC3Hook: Invalid -training-frame-limit; use an integer from 1 to 10000000\n");
		g_trainingFrameLimit = 0;
	}
	FrameStepper_SetTrainingFrameLimit(g_trainingFrameLimit);
	if (g_trainingFrameLimit > 0) {
		HookLog("UMVC3Hook: Training frame limit set to %d fighting frames\n",
			g_trainingFrameLimit);
	}
	AutoTest_ConfigureCommandLine(cmdLineA);
	if (autoTestPort < 0) {
		HookLog("UMVC3Hook: Invalid -autotest-port; use a port from 1 to 65535\n");
	}
	if (autoTestCharacter > 0 && autoTestPort >= 0 &&
		ConfigureAutoTestDualTraining(autoTestCharacter, autoTestPort)) {
		trainingArg = true;
		HookLog("UMVC3Hook: Detected -autotest=%s, launching dual training with character ID %d%s on port %d\n",
			kCharNames[autoTestCharacter], autoTestCharacter,
			autoTestCharacter == 17 ? " active and Ryu assists" : " in all slots",
			GetLaunchDialogConfig().hostPort);
	} else if (autoTestCharacter == 0) {
		HookLog("UMVC3Hook: Invalid -autotest character; use a character name or ID 1-%d\n",
			kCharNameCount - 1);
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
		if (g_autoConnectPort > 0)
			SetLaunchOption(LAUNCH_DUAL_TRAINING);
	}

	// Show launch menu on first instance only
	LaunchOption opt = GetLaunchOption();
	if (!trainingArg) {
		opt = ShowLaunchMenu();
	}
	if (opt == LAUNCH_CANCELLED) {
		HookLog("UMVC3Hook: Launch menu closed; exiting game\n");
		ExitProcess(0);
		return;
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
		InstallAppD3DResultLogging();
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
	// Install before sRender enumerates display modes. The mode-count hook protects
	// its fixed 256-entry array. Keep CreateDevice serialization scoped to the two
	// processes launched for local dual training.
	g_serializeD3D9CreateDevice =
		opt == LAUNCH_DUAL_TRAINING || g_autoConnectPort != 0;
	InstallD3D9Hooks();

	// ---- Netplay pause patches ----
	// Force uBattleUiCreatePauseMenu_maybe (0x1402ba150) to always create the
	// uBattleUiPauseNetGallery (quit-match dialog overlay) and never create the
	// real uBattleUiPause menu (which freezes game logic).
	//
	// Patch 1: At the JZ that skips NetGallery creation when NOT in ranked mode,
	// NOP it so NetGallery is always created.
	//   Original: 0F 84 90 00 00 00  (JZ +0x90)
	//   Patched:  90 90 90 90 90 90  (NOP × 6)
	//Memory::VP::Nop(_addr(0x1402BA17E), 6);
	//HookLog("UMVC3Hook: Patched uBattleUiCreatePauseMenu JZ→NOP (force NetGallery)\n");

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
	//Memory::VP::Patch<uint8_t>(_addr(0x1402E433B), 0xEB);
	//HookLog("UMVC3Hook: Patched uBattleUiPauseNetGallery_vfn5 JNZ→JMP (force render)\n");

	if (opt == LAUNCH_DUAL_TRAINING && g_autoConnectPort == 0) {
		// Position host window to top-right corner
		g_windowCorner = 1;

		// Switch host log to host-specific file so host + client don't clobber each other
		HookLogSetFile("Timestone_host.log");
		HookLog("UMVC3Hook: Dual training host - switched to host log file\n");

		const LaunchDialogConfig& dlgCfg = GetLaunchDialogConfig();

		// Create the ENet host before launching the child. Both processes initialize
		// in parallel; the child gates only its D3D9 CreateDevice call on the host.
		ENetAddress addr;
		addr.host = ENET_HOST_ANY;
		addr.port = (enet_uint16)dlgCfg.hostPort;

		ENetHost* server = enet_host_create(&addr, 1, 2, 0, 0);
		if (!server) {
			HookLog("UMVC3Hook: Dual training - FAILED to create enet host on port %d\n",
				dlgCfg.hostPort);
		} else {
			Netplay_ConfigureHostLimits(server);
			SetNetworkHandles(server, nullptr);
			g_dualSpawnPending.store(true, std::memory_order_release);
			HANDLE hostReady = GetDualHostDeviceReadyEvent();
			if (hostReady)
				ResetEvent(hostReady);
			if (g_d3d9CreateDeviceHookReady.load(std::memory_order_acquire))
				SpawnDualTrainingClient();
			HookLog("UMVC3Hook: Dual training - hosting on port %d, child launch protected by D3D9 device serialization\n",
				dlgCfg.hostPort);
		}
	}

	// Auto-connect: second instance spawned by dual training connects back to host
	if (g_autoConnectPort != 0 && g_autoConnectIp[0] != '\0') {
		// Position client window to bottom-left corner
		g_windowCorner = 2;

		// Switch client log to client-specific file so host + client don't clobber each other
		HookLogSetFile("Timestone_client.log");

		HookLog("UMVC3Hook: Auto-connecting to host %s:%d\n",
			g_autoConnectIp, g_autoConnectPort);

		ENetHost* client = enet_host_create(NULL, 1, 2, 0, 0);
		if (client) {
			Netplay_ConfigureHostLimits(client);
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
	HookLog("UMVC3Hook: Startup patch and hook initialization complete\n");
	g_launchSetupComplete.store(true, std::memory_order_release);
	if (g_launchSetupCompleteEvent)
		SetEvent(g_launchSetupCompleteEvent);
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
		g_asiModule = hMod;
		DisableThreadLibraryCalls(hMod);
		break;
	case DLL_PROCESS_DETACH:
		Exception_RemoveHandler();
		CleanupNetworkOnExit();
		enet_deinitialize();
		kiero::shutdown();
		if (g_launchSetupCompleteEvent) {
			CloseHandle(g_launchSetupCompleteEvent);
			g_launchSetupCompleteEvent = nullptr;
		}
		break;
	}
	return TRUE;
}

// Ultimate ASI Loader invokes this export after LoadLibrary has returned.  The
// launch menu pumps Windows messages and must never run while DllMain holds the
// loader lock; clipboard, IME, accessibility, and themed-control code may load
// DLLs and otherwise deadlock the process.
extern "C" __declspec(dllexport) void InitializeASI()
{
	if (!CheckGame())
		return;


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

	Exception_InstallHandler();
	BootTimer_Init();
	BootTimer_Stamp("InitializeASI entry");
	SettingsMgr->Init();
	g_launchSetupCompleteEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

	if (enet_initialize() != 0) {
		HookLog("UMVC3Hook: Failed to initialize enet!\n");
	} else {
		HookLog("UMVC3Hook: enet initialized successfully\n");
	}

	CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
	BootTimer_Stamp("Before OnInitializeHook");
	OnInitializeHook();
	BootTimer_Stamp("After OnInitializeHook");
}
