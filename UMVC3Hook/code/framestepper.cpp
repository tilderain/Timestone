// framestepper.cpp
// Frame advance mod for UMvC3 training mode.
// Ported from umvc3_framestep by Gneiss and EternalYoshi.
#include "../pch.h"
#include "framestepper.h"
#include "mk10utils.h"       // _addr
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include <Windows.h>
#include <d3d9.h>
#include "../imgui/imgui.h"
#include "../kiero/minhook/include/MinHook.h"
#include "netplay.h"
#include "LaunchMenu.h"

using namespace Memory::VP;

// HookLog from dllmain.cpp
extern void HookLog(const char* fmt, ...);

// Netplay state queries from netplay.cpp
extern bool Netplay_HasRemoteInput();

// ---------------------------------------------------------------------------
// Addresses (VAs, converted at runtime via _addr)
// ---------------------------------------------------------------------------

// vtable entry for sMvc3Main::move
static constexpr uintptr_t MOVE_VTABLE_ENTRY = 0x140B091F0;

// CALL at umvc3.exe+31CD59 calls sub_1402CE750 (SetText)
static constexpr uintptr_t HOOK_CALL_SITE = 0x14031CD59;

// Pointers
static constexpr uintptr_t MVC3_MAIN_PTR    = 0x140E177E8;
static constexpr uintptr_t MVC3_MANAGER_PTR = 0x140D510A0;
static constexpr uintptr_t CHARACTER_PTR    = 0x140D44A70;
static constexpr uintptr_t GAME_CONFIG_PTR  = 0x140D50F88;

// Functions
static constexpr uintptr_t SHOW_LOADING_ADDR = 0x140253380;
static constexpr uintptr_t SET_TEXT_FUNC     = 0x1402CE750;

// ---------------------------------------------------------------------------
// Offsets
// ---------------------------------------------------------------------------
static constexpr uintptr_t BattleSetting_offset = 0x40;
static constexpr uintptr_t GameMode_offset      = 0x34C;
static constexpr uintptr_t P1ChrNode_offset     = 0x58;

// ---------------------------------------------------------------------------
// Message IDs (from ColorExpand msg_sys_menu_en.msd)
// ---------------------------------------------------------------------------
static constexpr int32_t MSG_NOW_LOADING = 0x762; // "NOW LOADING"
static constexpr int32_t MSG_PAUSED      = 0xC59; // "PAUSED"

// ---------------------------------------------------------------------------
// Configurable keybinds
// ---------------------------------------------------------------------------
static int KEY_PAUSE   = VK_F2;
static int KEY_ADVANCE = VK_F3;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool isPaused     = false;
static bool advanceFrame = false;

// Frame counter: incremented each time a game frame actually advances while paused
static int frameCounter  = 0;

// aNextBattle tracker for one-time flag init
static uint64_t s_lastBattleObj = 0;

// Network input capture (captured from HookedMove after oMove runs)
static uint32_t g_netP1Input   = 0; // battleState+0xd58 — P1's processed input (what would be sent)
static uint32_t g_bsP2Input    = 0; // battleState+0xd60 — P2 input candidate
static uint32_t g_netP2Read    = 0; // GetsCurrentInput_withSOCD(pad,1)
static uint32_t g_padP2Input   = 0; // pad + 0x49c — raw pad slot 1 input
static bool     g_netDataValid = false;


// Slot stride in the pad structure
static constexpr uint32_t PAD_SLOT_STRIDE = 0x2E0;
static constexpr uint32_t PAD_SLOT_INPUT  = 0x1BC; // offset within each slot for the active input
// P2's input would be at pad + PAD_SLOT_STRIDE*1 + PAD_SLOT_INPUT = pad + 0x49c

// Input buffer data — captured from cChrInput__getInputBuffer (0x1401cc5d0)
// and the buffer reader FUN_1402894d0 (0x1402894d0)
static uint64_t   g_inputBufBase = 0;     // input buffer base pointer (cChrInput object)
static uint64_t   g_inputBufPtr  = 0;     // actual buffer pointer (from +0x2030)
static uint32_t   g_inputBuf[10] = {0};   // first 10 uint32 from buffer
static int        g_inputBufFrames = 0;   // how many frames of data seen
static bool       g_inputBufValid = false;

// D3D9 device pointer, set from FrameStepper_SetDevice
static LPDIRECT3DDEVICE9 g_pDevice = nullptr;

// Game window handle for forcing repaints
static HWND g_hWnd = nullptr;

// Pause frame snapshot: captures the back buffer once when pause starts
// and whenever a frame is advanced, so the frozen game image stays
// consistent across D3D9 buffer swaps.
static IDirect3DTexture9* g_pPauseTexture    = nullptr;
static bool               g_captureRequested = false;

// ---------------------------------------------------------------------------
// Input bit definitions (derived from XINPUT_GAMEPAD processing at 0x1405111b0)
// ---------------------------------------------------------------------------
static constexpr uint32_t IN_UP       = 0x0010;
static constexpr uint32_t IN_RIGHT    = 0x0020;
static constexpr uint32_t IN_DOWN     = 0x0040;
static constexpr uint32_t IN_LEFT     = 0x0080;
static constexpr uint32_t IN_LB       = 0x0100; // Left shoulder
static constexpr uint32_t IN_RB       = 0x0200; // Right shoulder
static constexpr uint32_t IN_LT       = 0x0400; // Left trigger
static constexpr uint32_t IN_RT       = 0x0800; // Right trigger
static constexpr uint32_t IN_Y        = 0x1000;
static constexpr uint32_t IN_B        = 0x2000;
static constexpr uint32_t IN_A        = 0x4000;
static constexpr uint32_t IN_X        = 0x8000;
static constexpr uint32_t IN_BACK     = 0x0001;
static constexpr uint32_t IN_L3       = 0x0002;
static constexpr uint32_t IN_R3       = 0x0004;
static constexpr uint32_t IN_START    = 0x0008;
static constexpr uint32_t IN_STICK_UP    = 0x10000;
static constexpr uint32_t IN_STICK_RIGHT = 0x20000;
static constexpr uint32_t IN_STICK_DOWN  = 0x40000;
static constexpr uint32_t IN_STICK_LEFT  = 0x80000;

static const char* DecodeDirection(uint32_t input)
{
    // Lower byte gives DPAD-based direction (post-SOCD these should be clean)
    int ud = input & (IN_UP | IN_DOWN);
    int lr = input & (IN_LEFT | IN_RIGHT);

    if (lr == IN_LEFT && ud == IN_UP)     return "7";
    if (lr == IN_LEFT && ud == IN_DOWN)   return "1";
    if (lr == IN_RIGHT && ud == IN_UP)    return "9";
    if (lr == IN_RIGHT && ud == IN_DOWN)  return "3";
    if (lr == IN_LEFT)                    return "4";
    if (lr == IN_RIGHT)                   return "6";
    if (ud == IN_UP)                      return "8";
    if (ud == IN_DOWN)                    return "2";
    return "5";
}

static void AppendButtons(char* buf, size_t len, uint32_t input)
{
    char tmp[128] = "";
    auto append = [&](const char* name, uint32_t bit) {
        if (input & bit) {
            if (tmp[0]) strcat(tmp, " ");
            strcat(tmp, name);
        }
    };
    append("A", IN_A);
    append("B", IN_B);
    append("X", IN_X);
    append("Y", IN_Y);
    append("LB", IN_LB);
    append("RB", IN_RB);
    append("LT", IN_LT);
    append("RT", IN_RT);
    append("BACK", IN_BACK);
    append("START", IN_START);
    append("L3", IN_L3);
    append("R3", IN_R3);
    strncpy(buf, tmp, len - 1);
    buf[len - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Input display function pointers (used during pause to show current inputs)
// ---------------------------------------------------------------------------
typedef uint64_t(__fastcall* sPad__getInstance_t)();
static constexpr uintptr_t SPAD_GET_INSTANCE_ADDR = 0x140001B00;

typedef uint64_t(__fastcall* GetsCurrentInput_withSOCD_t)(uint64_t padInstance, uint32_t playerIndex);
static constexpr uintptr_t GET_CURRENT_INPUT_ADDR = 0x1402B3700;

// Unit flags offset: contains game action bits from the keybind/action system
// (cUnit__getUnitFlags reads *(uint32_t*)(pad + 0xbd8))
static constexpr uint32_t UNIT_FLAGS_OFFSET = 0xBD8;

// Common UMVC3 keyboard VK codes not in GetsCurrentInput_withSOCD's hardcoded list
// (default UMVC3 layout: arrows=move, A=LP, S=MP, D=HP, Z=LK, X=MK, C=HK, F=Special, Q=A1, W=A2)
static constexpr int EXTRA_VK[] = {
    0x5A, // VK_Z
    0x53, // VK_S
    0x44, // VK_D
    0x43, // VK_C
    0x46, // VK_F
    0x51, // VK_Q
    0x57, // VK_W
    0x45, // VK_E
    0x52, // VK_R
};
static constexpr const char* EXTRA_VK_NAMES[] = {
    "Z", "S", "D", "C", "F", "Q", "W", "E", "R",
};
static constexpr int EXTRA_VK_COUNT = sizeof(EXTRA_VK) / sizeof(EXTRA_VK[0]);

// ---------------------------------------------------------------------------
// Original function pointer for the vtable-hooked move
// ---------------------------------------------------------------------------
typedef void(__fastcall* Move_t)(void*);
static Move_t oMove = nullptr;

// Forward declarations
static void CaptureNetInput();
static bool IsTrainingMode();
static constexpr uintptr_t SGAMEUI_GET_INSTANCE              = 0x140001ac0;
// ---------------------------------------------------------------------------
// Hooked sMvc3Main::move — skips the call when paused unless advancing.
// ---------------------------------------------------------------------------
static void __fastcall HookedMove(void* sMvc3Main)
{
    // --- One-time aNextBattle flag init for training mode ---
    // Ensure the +0xC4 flags have bit 15 (0x8000) set and bit 25 (0x2000000)
    // cleared so executeMatchPhase's case 3/4 init block runs. Without this,
    // the init block (which calls transitionToStep(-1) to advance the phase
    // from 1 to 2) is skipped, and the engine gets stuck in phase 1 forever
    // — no pause menu, no intro skip.
    /*{
        auto getNextBattle = (uint64_t(__fastcall*)())_addr(0x140001A90);
        uint64_t aNextBattle = getNextBattle();
        if (aNextBattle) {
            if (aNextBattle != s_lastBattleObj) {
                s_lastBattleObj = aNextBattle;
                uint32_t* flags = (uint32_t*)(aNextBattle + 0xC4);
                *flags |= 0x8000;
                *flags &= ~0x2000000;
                HookLog("UMVC3Hook: Initialized aNextBattle flags for training mode\n");
            }
        } else {
            s_lastBattleObj = 0;
        }
    }*/

    if (isPaused)
    {
        if (advanceFrame)
        {
            advanceFrame = false;
            frameCounter++;
            oMove(sMvc3Main);
        }
        else if (g_pDevice)
        {
            if (SUCCEEDED(g_pDevice->BeginScene()))
                g_pDevice->EndScene();
            g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
        }
        return;
    }

    /*uint64_t sm = *(uint64_t*)_addr(MVC3_MAIN_PTR);
    if (sm) {
    auto getAmaterasuObj = (uint64_t(__fastcall*)())_addr(0x140001A90);
    uint64_t aNextBattle = getAmaterasuObj();
    
        if (aNextBattle) {
            int step = *(int*)(aNextBattle + 0xD8);

            // Once we are in gameplay (Step 5)
            if (step == 5) {
                auto getGameUI = (uint64_t(__fastcall*)())_addr(SGAMEUI_GET_INSTANCE);
                uint64_t ui = getGameUI();
                if (ui) {
                    uint32_t* bootState = (uint32_t*)(ui + 0x5b74);
                    // If it is still stuck at 0 (Boot/Load state)
                    if (*bootState == 0) {
                    //    *bootState = 2; // Force UI state to 2 (Active Gameplay)
                     //   HookLog("UMVC3Hook: Forced sGameUI+0x5b74 bootState to 2\n");
                    }
                }
            }
        }
    }*/
    oMove(sMvc3Main);

    CaptureNetInput();

    // ===== Netplay: send local P1 input, receive remote P2 input =====
    // Only send input when the match is actually live (step >= 2).
    // Step 0 = initial/loading, Step 1 = pre-fight setup,
    // Step 2+ = input is ready, gameplay has begun.
    if (Netplay_IsActive())
    {
        // Check aNextBattle step to know if gameplay has started
        typedef uint64_t(__fastcall* GetAmaterasu_t)();
        GetAmaterasu_t getAmaterasuObj = (GetAmaterasu_t)_addr(0x140001A90);
        uint64_t aNextBattle = getAmaterasuObj();
        int step = aNextBattle ? *(int*)(aNextBattle + 0xD8) : 0;

        static bool s_inputStarted = false;
        if (step >= 2 && !s_inputStarted) {
            s_inputStarted = true;
            HookLog("Netplay: First frame input ready (step=%d, aNextBattle=0x%llX) — blasting packets\n",
                    step, aNextBattle);
        }

        if (step >= 2)
        {
            uint64_t sm = *(uint64_t*)_addr(MVC3_MAIN_PTR);
            if (sm) {
                uint64_t bs = *(uint64_t*)(sm + 0x401c0);
                if (bs) {
                    // Read local P1's processed input from battle state
                    uint32_t localP1Input = *(uint32_t*)(bs + 0xd58);

                    // Queue it for the netplay service thread to send
                    Netplay_SendInput(localP1Input);

                    // Read whatever remote input the service thread has received
                    if (Netplay_HasRemoteInput()) {
                        uint32_t remoteInput = Netplay_GetRemoteInput();
                        // Overwrite the local P2 input slot with the remote player's input (including 0)
                        *(uint32_t*)(bs + 0xd60) = remoteInput;
                    }

                    // Retrieve the actual active P2 input value for this frame
                    uint32_t activeP2Input = *(uint32_t*)(bs + 0xd60);

                    // Track active netplay frames and print logs every 60 frames
                    static uint32_t s_netplayFrameCounter = 0;
                    s_netplayFrameCounter++;
                    if (s_netplayFrameCounter % 60 == 0) {
                        HookLog("[Netplay] Frame %u | Sent P1: 0x%08X | Recv P2: 0x%08X | RTT: %u ms\n",
                                s_netplayFrameCounter, localP1Input, activeP2Input, Netplay_GetRTT());
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Capture the data that would be sent/received over network — called after oMove
// ---------------------------------------------------------------------------
static void CaptureNetInput()
{
    // 1) P1's processed input from battle state (what would be SENT)
    g_netP1Input = 0;
    g_bsP2Input  = 0;
    uint64_t sm = *(uint64_t*)_addr(MVC3_MAIN_PTR);
    if (sm) {
        uint64_t bs = *(uint64_t*)(sm + 0x401c0);
        if (bs) {
            g_netP1Input = *(uint32_t*)(bs + 0xd58);
            g_bsP2Input  = *(uint32_t*)(bs + 0xd60);
        }
    }

    // 2) P2 input from GetsCurrentInput_withSOCD and raw pad
    g_netP2Read  = 0;
    g_padP2Input = 0;
    auto sPadGetInstance = (sPad__getInstance_t)_addr(SPAD_GET_INSTANCE_ADDR);
    auto getInput = (GetsCurrentInput_withSOCD_t)_addr(GET_CURRENT_INPUT_ADDR);
    uint64_t pad = sPadGetInstance();
    if (pad) {
        g_netP2Read  = (uint32_t)getInput(pad, 1);
        g_padP2Input = *(uint32_t*)(pad + PAD_SLOT_STRIDE * 1 + PAD_SLOT_INPUT);
    }

    g_netDataValid = true;
}

// ---------------------------------------------------------------------------
// Runtime check: is the game currently in a training-mode match?
// ---------------------------------------------------------------------------
static bool IsTrainingMode()
{
    uint64_t sMvc3Manager = *(uint64_t*)_addr(MVC3_MANAGER_PTR);
    if (!sMvc3Manager) return false;

    uint64_t sBattleSetting = *(uint64_t*)(sMvc3Manager + BattleSetting_offset);
    if (!sBattleSetting) return false;

    int GameMode = *(int*)(sBattleSetting + GameMode_offset);
    if (GameMode != 5) return false; // 5 = training

    uint64_t sCharacter = *(uint64_t*)_addr(CHARACTER_PTR);
    if (!sCharacter) return false;

    uint64_t P1ChrNode = *(uint64_t*)(sCharacter + P1ChrNode_offset);
    return P1ChrNode != 0;
}

// ---------------------------------------------------------------------------
// Load keybind config from UMVC3Hook.ini  [FrameStepper] section
// ---------------------------------------------------------------------------
static void LoadConfig()
{
    wchar_t iniPath[MAX_PATH];
    GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
    wcsrchr(iniPath, L'\\')[1] = L'\0';
    wcscat(iniPath, L"UMVC3Hook.ini");

    KEY_PAUSE   = GetPrivateProfileIntW(L"FrameStepper", L"PauseKey",   VK_F2, iniPath);
    KEY_ADVANCE = GetPrivateProfileIntW(L"FrameStepper", L"AdvanceKey", VK_F3, iniPath);
}

// ---------------------------------------------------------------------------
// Hooked SetText — show "PAUSED" instead of "NOW LOADING" when stepper is on
// ---------------------------------------------------------------------------
static int64_t __fastcall HookedSetText(uint64_t fontWidget, int32_t msgID)
{
    if (msgID == MSG_NOW_LOADING && isPaused)
        msgID = MSG_PAUSED;

    typedef int64_t(__fastcall* SetText_t)(uint64_t, int32_t);
    return ((SetText_t)_addr(SET_TEXT_FUNC))(fontWidget, msgID);
}

// ---------------------------------------------------------------------------
// Show the loading/pause popup
// ---------------------------------------------------------------------------
static void ShowPopup()
{
    uint64_t sGameConfig = *(uint64_t*)_addr(GAME_CONFIG_PTR);
    if (!sGameConfig) return;

    typedef void(__fastcall* ShowLoading_t)(uint64_t);
    ((ShowLoading_t)_addr(SHOW_LOADING_ADDR))(sGameConfig);
}

// ---------------------------------------------------------------------------
// Store the D3D9 device so we can force a Present when entering pause
// ---------------------------------------------------------------------------
void FrameStepper_SetDevice(LPDIRECT3DDEVICE9 device)
{
    g_pDevice = device;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (device && SUCCEEDED(device->GetCreationParameters(&cp)))
        g_hWnd = cp.hFocusWindow;
}

// ---------------------------------------------------------------------------
// Capture the current back buffer into a GPU texture so it can be drawn as a
// static background during pause (avoids flicker from D3D9 buffer swaps).
// ---------------------------------------------------------------------------
static void CaptureBackBuffer(LPDIRECT3DDEVICE9 pDevice)
{
    if (!pDevice) return;

    IDirect3DSurface9* pBackBuffer = nullptr;
    if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)))
        return;

    D3DSURFACE_DESC desc;
    pBackBuffer->GetDesc(&desc);

    // Free old texture
    if (g_pPauseTexture) {
        g_pPauseTexture->Release();
        g_pPauseTexture = nullptr;
    }

    // Create a render-target texture matching the back buffer
    if (SUCCEEDED(pDevice->CreateTexture(desc.Width, desc.Height, 1,
        D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &g_pPauseTexture, nullptr)))
    {
        IDirect3DSurface9* pTexSurface = nullptr;
        if (SUCCEEDED(g_pPauseTexture->GetSurfaceLevel(0, &pTexSurface))) {
            pDevice->StretchRect(pBackBuffer, nullptr, pTexSurface, nullptr, D3DTEXF_NONE);
            pTexSurface->Release();
        }
    }
    pBackBuffer->Release();
}

// ---------------------------------------------------------------------------
// Called from hkEndScene after the game has finished rendering the current
// frame — resolves any pending capture requests.
// ---------------------------------------------------------------------------
void FrameStepper_Capture(LPDIRECT3DDEVICE9 pDevice)
{
    if (g_captureRequested) {
        CaptureBackBuffer(pDevice);
        g_captureRequested = false;
    }
}

// Input buffer hook — hooks cChrInput__getInputBuffer (0x1401cc5d0)
// Returns the input buffer pointer from *(qword*)(RCX + 0x2030)
// ---------------------------------------------------------------------------
typedef uint64_t(__fastcall* GetInputBuf_t)(uint64_t obj);
static GetInputBuf_t oGetInputBuf = nullptr;

static uint64_t __fastcall HookedGetInputBuf(uint64_t obj)
{
    uint64_t bufPtr = oGetInputBuf(obj);

    if (!g_inputBufValid) {
        g_inputBufBase = obj;
        g_inputBufPtr  = bufPtr;
        if (bufPtr) {
            for (int i = 0; i < 10; i++)
                g_inputBuf[i] = *(uint32_t*)(bufPtr + i * 4);
            g_inputBufFrames = 10;
        }
        g_inputBufValid = true;
    }

    return bufPtr;
}

// ---------------------------------------------------------------------------
// Public: initialise hooks — call once from OnInitializeHook
// ---------------------------------------------------------------------------
void FrameStepper_Init()
{
    LoadConfig();

    // Hook sMvc3Main::move via vtable patch
    oMove = (Move_t)(*(uintptr_t*)_addr(MOVE_VTABLE_ENTRY));
    Patch((void*)_addr(MOVE_VTABLE_ENTRY), (uintptr_t)&HookedMove);

    // Hook SetText call site (CALL instruction override)
    Trampoline* tramp = Trampoline::MakeTrampoline(GetModuleHandleW(nullptr));
    InjectHook(_addr(HOOK_CALL_SITE), tramp->Jump(&HookedSetText), PATCH_CALL);

    // Hook cChrInput__getInputBuffer (0x1401cc5d0) to capture the input buffer pointer
    static bool inputBufHooked = false;
    if (!inputBufHooked) {
        MH_CreateHook((LPVOID)_addr(0x1401cc5d0), (LPVOID)&HookedGetInputBuf, (LPVOID*)&oGetInputBuf);
        MH_EnableHook((LPVOID)_addr(0x1401cc5d0));
        inputBufHooked = true;
    }

    // Initialize netplay if the launch menu set up an enet host/peer
    const LaunchDialogConfig& cfg = GetLaunchDialogConfig();
    if (cfg.hostEnabled && cfg.enetHost && cfg.enetPeer) {
        Netplay_Init(cfg.enetHost, cfg.enetPeer);
        HookLog("FrameStepper: Netplay initialized from launch config\n");
    }
}

// ---------------------------------------------------------------------------
// Public: per-frame processing — call from HookProcessStuff
// ---------------------------------------------------------------------------
void FrameStepper_Process()
{
    if (!IsTrainingMode())
    {
        if (isPaused) {
            isPaused = false;
            if (g_pPauseTexture) {
                g_pPauseTexture->Release();
                g_pPauseTexture = nullptr;
            }
        }
        return;
    }

    // Toggle pause
    if (GetAsyncKeyState(KEY_PAUSE) & 1)
    {
        isPaused = !isPaused;
        if (isPaused)
        {
            frameCounter = 0;
            ShowPopup();
            g_captureRequested = true; // snapshot the current frame
        }
        else
        {
            // Free the snapshot on unpause
            if (g_pPauseTexture) {
                g_pPauseTexture->Release();
                g_pPauseTexture = nullptr;
            }
        }
    }

    // Advance one frame while paused
    if (isPaused && (GetAsyncKeyState(KEY_ADVANCE) & 1))
    {
        advanceFrame = true;
        g_captureRequested = true; // snapshot the advanced frame
    }
}

// ---------------------------------------------------------------------------
// Public: draw pause indicator overlay — call from hkEndScene (after ImGui::NewFrame)
// ---------------------------------------------------------------------------
void FrameStepper_Draw()
{
    if (!IsTrainingMode())
        return;

    // --- Render the captured static frame as background when paused ---
    // This eliminates the D3D9 double/triple-buffer flicker by ensuring
    // every Present shows the same frozen game image behind the overlays.
    if (isPaused && g_pPauseTexture)
    {
        ImDrawList* bgDrawList = ImGui::GetBackgroundDrawList();
        if (bgDrawList)
        {
            bgDrawList->AddImage(
                (ImTextureID)g_pPauseTexture,
                ImVec2(0.0f, 0.0f),
                ImGui::GetIO().DisplaySize
            );
        }
    }

    // --- Always-visible P1/P2 pad input window (top-left) ---
    auto sPadGetInstance = (sPad__getInstance_t)_addr(SPAD_GET_INSTANCE_ADDR);
    auto getInput = (GetsCurrentInput_withSOCD_t)_addr(GET_CURRENT_INPUT_ADDR);

    uint64_t pad = sPadGetInstance();
    if (pad)
    {
        uint32_t p1 = (uint32_t)getInput(pad, 0);
        uint32_t p2 = (uint32_t)getInput(pad, 1);

        // OR in keybind-system flags so Z, S, etc. show up
        p1 |= *(uint32_t*)(pad + UNIT_FLAGS_OFFSET);
        p2 |= *(uint32_t*)(pad + UNIT_FLAGS_OFFSET);

        char btns1[128], btns2[128];
        AppendButtons(btns1, sizeof(btns1), p1);
        AppendButtons(btns2, sizeof(btns2), p2);

        ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
        ImGui::Begin("##FrameStepperPad", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_AlwaysAutoResize);

        // Netplay status (always visible when netplay is active, shown at the top)
        if (Netplay_IsActive()) {
            const char* role = Netplay_IsHost() ? "HOST" : "CLIENT";
            ImVec4 color = Netplay_IsHost()
                ? ImVec4(1.0f, 0.9f, 0.1f, 1.0f)   // yellow for host
                : ImVec4(0.3f, 0.7f, 1.0f, 1.0f);   // blue for client
            ImGui::TextColored(color, "NETPLAY: %s  Ping: %ums", role, Netplay_GetRTT());
        }

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "P1: [%s]  0x%08X", DecodeDirection(p1), p1);
        if (btns1[0])
            ImGui::SameLine(), ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f), "%s", btns1);

        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "P2: [%s]  0x%08X", DecodeDirection(p2), p2);
        if (btns2[0])
            ImGui::SameLine(), ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.8f, 1.0f), "%s", btns2);

        // Show extra keyboard keys (Z, S, etc.) not in GetsCurrentInput_withSOCD
        char kbd[256] = "";
        for (int i = 0; i < EXTRA_VK_COUNT; i++) {
            if (GetAsyncKeyState(EXTRA_VK[i]) & 0x8000) {
                if (kbd[0]) strcat(kbd, " ");
                strcat(kbd, EXTRA_VK_NAMES[i]);
            }
        }
        if (kbd[0])
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Kbd: %s", kbd);

        ImGui::PopStyleColor();
        ImGui::End();
    }

    // --- Pause-only content: shown below the pad window when paused ---
    if (!isPaused)
        return;

    ImGui::SetNextWindowPos(ImVec2(12.0f, 80.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
    ImGui::Begin("##FrameStepperPause", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize);

    // Draw pause bars in the dummy space, then text after
    const float sz = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(pos.x + 2.0f, pos.y + 3.0f), ImVec2(pos.x + sz * 0.32f, pos.y + sz - 3.0f),
        IM_COL32(220, 50, 50, 220), 2.0f);
    dl->AddRectFilled(ImVec2(pos.x + sz * 0.38f + 2.0f, pos.y + 3.0f), ImVec2(pos.x + sz * 0.68f + 2.0f, pos.y + sz - 3.0f),
        IM_COL32(220, 50, 50, 220), 2.0f);

    ImGui::Dummy(ImVec2(sz + 6.0f, sz));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "PAUSED  %d", frameCounter);

    // Input buffer display (captured from cChrInput__getInputBuffer hook)
    if (g_inputBufValid) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "InputBuf @ 0x%llX (obj=0x%llX)", g_inputBufPtr, g_inputBufBase);
        ImGui::Text("  idx      value");
        for (int i = 0; i < 10 && i < g_inputBufFrames; i++) {
            ImGui::Text("  [%d]  0x%08X", i, g_inputBuf[i]);
        }
    }

    // Network-relevant input display (captured from HookedMove after oMove)
    if (g_netDataValid) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Net send(bs+0xd58): 0x%08X", g_netP1Input);
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.8f, 1.0f), "P2 getInput(pad,1): 0x%08X", g_netP2Read);
    }

    // 1. Get the aNextBattle instance (Amaterasu is the internal name for the battle manager)
    typedef uint64_t(__fastcall* GetAmaterasu_t)();
    GetAmaterasu_t getAmaterasuObj = (GetAmaterasu_t)_addr(0x140001A90);
    uint64_t aNextBattle = getAmaterasuObj();

    if (aNextBattle) {
        uint32_t flags      = *(uint32_t*)(aNextBattle + 0xC4);
        int      step       = *(int*)(aNextBattle + 0xD8);
        int      phase      = *(int*)(aNextBattle + 0xDC);
        int      transition = *(int*)(aNextBattle + 0x174);

        ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "--- DIAGNOSTICS ---");
        ImGui::Text("aNextBattle: 0x%llX", aNextBattle);
        ImGui::Text("Flags (+0xC4): 0x%08X", flags);
        ImGui::Text("Step (+0xD8): %d", step);
        ImGui::Text("Phase (+0xDC): %d", phase);
        ImGui::Text("Trans (+0x174): 0x%08X", transition);
    }

    // 2. Get the sGameUI instance
    typedef uint64_t(__fastcall* GetGameUI_t)();
    GetGameUI_t getGameUI = (GetGameUI_t)_addr(0x140001AC0);
    uint64_t ui = getGameUI();

    if (ui) {
        uint64_t uiElement = *(uint64_t*)(ui + 0x5AA8);
        ImGui::Text("sGameUI: 0x%llX", ui);
        ImGui::Text("UI Element (+0x5AA8): 0x%llX", uiElement);
    }

    ImGui::PopStyleColor();
    ImGui::End();
}
