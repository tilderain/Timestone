#include "../pch.h"
#include "../imgui/imgui.h"
#include "escmenu_imgui.h"
#include "framestepper.h"
#include "netplay.h"
#include "umvc3menu.h"
#include <Windows.h>

extern bool ChrSelImGui_IsOpen();
extern bool ChrSelImGui_Open();

namespace {

bool g_open = false;

void ExitGame()
{
    Netplay_SendPlayerDisconnect();
    HWND window = GetForegroundWindow();
    if (window && PostMessageW(window, WM_CLOSE, 0, 0))
        return;
    ExitProcess(0);
}

} // namespace

void EscMenuImGui_Update()
{
    static bool previousEscape = false;
    if (KeyPressed(VK_ESCAPE, previousEscape)) {
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
    if (ImGui::Button("Exit Game", buttonSize))
        ExitGame();

    ImGui::End();
}

bool EscMenuImGui_IsOpen()
{
    return g_open;
}
