#include "../pch.h"
#include "../imgui/imgui.h"
#include "escmenu_imgui.h"
#include "framestepper.h"
#include "LaunchMenu.h"
#include "netplay.h"
#include "steam_lobby.h"
#include "umvc3menu.h"
#include "d3d9reset.h"
#include <Windows.h>

extern bool ChrSelImGui_IsOpen();
extern bool ChrSelImGui_Open();

namespace {

bool g_open = false;

void ExitGame()
{
    HWND window = GetForegroundWindow();
    if (window && PostMessageW(window, WM_CLOSE, 0, 0))
        return;
    PrepareForGameExit();
    ExitProcess(0);
}

} // namespace

void EscMenuImGui_Update()
{
    if (GetLaunchOption() == LAUNCH_NORMAL) {
        g_open = false;
        return;
    }

    if (KeyPressed(VK_ESCAPE)) {
        if (g_open)
            g_open = false;
        else if (!ChrSelImGui_IsOpen() && !TheMenu->GetActiveState())
            g_open = true;
    }
}

void EscMenuImGui_Draw()
{
    if (!g_open)
        return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##EscMenuBackdrop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::Begin("Settings##EscMenu", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);
    const int renderingOverride = D3D9Reset_GetRenderingThreadOverride();
    bool renderingThread = renderingOverride < 0
        ? D3D9Reset_IsRenderingThreadEnabled() : renderingOverride != 0;
    static bool saveFailed = false;
    if (ImGui::Checkbox("Rendering thread override", &renderingThread))
        saveFailed = !D3D9Reset_SetRenderingThreadOverride(renderingThread ? 1 : 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON adds one tick of display latency.\n"
                          "OFF may decrease performance or other various issues.");
    }
    if (renderingOverride < 0)
        ImGui::TextDisabled("Using game default until changed.");
    if (saveFailed)
        ImGui::TextUnformatted("Could not save; previous setting retained.");
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f));
    ImGui::Begin("Pause Menu", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    const ImVec2 buttonSize(-1.0f, 36.0f);
    if (ImGui::Button("Resume", buttonSize))
        g_open = false;
    if (ImGui::Button("Restart Match (f11)", buttonSize) &&
        FrameStepper_RequestMatchRestart())
        g_open = false;
    if (ImGui::Button("Character Select (f8)", buttonSize) && ChrSelImGui_Open())
        g_open = false;
    const bool steamProfileAvailable = SteamLobby_HasPeerProfile();
    if (!steamProfileAvailable) ImGui::BeginDisabled();
    if (ImGui::Button("Open Opponent Steam Profile", buttonSize))
        SteamLobby_OpenPeerProfile();
    if (!steamProfileAvailable) ImGui::EndDisabled();
    if (ImGui::Button("Exit Game", buttonSize))
        ExitGame();

    ImGui::End();
}

bool EscMenuImGui_IsOpen()
{
    return g_open;
}
