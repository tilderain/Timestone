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

// D3D9 device pointer, set from FrameStepper_SetDevice
static LPDIRECT3DDEVICE9 g_pDevice = nullptr;

// Game window handle for forcing repaints
static HWND g_hWnd = nullptr;

// ---------------------------------------------------------------------------
// Original function pointer for the vtable-hooked move
// ---------------------------------------------------------------------------
typedef void(__fastcall* Move_t)(void*);
static Move_t oMove = nullptr;

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

    ImGui::End();
}
