// The lobby's SETTINGS screen (top menu) and its resolution list.
#include "lobby_ui.h"
#include "lobby_ui_internal.h"

#include <cstdio>

namespace lobby {

namespace {

// Settings-screen rows, all the plugin's own settings (SettingsSource).
enum SettingsRow {
    SETTINGS_RESOLUTION, SETTINGS_FULLSCREEN, SETTINGS_VSYNC, SETTINGS_ROUTING, SETTINGS_NET_STATS,
    SETTINGS_ROWS
};
constexpr Setting kSettingsRowSource[SETTINGS_ROWS] = {
    Setting::Count, Setting::Fullscreen, Setting::Vsync, Setting::RelayOnly, Setting::Count};
// SettingsSource::netStats values, by name.
constexpr const char* kNetStatsNames[3] = {"OFF", "COMPACT", "FULL"};
constexpr int kResolutionRows = 9; // modes visible at once in the resolution list

// What NETWORK STATS shows in a match, with made-up numbers: the readout
// framestepper.cpp draws (FrameStepper_Draw) in the match's top-left corner,
// as the host sees it, in the same corner of this screen. Keep the lines in
// step with that function. Off draws nothing.
void DrawNetStatsPreview(const Painter& p, int mode)
{
    constexpr float kBoxLeft = 12.0f, top = 12.0f; // as the readout's window
    constexpr float kScale = 0.5f, kLine = 18.0f, kPad = 9.0f;
    constexpr Color kHost = Color{1.0f, 0.9f, 0.1f, 1.0f};
    const char* const full[] = {"--HOST--", "Ping: 48ms", "FPS: 60.0", "Loss: 0.0% / 0.0%",
                                "Jitter: 2ms", "Delay: 2  Rollback: 1"};
    const char* const compact[] = {"Ping 48", "FPS 60", "2D 1R"};
    const char* const* lines = mode == 2 ? full : compact;
    const int count = mode == 2 ? 6 : mode == 1 ? 3 : 0;
    if (count == 0) return;
    float width = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float w = TextWidth(lines[i]) * kScale;
        if (w > width) width = w;
    }
    const float left = kBoxLeft + kPad;
    const float centerX = left + width * 0.5f;
    p.Fill(left - kPad, top, width + 2.0f * kPad, count * kLine + 2.0f * kPad,
           Color{0.0f, 0.0f, 0.0f, 0.55f});
    for (int i = 0; i < count; ++i) {
        const float y = top + kPad + (i + 0.5f) * kLine;
        // The role heads the full readout, centred over the rest.
        if (mode == 2 && i == 0) p.Text(lines[i], centerX, y, kScale, A_MC, kHost);
        else p.Text(lines[i], left, y, kScale, A_ML, kHost);
    }
}

} // namespace

// RESOLUTION has too many values to step through: confirm lists them.
bool LobbyUi::SettingsRowOpensList() const { return settingsCursor_ == SETTINGS_RESOLUTION; }

bool LobbyUi::SettingOn(Setting setting) const
{
    return settings_.get && settings_.get(setting);
}

// The display's modes. Nothing changes while the list is moved through: the
// one confirmed is applied, and the list closes.
void LobbyUi::UpdateResolutionList(const Input& input)
{
    const int count = settings_.resolutionCount ? settings_.resolutionCount() : 0;
    if (input.cancel || input.menu || count <= 0) {
        resolutionOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (resolutionCursor_ >= count) resolutionCursor_ = count - 1;
    MoveCursor(resolutionCursor_, count, input);
    if (!input.confirm) return;
    resolutionOpen_ = false;
    if (!settings_.setResolution(resolutionCursor_)) {
        Play(SFX_CANCEL);
        ShowNotice("The resolution could not be changed.");
        return;
    }
    Play(SFX_CONFIRM);
}

// RESOLUTION opens a list. Every other row is one of two values: left, right
// and confirm all flip it, and it takes effect at once.
void LobbyUi::UpdateSettings(const Input& input)
{
    if (resolutionOpen_) {
        UpdateResolutionList(input);
        return;
    }
    MoveCursor(settingsCursor_, SETTINGS_ROWS, input);
    if (input.cancel) {
        Play(SFX_CANCEL);
        GoTo(Screen::Top);
        return;
    }
    if (settingsCursor_ == SETTINGS_RESOLUTION) {
        if (!input.confirm) return;
        const int count = settings_.resolutionCount ? settings_.resolutionCount() : 0;
        if (count <= 0) {
            Play(SFX_CANCEL);
            return;
        }
        // Open on the mode in use, if the list has it.
        int width = 0, height = 0;
        resolutionCursor_ = 0;
        if (settings_.resolution(-1, &width, &height)) {
            for (int i = 0; i < count; ++i) {
                int w = 0, h = 0;
                if (settings_.resolution(i, &w, &h) && w == width && h == height)
                    resolutionCursor_ = i;
            }
        }
        resolutionOpen_ = true;
        Play(SFX_CONFIRM);
        return;
    }
    if (!input.left && !input.right && !input.confirm) return;
    if (settingsCursor_ == SETTINGS_NET_STATS) {
        // Shown as FULL, COMPACT, OFF: right goes down the list, as F3 does.
        const int mode = settings_.netStats ? settings_.netStats() : 1;
        const int next = (mode + (input.left ? 1 : 2)) % 3;
        if (!settings_.setNetStats || !settings_.setNetStats(next)) {
            Play(SFX_CANCEL);
            ShowNotice("This setting could not be changed.");
            return;
        }
        Play(SFX_MOVE);
        return;
    }
    const Setting setting = kSettingsRowSource[settingsCursor_];
    const bool on = !SettingOn(setting);
    if (!settings_.set || !settings_.set(setting, on)) {
        Play(SFX_CANCEL);
        ShowNotice("This setting could not be changed.");
        return;
    }
    Play(SFX_MOVE);
}

const char* LobbyUi::DrawSettings(Canvas& canvas) const
{
    const float t = EaseOut(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, (1.0f - t) * 36.0f);

    // On NETWORK STATS the readout is previewed in its own corner, where the
    // title is: the title gives way to it, unless the value is OFF.
    const int netStats = settings_.netStats ? settings_.netStats() : 1;
    const bool previewStats =
        settingsCursor_ == SETTINGS_NET_STATS && !resolutionOpen_ && netStats != 0;
    if (previewStats) DrawNetStatsPreview(p, netStats);
    else p.Text("SETTINGS", 76.0f, 70.0f, 0.62f, A_ML, kText, FONT_HEADING);

    const float panelX = 170.0f, panelY = 180.0f, panelW = 940.0f;
    const float panelH = 104.0f + SETTINGS_ROWS * 46.0f;
    p.Box(TEX_PANEL_DARK, panelX, panelY, panelW, panelH);

    const char* const labels[SETTINGS_ROWS] = {"RESOLUTION", "FULLSCREEN", "VSYNC",
                                               "STEAM ROUTING", "NETWORK STATS"};
    char resolution[32] = "-";
    int width = 0, height = 0;
    if (settings_.resolution && settings_.resolution(-1, &width, &height))
        snprintf(resolution, sizeof(resolution), "%dx%d", width, height);
    for (int row = 0; row < SETTINGS_ROWS; ++row) {
        const float y = panelY + 75.0f + row * 46.0f;
        const bool selected = row == settingsCursor_;
        if (selected)
            p.Box(TEX_ROW_SEL, panelX + 26.0f, y - 23.0f, panelW - 52.0f, 46.0f,
                  WithAlpha(kWhite, SelectionPulse(frame_)));
        p.Text(labels[row], panelX + 64.0f, y, 0.86f, A_ML, selected ? kText : kDim);
        const char* value;
        if (row == SETTINGS_RESOLUTION) value = resolution;
        else if (row == SETTINGS_ROUTING)
            value = SettingOn(Setting::RelayOnly) ? "RELAY ONLY" : "AUTOMATIC";
        else if (row == SETTINGS_NET_STATS)
            value = kNetStatsNames[netStats >= 0 && netStats <= 2 ? netStats : 1];
        else value = SettingOn(kSettingsRowSource[row]) ? "ON" : "OFF";
        const float valueX = panelX + 660.0f;
        p.TextFit(value, valueX, y, 0.86f, kValueTextWidth, A_MC, kText);
        // The resolution has too many values for arrows: confirm lists them.
        if (selected && row != SETTINGS_RESOLUTION)
            DrawValueArrows(p, frame_, valueX, y, kValueArrowSpan);
    }

    if (resolutionOpen_) {
        DrawResolutionList(canvas);
        return nullptr;
    }
    const char* const help[SETTINGS_ROWS] = {
        "Choose the game's resolution from the ones your display offers.",
        "Fill the screen, or play in a window.",
        "On makes frame pacing consistent in exchange for adding input lag.",
        "Relay only hides your IP from your opponent. Automatic can be faster.",
        "Ping, delay and rollback shown during a match. F3 changes it in a match too.",
    };
    return help[settingsCursor_];
}

void LobbyUi::DrawResolutionList(Canvas& canvas) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});
    const int count = settings_.resolutionCount ? settings_.resolutionCount() : 0;
    const int rows = count < kResolutionRows ? count : kResolutionRows;
    const float w = 440.0f;
    const float h = 136.0f + rows * 44.0f;
    const float x = (kScreenW - w) * 0.5f;
    const float y = (kScreenH - h) * 0.5f - 10.0f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Text("RESOLUTION", x + w * 0.5f, y + 40.0f, 0.38f, A_MC, kYellow, FONT_HEADING);

    // Keep the cursor in the middle of the window where the list allows it.
    int first = resolutionCursor_ - rows / 2;
    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;
    int currentW = 0, currentH = 0;
    settings_.resolution(-1, &currentW, &currentH);
    for (int row = 0; row < rows; ++row) {
        const int index = first + row;
        int width = 0, height = 0;
        if (!settings_.resolution(index, &width, &height)) continue;
        const float rowY = y + 88.0f + row * 44.0f;
        const bool selected = index == resolutionCursor_;
        if (selected)
            p.Box(TEX_ROW_SEL, x + 22.0f, rowY - 24.0f, w - 44.0f, 48.0f,
                  WithAlpha(kWhite, SelectionPulse(frame_)));
        char label[32];
        snprintf(label, sizeof(label), "%dx%d", width, height);
        // The mode in use is cyan, as the track playing is in the music list.
        p.Text(label, x + w * 0.5f, rowY, 0.8f, A_MC,
               width == currentW && height == currentH ? kCyan : selected ? kText : kDim);
    }
    char position[24];
    snprintf(position, sizeof(position), "%d / %d", resolutionCursor_ + 1, count);
    p.Text(position, x + w - 48.0f, y + h - 30.0f, 0.62f, A_MR, kDim);
}

} // namespace lobby
