#include "../pch.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "escmenu_imgui.h"
#include "framestepper.h"
#include "LaunchMenu.h"
#include "LaunchMenu_settings.h"
#include "lobby_flow.h"
#include "netplay.h"
#include "steam_lobby.h"
#include "umvc3menu.h"
#include "d3d9reset.h"
#include "d3d9ex.h"
#include "frame_pacing.h"
#include "lag_diagnostics.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

extern bool ChrSelImGui_IsOpen();
extern bool ChrSelImGui_Open();

namespace {

bool g_open = false;   // overlay thread
bool g_justOpened = false;

// sPad slots (sPad__perFrameUpdate 0x140514570, sPad__cnvX360Pad 0x1405111B0):
// four at sPad+0x58, 0x2E0 apart. Held buttons at +0x164. +0x164..+0x18F
// (masks, sticks, triggers) is what sPad__Pad__readPad clears while
// sPad::mActive is off.
constexpr size_t kPadSlotBase = 0x58;
constexpr size_t kPadSlotStride = 0x2E0;
constexpr size_t kPadSlotActive = 0x08;
constexpr size_t kPadSlotHeld = 0x164;
constexpr size_t kPadSlotInputBytes = 0x2C;

// Decoded mask bits (controller.h IN_*).
constexpr uint32_t kPadStart = 0x0008;
constexpr uint32_t kPadUp = 0x0010, kPadRight = 0x0020, kPadDown = 0x0040, kPadLeft = 0x0080;
constexpr uint32_t kPadB = 0x2000, kPadA = 0x4000;
constexpr uint32_t kStickUp = 0x10000, kStickRight = 0x20000, kStickDown = 0x40000,
                   kStickLeft = 0x80000;

// Shared between the game thread (pad poll) and the overlay thread.
std::atomic<bool> g_available{false};
std::atomic<bool> g_openShared{false};
std::atomic<bool> g_toggleRequested{false};
std::atomic<uint32_t> g_padHeld{0};
bool g_blockingPad = false;  // game thread
uint32_t g_pollPrevHeld = 0; // game thread: held buttons of the previous poll
uint32_t g_navPrevHeld = 0;  // overlay thread
bool g_closeRequested = false;

void ExitGame()
{
    HWND window = GetForegroundWindow();
    if (window && PostMessageW(window, WM_CLOSE, 0, 0))
        return;
    PrepareForGameExit();
    ExitProcess(0);
}

} // namespace

void EscMenuImGui_OnPadPoll(uint64_t sPad)
{
    if (!sPad || !g_available.load(std::memory_order_relaxed)) {
        g_padHeld.store(0, std::memory_order_relaxed);
        g_blockingPad = false;
        g_pollPrevHeld = 0;
        return;
    }
    uint32_t held = 0;
    for (size_t slot = 0; slot < 4; ++slot) {
        const uint8_t* pad = reinterpret_cast<const uint8_t*>(
            sPad + kPadSlotBase + slot * kPadSlotStride);
        if (pad[kPadSlotActive]) held |= *reinterpret_cast<const uint32_t*>(pad + kPadSlotHeld);
    }
    // Presses from our own previous poll, not the slot's: clearing the slot
    // below also clears its previous-frame mask, which would make a held
    // Start read as a new press every frame.
    const uint32_t pressed = held & ~g_pollPrevHeld;
    g_pollPrevHeld = held;
    g_padHeld.store(held, std::memory_order_relaxed);

    bool open = g_openShared.load(std::memory_order_acquire);
    if (pressed & kPadStart) {
        g_toggleRequested.store(true, std::memory_order_release);
        open = !open; // what the overlay is about to do; this frame's Start stays ours
    }
    // Keep the pad from the game while the menu is open, and after it closes
    // until everything is let go: the B or Start that closed it must not
    // arrive in the match as a fresh press.
    g_blockingPad = open || (g_blockingPad && held != 0);
    if (!g_blockingPad) return;
    for (size_t slot = 0; slot < 4; ++slot) {
        uint8_t* pad = reinterpret_cast<uint8_t*>(sPad + kPadSlotBase + slot * kPadSlotStride);
        if (pad[kPadSlotActive]) memset(pad + kPadSlotHeld, 0, kPadSlotInputBytes);
    }
}

void EscMenuImGui_FeedGamepad()
{
    ImGuiIO& io = ImGui::GetIO();
    if (!g_open) {
        g_navPrevHeld = g_padHeld.load(std::memory_order_relaxed);
        return;
    }
    // Set only across ImGui::NewFrame (EscMenuImGui_Update clears them): the
    // Win32 backend polls XInput itself whenever it sees them at its own
    // NewFrame, and this menu already has the game's poll.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    const uint32_t held = g_padHeld.load(std::memory_order_relaxed);
    const auto nav = [&](ImGuiNavInput input, uint32_t mask) {
        if (held & mask) io.NavInputs[input] = 1.0f;
    };
    nav(ImGuiNavInput_Activate, kPadA);
    nav(ImGuiNavInput_Cancel, kPadB);
    // ImGui moves between items only on the d-pad inputs (its stick inputs
    // scroll), so the stick, and sticks that report as one, count as d-pad.
    nav(ImGuiNavInput_DpadUp, kPadUp | kStickUp);
    nav(ImGuiNavInput_DpadDown, kPadDown | kStickDown);
    nav(ImGuiNavInput_DpadLeft, kPadLeft | kStickLeft);
    nav(ImGuiNavInput_DpadRight, kPadRight | kStickRight);

    // B backs out of an open combo first (ImGui's own Cancel); with nothing
    // open it closes the menu. Checked before NewFrame closes any popup.
    const bool bPressed = (held & kPadB) && !(g_navPrevHeld & kPadB);
    if (bPressed && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        g_closeRequested = true;
    g_navPrevHeld = held;
}

void EscMenuImGui_Update()
{
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;

    // In a netplay match this is the pause menu: Esc or the pad's Start
    // opens it. Offline the game's own pause menu keeps those, and F3 opens
    // this one for its settings (not while the frame stepper is paused,
    // where F3 advances a frame). The lobby and a match ending close it.
    const bool lobby = LobbyFlow_IsLobbyVisible();
    const bool netplay = Netplay_IsActive() && !lobby;
    const bool offline = !Netplay_IsActive() && !lobby;
    const bool padToggle = g_toggleRequested.exchange(false, std::memory_order_acq_rel);
    static bool wasNetplay = false;
    if ((!netplay && !offline) || netplay != wasNetplay) {
        g_open = false;
        g_closeRequested = false;
    }
    wasNetplay = netplay;
    // Offline the pad belongs to the game until the menu is open.
    g_available.store(netplay || (offline && g_open), std::memory_order_relaxed);
    if (!netplay && !offline) {
        g_openShared.store(false, std::memory_order_release);
        return;
    }

    const bool toggle = netplay ? (KeyPressed(VK_ESCAPE) || padToggle)
        : (KeyPressed(VK_F3) && !FrameStepper_IsPaused()) ||
          (g_open && padToggle); // not Esc: offline that is the game's own pause
    if (toggle) {
        if (g_open)
            g_open = false;
        else if (!ChrSelImGui_IsOpen() && !TheMenu->GetActiveState())
            g_open = g_justOpened = true;
    }
    if (g_closeRequested) {
        g_open = false;
        g_closeRequested = false;
    }
    g_openShared.store(g_open, std::memory_order_release);
}

void EscMenuImGui_Draw()
{
    if (!g_open)
        return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                                                 IM_COL32(0, 0, 0, 140));

    const float statusHudBottom = FrameStepper_GetStatusHudBottom();
    ImGui::SetNextWindowPos(ImVec2(12.0f, statusHudBottom > 0.0f ? statusHudBottom + 8.0f : 16.0f),
                            ImGuiCond_Always);
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
    // Mouse only: controller and keyboard navigation stay on the pause menu.
    ImGui::Begin("Settings##EscMenu", nullptr,
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextDisabled("Renderer: %s", D3D9Ex_BackendName());
    // RenderingThread toggle: hidden for now.
    // const int renderingOverride = D3D9Reset_GetRenderingThreadOverride();
    // bool renderingThread = renderingOverride < 0
    //     ? D3D9Reset_IsRenderingThreadEnabled() : renderingOverride != 0;
    // static bool saveFailed = false;
    // ImGui::BeginDisabled(!D3D9Reset_RenderingThreadOverrideAvailable);
    // if (ImGui::Checkbox("RenderingThread", &renderingThread))
    //     saveFailed = !D3D9Reset_SetRenderingThreadOverride(renderingThread ? 1 : 0);
    // if (ImGui::IsItemHovered()) {
    //     ImGui::SetTooltip("ON adds one frame of display latency.\n"
    //                       "OFF may decrease performance or other various issues.");
    // }
    // if (renderingOverride < 0 && D3D9Reset_RenderingThreadOverrideAvailable)
    //     ImGui::TextDisabled("Using game default until changed.");
    // ImGui::EndDisabled();
    // if (saveFailed)
    //     ImGui::TextUnformatted("Could not save; previous setting retained.");
    EscMenuImGui_DrawDisplaySettings();
    ImGui::Separator();
    const auto capture = LagDiagnostics_GetCaptureStatus();
    const bool capturing = capture == LagCaptureStatus::Starting ||
        capture == LagCaptureStatus::Recording || capture == LagCaptureStatus::Finishing ||
        capture == LagCaptureStatus::Continuous;
    static bool captureStartFailed = false;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
    ImGui::BeginDisabled(capturing);
    if (ImGui::Button("Start lag diagnostic")) {
        captureStartFailed = !LagDiagnostics_StartCapture();
        if (!captureStartFailed) g_open = false;
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    if (capture == LagCaptureStatus::Recording)
        ImGui::TextDisabled("Recording: %u seconds remaining", LagDiagnostics_GetCaptureSecondsRemaining());
    else if (capture == LagCaptureStatus::Starting)
        ImGui::TextDisabled("Starting capture...");
    else if (capture == LagCaptureStatus::Finishing)
        ImGui::TextDisabled("Finishing log...");
    else if (capture == LagCaptureStatus::Continuous)
        ImGui::TextDisabled("Continuous diagnostics enabled in Timestone.ini.");
    else if (captureStartFailed || capture == LagCaptureStatus::Failed)
        ImGui::TextUnformatted("Could not capture log. Check folder write access.");
    else if (capture == LagCaptureStatus::OpenFailed)
        ImGui::TextUnformatted("Log saved beside the game, but could not open it.");
    else if (capture == LagCaptureStatus::Complete)
        ImGui::TextDisabled("Capture complete. Log opened.");
    else
        ImGui::TextDisabled("Records for 15 seconds, then opens the log.");
    const ImVec2 settingsPos = ImGui::GetWindowPos();
    const ImVec2 settingsSize = ImGui::GetWindowSize();
    ImGui::End();

    // Watching: who else is, the setup's queue and where the feed comes
    // from, in a second panel under the settings.
    const SpectatorInfo watch = LobbyFlow_IsSpectating() ? LobbyFlow_GetSpectatorInfo()
                                                         : SpectatorInfo{};
    if (watch.valid) {
        const ImVec4 gold(0.90f, 0.81f, 0.48f, 1.0f);
        const ImVec4 dim(0.62f, 0.60f, 0.50f, 1.0f);
        ImGui::SetNextWindowPos(ImVec2(settingsPos.x, settingsPos.y + settingsSize.y + 8.0f),
                                ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(settingsSize.x, 0.0f),
                                            ImVec2(settingsSize.x, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.58f, 0.54f, 0.39f, 0.45f));
        ImGui::Begin("Spectating##EscMenu", nullptr,
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
        // A gold heading with its count against the right edge.
        const auto rightText = [](const ImVec4& color, const char* text) {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(text).x);
            ImGui::TextColored(color, "%s", text);
        };
        const auto heading = [&](const char* title, int count) {
            ImGui::TextColored(gold, "%s", title);
            if (count >= 0) {
                char number[8];
                snprintf(number, sizeof(number), "%d", count);
                rightText(dim, number);
            }
            ImGui::Separator();
        };
        const auto person = [&](const SpectatorInfo::Person& who, int place) {
            ImGui::Indent(6.0f);
            if (place > 0) {
                ImGui::TextColored(dim, "%d", place);
                ImGui::SameLine(34.0f);
            }
            if (who.local) ImGui::TextColored(gold, "%s", who.name);
            else ImGui::TextUnformatted(who.name);
            if (who.local) rightText(dim, "you");
            ImGui::Unindent(6.0f);
        };
        const auto nobody = [&]() {
            ImGui::Indent(6.0f);
            ImGui::TextColored(dim, "Nobody");
            ImGui::Unindent(6.0f);
        };

        heading("RECEIVING FROM", -1);
        ImGui::Indent(6.0f);
        if (watch.feed[0]) {
            ImGui::TextUnformatted(watch.feed);
            if (watch.feedPing >= 0) {
                char ping[16];
                snprintf(ping, sizeof(ping), "%d ms", watch.feedPing);
                const ImVec4 color = watch.feedPing < 100 ? ImVec4(0.55f, 0.86f, 0.50f, 1.0f)
                                   : watch.feedPing < 200 ? ImVec4(0.93f, 0.80f, 0.40f, 1.0f)
                                                          : ImVec4(0.93f, 0.47f, 0.40f, 1.0f);
                rightText(color, ping);
            }
        } else {
            ImGui::TextColored(dim, "Nobody yet");
        }
        ImGui::Unindent(6.0f);

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        heading("WATCHING", watch.watcherCount);
        for (int i = 0; i < watch.watcherCount; ++i) person(watch.watchers[i], 0);
        if (!watch.watcherCount) nobody();

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        heading("IN QUEUE", watch.queueCount);
        for (int i = 0; i < watch.queueCount; ++i) person(watch.queue[i], i + 1);
        if (!watch.queueCount) nobody();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(9);

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f));
    // Gamepad and keyboard navigation start here, on Resume, when it opens.
    const bool selectResume = g_justOpened;
    g_justOpened = false;
    if (selectResume) ImGui::SetNextWindowFocus();
    ImGui::Begin("PAUSE MENU", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    const ImVec2 buttonSize(-1.0f, 36.0f);
    if (ImGui::Button("Resume", buttonSize))
        g_open = false;
    if (selectResume) {
        // Highlighted at once, not only after the first d-pad press.
        ImGuiContext& g = *GImGui;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        g.NavDisableHighlight = false;
        g.NavDisableMouseHover = true;
    }
    // Someone watching has no match of their own to restart or re-pick.
    const bool spectating = LobbyFlow_IsSpectating();
    if (!spectating) {
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
    }
    if (LobbyFlow_InRoomMatch() &&
        ImGui::Button(spectating ? "Stop Watching" : "Return to Lobby", buttonSize)) {
        LobbyFlow_RequestLeaveMatch();
        g_open = false;
    }
    // In a lobby match the way out is Return to Lobby; quitting the game
    // from here would drop the opponent and the room in one click.
    if (!LobbyFlow_InRoomMatch() && ImGui::Button("Exit Game", buttonSize))
        ExitGame();

    ImGui::End();
    // Buttons above close the menu directly; the pad poll follows at once.
    g_openShared.store(g_open, std::memory_order_release);
}

void EscMenuImGui_DrawDisplaySettings()
{
    UINT width = 0, height = 0;
    if (!D3D9Reset_GetResolution(width, height))
        return;

    static std::vector<D3DDISPLAYMODE> resolutions;
    static bool modesLoaded = false;
    if (!modesLoaded) {
        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (d3d) {
            const UINT count = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
            for (UINT i = 0; i < count; ++i) {
                D3DDISPLAYMODE mode = {};
                if (FAILED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &mode)) ||
                    mode.Width < 640 || mode.Height < 480 || (mode.Width & 7) != 0)
                    continue;
                if (std::none_of(resolutions.begin(), resolutions.end(),
                    [&](const D3DDISPLAYMODE& entry) {
                        return entry.Width == mode.Width && entry.Height == mode.Height;
                    }))
                    resolutions.push_back(mode);
            }
            d3d->Release();
            modesLoaded = true;
        }
    }

    ImGui::Separator();
    char current[32] = {};
    snprintf(current, sizeof(current), "%ux%u", width, height);
    static bool resolutionSaveFailed = false;
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("Resolution", current)) {
        for (const D3DDISPLAYMODE& mode : resolutions) {
            char label[32] = {};
            snprintf(label, sizeof(label), "%ux%u", mode.Width, mode.Height);
            const bool selected = width == mode.Width && height == mode.Height;
            if (ImGui::Selectable(label, selected))
                resolutionSaveFailed = !D3D9Reset_SetResolution(mode.Width, mode.Height);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (resolutionSaveFailed)
        ImGui::TextUnformatted("Could not save/apply resolution");

    bool fullscreen = D3D9Reset_IsFullscreen();
    static bool fullscreenSaveFailed = false;
    if (ImGui::Checkbox("Fullscreen", &fullscreen))
        fullscreenSaveFailed = !D3D9Reset_SetFullscreen(fullscreen);
    if (fullscreenSaveFailed)
        ImGui::TextUnformatted("Could not save/apply fullscreen.");

    bool vsync = IsLaunchVsyncEnabled();
    static bool vsyncSaveFailed = false;
    if (ImGui::Checkbox("VSync", &vsync))
        vsyncSaveFailed = !D3D9Reset_SetVsync(vsync);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON makes frame pacing consistent in exchange for adding input lag");
    if (vsyncSaveFailed)
        ImGui::TextUnformatted("Could not save VSync.");

    // Only does anything with VSync off, where VSync is not pacing the frames.
    bool evenPacing = FramePacing_IsHoldEnabled();
    static bool evenPacingSaveFailed = false;
    ImGui::BeginDisabled(vsync);
    if (ImGui::Checkbox("Even frame pacing", &evenPacing))
        evenPacingSaveFailed = !FramePacing_SetHoldEnabled(evenPacing);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("VSync off only. Shows every frame at the same point after it starts,\n"
                          "so frames land evenly. Adds a millisecond or two of input lag.");
    if (evenPacingSaveFailed)
        ImGui::TextUnformatted("Could not save even pacing.");
}

bool EscMenuImGui_IsOpen()
{
    return g_open;
}
