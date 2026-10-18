#include "../pch.h"
#include "../imgui/imgui.h"
#include "escmenu_imgui.h"
#include "framestepper.h"
#include "LaunchMenu.h"
#include "LaunchMenu_settings.h"
#include "netplay.h"
#include "steam_lobby.h"
#include "steam_transport.h"
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

    if (SteamTransport_IsActive()) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 16.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.07f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.58f, 0.54f, 0.39f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.87f, 0.73f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##EscMenuConnection", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Steam connection: %s", SteamLobby_GetConnectionType());
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.07f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.16f, 0.16f, 0.14f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.22f, 0.22f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.58f, 0.54f, 0.39f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.90f, 0.81f, 0.48f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.87f, 0.73f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.22f, 0.22f, 0.19f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.40f, 0.37f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.55f, 0.49f, 0.30f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("Settings##EscMenu", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);
    const int renderingOverride = D3D9Reset_GetRenderingThreadOverride();
    bool renderingThread = renderingOverride < 0
        ? D3D9Reset_IsRenderingThreadEnabled() : renderingOverride != 0;
    static bool saveFailed = false;
    if (ImGui::Checkbox("RenderingThread", &renderingThread))
        saveFailed = !D3D9Reset_SetRenderingThreadOverride(renderingThread ? 1 : 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ON adds one frame of display latency.\n"
                          "OFF may decrease performance or other various issues.");
    }
    if (renderingOverride < 0)
        ImGui::TextDisabled("Using game default until changed.");
    bool singleThreaded = IsGameSingleThreaded();
    if (ImGui::Checkbox("Run entire game single-threaded", &singleThreaded))
        saveFailed = !SetGameSingleThreaded(singleThreaded);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sets game worker count to 1. May massively improve performance by 100%% on pcs with bad thread work, but may decrease performance by about 10% on higher end pcs");
    if (saveFailed)
        ImGui::TextUnformatted("Could not save; previous setting retained.");
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(9);

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f));
    ImGui::Begin("PAUSE MENU", nullptr,
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
