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

using namespace Memory::VP;

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

// Forward declaration
static void CaptureNetInput();

// ---------------------------------------------------------------------------
// Hooked sMvc3Main::move — skips the call when paused unless advancing
// ---------------------------------------------------------------------------
static void __fastcall HookedMove(void* sMvc3Main)
{
    if (isPaused)
    {
        if (advanceFrame)
        {
            advanceFrame = false;
            frameCounter++;
            oMove(sMvc3Main);
            CaptureNetInput();
        }
        else if (g_pDevice)
        {
            // Force a render + present so the ImGui overlay stays visible.
            // Calling EndScene triggers our hkEndScene hook which renders
            // FrameStepper_Draw (and any other ImGui content). Then Present
            // flips the back buffer to screen.
            // BeginScene first to ensure D3D9 is in a valid render state.
            if (SUCCEEDED(g_pDevice->BeginScene()))
                g_pDevice->EndScene();
            g_pDevice->Present(nullptr, nullptr, nullptr, nullptr);
        }
        return;
    }
    oMove(sMvc3Main);
    CaptureNetInput();
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
}

// ---------------------------------------------------------------------------
// Public: per-frame processing — call from HookProcessStuff
// ---------------------------------------------------------------------------
void FrameStepper_Process()
{
    if (!IsTrainingMode())
    {
        if (isPaused)
            isPaused = false;
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
        }
    }

    // Advance one frame while paused
    if (isPaused && (GetAsyncKeyState(KEY_ADVANCE) & 1))
        advanceFrame = true;
}

// ---------------------------------------------------------------------------
// Public: draw pause indicator overlay — call from hkEndScene (after ImGui::NewFrame)
// ---------------------------------------------------------------------------
void FrameStepper_Draw()
{
    if (!isPaused || !IsTrainingMode())
        return;

    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##FrameStepper", nullptr,
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

    // Read and display current input values for P1 and P2
    // GetsCurrentInput_withSOCD handles hardcoded VK codes (arrows, X, Y, modifiers)
    // OR in unit flags from cUnit__getUnitFlags to pick up keybind-mapped keys (Z, S, etc.)
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
    }

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

    ImGui::End();
}
