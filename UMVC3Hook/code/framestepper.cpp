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
#include "rollback.h"

// Netplay debug info from controller.cpp
extern "C" int32_t Controller_GetDelayFrames();
extern "C" int32_t Controller_GetNetFrameCount();
extern "C" int32_t Controller_GetLastRemoteFrame();
extern "C" int32_t Controller_GetRollbackFrames();
extern "C" uint32_t Controller_GetRecordedLocalInput(int32_t frame);
extern "C" uint32_t Controller_GetUsedRemoteInput(int32_t frame);

// Desync detection flags from controller.cpp
extern bool     g_desyncDetected;
extern bool     g_desyncSeen;
extern int32_t  g_desyncFrame;
extern uint32_t g_desyncLocalCS;
extern uint32_t g_desyncRemoteCS;

using namespace Memory::VP;

// HookLog from dllmain.cpp
extern void HookLog(const char* fmt, ...);

// Netplay state queries from netplay.cpp
extern bool Netplay_HasRemoteInput();
extern bool Netplay_GetInboundDelayReq(DelayChangeRequest& outReq);
extern bool Netplay_GetInboundDelayAck(DelayChangeRequest& outAck);
extern void Netplay_SendDelayReq(int32_t delay, int32_t rollbackFrames);
extern void Netplay_SendDelayAck(int32_t delay, int32_t rollbackFrames, int32_t activationFrame);
extern void Netplay_SendRollback(int32_t rollbackFrames);
extern bool Netplay_GetInboundRollback(int32_t& outRollback);

// Controller setters from controller.cpp
extern "C" void Controller_SetDelayFrames(int32_t delay);
extern "C" void Controller_SetRollbackFrames(int32_t n);
extern "C" void Controller_ScheduleDelayChange(int32_t delay, int32_t activationFrame);
extern "C" bool Controller_IsDelayChangePending();
extern "C" bool Controller_IsNegotiationPending();
extern "C" void Controller_StartDelayNegotiation(int32_t delay);
extern "C" void Controller_CompleteDelayNegotiation(int32_t delay, int32_t activationFrame);
extern "C" int32_t Controller_GetStallFrames();

// Declarations from controller.cpp
extern int32_t g_stallFrames;

// Handshake margin constant (controller.cpp)
static constexpr int32_t DELAY_HANDSHAKE_MARGIN = 30;

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
// Battle type values at sBattleSetting+0x34C (confirmed via Ghidra):
//   1 = Arcade
//   2 = Attract mode
//   3 = Online (player match)
//   4 = Online Ranked
//   5 = Training
//   7 = Unknown mode 7
//   8 = Heroes & Heralds
//   9 = Online (alt)
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
// Input bit definitions (GameInput format returned by GetsCurrentInput_withSOCD)
// ---------------------------------------------------------------------------
static constexpr uint32_t IN_RIGHT    = 0x0001;
static constexpr uint32_t IN_LEFT     = 0x0002;
static constexpr uint32_t IN_UP       = 0x0004;
static constexpr uint32_t IN_DOWN     = 0x0008;
static constexpr uint32_t IN_L        = 0x0010; // Light attack
static constexpr uint32_t IN_M        = 0x0020; // Medium attack
static constexpr uint32_t IN_H        = 0x0040; // Heavy attack
static constexpr uint32_t IN_S        = 0x0080; // Special / Exchange
static constexpr uint32_t IN_A1       = 0x0100; // Assist 1
static constexpr uint32_t IN_A2       = 0x0200; // Assist 2
static constexpr uint32_t IN_START    = 0x100000;

static const char* DecodeDirection(uint32_t input)
{
    int lr = input & (IN_LEFT | IN_RIGHT);
    int ud = input & (IN_UP | IN_DOWN);

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
    append("L", IN_L);
    append("M", IN_M);
    append("H", IN_H);
    append("S", IN_S);
    append("A1", IN_A1);
    append("A2", IN_A2);
    append("Taunt", IN_START);
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


extern "C" void Controller_OnFrameStart();
extern "C" void Controller_OnFrameEnd();
// ---------------------------------------------------------------------------
// Hooked sMvc3Main::move — skips the call when paused unless advancing.
// ---------------------------------------------------------------------------
static void __fastcall HookedMove(void* sMvc3Main)
{
    if (isPaused)
    {
        if (advanceFrame)
        {
            advanceFrame = false;
            frameCounter++;
            if (Netplay_IsActive())
            {
                Controller_OnFrameStart();
            }
            oMove(sMvc3Main);
            if (Netplay_IsActive())
            {
                Controller_OnFrameEnd();
            }
        }
        else if (g_pDevice)
        {
            if (SUCCEEDED(g_pDevice->BeginScene()))
                g_pDevice->EndScene();
            g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
        }
        return;
    }

    if (Netplay_IsActive())
    {
        Controller_OnFrameStart();

        // --- Drain inbound delay-change REQuests.
        //     Receiver computes a safe commit frame T and sends an ACK back.
        DelayChangeRequest req;
        while (Netplay_GetInboundDelayReq(req))
        {
            if (req.delay >= 1 && req.delay <= 15)
            {
                int32_t T = Controller_GetNetFrameCount() + DELAY_HANDSHAKE_MARGIN;
                if (T < DELAY_HANDSHAKE_MARGIN) T = DELAY_HANDSHAKE_MARGIN;
                Controller_ScheduleDelayChange(req.delay, T);
                Netplay_SendDelayAck(req.delay, req.rollbackFrames, T);
                HookLog("Netplay: Delay REQ(d=%d) → ACK(T=%d)\n", req.delay, T);
            }
        }

        // --- Drain inbound delay-change ACKnowledgments.
        //     Initiator completes the negotiation and schedules the change.
        DelayChangeRequest ack;
        while (Netplay_GetInboundDelayAck(ack))
        {
            if (ack.delay >= 1 && ack.delay <= 15 &&
                Controller_IsNegotiationPending())
            {
                Controller_CompleteDelayNegotiation(ack.delay, ack.activationFrame);
                HookLog("Netplay: Delay ACK(d=%d, T=%d) → committed\n",
                        ack.delay, ack.activationFrame);
            }
        }

        // --- Drain inbound rollback-only packets (applied immediately).
        int32_t remoteRollback = -1;
        while (Netplay_GetInboundRollback(remoteRollback))
        {
            if (remoteRollback >= 0 && remoteRollback <= 16)
                Controller_SetRollbackFrames(remoteRollback);
        }
    }

    // --- If a simulation stall is active (from a recent delay increase),
    //     skip oMove but still accumulate local input and transmit packets.
    bool stalled = (g_stallFrames > 0);
    if (!stalled)
    {
        oMove(sMvc3Main);
    }

    if (Netplay_IsActive())
    {
        Controller_OnFrameEnd();
        if (stalled)
            g_stallFrames--;
    }

    CaptureNetInput();

    // ===== Netplay: logging only =====
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
            auto sPadGetInstance = (sPad__getInstance_t)_addr(SPAD_GET_INSTANCE_ADDR);
            uint64_t pad = sPadGetInstance();
            if (pad) {
                static uint32_t s_netplayFrameCounter = 0;
                s_netplayFrameCounter++;
                if (s_netplayFrameCounter % 60 == 0) {
                    uint32_t recvInput = Netplay_HasRemoteInput() ? Netplay_GetRemoteInput() : 0;
                    HookLog("[Netplay] Frame %u | Recv: 0x%08X | RTT: %u ms\n",
                            s_netplayFrameCounter, recvInput, Netplay_GetRTT());
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
    auto sPadGetInstance = (sPad__getInstance_t)_addr(SPAD_GET_INSTANCE_ADDR);
    auto getInput = (GetsCurrentInput_withSOCD_t)_addr(GET_CURRENT_INPUT_ADDR);
    uint64_t pad = sPadGetInstance();
    if (pad) {
        g_netP1Input = (uint32_t)getInput(pad, 0);
        g_netP2Read  = (uint32_t)getInput(pad, 1);
        g_padP2Input = *(uint32_t*)(pad + PAD_SLOT_STRIDE * 1 + PAD_SLOT_INPUT);
    } else {
        g_netP1Input = 0;
        g_netP2Read  = 0;
        g_padP2Input = 0;
    }
    g_bsP2Input = 0;
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
    //if (GameMode != 5) return false; // 5 = training (see GameMode_offset comment for full enum)

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

    // Initialize rollback snapshot engine (hooks sMvc3Main__move)
    Rollback_InitHooks();
}

// ---------------------------------------------------------------------------
// Public: per-frame processing — call from HookProcessStuff
// ---------------------------------------------------------------------------
// Edge-detection helper for use on threads without a message queue.
// GetAsyncKeyState's &1 bit is per-thread and requires a message pump;
// on the HookUpdate thread it always returns 0.  This helper uses the
// global &0x8000 (key-is-down) bit with tracked prior state instead.
static inline bool KeyPressed(int vk, bool& prevState)
{
    bool cur = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = cur && !prevState;
    prevState = cur;
    return pressed;
}

void FrameStepper_Process()
{
    // Netplay delay adjustment via [ (decrease) and ] (increase) keys.
    // Uses the negotiated handshake: sends REQ, waits for ACK, then commits.
    static bool prevBracketL = false, prevBracketR = false;
    if (Netplay_IsActive())
    {
        // Guard against overlapping negotiations and pending commits
        if (!Controller_IsDelayChangePending() && !Controller_IsNegotiationPending())
        {
            if (KeyPressed(VK_OEM_4, prevBracketL))  // [ = decrease delay
            {
                int32_t delay = Controller_GetDelayFrames() - 1;
                if (delay < 1) delay = 1;

                Controller_StartDelayNegotiation(delay);
                Netplay_SendDelayReq(delay, Controller_GetRollbackFrames());
                HookLog("Netplay: Delay REQ(d=%d) sent\n", delay);
            }
            if (KeyPressed(VK_OEM_6, prevBracketR))  // ] = increase delay
            {
                int32_t delay = Controller_GetDelayFrames() + 1;
                if (delay > 15) delay = 15;

                Controller_StartDelayNegotiation(delay);
                Netplay_SendDelayReq(delay, Controller_GetRollbackFrames());
                HookLog("Netplay: Delay REQ(d=%d) sent\n", delay);
            }
        }
    }

    // --- Desync auto-pause (works in any mode). ---
    // P key dismisses the alert and unpauses.  The overlay persists
    // (g_desyncSeen remains true) but future mismatches won't re-pause.
    static bool prevDesyncP = false;
    bool desyncActive = g_desyncDetected || g_desyncSeen;
    if (desyncActive)
    {
        if (KeyPressed(0x50, prevDesyncP))
        {
            g_desyncDetected = false;
            g_desyncSeen     = true;
            isPaused         = false;
            advanceFrame     = false;
        }
        else if (g_desyncDetected)
        {
            isPaused = true;
        }
    }

    if (!IsTrainingMode())
    {
        if (isPaused && !g_desyncDetected) {
            isPaused = false;
            if (g_pPauseTexture) {
                g_pPauseTexture->Release();
                g_pPauseTexture = nullptr;
            }
        }
        return;
    }

    // Toggle pause
    static bool prevPause = false;
    if (KeyPressed(KEY_PAUSE, prevPause))
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
    static bool prevAdvance = false;
    if (isPaused && KeyPressed(KEY_ADVANCE, prevAdvance))
    {
        advanceFrame = true;
        g_captureRequested = true; // snapshot the advanced frame
    }

    // --- Rollback save/restore test keys (F5 / F6 / F7) ---
    {
        // Read the game's absolute frame counter from sReplay+0x54
        static int32_t lastSaveFrame = 0;
        auto sReplay = *(uint64_t*)_addr(0x140d4d9d8);
        int32_t curFrame = sReplay ? *(int32_t*)(sReplay + 0x54) : 0;
        static constexpr int32_t RESIM_WINDOW = 20;

        static bool prevF5 = false, prevF6 = false, prevF7 = false;
        if (KeyPressed(VK_F5, prevF5)) {
            Rollback_SaveSnapshot(curFrame);
            lastSaveFrame = curFrame;
            HookLog("Rollback: saved snapshot at game frame %d\n", curFrame);
        }
        if (KeyPressed(VK_F6, prevF6)) {
            g_restoreRequestedFrame = lastSaveFrame;
            HookLog("Rollback: requesting restore to frame %d (deferred to frame boundary)\n", lastSaveFrame);
        }
        if (KeyPressed(VK_F7, prevF7)) {
            int32_t target = curFrame + RESIM_WINDOW;
            int32_t start = target - RESIM_WINDOW;
            if (start < 0) start = 0;
            HookLog("Rollback: requesting re-sim frames %d → %d (on main thread)\n", start, target);
            g_resimRequestedStart = start;
            g_resimRequestedEnd   = target;
        }
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
        uint32_t p1, p2;
        if (Netplay_IsActive())
        {
            int32_t frame = Controller_GetNetFrameCount();
            if (Netplay_IsHost())
            {
                p1 = Controller_GetRecordedLocalInput(frame);
                p2 = Controller_GetUsedRemoteInput(frame);
            }
            else
            {
                p1 = Controller_GetUsedRemoteInput(frame);
                p2 = Controller_GetRecordedLocalInput(frame);
            }
        }
        else
        {
            p1 = (uint32_t)getInput(pad, 0);
            p2 = (uint32_t)getInput(pad, 1);
            // OR in keybind-system flags so Z, S, etc. show up
            p1 |= *(uint32_t*)(pad + UNIT_FLAGS_OFFSET);
            p2 |= *(uint32_t*)(pad + UNIT_FLAGS_OFFSET);
        }

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
            ImGuiWindowFlags_AlwaysAutoResize);

        // Netplay status (always visible when netplay is active, shown at the top)
        if (Netplay_IsActive()) {
            const char* role = Netplay_IsHost() ? "HOST" : "CLIENT";
            ImVec4 color = Netplay_IsHost()
                ? ImVec4(1.0f, 0.9f, 0.1f, 1.0f)   // yellow for host
                : ImVec4(0.3f, 0.7f, 1.0f, 1.0f);   // blue for client
            ImGui::TextColored(color, "NETPLAY: %s  Ping: %ums", role, Netplay_GetRTT() / 2);
            int32_t curDelay = Controller_GetDelayFrames();
            int32_t curRollback = Controller_GetRollbackFrames();
            ImGui::TextColored(color, "  Frame:%d  Delay: %d  Rollback: %d  LastRecv:%d",
                Controller_GetNetFrameCount(),
                curDelay,
                curRollback,
                Controller_GetLastRemoteFrame());

            // Allow both peers to interact with the delay buttons, as long as
            // no change or negotiation is currently pending.
            if (!Controller_IsDelayChangePending() && !Controller_IsNegotiationPending())
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("[-]"))
                {
                    curDelay = Controller_GetDelayFrames() - 1;
                    if (curDelay < 1) curDelay = 1;
                    Controller_StartDelayNegotiation(curDelay);
                    Netplay_SendDelayReq(curDelay, Controller_GetRollbackFrames());
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("[+]"))
                {
                    curDelay = Controller_GetDelayFrames() + 1;
                    if (curDelay > 15) curDelay = 15;
                    Controller_StartDelayNegotiation(curDelay);
                    Netplay_SendDelayReq(curDelay, Controller_GetRollbackFrames());
                }
            }

            // Rollback adjustment buttons — also syncs both sides via delay packet.
            if (ImGui::SmallButton("[R-]"))
            {
                curRollback = Controller_GetRollbackFrames() - 1;
                if (curRollback < 0) curRollback = 0;
                Controller_SetRollbackFrames(curRollback);
                Netplay_SendRollback(curRollback);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("[R+]"))
            {
                curRollback = Controller_GetRollbackFrames() + 1;
                if (curRollback > 16) curRollback = 16;
                Controller_SetRollbackFrames(curRollback);
                Netplay_SendRollback(curRollback);
            }
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

    // --- Desync detected overlay (visible in all modes) ---
    if (g_desyncDetected || g_desyncSeen)
    {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                       ImGui::GetIO().DisplaySize.y * 0.18f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.0f, 0.0f, 0.92f));
        ImGui::Begin("##DesyncOverlay", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoInputs);

        ImGui::PushStyleColor(ImGuiCol_Text,
            g_desyncDetected ? ImVec4(1.0f, 0.1f, 0.1f, 1.0f)
                             : ImVec4(0.8f, 0.5f, 0.1f, 1.0f));
        ImGui::SetWindowFontScale(2.5f);
        ImGui::Text("DESYNC DETECTED");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();

        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f),
            "Frame %d: local 0x%08X  remote 0x%08X",
            g_desyncFrame, g_desyncLocalCS, g_desyncRemoteCS);
        if (g_desyncDetected) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                "Press P to unpause and continue");
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                "(dismissed — further mismatches logged only)");
        }

        ImGui::End();
        ImGui::PopStyleColor();
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
