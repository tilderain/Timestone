// The lobby's SETTINGS screen (top menu) and its resolution list.
#include "lobby_ui.h"
#include "lobby_ui_internal.h"

#include <cstdio>

namespace lobby {

namespace {

// Settings-screen rows, all the plugin's own settings (SettingsSource).
enum SettingsRow {
    SETTINGS_RESOLUTION, SETTINGS_FULLSCREEN, SETTINGS_VSYNC, SETTINGS_ROUTING, SETTINGS_NET_STATS,
    SETTINGS_SPECTATOR,
    SETTINGS_ROWS
};
constexpr Setting kSettingsRowSource[SETTINGS_ROWS] = {
    Setting::Count, Setting::Fullscreen, Setting::Vsync, Setting::RelayOnly, Setting::Count,
    Setting::Count};
// The rows that are a Level, and what each calls its values (0, 1, 2).
constexpr Level kSettingsRowLevel[SETTINGS_ROWS] = {
    Level::Count, Level::Count, Level::Count, Level::Count, Level::NetStats,
    Level::SpectatorOverlay};
constexpr const char* kLevelNames[static_cast<int>(Level::Count)][3] = {
    {"OFF", "COMPACT", "FULL"}, {"NONE", "COMPACT", "FULL"}};
constexpr int kResolutionRows = 9; // modes visible at once in the resolution list

// What NETWORK STATS shows in a match, with made-up numbers: the readout
// framestepper.cpp draws (FrameStepper_Draw) in the match's top-left corner,
// as the host sees it, in the same corner of this screen. Keep the lines in
// step with that function. Off draws nothing.
void DrawNetStatsPreview(const Painter& p, int mode)
{
    constexpr float kBoxLeft = 12.0f, top = 12.0f; // as the readout's window
    constexpr float kScale = 0.4f, kLine = 14.0f, kPad = 6.0f;
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

// What SPECTATOR OVERLAY leaves over a match being watched, with made-up
// players. Full is what the battle HUD has at the top centre
// (the set score, SPECTATING under it) and the two name
// plates under the life bars; compact is the name plates alone; none draws
// nothing.
// The name plates are the game's own text units, Font_UserName_1P and _2P of
// ui\game\ga_sprite\ga_pos05 (uBtScore's layout), and are drawn as that
// layout has them: the body font at mScale (0.85, 0.9), player 1 at
// (218, 146) anchored left and player 2 at (1060, 146) anchored right, each
// with a shadow 2 px down-right at alpha 0.749. A text unit's size on screen
// goes with mScale squared and a single line hangs from its anchor (see
// NativeCanvas::Text in lobby_native.cpp), so that is 0.7225 by 0.81 of the
// 32 px cell with its top at y 146. The game shades each from mColor at
// the top to mColor1 (1P 1, 0.494, 0; 2P 0.8, 0, 1); the canvas has one
// colour per text, so this is mColor alone.
void DrawNamePlate(const Painter& p, const char* name, float x, Anchor anchor, Color color,
                   Color shadow)
{
    constexpr float kScale = 0.85f * 0.85f, kStretch = (0.9f * 0.9f) / kScale;
    constexpr float kY = 146.0f + 16.0f * 0.9f * 0.9f; // the line's middle
    p.Text(name, x + 2.0f, kY + 2.0f, kScale, anchor, shadow, FONT_BODY, kStretch);
    p.Text(name, x, kY, kScale, anchor, color, FONT_BODY, kStretch);
}

void DrawSpectatorPreview(const Painter& p, int level)
{
    if (level <= 0) return;
    if (level == 2) {
        p.Text("2 - 1", kScreenW * 0.5f, 20.0f, 0.68f, A_MC);
        p.Text("SPECTATING", kScreenW * 0.5f, 41.0f, 0.5f, A_MC, Color{0.47f, 0.86f, 1.0f, 1.0f});
    }
    DrawNamePlate(p, "Gamer", 218.0f, A_ML, Color{1.0f, 1.0f, 0.2f, 1.0f},
                  Color{0.2f, 0.094f, 0.0f, 0.749f});
    DrawNamePlate(p, "Animelover", 1060.0f, A_MR, Color{0.937f, 0.392f, 1.0f, 1.0f},
                  Color{0.094f, 0.0f, 0.2f, 0.749f});
}

} // namespace

int LobbyUi::SettingLevel(Level setting) const
{
    const int level = settings_.level ? settings_.level(setting) : 1;
    return level >= 0 && level <= 2 ? level : 1;
}

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
    if (const Level level = kSettingsRowLevel[settingsCursor_]; level != Level::Count) {
        // Shown as FULL, COMPACT, OFF: right goes down the list, as F3 does.
        const int next = (SettingLevel(level) + (input.left ? 1 : 2)) % 3;
        if (!settings_.setLevel || !settings_.setLevel(level, next)) {
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

    // On NETWORK STATS and SPECTATOR OVERLAY what the value shows in a match
    // is previewed where a match has it. The top of the screen is the
    // match's there, so the title is not drawn on those rows, whatever the
    // value: off and none preview as nothing at all.
    const Level previewed = kSettingsRowLevel[settingsCursor_];
    if (previewed == Level::Count)
        p.Text("SETTINGS", 76.0f, 70.0f, 0.62f, A_ML, kText, FONT_HEADING);
    else if (previewed == Level::NetStats) DrawNetStatsPreview(p, SettingLevel(previewed));
    else DrawSpectatorPreview(p, SettingLevel(previewed));

    const float panelX = 170.0f, panelY = 180.0f, panelW = 940.0f;
    const float panelH = 104.0f + SETTINGS_ROWS * 46.0f;
    p.Box(TEX_PANEL_DARK, panelX, panelY, panelW, panelH);

    const char* const labels[SETTINGS_ROWS] = {"RESOLUTION", "FULLSCREEN", "VSYNC",
                                               "STEAM ROUTING", "NETWORK STATS",
                                               "SPECTATOR OVERLAY"};
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
        else if (const Level level = kSettingsRowLevel[row]; level != Level::Count)
            value = kLevelNames[static_cast<int>(level)][SettingLevel(level)];
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
        "What is drawn over a match you are watching. F3 changes it while watching too.",
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
