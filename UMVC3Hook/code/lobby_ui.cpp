#include "lobby_ui.h"
#include "space_bg.h"
#include "lobby_music.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

// The plugin's build passes its version; the offline preview tool does not.
#ifndef UMVC3HOOK_VERSION
#define UMVC3HOOK_VERSION "0.0.0"
#endif

namespace lobby {

namespace {

// ---------------------------------------------------------------------------
// Palette and shared geometry (1280x720 UI space)
// ---------------------------------------------------------------------------
constexpr Color kText = kWhite;
// The option menu's selection animation, read from the shared sprite
// animation list (ui\cmn\anm\sprite_base, CAC): animation 8 drives the
// highlight bar and 9/10 the left/right arrows, all on one 76-frame loop, so
// they move together. From frame 1 the highlight dims (colour 0xBF -> 0x3F)
// while the arrows slide 5 px inward, over 50 frames; from frame 51 the
// highlight brightens again and the arrows slide back, over 25 frames.
// Easing as uUiAnmSprite__interpolateChannel (0x1402C7B90): mode 5 is
// 1-(1-u)^2, mode 2 is u^2.
constexpr uint32_t kSelectLoopFrames = 76;
constexpr float kSelectArrowTravel = 5.0f;
inline float EaseOutQuad(float u) { return 1.0f - (1.0f - u) * (1.0f - u); }

// Highlight brightness, 1 = brightest, 0 = dimmest.
inline float SelectionLevel(uint32_t frame)
{
    const uint32_t t = frame % kSelectLoopFrames;
    if (t < 1u) return 1.0f;
    if (t < 51u) return 1.0f - EaseOutQuad((t - 1u) / 50.0f);
    return EaseOutQuad((t - 51u) / 25.0f);
}

// How far each arrow sits in from its rest position, toward the value.
inline float SelectionArrowInset(uint32_t frame)
{
    const uint32_t t = frame % kSelectLoopFrames;
    if (t < 1u) return 0.0f;
    if (t < 51u) return kSelectArrowTravel * EaseOutQuad((t - 1u) / 50.0f);
    const float u = (t - 51u) / 25.0f;
    return kSelectArrowTravel * (1.0f - u * u);
}

// -1..1 form of SelectionLevel for the highlight alpha expressions.
inline float SelectionWave(uint32_t frame) { return SelectionLevel(frame) * 2.0f - 1.0f; }

// Highlight bar opacity, from `dimmest` up to the native peak colour 0xBF.
constexpr float kSelectPeak = 0xBF / 255.0f;
inline float SelectionPulse(uint32_t frame, float dimmest = 0.44f)
{
    return dimmest + (kSelectPeak - dimmest) * SelectionLevel(frame);
}

// Opacity of the top menu's white selection bar: 1.0 down to kMenuBarFadeMin
// and back, once every kMenuBarFadeFrames frames.
constexpr float kMenuBarFadeMin = 0.82f;
constexpr float kMenuBarFadeFrames = 96.0f;
inline float MenuBarFade(uint32_t frame)
{
    const float phase = std::cos(frame * (6.2831853f / kMenuBarFadeFrames));
    return kMenuBarFadeMin + (1.0f - kMenuBarFadeMin) * (0.5f + 0.5f * phase);
}

constexpr Color kDim = Rgb(0xBAC4D2);
constexpr Color kFaint = Rgb(0x7E8A9A);
constexpr Color kYellow = Rgb(0xFFD63A);
constexpr Color kCyan = Rgb(0x5CE4FF);
constexpr Color kRed = Rgb(0xFF5A4C);
constexpr Color kGreen = Rgb(0x86F25E);

constexpr float kScreenW = 1280.0f;
constexpr float kScreenH = 720.0f;
constexpr float kSafeLeft = 56.0f;
constexpr float kSafeRight = 1224.0f;
constexpr float kFooterY = 664.0f;

constexpr int kBrowseRows = 7;
constexpr int kChatRows = 5;
constexpr int kChatExpandFrames = 12; // the chat panel's slide open or shut
constexpr uint32_t kTransitionFrames = 9;
// The room's backdrop fades in over the title art, and out faster on leaving.
constexpr uint32_t kRoomBackdropFrames = 90;

// Button glyph cells inside the 4x4 glyph sheets (mai_hlp03 / mai_hlp04).
struct GlyphCell { uint8_t col, row; };
// BTN_QUICK_CHAT is RB on a pad and X on the keyboard.
enum Button : uint8_t {
    BTN_CONFIRM, BTN_CANCEL, BTN_X, BTN_Y, BTN_MENU, BTN_DPAD, BTN_QUICK_CHAT, BTN_COUNT
};
constexpr GlyphCell kPadGlyphs[BTN_COUNT] = {{1, 1}, {0, 1}, {2, 1}, {3, 1}, {1, 3}, {0, 0}, {2, 2}};
// The game's fixed keyboard menu mapping: Enter, Backspace, X, Y, Esc (Start).
constexpr GlyphCell kKeyGlyphs[BTN_COUNT] = {{1, 1}, {0, 1}, {2, 1}, {3, 1}, {1, 3}, {0, 0}, {2, 1}};

struct Prompt {
    Button button;
    const char* label;
};

const char* const kTopItems[] = {"SEARCH LOBBY", "CREATE LOBBY", "TEAM SETUP", "MAIN MENU"};
const char* const kTopHelp[] = {
    "Search for a lobby and join the players inside.",
    "Set your conditions and create a lobby.",
    "Choose the team you bring to a match and how you enter battle.",
    "Leave the lobby and return to the game's main menu.",
};
constexpr int kTopCount = 4;

const char* const kSlotMenuItems[] = {"JOIN QUEUE & SPECTATE", "JOIN QUEUE", "SPECTATE ONLY"};
constexpr int kSlotMenuCount = 3;
const char* const kRoomMenuItems[] = {"TEAM SETUP", "LOBBY MUSIC", "INVITE FRIENDS",
                                       "COPY LOBBY LINK", "LEAVE LOBBY"};
constexpr int kRoomMenuCount = 5;
constexpr int kMusicRows = 9; // tracks visible at once in the music picker
constexpr int kCharGridColumns = 10; // faces across in the team screen's character grid
constexpr int kColorGridColumns = 8; // faces across in the team screen's color grid
constexpr int kStageGridColumns = 5; // icons across in the team screen's stage grid
// The art in every select-screen face texture (128x128) is the same 120x84
// window; the rest is transparent.
constexpr Uv kFaceUv{4.0f / 128.0f, 42.0f / 128.0f, 124.0f / 128.0f, 126.0f / 128.0f};
constexpr float kFaceAspect = 84.0f / 120.0f;

const char* const kQuickChat[] = {
    "Type a message...", "Great fight!", "One more?", "Nice!", "Hello.",
    "Good game.", "Be right back.", "Thanks for the games!",
};
constexpr int kQuickChatCount = 8;

const char* const kVisibilityNames[] = {"PUBLIC", "FRIENDS ONLY", "PRIVATE"};
const char* RotationName(Rotation rotation)
{
    return rotation == Rotation::WinnerLeaves ? "WINNER LEAVES" : "WINNER STAYS";
}
const char* const kSlotStateNames[] = {"OPEN", "WAITING", "STANDBY", "", ""};
const char* const kTeamSlotNames[] = {"POINT", "SECOND", "ANCHOR"};
const char* const kAssistLetters[] = {"A", "B", "Y"};

// Colors exactly as the game's select screen labels them: six numbered
// colors, then "H" (the Herald color) and "ALT" (the alternate costume).
const char* ColorLabel(int color)
{
    static const char* const labels[] = {"1", "2", "3", "4", "5", "6", "H", "ALT"};
    return color >= 0 && color < 8 ? labels[color] : "1";
}

// Create-screen rows.
enum CreateRow {
    CREATE_NAME, CREATE_SIZE, CREATE_SLOTS, CREATE_VISIBILITY, CREATE_FORMAT, CREATE_ROTATION,
    CREATE_MUSIC, CREATE_GO, CREATE_ROWS
};
// Team-screen rows: preset selector, 3 x (character, assist, colour), entry, done.
constexpr int kTeamRowPreset = 0;
constexpr int kTeamRowFirstSlot = 1;
constexpr int kTeamRowIcon = 10; // the player's lobby icon
constexpr int kIconGridColumns = 14;
constexpr int kIconGridRows = 6;
constexpr int kTeamRowStage = 11; // the stage the player would like to fight on
constexpr int kTeamRowDone = 12;
constexpr int kTeamRows = 13;

// Pre-match prompts: how long each waits for an answer (frames at 60 fps).
// Picking a delay is the ready-up: with no pick, the seat is given up.
constexpr uint32_t kDelayTimeout = 20 * 60;
// The start/character-select choice has 30 seconds, and character select 40
// of its own: one clock per ready-up (teamFrames_), which backing out and
// going in again does not reset. entryFrames_ counts both.
constexpr uint32_t kEntryTimeout = 30 * 60;
constexpr uint32_t kTeamTimeout = 40 * 60;

float Clamp01(float value) { return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value; }
float EaseOut(float t) { t = 1.0f - Clamp01(t); return 1.0f - t * t * t; }

int PingLevel(int pingMs)
{
    if (pingMs < 0) return 0;
    if (pingMs < 100) return 5;
    if (pingMs < 180) return 4;
    if (pingMs < 260) return 3;
    if (pingMs < 360) return 2;
    return 1;
}

// Applies the screen transition (fade + slide) to everything drawn through it.
class Painter {
public:
    Painter(Canvas& canvas, float alpha, float dx) : canvas_(canvas), alpha_(alpha), dx_(dx) {}

    Painter Fixed() const { return Painter(canvas_, 1.0f, 0.0f); }
    Painter Faded(float alpha) const { return Painter(canvas_, alpha_ * alpha, dx_); }

    void Sprite(Tex tex, float x, float y, float w, float h, Uv uv, Anchor anchor,
                Color color = kWhite) const
    {
        canvas_.Sprite(tex, x + dx_, y, w, h, uv, anchor, WithAlpha(color, alpha_));
    }

    void Fill(float x, float y, float w, float h, Color color) const
    {
        canvas_.Sprite(TEX_WHITE, x + dx_, y, w, h, Uv{0.25f, 0.25f, 0.75f, 0.75f}, A_TL,
                       WithAlpha(color, alpha_));
    }

    // 9-slice frame described by the rectangle its visible art should cover
    // (top-left origin). The frame's transparent margin is added around it.
    void Box(Tex tex, float x, float y, float w, float h, Color color = kWhite) const
    {
        const FrameStyle& style = GetFrameStyle(tex);
        const float totalW = w + style.insetLeft + style.insetRight;
        const float totalH = h + style.insetTop + style.insetBottom;
        float innerW = totalW - 2.0f * FrameBorderX(tex, style.uv);
        float innerH = totalH - 2.0f * FrameBorderY(tex, style.uv);
        if (innerW < 0.0f) innerW = 0.0f;
        if (innerH < 0.0f) innerH = 0.0f;
        canvas_.Frame(tex, x - style.insetLeft + dx_, y - style.insetTop, innerW, innerH,
                      style.uv, A_TL, WithAlpha(color, alpha_));
    }

    void Text(const char* text, float x, float y, float scale, Anchor anchor,
              Color color = kText, Font font = FONT_BODY, float stretch = 1.0f,
              Blend blend = BLEND_NORMAL) const
    {
        if (!text || !text[0]) return;
        canvas_.Text(text, x + dx_, y, scale, anchor, WithAlpha(color, alpha_), font, stretch,
                     blend);
    }

    void TextFit(const char* text, float x, float y, float scale, float maxWidth,
                 Anchor anchor, Color color = kText, Font font = FONT_BODY) const
    {
        char fitted[192];
        FitText(fitted, sizeof(fitted), text, maxWidth, scale, font);
        Text(fitted, x, y, scale, anchor, color, font);
    }

    // Like TextFit, but the whole text is always shown: it is drawn smaller
    // until it fits maxWidth, never cut. Returns the scale used.
    float TextShrink(const char* text, float x, float y, float scale, float maxWidth,
                     Anchor anchor, Color color = kText, Font font = FONT_BODY) const
    {
        const float width = TextWidth(text, font) * scale;
        if (width > maxWidth && width > 0.0f) scale *= maxWidth / width;
        Text(text, x, y, scale, anchor, color, font);
        return scale;
    }

    // Small label with a dark drop for legibility over the busy backdrop.
    void Label(const char* text, float x, float y, float scale, Anchor anchor,
               Color color = kText) const
    {
        Text(text, x + 2.0f, y + 2.0f, scale, anchor, Color{0.0f, 0.0f, 0.0f, 0.75f});
        Text(text, x, y, scale, anchor, color);
    }

private:
    Canvas& canvas_;
    float alpha_;
    float dx_;
};

// A button glyph of `size` px, its right edge at `right`, centred on `y`.
void DrawGlyph(const Painter& p, Button button, bool keyboard, float right, float y, float size)
{
    const Tex sheet = keyboard ? TEX_KEY_GLYPHS : TEX_PAD_GLYPHS;
    const GlyphCell cell = (keyboard ? kKeyGlyphs : kPadGlyphs)[button];
    p.Sprite(sheet, right, y, size, size,
             Uv{cell.col * 0.25f, cell.row * 0.25f, (cell.col + 1) * 0.25f, (cell.row + 1) * 0.25f},
             A_MR);
}

// Distance from a value's centre to each arrow. The option menu uses 130; the
// lobby's values run longer (character and assist names), so a little more,
// and the value text is fitted to stay clear of the arrows.
constexpr float kValueArrowSpan = 150.0f;
constexpr float kValueTextWidth = 2.0f * (kValueArrowSpan - 24.0f);
// Team screen assist names: the room between the row label and the panel edge.
constexpr float kAssistTextWidth = 380.0f;

// Left/right arrows around a selectable value: the option menu's own arrow
// art (drawn pointing left; the right one is the mirror image), 32 px as
// there, sliding inward and back in step with the selection highlight.
void DrawValueArrows(const Painter& p, uint32_t frame, float centerX, float y, float halfSpan)
{
    const float inset = SelectionArrowInset(frame);
    p.Sprite(TEX_OPT_ARROW, centerX - halfSpan + inset, y, 32.0f, 32.0f, kFullUv, A_MC);
    p.Sprite(TEX_OPT_ARROW, centerX + halfSpan - inset, y, 32.0f, 32.0f,
             Uv{1.0f, 0.0f, 0.0f, 1.0f}, A_MC);
}

void DrawPing(const Painter& p, float x, float y, int pingMs, float scale = 1.0f,
              float width = 94.0f)
{
    // Six 32px rows; the bars occupy columns 35..222 of the 256px sheet.
    p.Sprite(TEX_PING, x, y, width * scale, 16.0f * scale,
             Uv{35.0f / 256.0f, (PingLevel(pingMs) * 32 + 4) / 256.0f, 222.0f / 256.0f,
                (PingLevel(pingMs) * 32 + 28) / 256.0f},
             A_ML);
}

// Bars narrowed to barsWidth (their height still follows scale), then the
// ping in ms, right-aligned in numberWidth.
void DrawPingWithNumber(const Painter& p, float x, float y, int pingMs, float scale,
                        float barsWidth, float numberWidth, float textScale)
{
    DrawPing(p, x, y, pingMs, scale, barsWidth / scale);
    if (pingMs < 0) return;
    char text[8];
    snprintf(text, sizeof(text), "%d", pingMs > 999 ? 999 : pingMs);
    p.TextShrink(text, x + barsWidth + 4.0f + numberWidth, y, textScale, numberWidth, A_MR, kText);
}

// A team member's select-screen face in a box `w` wide, anchored A_TC or
// A_MC. A random member is the stage select's RANDOM icon, centred in the
// same box.
void DrawFace(const Painter& p, int32_t id, int color, float x, float y, float w, Anchor anchor,
              Color tint = kWhite)
{
    if (id != kCharRandom) {
        p.Sprite(CharacterFace(id, color), x, y, w, w * kFaceAspect, kFaceUv, anchor, tint);
        return;
    }
    const float cy = anchor == A_TC ? y + w * kFaceAspect * 0.5f : y;
    p.Sprite(TEX_STAGE_FIRST, x, cy, w, w * kStageIconAspect, kStageIconUv, A_MC, tint);
}

const char* CharacterLabel(const RosterSource& roster, int32_t id)
{
    if (id == kCharRandom) return "RANDOM";
    const char* name = roster.characterName ? roster.characterName(id) : "";
    return name ? name : "";
}

void TeamSummary(const RosterSource& roster, const Team& team, char* out, size_t size)
{
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < 3; ++i) {
        const char* name = CharacterLabel(roster, team.chars[i]);
        if (!name[0]) name = "?";
        const int written = snprintf(out + used, size - used, "%s%s", i ? " / " : "", name);
        if (written < 0 || static_cast<size_t>(written) >= size - used) break;
        used += static_cast<size_t>(written);
    }
}

// Removes the last UTF-8 sequence from a buffer.
void PopUtf8(char* text)
{
    size_t length = strlen(text);
    if (!length) return;
    --length;
    while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
    text[length] = '\0';
}

} // namespace

// ---------------------------------------------------------------------------
// Construction and service synchronisation
// ---------------------------------------------------------------------------
LobbyUi::LobbyUi(RoomService& service, const RosterSource& roster, const UiPrefs& prefs)
    : service_(service), roster_(roster), prefs_(prefs)
{
    if (prefs_.create.maxMembers < 2 || prefs_.create.maxMembers > kMaxMembers)
        prefs_.create.maxMembers = kMaxMembers;
    if (prefs_.create.slotCount < 1 || prefs_.create.slotCount > kMaxSlots)
        prefs_.create.slotCount = 2;
    if (static_cast<int>(prefs_.create.visibility) > 2)
        prefs_.create.visibility = Visibility::Public;
    if (FindMusicTrack(prefs_.create.music) < 0) prefs_.create.music = kMusicDefault;
    bool knownFormat = false;
    for (int choice : kFirstToChoices) knownFormat |= choice == prefs_.create.firstTo;
    if (!knownFormat) prefs_.create.firstTo = 3;
    if (prefs_.create.rotation != Rotation::WinnerLeaves) prefs_.create.rotation = Rotation::WinnerStays;
    // The team in use always belongs to a slot. (A reserve unit is checked
    // once the game's profile can be read: RefreshPreset.)
    if (prefs_.slot < 0 || prefs_.slot >= kTeamSlots ||
        (prefs_.slot < kTeamPresets && !prefs_.presets[prefs_.slot].used)) {
        prefs_.slot = 0;
        for (int i = kTeamPresets - 1; i >= 0; --i)
            if (prefs_.presets[i].used) prefs_.slot = i;
        if (prefs_.presets[prefs_.slot].used) prefs_.team = prefs_.presets[prefs_.slot].team;
        else StoreTeam();
    }
}

void LobbyUi::PlayEntrance(uint32_t delayFrames)
{
    screenFrame_ = 0;
    entranceDelay_ = delayFrames;
    if (screen_ == Screen::Top) topCursor_ = 0; // always open on SEARCH LOBBY
}

void LobbyUi::GoTo(Screen screen)
{
    screen_ = screen;
    screenFrame_ = 0;
    roomMenuOpen_ = false;
    slotMenuOpen_ = false;
    quickChatOpen_ = false;
    musicOpen_ = false;
    charPickerOpen_ = false;
    colorPickerOpen_ = false;
    iconPickerOpen_ = false;
    presetListOpen_ = false;
    stagePickerOpen_ = false;
    memberFocus_ = false;
    memberMenuOpen_ = false;
    chatFocus_ = false;
    chatExpanded_ = false;
    confirm_ = Confirm::None;
    if (textTarget_ != TextTarget::None) EndText(false);
}

uint32_t LobbyUi::TakeSfx()
{
    const uint32_t sfx = sfx_;
    sfx_ = 0;
    return sfx;
}

bool LobbyUi::TakeMainMenuRequested()
{
    const bool requested = mainMenuRequested_;
    mainMenuRequested_ = false;
    return requested;
}

bool LobbyUi::TakePrefsDirty()
{
    const bool dirty = prefsDirty_;
    prefsDirty_ = false;
    return dirty;
}

int32_t LobbyUi::PreviewMusic() const
{
    return musicOpen_ && screen_ == Screen::Create ? kMusicTracks[musicCursor_].id : -1;
}

void LobbyUi::ShowNotice(const char* text)
{
    snprintf(notice_, sizeof(notice_), "%s", text ? text : "");
}

void LobbyUi::SetBusy(const char* text)
{
    snprintf(busy_, sizeof(busy_), "%s", text ? text : "");
}

void LobbyUi::OfferUpdate(const char* installed, const char* available)
{
    snprintf(updateOffer_, sizeof(updateOffer_),
             "A Timestone update is available.\nInstalled: %s     Latest: %s\n"
             "Download and install it now?",
             installed ? installed : "?", available ? available : "?");
    confirm_ = Confirm::Update;
    confirmCursor_ = 0; // default to "Yes"
}

bool LobbyUi::TakeUpdateAccepted()
{
    const bool accepted = updateAccepted_;
    updateAccepted_ = false;
    return accepted;
}

void LobbyUi::SetUpdateProgress(const UpdateView* view)
{
    updateActive_ = view != nullptr;
    if (view) update_ = *view;
}

bool LobbyUi::TakeUpdateCancelled()
{
    const bool cancelled = updateCancelled_;
    updateCancelled_ = false;
    return cancelled;
}

void LobbyUi::ReturnToRoom()
{
    if (!service_.Room().inRoom) return;
    screen_ = Screen::Room;
    screenFrame_ = kTransitionFrames;
    roomBackdrop_ = kRoomBackdropFrames;
    roomMenuOpen_ = false;
    slotMenuOpen_ = false;
    quickChatOpen_ = false;
    musicOpen_ = false;
    charPickerOpen_ = false;
    colorPickerOpen_ = false;
    iconPickerOpen_ = false;
    presetListOpen_ = false;
    stagePickerOpen_ = false;
    memberFocus_ = false;
    memberMenuOpen_ = false;
    chatFocus_ = false;
    chatExpanded_ = false;
    teamForMatch_ = false;
    confirm_ = Confirm::None;
    wasInRoom_ = true;
    // During a match the room is shown the team being played, with its random
    // members drawn (lobby_flow.cpp, BeginLaunch); back here it is the
    // player's own choice again.
    service_.SetTeam(prefs_.team);
}

const Member* LobbyUi::LocalMember() const
{
    const RoomState& room = service_.Room();
    for (int i = 0; i < room.memberCount; ++i)
        if (room.members[i].isLocal) return &room.members[i];
    return nullptr;
}

const Member* LobbyUi::SeatOccupant(int slot, int side) const
{
    const RoomState& room = service_.Room();
    for (int i = 0; i < room.memberCount; ++i) {
        const Member& member = room.members[i];
        if (member.slot == slot && member.side == side) return &member;
    }
    return nullptr;
}

void LobbyUi::SyncWithService()
{
    char error[256];
    if (service_.TakeError(error, sizeof(error))) ShowNotice(error);

    const bool inRoom = service_.Room().inRoom;
    if (inRoom && !wasInRoom_) {
        // Create/Join completed: publish our team and show the room.
        service_.SetTeam(prefs_.team);
        service_.SetIcon(prefs_.icon);
        service_.SetStage(prefs_.stage);
        seatSlot_ = 0;
        seatSide_ = 0;
        leftByChoice_ = false;
        GoTo(Screen::Room);
    } else if (!inRoom && wasInRoom_) {
        if (screen_ == Screen::Room || (screen_ == Screen::Team && teamReturn_ == Screen::Room)) {
            // Back to the lobby list, freshly searched, ready to pick another.
            // An accepted invite is already moving us on: leave that alone.
            if (service_.Busy() == RoomBusy::None) {
                browseCursor_ = 0;
                browseScroll_ = 0;
                service_.Search();
                GoTo(Screen::Browse);
            } else {
                GoTo(Screen::Top);
            }
            // An error from the service (e.g. removed by the host) says more.
            // Nor is it closed when an accepted invite is moving us on.
            if (!leftByChoice_ && !notice_[0] && service_.Busy() != RoomBusy::Joining)
                ShowNotice("Lobby has been closed.");
        }
        leftByChoice_ = false;
    }
    wasInRoom_ = inRoom;

    if (inRoom) {
        const int slots = service_.Room().settings.slotCount;
        if (seatSlot_ >= slots) seatSlot_ = slots > 0 ? slots - 1 : 0;
    }
}

bool LobbyUi::MoveCursor(int& cursor, int count, const Input& input, bool wrap)
{
    if (count <= 0) return false;
    int next = cursor;
    if (input.up) --next;
    if (input.down) ++next;
    if (next < 0) next = wrap ? count - 1 : 0;
    if (next >= count) next = wrap ? 0 : count - 1;
    if (next == cursor) return false;
    cursor = next;
    Play(SFX_MOVE);
    return true;
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
void LobbyUi::Update(const Input& input)
{
    ++frame_;
    // The team screen keeps the backdrop of the screen it was opened from.
    const bool roomBackdrop =
        screen_ == Screen::Room || (screen_ == Screen::Team && teamReturn_ == Screen::Room);
    if (roomBackdrop) {
        if (roomBackdrop_ < kRoomBackdropFrames) ++roomBackdrop_;
    } else {
        roomBackdrop_ = roomBackdrop_ > 3 ? roomBackdrop_ - 3 : 0;
    }
    if (entranceDelay_ > 0) {
        --entranceDelay_;
        screenFrame_ = 0;
    } else if (screenFrame_ < 100000) {
        ++screenFrame_;
    }
    usingKeyboard_ = input.usingKeyboard;

    SyncWithService();
    if (screen_ == Screen::Room && (chatFocus_ || chatExpanded_))
        chatHoverFrames_ = chatHoverFrames_ < 100000 ? chatHoverFrames_ + 1 : chatHoverFrames_;
    else
        chatHoverFrames_ = 0;
    // The chat panel slides open and shut rather than jumping.
    if (screen_ != Screen::Room) chatExpandFrames_ = 0;
    else if (chatExpanded_ && chatExpandFrames_ < kChatExpandFrames) ++chatExpandFrames_;
    else if (!chatExpanded_ && chatExpandFrames_ > 0) --chatExpandFrames_;

    // A match becoming ready closes whatever modal is up so the delay pick
    // (the ready-up) is seen at once; UpdateHandshake then clears the room's
    // own menus. A half-typed message is dropped. Busy and update progress
    // stay: they are work in progress, not something the player opened.
    if (screen_ == Screen::Room && handshake_ != Handshake::Delay &&
        HandshakeStep() == Handshake::Delay) {
        notice_[0] = '\0';
        if (confirm_ != Confirm::Update) confirm_ = Confirm::None;
        if (textTarget_ != TextTarget::None) EndText(false);
    }

    // Modal layers take input in priority order.
    if (updateActive_) {
        if (input.cancel && update_.cancellable) {
            updateCancelled_ = true;
            Play(SFX_CANCEL);
        }
        return;
    }
    if (busy_[0]) return;
    if (notice_[0]) {
        if (input.confirm || input.cancel) {
            notice_[0] = '\0';
            Play(SFX_CONFIRM);
        }
        return;
    }
    if (teamForMatch_ &&
        (textTarget_ == TextTarget::PresetName || confirm_ == Confirm::DeletePreset)) {
        // A team being named or deleted while the opponent waits: the clock
        // still runs, and at its end the match starts with the team as it is.
        ++handshakeFrames_;
        ++entryFrames_;
        if (++teamFrames_ >= kTeamTimeout) {
            if (textTarget_ != TextTarget::None) EndText(false);
            confirm_ = Confirm::None;
        }
    }
    if (textTarget_ != TextTarget::None) {
        UpdateText(input);
        return;
    }
    if (confirm_ != Confirm::None) {
        if (input.left || input.right || input.up || input.down) {
            confirmCursor_ ^= 1;
            Play(SFX_MOVE);
        }
        if (input.cancel || (input.confirm && confirmCursor_ == 1)) {
            confirm_ = Confirm::None;
            Play(SFX_CANCEL);
        } else if (input.confirm) {
            const Confirm action = confirm_;
            confirm_ = Confirm::None;
            Play(SFX_CONFIRM);
            if (action == Confirm::LeaveRoom) {
                leftByChoice_ = true;
                service_.Leave();
            } else if (action == Confirm::KickMember) {
                service_.Kick(memberMenuId_);
            } else if (action == Confirm::Update) {
                updateAccepted_ = true;
            } else if (action == Confirm::DeletePreset) {
                DeletePreset(presetListCursor_);
            }
        }
        return;
    }

    switch (screen_) {
    case Screen::Top: UpdateTop(input); break;
    case Screen::Browse: UpdateBrowse(input); break;
    case Screen::Create: UpdateCreate(input); break;
    case Screen::Room: UpdateRoom(input); break;
    case Screen::Team: UpdateTeam(input); break;
    }
}

void LobbyUi::UpdateTop(const Input& input)
{
    MoveCursor(topCursor_, kTopCount, input);
    if (input.cancel) {
        mainMenuRequested_ = true;
        Play(SFX_MENU_EXIT);
        return;
    }
    if (!input.confirm) return;
    Play(SFX_CONFIRM);
    switch (topCursor_) {
    case 0:
        browseCursor_ = 0;
        browseScroll_ = 0;
        service_.Search();
        GoTo(Screen::Browse);
        break;
    case 1:
        createCursor_ = 0;
        if (!prefs_.create.name[0])
            snprintf(prefs_.create.name, sizeof(prefs_.create.name), "%.40s's Lobby",
                     service_.LocalName());
        GoTo(Screen::Create);
        break;
    case 2:
        teamReturn_ = Screen::Top;
        teamCursor_ = 0;
        GoTo(Screen::Team);
        break;
    default:
        mainMenuRequested_ = true;
        Play(SFX_MENU_EXIT);
        break;
    }
}

void LobbyUi::UpdateBrowse(const Input& input)
{
    const int count = service_.ResultCount();
    if (browseCursor_ >= count) browseCursor_ = count > 0 ? count - 1 : 0;
    MoveCursor(browseCursor_, count, input, false);
    if (browseCursor_ < browseScroll_) browseScroll_ = browseCursor_;
    if (browseCursor_ >= browseScroll_ + kBrowseRows) browseScroll_ = browseCursor_ - kBrowseRows + 1;
    if (browseScroll_ < 0) browseScroll_ = 0;

    if (input.cancel) {
        Play(SFX_CANCEL);
        GoTo(Screen::Top);
        return;
    }
    if (service_.Busy() != RoomBusy::None) return;
    if (input.actionX) {
        Play(SFX_CONFIRM);
        service_.Search();
        return;
    }
    if (input.actionY) {
        Play(SFX_CONFIRM);
        createCursor_ = 0;
        if (!prefs_.create.name[0])
            snprintf(prefs_.create.name, sizeof(prefs_.create.name), "%.40s's Lobby",
                     service_.LocalName());
        GoTo(Screen::Create);
        return;
    }
    if (input.confirm && count > 0) {
        const SearchResult& result = service_.Result(browseCursor_);
        if (!result.compatible) {
            Play(SFX_CANCEL);
            ShowNotice(result.versionNotice[0] ? result.versionNotice
                           : "This lobby is running a different Timestone version.");
        } else {
            // A lobby that reads as full is tried all the same: the count in
            // the list can be stale, and Steam has the final say.
            Play(SFX_CONFIRM);
            service_.Join(result.id);
        }
    }
}

void LobbyUi::UpdateCreate(const Input& input)
{
    if (musicOpen_) {
        UpdateMusicMenu(input);
        return;
    }
    MoveCursor(createCursor_, CREATE_ROWS, input);
    if (input.cancel) {
        Play(SFX_CANCEL);
        GoTo(Screen::Top);
        return;
    }
    if (service_.Busy() != RoomBusy::None) return;

    RoomSettings& draft = prefs_.create;
    const int delta = (input.right ? 1 : 0) - (input.left ? 1 : 0);
    if (delta != 0) {
        bool changed = true;
        switch (createCursor_) {
        case CREATE_SIZE: {
            int size = draft.maxMembers + delta;
            if (size < 2) size = kMaxMembers;
            if (size > kMaxMembers) size = 2;
            draft.maxMembers = size;
            // A slot seats two players; never offer more slots than can be filled.
            if (draft.slotCount > size / 2) draft.slotCount = size / 2;
            break;
        }
        case CREATE_SLOTS: {
            const int maxSlots = draft.maxMembers / 2 < kMaxSlots ? draft.maxMembers / 2 : kMaxSlots;
            int slots = draft.slotCount + delta;
            if (slots < 1) slots = maxSlots;
            if (slots > maxSlots) slots = 1;
            draft.slotCount = slots;
            break;
        }
        case CREATE_VISIBILITY:
            draft.visibility = static_cast<Visibility>((static_cast<int>(draft.visibility) + delta + 3) % 3);
            break;
        case CREATE_ROTATION:
            // No set ever ends in an endless room, so there is nothing to set.
            if (draft.firstTo == kFirstToEndless) {
                changed = false;
                break;
            }
            draft.rotation = draft.rotation == Rotation::WinnerStays ? Rotation::WinnerLeaves
                                                                     : Rotation::WinnerStays;
            break;
        case CREATE_FORMAT: {
            int index = 0;
            for (int i = 0; i < kFirstToChoiceCount; ++i)
                if (kFirstToChoices[i] == draft.firstTo) index = i;
            index = (index + delta + kFirstToChoiceCount) % kFirstToChoiceCount;
            draft.firstTo = kFirstToChoices[index];
            break;
        }
        default:
            changed = false;
            break;
        }
        if (changed) {
            prefsDirty_ = true;
            Play(SFX_MOVE);
        }
    }

    if (!input.confirm) return;
    if (createCursor_ == CREATE_NAME) {
        Play(SFX_CONFIRM);
        BeginText(TextTarget::RoomName, draft.name);
    } else if (createCursor_ == CREATE_GO) {
        if (!service_.Available()) {
            Play(SFX_CANCEL);
            ShowNotice(service_.StatusText());
            return;
        }
        Play(SFX_CONFIRM);
        service_.Create(draft);
    } else if (createCursor_ == CREATE_MUSIC) {
        // Too many tracks for a left/right value: confirm opens the full list.
        Play(SFX_CONFIRM);
        const int current = FindMusicTrack(draft.music);
        musicCursor_ = current < 0 ? 0 : current;
        musicOpen_ = true;
    } else {
        // Confirm on a value row advances it, so the screen is usable one-handed.
        Input step = {};
        step.right = true;
        step.usingKeyboard = input.usingKeyboard;
        UpdateCreate(step);
    }
}

void LobbyUi::UpdateRoom(const Input& input)
{
    // Chat shortcuts work over the room's own menus: RB (X on the keyboard)
    // toggles the quick chat, and Y on the keyboard starts typing at once. A
    // match waiting on the local player keeps the input, bar closing the list.
    const bool typeKey = input.actionY && input.usingKeyboard;
    if ((input.quickChat || typeKey) &&
        (quickChatOpen_ || HandshakeStep() == Handshake::None)) {
        roomMenuOpen_ = false;
        slotMenuOpen_ = false;
        musicOpen_ = false;
        memberMenuOpen_ = false;
        if (typeKey) {
            quickChatOpen_ = false;
            Play(SFX_CONFIRM);
            BeginText(TextTarget::Chat, "");
            return;
        }
        quickChatOpen_ = !quickChatOpen_;
        Play(quickChatOpen_ ? SFX_CONFIRM : SFX_CANCEL);
        return;
    }
    if (roomMenuOpen_) {
        UpdateRoomMenu(input);
        return;
    }
    if (slotMenuOpen_) {
        UpdateSlotMenu(input);
        return;
    }
    if (quickChatOpen_) {
        UpdateQuickChat(input);
        return;
    }
    if (musicOpen_) {
        UpdateMusicMenu(input);
        return;
    }
    if (memberMenuOpen_) {
        UpdateMemberMenu(input);
        return;
    }

    // A match waiting on the local player comes before everything else here.
    if (UpdateHandshake(input)) return;

    const RoomState& room = service_.Room();
    const Member* local = LocalMember();
    const int seatCount = room.settings.slotCount * 2;
    if (seatCount <= 0) return;

    if (input.menu) {
        roomMenuOpen_ = true;
        roomMenuCursor_ = 0;
        Play(SFX_CONFIRM);
        return;
    }
    if (input.actionY) {
        quickChatOpen_ = true;
        Play(SFX_CONFIRM);
        return;
    }
    if (chatFocus_ || chatExpanded_) {
        UpdateChatFocus(input);
        return;
    }
    if (memberFocus_) {
        UpdateMemberList(input);
        return;
    }
    // The player list sits to the left of the seats: left from the leftmost
    // column of seats moves into it.
    if (input.left && (room.settings.slotCount <= 2 || seatSlot_ % 2 == 0) && room.memberCount > 0) {
        memberFocus_ = true;
        if (memberCursor_ >= room.memberCount) memberCursor_ = room.memberCount - 1;
        Play(SFX_MOVE);
        return;
    }

    const int index = seatSlot_ * 2 + seatSide_;
    int next = index;
    const int slots = room.settings.slotCount;
    if (slots > 2) {
        // Three or four setups sit in a two-column grid, each with its seats
        // stacked: up/down walks the seats of one column, left/right crosses
        // to the same seat of the setup alongside.
        const int column = seatSlot_ % 2;
        const int inColumn = (slots - column + 1) / 2; // setups in this column
        if (input.up || input.down) {
            int place = (seatSlot_ / 2) * 2 + seatSide_ + (input.down ? 1 : -1);
            place = (place + inColumn * 2) % (inColumn * 2);
            next = ((place / 2) * 2 + column) * 2 + place % 2;
        }
        if (input.right || (input.left && column == 1)) {
            const int across = seatSlot_ + (column == 0 ? 1 : -1);
            if (across < slots) next = across * 2 + seatSide_;
        }
    } else {
        // One column: the seats form a list.
        if (input.up) --next;
        if (input.down) ++next;
        if (input.left) next -= 2;
        if (input.right) next += 2;
        next = ((next % seatCount) + seatCount) % seatCount;
    }
    if (next != index) {
        seatSlot_ = next / 2;
        seatSide_ = next % 2;
        Play(SFX_MOVE);
    }

    const bool seated = local && local->slot >= 0 && local->side < 2;
    const bool onOwnSeat = seated && local->slot == seatSlot_ && local->side == seatSide_;

    const bool queued = local && local->queueSlot >= 0;
    if (input.actionX && (seated || queued)) {
        if (seated) service_.Stand();
        else service_.LeaveQueue();
        Play(SFX_CANCEL);
        return;
    }
    if (input.cancel) {
        Play(SFX_CANCEL);
        if (seated) {
            service_.Stand();
        } else if (queued) {
            service_.LeaveQueue();
        } else {
            confirm_ = Confirm::LeaveRoom;
            confirmCursor_ = 0; // default to "Yes"
        }
        return;
    }
    if (!input.confirm) return;

    // Nothing to do on one's own seat: readying up happens in the prompt that
    // appears once the opposite seat is taken.
    if (onOwnSeat) return;
    if (queued && local->queueSlot == seatSlot_) {
        // Already in line here: watch its matches meanwhile, or stop.
        slotMenuOpen_ = true;
        slotMenuQueued_ = true;
        slotMenuCursor_ = 0;
        Play(SFX_CONFIRM);
        return;
    }
    // A seat is taken directly only when it is free and nobody is waiting
    // for this slot; otherwise confirm lines up for it.
    const bool full = SeatOccupant(seatSlot_, 0) && SeatOccupant(seatSlot_, 1);
    const bool open = !full && SlotQueueLength(seatSlot_) == 0 &&
                      !SeatOccupant(seatSlot_, seatSide_) &&
                      room.slots[seatSlot_] < SlotState::Starting;
    if (seated && local->slot == seatSlot_) {
        // The other seat of one's own setup: move across when it is free,
        // whoever is in line (they are waiting for the seat left behind).
        if (!SeatOccupant(seatSlot_, seatSide_) && room.slots[seatSlot_] < SlotState::Starting) {
            service_.Sit(seatSlot_, seatSide_);
            Play(SFX_CONFIRM);
        } else {
            Play(SFX_CANCEL);
        }
        return;
    }
    if (seated && open) {
        // One place at a time: give the seat up first.
        Play(SFX_CANCEL);
        return;
    }
    if (open) {
        service_.Sit(seatSlot_, seatSide_);
    } else {
        // Line up, watch, or both. From a seat at another setup too: picking
        // one gives that seat up.
        slotMenuOpen_ = true;
        slotMenuQueued_ = false;
        slotMenuCursor_ = 0;
    }
    Play(SFX_CONFIRM);
}

int LobbyUi::SlotMenuItems(const char* items[3]) const
{
    if (slotMenuQueued_) {
        // Leaving the line is the cancel button's job, not this menu's.
        items[0] = service_.WatchIntent() == seatSlot_ ? "STOP SPECTATING" : "SPECTATE";
        return 1;
    }
    for (int i = 0; i < kSlotMenuCount; ++i) items[i] = kSlotMenuItems[i];
    return kSlotMenuCount;
}

void LobbyUi::UpdateSlotMenu(const Input& input)
{
    const char* items[3];
    MoveCursor(slotMenuCursor_, SlotMenuItems(items), input);
    if (input.cancel || input.menu) {
        slotMenuOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (!input.confirm) return;
    slotMenuOpen_ = false;
    Play(SFX_CONFIRM);
    if (slotMenuQueued_) {
        service_.SetWatchIntent(service_.WatchIntent() == seatSlot_ ? -1 : seatSlot_);
        return;
    }
    // Chosen from a seat: one place at a time.
    const Member* local = LocalMember();
    if (local && local->slot >= 0) service_.Stand();
    switch (slotMenuCursor_) {
    case 0: // queue and watch
        service_.JoinQueue(seatSlot_); // moves over from any other queue
        service_.SetWatchIntent(seatSlot_);
        break;
    case 1: // queue only
        service_.JoinQueue(seatSlot_);
        service_.SetWatchIntent(-1);
        break;
    default: // watch only
        service_.SetWatchIntent(seatSlot_);
        break;
    }
}

int LobbyUi::SlotWatcherCount(int slot) const
{
    const RoomState& room = service_.Room();
    int count = 0;
    for (int i = 0; i < room.memberCount; ++i) count += room.members[i].watchSlot == slot;
    return count;
}

int LobbyUi::SlotQueueLength(int slot) const
{
    const RoomState& room = service_.Room();
    int length = 0;
    for (int i = 0; i < room.memberCount; ++i) length += room.members[i].queueSlot == slot;
    return length;
}


// Which pre-match step the local player is on, from the published state of
// both seats. None unless both seats of their slot are taken and idle.
LobbyUi::Handshake LobbyUi::HandshakeStep() const
{
    const Member* local = LocalMember();
    if (!local || local->slot < 0 || local->side > 1 || local->status == MemberStatus::Fighting)
        return Handshake::None;
    const Member* other = SeatOccupant(local->slot, 1 - local->side);
    if (!other || other->status == MemberStatus::Fighting) return Handshake::None;
    // The delay pick is the ready-up: confirming one also accepts the match.
    if (!local->accepted || local->delayPick == kDelayPickNone) return Handshake::Delay;
    if (!other->accepted || other->delayPick == kDelayPickNone) return Handshake::WaitDelay;
    if (!local->ready) return Handshake::Entry;
    if (!other->ready) return Handshake::WaitStart;
    return Handshake::None; // both ready: the match is being started
}

bool LobbyUi::UpdateHandshake(const Input& input)
{
    const Handshake step = HandshakeStep();
    if (step != handshake_) {
        if (step == Handshake::Delay) {
            // A match is waiting: whatever was open gives way, and the player
            // is called to the window.
            roomMenuOpen_ = false;
            slotMenuOpen_ = false;
            quickChatOpen_ = false;
            musicOpen_ = false;
            memberMenuOpen_ = false;
            memberFocus_ = false;
            chatFocus_ = false;
            chatExpanded_ = false;
            Play(SFX_MATCH_FOUND);
        }
        handshake_ = step;
        handshakeFrames_ = 0;
        handshakeCursor_ = 0;
        if (step == Handshake::Delay) delayCursor_ = kDelayPickAuto;
        // Both players reach the start/character-select choice together.
        if (step == Handshake::Entry) entryFrames_ = teamFrames_ = 0;
    }
    // The other player watches the highlight move before it is locked in.
    if (step == Handshake::Delay) service_.HoverDelay(delayCursor_);
    if (step == Handshake::None) return false;
    ++handshakeFrames_;
    if (step == Handshake::Entry || step == Handshake::WaitStart) ++entryFrames_;

    if (input.cancel) {
        // Backing out of any step gives the seat up.
        service_.Stand();
        Play(SFX_CANCEL);
        return true;
    }
    switch (step) {
    case Handshake::Delay: {
        if (handshakeFrames_ >= kDelayTimeout) {
            service_.Stand();
            ShowNotice("You did not ready up in time\nand were removed from the seat.");
            break;
        }
        const int delta = (input.right ? 1 : 0) - (input.left ? 1 : 0);
        if (delta != 0) {
            // Auto is not a stop of its own: it is the label on the delay the
            // ping calls for, and the cursor steps up and down from there.
            const Member* local = LocalMember();
            const Member* other = local ? SeatOccupant(local->slot, 1 - local->side) : nullptr;
            int autoDelay = other ? service_.AutoDelay(other->pingMs) : -1;
            if (autoDelay < 0) autoDelay = 3; // ping not known yet
            const int maxDelay = kMaxDelayPick - 1;
            const int from = delayCursor_ > kDelayPickAuto ? delayCursor_ - 1 : autoDelay;
            int to = from + delta;
            if (to < 0) to = 0;
            if (to > maxDelay) to = maxDelay;
            if (to != from) {
                delayCursor_ = to == autoDelay ? kDelayPickAuto : to + 1;
                Play(SFX_MOVE);
            }
        }
        if (input.confirm) {
            service_.AcceptMatch();
            service_.PickDelay(delayCursor_);
            Play(SFX_CONFIRM);
        }
        break;
    }
    case Handshake::Entry:
        if (input.up || input.down) {
            handshakeCursor_ ^= 1;
            Play(SFX_MOVE);
        }
        // START carries the team it starts with: left and right change it.
        if (handshakeCursor_ == 0 && input.left != input.right) StepSlot(input.right ? 1 : -1);
        // Time spent in character select does not count against the choice.
        if (entryFrames_ - teamFrames_ >= kEntryTimeout || (input.confirm && handshakeCursor_ == 0)) {
            service_.SetReady(true);
            Play(SFX_CONFIRM);
        } else if (input.confirm) {
            Play(SFX_CONFIRM);
            teamForMatch_ = true;
            teamReturn_ = Screen::Room;
            teamCursor_ = kTeamRowFirstSlot;
            GoTo(Screen::Team);
            handshakeFrames_ = 0; // the team screen runs on what is left of the entry clock
        }
        break;
    default:
        break; // waiting on the other player: only cancel does anything
    }
    return true;
}

// Cursor in the player list. Confirm opens the actions for that player.
void LobbyUi::UpdateMemberList(const Input& input)
{
    const RoomState& room = service_.Room();
    if (room.memberCount <= 0) {
        memberFocus_ = false;
        return;
    }
    if (memberCursor_ >= room.memberCount) memberCursor_ = room.memberCount - 1;
    // The chat panel sits under the list: stepping off either end lands on it.
    if ((input.down && memberCursor_ == room.memberCount - 1) || (input.up && memberCursor_ == 0)) {
        memberFocus_ = false;
        chatFocus_ = true;
        Play(SFX_MOVE);
        return;
    }
    MoveCursor(memberCursor_, room.memberCount, input, false);
    if (input.right || input.cancel) {
        memberFocus_ = false;
        Play(input.cancel ? SFX_CANCEL : SFX_MOVE);
        return;
    }
    if (!input.confirm) return;
    const Member& member = room.members[memberCursor_];
    memberMenuId_ = member.id;
    snprintf(memberMenuName_, sizeof(memberMenuName_), "%s", member.name);
    memberMenuCursor_ = 0;
    memberMenuOpen_ = true;
    Play(SFX_CONFIRM);
}

// Lines the chat shows when it is expanded over the player list.
constexpr int kChatExpandedRows = 23;

// Chat panel geometry shared by layout and drawing.
constexpr float kChatX = 40.0f, kChatW = 430.0f;
constexpr float kChatScale = 0.56f;
constexpr float kChatTextX = 140.0f;                    // message column, after the sender
constexpr float kChatTextW = kChatW - 162.0f;           // its width
constexpr float kChatSystemW = kChatW - 44.0f;          // system lines use the whole row
constexpr int kChatMaxWrap = 8;                         // rows one message may take

struct ChatSpan {
    uint8_t start, length;
};

// Breaks a message into rows no wider than maxWidth: at spaces where it can,
// inside a word (on a character boundary) where it must. Returns the row
// count, at least 1; text beyond kChatMaxWrap rows is dropped.
int WrapChat(const char* text, float maxWidth, ChatSpan spans[kChatMaxWrap])
{
    const size_t total = strlen(text);
    size_t at = 0;
    int rows = 0;
    while (at < total && rows < kChatMaxWrap) {
        while (text[at] == ' ') ++at; // rows do not start with the break's space
        if (at >= total) break;
        size_t end = at, lastSpace = 0;
        char probe[kChatCapacity + 4];
        while (end < total) {
            size_t next = end + 1;
            while (next < total && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
            const size_t length = next - at;
            if (length >= sizeof(probe)) break;
            memcpy(probe, text + at, length);
            probe[length] = '\0';
            if (TextWidth(probe) * kChatScale > maxWidth && end > at) break;
            if (text[end] == ' ') lastSpace = end;
            end = next;
        }
        if (end < total && lastSpace > at) end = lastSpace;
        size_t length = end - at;
        if (length > 255) length = 255;
        spans[rows++] = ChatSpan{static_cast<uint8_t>(at), static_cast<uint8_t>(length)};
        at = end;
    }
    if (rows == 0) spans[rows++] = ChatSpan{0, 0};
    return rows;
}

int ChatRowCount(const ChatLine& line)
{
    ChatSpan spans[kChatMaxWrap];
    return WrapChat(line.text, line.system ? kChatSystemW : kChatTextW, spans);
}

// Cursor on the chat panel. Confirm expands it over the player list; expanded,
// up/down scroll kChatScrollStep lines and the shoulder buttons a page.
constexpr int kChatScrollStep = 5;

void LobbyUi::UpdateChatFocus(const Input& input)
{
    if (!chatExpanded_) {
        if (input.confirm) {
            chatExpanded_ = true;
            chatScroll_ = 0;
            chatSeenCount_ = service_.ChatCount();
            Play(SFX_CONFIRM);
        } else if (input.up || input.down) {
            // Back into the player list, at the end nearest the chat.
            const int count = service_.Room().memberCount;
            chatFocus_ = false;
            if (count > 0) {
                memberFocus_ = true;
                memberCursor_ = input.up ? count - 1 : 0;
            }
            Play(SFX_MOVE);
        } else if (input.right || input.cancel) {
            chatFocus_ = false;
            Play(input.cancel ? SFX_CANCEL : SFX_MOVE);
        }
        return;
    }
    if (input.cancel || input.confirm) {
        chatExpanded_ = false;
        chatFocus_ = true;
        Play(SFX_CANCEL);
        return;
    }
    // While scrolled back, lines that arrive push the view up by their own
    // height, so what is being read stays put.
    const int count = service_.ChatCount();
    if (chatScroll_ > 0 && count > chatSeenCount_) chatScroll_ += ChatRowsFrom(chatSeenCount_);
    chatSeenCount_ = count;

    int next = chatScroll_;
    if (input.up) next += kChatScrollStep;
    if (input.down) next -= kChatScrollStep;
    if (input.pageLeft) next += kChatExpandedRows - 2;
    if (input.pageRight) next -= kChatExpandedRows - 2;
    if (next == chatScroll_) return;
    if (next > chatScroll_) {
        // Only now is the whole history measured, to find the top.
        const int limit = ChatRowsFrom(0) - kChatExpandedRows;
        if (next > limit) next = limit;
    }
    if (next < 0) next = 0;
    if (next != chatScroll_) {
        chatScroll_ = next;
        Play(SFX_MOVE);
    }
}

int LobbyUi::ChatRowsFrom(int firstMessage) const
{
    int rows = 0;
    for (int i = firstMessage < 0 ? 0 : firstMessage; i < service_.ChatCount(); ++i)
        rows += ChatRowCount(service_.Chat(i));
    return rows;
}

void LobbyUi::UpdateMemberMenu(const Input& input)
{
    // The player may leave while their menu is open.
    const RoomState& room = service_.Room();
    const Member* target = nullptr;
    for (int i = 0; i < room.memberCount; ++i)
        if (room.members[i].id == memberMenuId_) target = &room.members[i];
    if (!target) {
        memberMenuOpen_ = false;
        return;
    }
    const Member* local = LocalMember();
    const bool canKick = local && local->isHost && !target->isLocal;
    MoveCursor(memberMenuCursor_, canKick ? 2 : 1, input);
    if (input.cancel || input.menu) {
        memberMenuOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (!input.confirm) return;
    Play(SFX_CONFIRM);
    memberMenuOpen_ = false;
    if (memberMenuCursor_ == 0) {
        service_.ViewProfile(memberMenuId_);
    } else if (canKick) {
        confirm_ = Confirm::KickMember;
        confirmCursor_ = 1;
    }
}

void LobbyUi::UpdateRoomMenu(const Input& input)
{
    MoveCursor(roomMenuCursor_, kRoomMenuCount, input);
    if (input.cancel || input.menu) {
        roomMenuOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (!input.confirm) return;
    Play(SFX_CONFIRM);
    roomMenuOpen_ = false;
    switch (roomMenuCursor_) {
    case 0:
        teamReturn_ = Screen::Room;
        teamCursor_ = 0;
        GoTo(Screen::Team);
        break;
    case 1: {
        // The host picks the music; everyone else can only silence it.
        const Member* local = LocalMember();
        if (!local || !local->isHost) {
            prefs_.muteMusic = !prefs_.muteMusic;
            prefsDirty_ = true;
            break;
        }
        const int current = FindMusicTrack(service_.Room().music);
        musicCursor_ = current < 0 ? 0 : current;
        musicOpen_ = true;
        break;
    }
    case 2:
        service_.InviteFriends();
        break;
    case 3:
        service_.CopyLobbyLink();
        break;
    default:
        confirm_ = Confirm::LeaveRoom;
        confirmCursor_ = 0; // default to "Yes"
        break;
    }
}

// The music picker: one long list in three groups. Up/down walks the tracks,
// left/right jumps to the previous or next group. It serves the create screen
// (the track a new lobby starts with) and the host of a running lobby.
void LobbyUi::UpdateMusicMenu(const Input& input)
{
    const bool inRoom = screen_ == Screen::Room;
    const Member* local = LocalMember();
    if (inRoom && (!local || !local->isHost)) {
        // Host changed while the picker was open.
        musicOpen_ = false;
        return;
    }
    if (input.cancel || input.menu) {
        musicOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (!MoveCursor(musicCursor_, kMusicTrackCount, input)) {
        const MusicGroup group = kMusicTracks[musicCursor_].group;
        int next = musicCursor_;
        if (input.right || input.pageRight) {
            while (next < kMusicTrackCount && kMusicTracks[next].group == group) ++next;
            if (next >= kMusicTrackCount) next = 0;
        } else if (input.left || input.pageLeft) {
            // Start of this group, or of the previous one if already there.
            while (next > 0 && kMusicTracks[next - 1].group == group) --next;
            if (next == musicCursor_) {
                next = next > 0 ? next - 1 : kMusicTrackCount - 1;
                const MusicGroup previous = kMusicTracks[next].group;
                while (next > 0 && kMusicTracks[next - 1].group == previous) --next;
            }
        }
        if (next != musicCursor_) {
            musicCursor_ = next;
            Play(SFX_MOVE);
        }
    }
    if (!input.confirm) return;
    Play(SFX_CONFIRM);
    // Either way the choice becomes the one the next created lobby starts with.
    prefs_.create.music = kMusicTracks[musicCursor_].id;
    prefsDirty_ = true;
    if (inRoom) {
        // Stays open so the host can audition tracks; everyone hears each choice.
        service_.SetMusic(prefs_.create.music);
    } else {
        musicOpen_ = false;
    }
}

void LobbyUi::UpdateQuickChat(const Input& input)
{
    MoveCursor(quickChatCursor_, kQuickChatCount, input);
    if (input.cancel || input.actionY) {
        quickChatOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (!input.confirm) return;
    Play(SFX_CONFIRM);
    quickChatOpen_ = false;
    if (quickChatCursor_ == 0)
        BeginText(TextTarget::Chat, "");
    else
        service_.SendChat(kQuickChat[quickChatCursor_]);
}

void LobbyUi::BeginText(TextTarget target, const char* initial)
{
    textTarget_ = target;
    textCapacity_ = target == TextTarget::RoomName ? 40
                  : target == TextTarget::PresetName ? kPresetNameCapacity - 1
                  : kChatCapacity - 1;
    snprintf(text_, sizeof(text_), "%s", initial ? initial : "");
    if (strlen(text_) > textCapacity_) text_[0] = '\0';
}

void LobbyUi::EndText(bool accept)
{
    const TextTarget target = textTarget_;
    textTarget_ = TextTarget::None;
    if (!accept) return;
    if (target == TextTarget::RoomName) {
        if (text_[0]) {
            snprintf(prefs_.create.name, sizeof(prefs_.create.name), "%.*s",
                     static_cast<int>(sizeof(prefs_.create.name) - 1), text_);
            prefsDirty_ = true;
        }
    } else if (target == TextTarget::Chat && text_[0]) {
        service_.SendChat(text_);
    } else if (target == TextTarget::PresetName && text_[0] &&
               presetListCursor_ >= 0 && presetListCursor_ < kTeamPresets) {
        TeamPreset& preset = prefs_.presets[presetListCursor_];
        snprintf(preset.name, sizeof(preset.name), "%.*s",
                 static_cast<int>(sizeof(preset.name) - 1), text_);
        prefsDirty_ = true;
    }
}

void LobbyUi::UpdateText(const Input& input)
{
    if (input.backspace) PopUtf8(text_);
    if (input.typed) {
        for (const char* cursor = input.typed; *cursor;) {
            const unsigned char lead = static_cast<unsigned char>(*cursor);
            size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
            if (strlen(cursor) < length) break;
            const size_t used = strlen(text_);
            if (lead >= 0x20 && used + length <= textCapacity_ && used + length < sizeof(text_)) {
                memcpy(text_ + used, cursor, length);
                text_[used + length] = '\0';
            }
            cursor += length;
        }
    }
    if (input.confirm) {
        Play(SFX_CONFIRM);
        EndText(true);
    } else if (input.cancel) {
        Play(SFX_CANCEL);
        EndText(false);
    }
}

bool LobbyUi::EditingTeam() const
{
    return screen_ == Screen::Team || HandshakeStep() != Handshake::None;
}

bool LobbyUi::SlotTeam(int slot, Team* team) const
{
    if (slot < 0 || slot >= kTeamSlots) return false;
    if (slot < kTeamPresets) {
        if (!prefs_.presets[slot].used) return false;
        *team = prefs_.presets[slot].team;
        return true;
    }
    const int unit = slot - kTeamPresets;
    return roster_.presetCount && roster_.preset && unit < roster_.presetCount() &&
           roster_.preset(unit, team);
}

bool LobbyUi::SlotUsed(int slot) const
{
    Team team;
    return SlotTeam(slot, &team);
}

void LobbyUi::SlotName(int slot, char* out, size_t size) const
{
    if (slot >= 0 && slot < kTeamPresets)
        snprintf(out, size, "%s", prefs_.presets[slot].name);
    else
        snprintf(out, size, "RESERVE UNIT %c", 'A' + (slot - kTeamPresets) % kReserveUnits);
}

// The nearest slot holding a team in that direction, wrapping; `from` itself
// when there is no other.
int LobbyUi::NextUsedSlot(int from, int delta) const
{
    for (int step = 1; step < kTeamSlots; ++step) {
        const int slot = ((from + delta * step) % kTeamSlots + kTeamSlots) % kTeamSlots;
        if (SlotUsed(slot)) return slot;
    }
    return from;
}

void LobbyUi::SelectSlot(int slot)
{
    Team team;
    if (!SlotTeam(slot, &team)) return;
    prefs_.team = team;
    prefs_.slot = slot;
    CommitTeam();
}

void LobbyUi::StepSlot(int delta)
{
    const int next = NextUsedSlot(prefs_.slot, delta);
    if (next == prefs_.slot) return;
    SelectSlot(next);
    Play(SFX_MOVE);
}

// An edit to the team in use belongs to its slot: there is no unsaved team.
void LobbyUi::StoreTeam()
{
    if (prefs_.slot >= 0 && prefs_.slot < kTeamPresets) {
        TeamPreset& preset = prefs_.presets[prefs_.slot];
        preset.team = prefs_.team;
        if (!preset.used) {
            preset.used = true;
            snprintf(preset.name, sizeof(preset.name), "PRESET %d", prefs_.slot + 1);
        }
    } else if (roster_.setPreset) {
        roster_.setPreset(prefs_.slot - kTeamPresets, prefs_.team);
    }
    prefsDirty_ = true;
}

void LobbyUi::DeletePreset(int slot)
{
    if (slot < 0 || slot >= kTeamPresets || !prefs_.presets[slot].used) return;
    if (slot == prefs_.slot) {
        // The team in use moves on to the next one first.
        const int next = NextUsedSlot(slot, 1);
        if (next == slot) {
            ShowNotice("This is your only team.\nIt cannot be deleted.");
            return;
        }
        SelectSlot(next);
    }
    prefs_.presets[slot] = TeamPreset{};
    prefsDirty_ = true;
}

void LobbyUi::RefreshPreset()
{
    if (prefs_.slot < kTeamPresets) return;
    Team team;
    if (!SlotTeam(prefs_.slot, &team)) {
        // The reserve unit holds no team any more: carry on with another,
        // or keep the team as the first preset.
        const int next = NextUsedSlot(prefs_.slot, 1);
        if (next != prefs_.slot) {
            SelectSlot(next);
            return;
        }
        prefs_.slot = 0;
        StoreTeam();
        return;
    }
    bool same = true;
    for (int i = 0; i < 3; ++i)
        same &= team.chars[i] == prefs_.team.chars[i] &&
                team.assists[i] == prefs_.team.assists[i] &&
                team.colors[i] == prefs_.team.colors[i];
    if (same) return;
    prefs_.team = team;
    CommitTeam();
}

// Every team slot at once: confirm uses one (a free slot first takes a copy
// of the team in use), X renames a preset and Y deletes it.
void LobbyUi::UpdatePresetList(const Input& input)
{
    if (input.cancel || input.menu) {
        presetListOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    const int rows = kTeamPresets + (roster_.presetCount ? roster_.presetCount() : 0);
    if (presetListCursor_ >= rows) presetListCursor_ = rows - 1;
    MoveCursor(presetListCursor_, rows, input);
    const int slot = presetListCursor_;
    const bool preset = slot < kTeamPresets;
    if (input.confirm) {
        if (!SlotUsed(slot)) {
            bool made = false;
            if (preset) {
                TeamPreset& fresh = prefs_.presets[slot];
                fresh.used = true;
                fresh.team = prefs_.team;
                snprintf(fresh.name, sizeof(fresh.name), "PRESET %d", slot + 1);
                made = true;
            } else if (roster_.setPreset) {
                made = roster_.setPreset(slot - kTeamPresets, prefs_.team);
            }
            if (!made) {
                bool random = false;
                for (int i = 0; i < 3; ++i) random |= prefs_.team.chars[i] == kCharRandom;
                ShowNotice(random ? "A reserve unit cannot hold\na random character."
                                  : "A reserve unit cannot hold\nthe same character twice.");
                return;
            }
        }
        SelectSlot(slot);
        presetListOpen_ = false;
        Play(SFX_CONFIRM);
        return;
    }
    if (!preset || !prefs_.presets[slot].used) return;
    if (input.actionX) {
        BeginText(TextTarget::PresetName, prefs_.presets[slot].name);
        Play(SFX_CONFIRM);
    } else if (input.actionY) {
        confirm_ = Confirm::DeletePreset;
        confirmCursor_ = 1;
        Play(SFX_CONFIRM);
    }
}

void LobbyUi::CommitTeam()
{
    prefsDirty_ = true;
    if (service_.Room().inRoom) service_.SetTeam(prefs_.team);
}

void LobbyUi::StepTeamField(int delta)
{
    if (teamCursor_ == kTeamRowPreset) {
        return; // teams are picked from their list (confirm), not stepped
    } else if (teamCursor_ >= kTeamRowFirstSlot && teamCursor_ < kTeamRowIcon) {
        const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
        const int field = (teamCursor_ - kTeamRowFirstSlot) % 3;
        Team& team = prefs_.team;
        if (field == 0) {
            return; // characters are picked from the grid (confirm), not stepped
        } else if (field == 1) {
            if (team.chars[member] == kCharRandom) return; // drawn with the character
            team.assists[member] = ((team.assists[member] + delta) % 3 + 3) % 3;
        } else {
            return; // colors are picked from their panel (confirm), not stepped
        }
        StoreTeam();
    } else {
        return; // the icon and stage rows open their grids on confirm
    }
    Play(SFX_MOVE);
    CommitTeam();
}

Tex LobbyUi::IconTex(uint64_t memberId, int icon) const
{
    return icon > kIconSteam && icon <= kIconMax ? LicenseIcon(icon - 1) : service_.AvatarTex(memberId);
}

// The icon grid: the Steam picture first, then the license icons in order.
// Directions move a cell, the page keys a screenful, confirm picks.
void LobbyUi::UpdateIconPicker(const Input& input)
{
    if (input.cancel || input.menu) {
        iconPickerOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    const int count = kIconMax + 1;
    int next = iconCursor_;
    if (input.left) next -= 1;
    if (input.right) next += 1;
    if (input.up) next -= kIconGridColumns;
    if (input.down) next += kIconGridColumns;
    if (input.pageLeft) next -= kIconGridColumns * kIconGridRows;
    if (input.pageRight) next += kIconGridColumns * kIconGridRows;
    // Left and right wrap around the ends; a page stops at them; up on the
    // first row stays put.
    if (next < 0) next = input.left ? count - 1 : input.pageLeft ? 0 : iconCursor_;
    if (next >= count) next = input.right ? 0 : count - 1;
    if (next != iconCursor_) {
        iconCursor_ = next;
        Play(SFX_MOVE);
    }
    // Keep the cursor's row inside the window.
    const int row = iconCursor_ / kIconGridColumns;
    if (row < iconScroll_) iconScroll_ = row;
    if (row >= iconScroll_ + kIconGridRows) iconScroll_ = row - kIconGridRows + 1;
    if (!input.confirm) return;
    iconPickerOpen_ = false;
    Play(SFX_CONFIRM);
    SetIcon(iconCursor_);
}

void LobbyUi::SetStage(int stage)
{
    if (stage == prefs_.stage) return;
    prefs_.stage = stage;
    prefsDirty_ = true;
    if (service_.Room().inRoom) service_.SetStage(stage);
}

// The stage grid: ANY STAGE first, then every stage as the stage select
// screen shows it. Directions move a cell (left and right wrap around the
// ends), confirm makes it the preferred stage.
void LobbyUi::UpdateStagePicker(const Input& input)
{
    const int count = (roster_.stageCount && roster_.stageId ? roster_.stageCount() : 0) + 1;
    if (input.cancel || input.menu) {
        stagePickerOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (stagePickerCursor_ >= count) stagePickerCursor_ = count - 1;
    int next = stagePickerCursor_;
    if (input.left) next -= 1;
    if (input.right) next += 1;
    if (input.up) next -= kStageGridColumns;
    if (input.down) next += kStageGridColumns;
    if (next < 0) next = input.left ? count - 1 : stagePickerCursor_;
    if (next >= count) next = input.right ? 0 : stagePickerCursor_;
    if (next != stagePickerCursor_) {
        stagePickerCursor_ = next;
        Play(SFX_MOVE);
    }
    if (!input.confirm) return;
    stagePickerOpen_ = false;
    Play(SFX_CONFIRM);
    SetStage(stagePickerCursor_ == 0 ? kStageAny : roster_.stageId(stagePickerCursor_ - 1));
}

const char* LobbyUi::StageName(int stage) const
{
    const char* name = stage != kStageAny && roster_.stageName ? roster_.stageName(stage) : nullptr;
    return name && name[0] ? name : "ANY STAGE";
}

void LobbyUi::SetIcon(int icon)
{
    if (icon < kIconSteam || icon > kIconMax) icon = kIconSteam;
    if (icon == prefs_.icon) return;
    prefs_.icon = icon;
    prefsDirty_ = true;
    if (service_.Room().inRoom) service_.SetIcon(icon);
}

// Puts a character in a team member's slot, keeping the color in range.
void LobbyUi::SetTeamCharacter(int member, int32_t id)
{
    Team& team = prefs_.team;
    team.chars[member] = id;
    if (id == kCharRandom) {
        // The color and assist come with the character when it is drawn.
        team.colors[member] = 0;
        team.assists[member] = 0;
        return;
    }
    const int colors = roster_.colorCount ? roster_.colorCount(id) : 1;
    if (team.colors[member] >= colors) team.colors[member] = 0;
}

// A random member can only be kept in the player's own presets: the game's
// reserve units hold real characters.
bool LobbyUi::RandomOffered() const
{
    return prefs_.slot >= 0 && prefs_.slot < kTeamPresets;
}

// The character grid for a team member row: the roster's select-screen faces.
// Directions move a cell (left and right wrap around the ends), confirm puts
// the character in the slot.
void LobbyUi::UpdateCharPicker(const Input& input)
{
    const int count = roster_.characterCount ? roster_.characterCount() : 0;
    if (count <= 0 || input.cancel || input.menu) {
        charPickerOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    // The cell after the roster, on a row of its own under it, is RANDOM.
    const int cells = count + (RandomOffered() ? 1 : 0);
    const bool onRandom = charPickerCursor_ >= count;
    int next = charPickerCursor_;
    if (input.left) next -= 1;
    if (input.right) next += 1;
    if (onRandom) {
        if (input.up) {
            // Back up to the column it was reached from.
            next = (count - 1) / kCharGridColumns * kCharGridColumns + charPickerColumn_;
            if (next >= count) next -= kCharGridColumns;
        }
    } else {
        if (input.up) next -= kCharGridColumns;
        if (input.down) {
            next += kCharGridColumns;
            if (next >= count && cells > count) {
                charPickerColumn_ = charPickerCursor_ % kCharGridColumns;
                next = count;
            }
        }
    }
    if (next < 0) next = input.left ? cells - 1 : charPickerCursor_;
    if (next >= cells) next = input.right ? 0 : charPickerCursor_;
    if (next != charPickerCursor_) {
        charPickerCursor_ = next;
        Play(SFX_MOVE);
    }
    if (!input.confirm) return;
    charPickerOpen_ = false;
    Play(SFX_CONFIRM);
    const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
    const int32_t id = charPickerCursor_ >= count ? kCharRandom
                                                  : roster_.characterId(charPickerCursor_);
    if (id == prefs_.team.chars[member]) return; // same character: the team stays as it was
    Team& team = prefs_.team;
    int other = -1;
    // Any number of members may be random: only a real character is traded.
    for (int i = 0; i < 3 && id != kCharRandom; ++i)
        if (i != member && team.chars[i] == id) other = i;
    if (other >= 0) {
        // A team never holds a character twice (the game's own team editor
        // does not allow it either): the two members trade places instead.
        std::swap(team.chars[member], team.chars[other]);
        std::swap(team.assists[member], team.assists[other]);
        std::swap(team.colors[member], team.colors[other]);
    } else {
        SetTeamCharacter(member, id);
    }
    StoreTeam();
    CommitTeam();
}

// Every color of a team member's character at once. Directions move a cell
// (left and right wrap around the ends), confirm puts the color on the slot.
void LobbyUi::UpdateColorPicker(const Input& input)
{
    const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
    const int count = roster_.colorCount ? roster_.colorCount(prefs_.team.chars[member]) : 1;
    if (count <= 0 || input.cancel || input.menu) {
        colorPickerOpen_ = false;
        Play(SFX_CANCEL);
        return;
    }
    if (colorPickerCursor_ >= count) colorPickerCursor_ = count - 1;
    int next = colorPickerCursor_;
    if (input.left) next -= 1;
    if (input.right) next += 1;
    if (input.up) next -= kColorGridColumns;
    if (input.down) next += kColorGridColumns;
    if (next < 0) next = input.left ? count - 1 : colorPickerCursor_;
    if (next >= count) next = input.right ? 0 : colorPickerCursor_;
    if (next != colorPickerCursor_) {
        colorPickerCursor_ = next;
        Play(SFX_MOVE);
    }
    if (!input.confirm) return;
    colorPickerOpen_ = false;
    Play(SFX_CONFIRM);
    if (colorPickerCursor_ == prefs_.team.colors[member]) return; // same color: nothing changes
    prefs_.team.colors[member] = colorPickerCursor_;
    StoreTeam();
    CommitTeam();
}

void LobbyUi::UpdateTeam(const Input& input)
{
    bool timedOut = false;
    if (teamForMatch_) {
        // Opened from the pre-match prompt: the other player is waiting.
        if (HandshakeStep() != Handshake::Entry) {
            // They left their seat while the team was being chosen.
            teamForMatch_ = false;
            CommitTeam();
            GoTo(Screen::Room);
            screenFrame_ = kTransitionFrames;
            return;
        }
        ++handshakeFrames_;
        ++entryFrames_;
        timedOut = ++teamFrames_ >= kTeamTimeout;
    }
    if (charPickerOpen_) {
        if (!timedOut) {
            UpdateCharPicker(input);
            return;
        }
        charPickerOpen_ = false; // out of time: go with the team as it stands
    }
    if (colorPickerOpen_) {
        if (!timedOut) {
            UpdateColorPicker(input);
            return;
        }
        colorPickerOpen_ = false;
    }
    if (iconPickerOpen_) {
        if (!timedOut) {
            UpdateIconPicker(input);
            return;
        }
        iconPickerOpen_ = false;
    }
    if (presetListOpen_) {
        if (!timedOut) {
            UpdatePresetList(input);
            return;
        }
        presetListOpen_ = false;
    }
    if (stagePickerOpen_) {
        if (!timedOut) {
            UpdateStagePicker(input);
            return;
        }
        stagePickerOpen_ = false;
    }
    MoveCursor(teamCursor_, kTeamRows, input);
    const int delta = (input.right ? 1 : 0) - (input.left ? 1 : 0);
    if (delta != 0) StepTeamField(delta);

    const bool confirmDone = (input.confirm && teamCursor_ == kTeamRowDone) || timedOut;
    const bool done = input.cancel || confirmDone;
    if (done) {
        // Done on a pre-match team is the player's final "start"; backing out
        // returns to the choice between character select and starting.
        const bool readyUp = confirmDone && teamForMatch_;
        teamForMatch_ = false;
        handshakeFrames_ = 0; // back at the start/character-select choice
        Play(confirmDone ? SFX_CONFIRM : SFX_CANCEL);
        CommitTeam();
        const Screen target = teamReturn_;
        GoTo(target);
        if (target == Screen::Room) {
            screenFrame_ = kTransitionFrames;
            const Member* local = LocalMember();
            if (readyUp && local && local->slot >= 0 && local->side < 2 && !local->ready)
                service_.SetReady(true);
        }
        return;
    }
    if (!input.confirm) return;
    if (teamCursor_ == kTeamRowPreset) {
        presetListCursor_ = prefs_.slot;
        presetListOpen_ = true;
        Play(SFX_CONFIRM);
        return;
    }
    const bool characterRow = teamCursor_ >= kTeamRowFirstSlot && teamCursor_ < kTeamRowIcon &&
                              (teamCursor_ - kTeamRowFirstSlot) % 3 == 0;
    const int count = roster_.characterCount ? roster_.characterCount() : 0;
    if (characterRow && count > 0) {
        const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
        charPickerCursor_ = 0;
        charPickerColumn_ = kCharGridColumns / 2;
        for (int i = 0; i < count; ++i)
            if (roster_.characterId(i) == prefs_.team.chars[member]) charPickerCursor_ = i;
        if (prefs_.team.chars[member] == kCharRandom && RandomOffered()) charPickerCursor_ = count;
        charPickerOpen_ = true;
        Play(SFX_CONFIRM);
        return;
    }
    const bool colorRow = teamCursor_ >= kTeamRowFirstSlot && teamCursor_ < kTeamRowIcon &&
                          (teamCursor_ - kTeamRowFirstSlot) % 3 == 2;
    if (colorRow) {
        const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
        const int colors = roster_.colorCount ? roster_.colorCount(prefs_.team.chars[member]) : 1;
        // One color leaves nothing to choose from, and a random member's
        // color is drawn with it.
        if (colors > 1 && prefs_.team.chars[member] != kCharRandom) {
            const int current = prefs_.team.colors[member];
            colorPickerCursor_ = current >= 0 && current < colors ? current : 0;
            colorPickerOpen_ = true;
            Play(SFX_CONFIRM);
        }
        return;
    }
    if (teamCursor_ == kTeamRowStage) {
        stagePickerCursor_ = 0;
        const int count = roster_.stageCount && roster_.stageId ? roster_.stageCount() : 0;
        for (int i = 0; i < count; ++i)
            if (roster_.stageId(i) == prefs_.stage) stagePickerCursor_ = i + 1;
        stagePickerOpen_ = true;
        Play(SFX_CONFIRM);
        return;
    }
    if (teamCursor_ == kTeamRowIcon) {
        iconCursor_ = prefs_.icon;
        iconScroll_ = iconCursor_ / kIconGridColumns - kIconGridRows / 2;
        const int lastScroll = (kIconMax + kIconGridColumns) / kIconGridColumns - kIconGridRows;
        if (iconScroll_ > lastScroll) iconScroll_ = lastScroll;
        if (iconScroll_ < 0) iconScroll_ = 0;
        iconPickerOpen_ = true;
        Play(SFX_CONFIRM);
        return;
    }
    StepTeamField(1);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void LobbyUi::Draw(Canvas& canvas)
{
    DrawBackdrop(canvas);
    const char* help = nullptr;
    switch (screen_) {
    case Screen::Top: help = DrawTop(canvas); break;
    case Screen::Browse: help = DrawBrowse(canvas); break;
    case Screen::Create: help = DrawCreate(canvas); break;
    case Screen::Room: help = DrawRoom(canvas); break;
    case Screen::Team: help = DrawTeam(canvas); break;
    }
    // The footer is drawn last so it stays readable above modal dimming.
    bool modal = DrawModal(canvas);
    if (updateActive_) {
        DrawUpdateProgress(canvas);
        modal = true;
    }
    DrawFooter(canvas, modal ? nullptr : help);
}

void LobbyUi::DrawBackdrop(Canvas& canvas) const
{
    // The title area keeps animating its key art behind the lobby; it is
    // pushed far back so panels and text read.
    canvas.Sprite(TEX_WHITE, 0.0f, 0.0f, kScreenW, kScreenH, Uv{0.25f, 0.25f, 0.75f, 0.75f}, A_TL,
                  Color{0.02f, 0.03f, 0.07f, 0.74f});
    // In a room the space backdrop fades in over it: stars and a scrolling
    // grid, after the lobby of the first game (space_bg.cpp).
    if (roomBackdrop_ > 0) {
        const float t = roomBackdrop_ / static_cast<float>(kRoomBackdropFrames);
        DrawSpaceBackground(canvas, frame_, t * t * (3.0f - 2.0f * t));
    }
    canvas.Sprite(TEX_WHITE, 0.0f, 0.0f, kScreenW, 132.0f, Uv{0.25f, 0.25f, 0.75f, 0.75f}, A_TL,
                  Color{0.0f, 0.0f, 0.0f, 0.38f});
}

void LobbyUi::DrawFooter(Canvas& canvas, const char* help) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, kFooterY - 4.0f, kScreenW, 60.0f, Color{0.0f, 0.0f, 0.0f, 0.62f});
    // A faint hairline, just enough to define the strip's top edge.
    p.Fill(0.0f, kFooterY - 4.0f, kScreenW, 1.0f, Color{1.0f, 1.0f, 1.0f, 0.28f});

    // Context prompts, laid out right-to-left.
    Prompt prompts[6];
    int count = 0;
    const auto add = [&](Button button, const char* label) {
        if (count < 6) prompts[count++] = Prompt{button, label};
    };
    // In a room, Y types straight into the chat on the keyboard; RB (X)
    // opens the quick chat from anywhere.
    const auto addChat = [&]() {
        if (usingKeyboard_) add(BTN_Y, "Type");
        add(BTN_QUICK_CHAT, "Quick chat");
    };

    const bool modal = notice_[0] || confirm_ != Confirm::None;
    if (updateActive_) {
        if (update_.cancellable) add(BTN_CANCEL, "Cancel");
    } else if (busy_[0]) {
        // No input is accepted while a blocking operation runs.
    } else if (textTarget_ != TextTarget::None) {
        add(BTN_CONFIRM, "OK");
        add(usingKeyboard_ ? BTN_MENU : BTN_CANCEL, "Cancel"); // Esc: Backspace edits
    } else if (modal) {
        add(BTN_CONFIRM, "OK");
        if (confirm_ != Confirm::None) add(BTN_CANCEL, "Back");
    } else if (screen_ == Screen::Room && HandshakeStep() != Handshake::None) {
        const Handshake step = HandshakeStep();
        if (step == Handshake::Entry) {
            add(BTN_CONFIRM, "Select");
            if (handshakeCursor_ == 0 && NextUsedSlot(prefs_.slot, 1) != prefs_.slot)
                add(BTN_DPAD, "Team");
        }
        if (step == Handshake::Delay) {
            add(BTN_CONFIRM, "Ready");
            add(BTN_DPAD, "Change");
        }
        add(BTN_CANCEL, "Leave seat");
    } else if (musicOpen_) {
        add(BTN_CONFIRM, screen_ == Screen::Room ? "Play" : "Select");
        add(BTN_DPAD, "Group");
        add(BTN_CANCEL, "Close");
    } else if (screen_ == Screen::Top) {
        add(BTN_CONFIRM, "Select");
        add(BTN_CANCEL, "Main Menu");
    } else if (screen_ == Screen::Browse) {
        add(BTN_CONFIRM, "Join");
        add(BTN_X, "Search again");
        add(BTN_Y, "Create lobby");
        add(BTN_CANCEL, "Back");
    } else if (screen_ == Screen::Create) {
        add(BTN_CONFIRM, "Select");
        if (createCursor_ != CREATE_MUSIC) add(BTN_DPAD, "Change");
        add(BTN_CANCEL, "Back");
    } else if (screen_ == Screen::Team && presetListOpen_) {
        const bool used = SlotUsed(presetListCursor_);
        add(BTN_CONFIRM, used ? "Use" : "New");
        if (used && presetListCursor_ < kTeamPresets) {
            add(BTN_X, "Rename");
            add(BTN_Y, "Delete");
        }
        add(BTN_CANCEL, "Close");
    } else if (screen_ == Screen::Team && iconPickerOpen_) {
        add(BTN_CONFIRM, "Select");
        add(BTN_DPAD, "Move");
        add(BTN_CANCEL, "Close");
    } else if (screen_ == Screen::Team && (charPickerOpen_ || colorPickerOpen_ || stagePickerOpen_)) {
        add(BTN_CONFIRM, "Select");
        add(BTN_DPAD, "Move");
        add(BTN_CANCEL, "Close");
    } else if (screen_ == Screen::Team) {
        const bool characterRow = teamCursor_ >= kTeamRowFirstSlot && teamCursor_ < kTeamRowIcon &&
                                  (teamCursor_ - kTeamRowFirstSlot) % 3 == 0;
        if (characterRow) add(BTN_CONFIRM, "Characters");
        const bool colorRow = teamCursor_ >= kTeamRowFirstSlot && teamCursor_ < kTeamRowIcon &&
                              (teamCursor_ - kTeamRowFirstSlot) % 3 == 2;
        if (colorRow) add(BTN_CONFIRM, "Colors");
        if (teamCursor_ == kTeamRowIcon) add(BTN_CONFIRM, "Icons");
        if (teamCursor_ == kTeamRowPreset) add(BTN_CONFIRM, "Teams");
        if (teamCursor_ == kTeamRowStage) add(BTN_CONFIRM, "Stages");
        if (!characterRow && !colorRow && teamCursor_ != kTeamRowIcon &&
            teamCursor_ != kTeamRowStage &&
            teamCursor_ != kTeamRowPreset && teamCursor_ != kTeamRowDone)
            add(BTN_DPAD, "Change");
        add(BTN_CANCEL, "Done");
    } else if (roomMenuOpen_ || slotMenuOpen_ || quickChatOpen_ || memberMenuOpen_) {
        add(BTN_CONFIRM, "Select");
        add(BTN_CANCEL, "Back");
    } else if (chatExpanded_) {
        add(BTN_DPAD, "Scroll");
        addChat();
        add(BTN_CANCEL, "Close");
    } else if (chatFocus_) {
        add(BTN_CONFIRM, "Expand");
        addChat();
        add(BTN_MENU, "Menu");
        add(BTN_CANCEL, "Back");
    } else if (memberFocus_) {
        add(BTN_CONFIRM, "Player");
        addChat();
        add(BTN_MENU, "Menu");
        add(BTN_CANCEL, "Back");
    } else {
        const Member* local = LocalMember();
        const bool seated = local && local->slot >= 0 && local->side < 2;
        const bool onOwnSeat = seated && local->slot == seatSlot_ && local->side == seatSide_;
        const bool queued = local && local->queueSlot >= 0;
        const bool full = SeatOccupant(seatSlot_, 0) && SeatOccupant(seatSlot_, 1);
        if (queued && local->queueSlot == seatSlot_)
            add(BTN_CONFIRM, "Leave queue");
        else if (!seated && !full && SlotQueueLength(seatSlot_) == 0 &&
                 !SeatOccupant(seatSlot_, seatSide_))
            add(BTN_CONFIRM, "Sit");
        else if (!seated)
            add(BTN_CONFIRM, "Join queue");
        // On the keyboard X is the quick chat; Backspace leaves the seat.
        if (!usingKeyboard_ && seated) add(BTN_X, "Leave seat");
        else if (!usingKeyboard_ && queued) add(BTN_X, "Leave queue");
        addChat();
        add(BTN_MENU, "Menu");
        if (usingKeyboard_ && (seated || queued))
            add(BTN_CANCEL, seated ? "Leave seat" : "Leave queue");
        else
            add(BTN_CANCEL, seated || queued ? "Back" : "Leave");
    }

    float x = kSafeRight;
    for (int i = count - 1; i >= 0; --i) {
        const float labelWidth = TextWidth(prompts[i].label) * 0.72f;
        p.Text(prompts[i].label, x, kFooterY + 22.0f, 0.72f, A_MR, kText);
        x -= labelWidth + 6.0f;
        DrawGlyph(p, prompts[i].button, usingKeyboard_, x, kFooterY + 22.0f, 34.0f);
        x -= 34.0f + 18.0f;
    }
    // The help line takes whatever the prompts left free.
    if (help)
        p.TextFit(help, kSafeLeft + 8.0f, kFooterY + 22.0f, 0.74f, x - kSafeLeft - 20.0f, A_ML, kDim);
}

void LobbyUi::DrawTeamCard(Canvas& canvas, float x, float y, float w) const
{
    const float t = Clamp01(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, 0.0f);
    p.Box(TEX_PANEL_DARK, x, y, w, 300.0f);
    p.Sprite(IconTex(0, prefs_.icon), x + 34.0f, y + 58.0f, 56.0f, 56.0f, kFullUv, A_ML);
    p.TextFit(service_.LocalName(), x + 104.0f, y + 46.0f, 0.95f, w - 130.0f, A_ML);
    char slotName[kPresetNameCapacity];
    SlotName(prefs_.slot, slotName, sizeof(slotName));
    p.TextFit(slotName, x + 104.0f, y + 74.0f, 0.62f, w - 130.0f, A_ML, kYellow);
    p.Sprite(TEX_LINE, x + 28.0f, y + 100.0f, w - 56.0f, 3.0f, Uv{0.1f, 0.55f, 0.9f, 0.95f}, A_TL,
             WithAlpha(kCyan, 0.8f));
    // The team as its select-screen faces, each in the color it is set to.
    const float column = (w - 56.0f) / 3.0f, face = 128.0f;
    for (int i = 0; i < 3; ++i) {
        const float cx = x + 28.0f + column * (i + 0.5f);
        p.Text(kTeamSlotNames[i], cx, y + 118.0f, 0.56f, A_MC, kFaint);
        DrawFace(p, prefs_.team.chars[i], prefs_.team.colors[i], cx, y + 144.0f, face, A_TC);
        p.TextFit(CharacterLabel(roster_, prefs_.team.chars[i]), cx, y + 256.0f, 0.7f,
                  column - 10.0f, A_MC);
        if (prefs_.team.chars[i] == kCharRandom) continue; // its assist is not known yet
        char assist[48];
        snprintf(assist, sizeof(assist), "ASSIST %s", kAssistLetters[prefs_.team.assists[i] % 3]);
        p.Text(assist, cx, y + 278.0f, 0.54f, A_MC, kDim);
    }
}

const char* LobbyUi::DrawTop(Canvas& canvas) const
{
    const float t = EaseOut(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, (1.0f - t) * 36.0f);

    // "LOBBY" title art (row 0 of ga_room_subtit2), slightly enlarged.
    p.Sprite(TEX_SUBTITLES2, 76.0f, 68.0f, 68.0f * 1.7f, 30.0f * 1.7f,
             Uv{0.0f, 1.0f / 64.0f, 68.0f / 256.0f, 31.0f / 64.0f}, A_ML);
    p.Label("Timestone v" UMVC3HOOK_VERSION, kSafeRight, 76.0f, 0.62f, A_MR, kYellow);

    DrawTeamCard(canvas, 72.0f, 168.0f, 470.0f);
    // Only the reason Steam is unusable; routine status ("Left Steam lobby") is noise here.
    if (!service_.Available())
        p.TextFit(service_.StatusText(), 84.0f, 494.0f, 0.62f, 450.0f, A_ML, kRed);

    for (int i = 0; i < kTopCount; ++i) {
        const float y = 214.0f + i * 82.0f;
        const float shift = i * 14.0f; // native main-menu stagger
        const bool selected = i == topCursor_;
        if (selected) {
            // The white bar carries a round ornament near its right end; the
            // label stops just short of it, as on the native main menu.
            // Like the native main menu's bar: full white easing to slightly
            // see-through white and back. Depth and rate are set by eye from
            // the game; its layout data holds no keys for this fade.
            p.Sprite(TEX_MENU_SEL, 1296.0f + shift, y, 640.0f, 72.0f,
                     Uv{12.0f / 512.0f, 10.0f / 64.0f, 1.0f, 58.0f / 64.0f}, A_MR,
                     WithAlpha(kWhite, MenuBarFade(frame_)));
            // The selected label as the native main menu draws it
            // (main_menu01 in ma_menu_main): mScale (0.8, 0.92), which a font
            // unit applies twice, white, with the menu's ink blend. Same
            // proportions here, at 85% of that size to suit the lobby's bar.
            p.Text(kTopItems[i], 1104.0f + shift, y - 2.0f, 0.54f, A_MR, kWhite, FONT_HEADING,
                   0.8464f / 0.64f, BLEND_MENU_INK);
        } else {
            p.Sprite(TEX_MENU_BAR, 1296.0f + shift, y, 600.0f, 58.0f,
                     Uv{14.0f / 512.0f, 12.0f / 64.0f, 452.0f / 512.0f, 52.0f / 64.0f}, A_MR,
                     WithAlpha(kWhite, 0.7f));
            p.Text(kTopItems[i], 1104.0f + shift, y - 2.0f, 0.44f, A_MR, kText, FONT_HEADING);
        }
    }

    return kTopHelp[topCursor_];
}

const char* LobbyUi::DrawBrowse(Canvas& canvas) const
{
    const float t = EaseOut(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, (1.0f - t) * 36.0f);

    p.Sprite(TEX_SEARCH_RESULTS, 60.0f, 66.0f, 200.0f * 1.15f, 40.0f * 1.15f,
             Uv{28.0f / 256.0f, 12.0f / 64.0f, 228.0f / 256.0f, 52.0f / 64.0f}, A_ML);
    p.TextFit(service_.StatusText(), kSafeRight, 70.0f, 0.7f, 620.0f, A_MR, kDim);

    p.Label("LOBBY", 150.0f, 122.0f, 0.6f, A_ML, kYellow);
    p.Label("HOST", 556.0f, 122.0f, 0.6f, A_ML, kYellow);
    p.Label("PLAYERS", 812.0f, 122.0f, 0.6f, A_ML, kYellow);
    p.Label("CONNECTION", 1032.0f, 122.0f, 0.6f, A_ML, kYellow);

    const int count = service_.ResultCount();
    const float rowWidth = kSafeRight - kSafeLeft;
    for (int row = 0; row < kBrowseRows; ++row) {
        const int index = browseScroll_ + row;
        if (index >= count) break;
        const SearchResult& result = service_.Result(index);
        const float y = 178.0f + row * 66.0f;
        const bool selected = index == browseCursor_;
        const Painter r = p.Faded(result.compatible ? 1.0f : 0.5f);

        // The parallelogram row plate, pushed to slate so white text reads.
        // A friends-only lobby is green: plate, name and a tag by the host.
        const Color plate = result.friendsOnly
            ? (selected ? Color{0.3f, 0.52f, 0.34f, 0.96f} : Color{0.13f, 0.3f, 0.18f, 0.9f})
            : (selected ? Color{0.42f, 0.44f, 0.5f, 0.96f} : Color{0.2f, 0.22f, 0.28f, 0.9f});
        r.Box(TEX_SLANT_ROW, kSafeLeft, y - 29.0f, rowWidth, 58.0f, plate);
        if (selected) {
            const float pulse = 0.5f + 0.22f * SelectionWave(frame_);
            p.Box(TEX_SLANT_SEL, kSafeLeft, y - 29.0f, rowWidth, 58.0f, WithAlpha(kWhite, pulse));
        }
        // The host's icon. A Steam picture needs their id; without one
        // (a lobby from an older version) it is the default icon.
        const Tex hostIcon = result.hostIcon > kIconSteam || result.hostId
            ? IconTex(result.hostId, result.hostIcon) : TEX_USER_ICON;
        r.Sprite(hostIcon, 104.0f, y, 38.0f, 38.0f, kFullUv, A_ML);
        r.TextFit(result.name, 152.0f, y, 0.86f, 420.0f, A_ML,
                  result.friendsOnly ? kGreen : kText);
        if (result.friendsOnly) {
            r.TextFit(result.host, 556.0f, y - 8.0f, 0.74f, 240.0f, A_ML, kDim);
            r.Text("FRIENDS ONLY", 556.0f, y + 13.0f, 0.46f, A_ML, kGreen);
        } else {
            r.TextFit(result.host, 556.0f, y, 0.74f, 240.0f, A_ML, kDim);
        }

        // One head per seat: host first, then members, then empty seats.
        for (int seat = 0; seat < result.maxMembers && seat < kMaxMembers; ++seat) {
            const int cell = seat == 0 ? 0 : seat < result.members ? 1 : 2;
            r.Sprite(TEX_MEMBER_ICONS, 812.0f + seat * 19.0f, y, 20.0f, 40.0f,
                     Uv{cell * 0.125f, 0.0f, (cell + 1) * 0.125f, 1.0f}, A_ML);
        }
        char players[16];
        snprintf(players, sizeof(players), "%d/%d", result.members, result.maxMembers);
        r.Text(players, 1008.0f, y, 0.74f, A_MR,
               result.members >= result.maxMembers ? kRed : kText);

        if (!result.compatible) {
            p.Text(result.versionOrder < 0 ? "OLD VERSION" : result.versionOrder > 0 ? "NEW VERSION"
                                           : "VERSION MISMATCH", 1032.0f, y, 0.5f, A_ML, kRed);
        } else {
            DrawPingWithNumber(r, 1032.0f, y - (result.matchInProgress ? 8.0f : 0.0f), result.pingMs,
                               1.0f, 50.0f, 36.0f, 0.56f);
            if (result.matchInProgress)
                r.Text("MATCH IN PROGRESS", 1032.0f, y + 13.0f, 0.46f, A_ML, kYellow);
        }
        if (result.compatible) {
            char format[12];
            if (result.firstTo == kFirstToEndless) snprintf(format, sizeof(format), "END");
            else snprintf(format, sizeof(format), "FT%d", result.firstTo);
            r.Text(format, 1136.0f, y - (result.matchInProgress ? 8.0f : 0.0f), 0.6f, A_ML, kDim);
        }
    }

    if (count == 0) {
        const char* message = service_.Busy() == RoomBusy::Searching ? "Searching..."
                             : !service_.Available() ? service_.StatusText()
                             : "Could not find a lobby.";
        p.Label(message, kScreenW * 0.5f, 340.0f, 1.0f, A_MC);
        if (service_.Busy() == RoomBusy::None && service_.Available())
            p.Label("Create your own lobby and do battle with all comers.", kScreenW * 0.5f,
                    384.0f, 0.7f, A_MC, kDim);
    } else if (count > kBrowseRows) {
        char page[32];
        snprintf(page, sizeof(page), "%d / %d", browseCursor_ + 1, count);
        p.Label(page, kSafeRight, 640.0f, 0.62f, A_MR, kDim);
    }
    if (service_.Busy() == RoomBusy::Joining)
        p.Label("Joining lobby.", kScreenW * 0.5f, 640.0f, 0.8f, A_MC, kCyan);

    return "Please select the lobby you wish to participate in.";
}

const char* LobbyUi::DrawCreate(Canvas& canvas) const
{
    const float t = EaseOut(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, (1.0f - t) * 36.0f);

    // "CREATE LOBBY" title art: row 1 of ga_room_subtit.
    p.Sprite(TEX_SUBTITLES, 76.0f, 68.0f, 144.0f * 1.5f, 30.0f * 1.5f,
             Uv{0.0f, 33.0f / 256.0f, 144.0f / 256.0f, 63.0f / 256.0f}, A_ML);

    const float panelX = 170.0f, panelY = 140.0f, panelW = 940.0f, panelH = 470.0f;
    p.Box(TEX_PANEL_DARK, panelX, panelY, panelW, panelH);
    p.Text("LOBBY SETTING", panelX + 44.0f, panelY + 44.0f, 0.4f, A_ML, kYellow, FONT_HEADING);

    const RoomSettings& draft = prefs_.create;
    char value[kChatCapacity + 8];
    const char* labels[CREATE_ROWS] = {"LOBBY NAME", "LOBBY SIZE", "SETUPS", "VISIBILITY",
                                       "MATCH FORMAT", "ROTATION", "LOBBY MUSIC", ""};
    for (int row = 0; row < CREATE_ROWS; ++row) {
        const float y = panelY + 104.0f + row * 46.0f;
        const bool selected = row == createCursor_;
        if (selected) {
            const float pulse = SelectionPulse(frame_);
            p.Box(TEX_ROW_SEL, panelX + 26.0f, y - 23.0f, panelW - 52.0f, 46.0f,
                  WithAlpha(kWhite, pulse));
        }
        if (row == CREATE_GO) {
            p.Text(service_.Busy() == RoomBusy::Creating ? "CREATING LOBBY..." : "CREATE LOBBY",
                   panelX + panelW * 0.5f, y, 0.46f, A_MC, kText, FONT_HEADING);
            continue;
        }
        p.Text(labels[row], panelX + 64.0f, y, 0.86f, A_ML, selected ? kText : kDim);
        switch (row) {
        case CREATE_NAME:
            snprintf(value, sizeof(value), "%s",
                     textTarget_ == TextTarget::RoomName ? text_ : draft.name);
            break;
        case CREATE_SIZE: snprintf(value, sizeof(value), "%d PLAYERS", draft.maxMembers); break;
        case CREATE_SLOTS: snprintf(value, sizeof(value), "%d", draft.slotCount); break;
        case CREATE_FORMAT:
            if (draft.firstTo == kFirstToEndless) snprintf(value, sizeof(value), "ENDLESS");
            else snprintf(value, sizeof(value), "FIRST TO %d", draft.firstTo);
            break;
        case CREATE_ROTATION:
            snprintf(value, sizeof(value), "%s",
                     draft.firstTo == kFirstToEndless ? "-" : RotationName(draft.rotation));
            break;
        case CREATE_MUSIC: {
            const int track = FindMusicTrack(draft.music);
            snprintf(value, sizeof(value), "%s", kMusicTracks[track < 0 ? 0 : track].name);
            break;
        }
        default:
            snprintf(value, sizeof(value), "%s", kVisibilityNames[static_cast<int>(draft.visibility) % 3]);
            break;
        }
        const float valueX = panelX + 660.0f;
        if (row == CREATE_NAME && textTarget_ == TextTarget::RoomName) {
            // The caret blinks just past the name, outside its centring, so
            // the name itself holds still.
            char fitted[sizeof(value)];
            FitText(fitted, sizeof(fitted), value, 440.0f, 0.86f);
            p.Text(fitted, valueX, y, 0.86f, A_MC, kCyan);
            if ((frame_ / 20) % 2)
                p.Text("_", valueX + TextWidth(fitted) * 0.86f * 0.5f + 2.0f, y, 0.86f, A_ML, kCyan);
            continue;
        }
        // A track name is never cut: the row has no arrows, so it gets the
        // name's width and is drawn smaller if it still does not fit.
        if (row == CREATE_MUSIC)
            p.TextShrink(value, valueX, y, 0.86f, 440.0f, A_MC, kText);
        else
            p.TextFit(value, valueX, y, 0.86f, row == CREATE_NAME ? 440.0f : kValueTextWidth, A_MC,
                      kText);
        if (selected && row != CREATE_NAME && row != CREATE_MUSIC &&
            !(row == CREATE_ROTATION && draft.firstTo == kFirstToEndless)) {
            DrawValueArrows(p, frame_, valueX, y, kValueArrowSpan);
        }
    }

    const char* const help[CREATE_ROWS] = {
        "Set your lobby title.",
        "Set the maximum number of players in your lobby.",
        "How many tables are in the lobby.",
        "Choose who can find and join this lobby.",
        "Games needed to win a set. Endless is rematch until someone leaves.",
        "Who makes way for the next player in line when a set ends.",
        "Choose the music everyone in the lobby hears.",
        "Create a lobby.",
    };
    if (musicOpen_) {
        DrawMusicMenu(canvas);
        return nullptr;
    }
    return textTarget_ == TextTarget::RoomName ? "Type a name, then confirm."
                                               : help[createCursor_];
}

void LobbyUi::DrawRoomMembers(Canvas& canvas) const
{
    const float t = Clamp01(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, 0.0f);
    const RoomState& room = service_.Room();

    const float x = 40.0f, top = 110.0f, width = 430.0f;
    for (int i = 0; i < room.settings.maxMembers && i < kMaxMembers; ++i) {
        const float y = top + i * 50.0f;
        const bool occupied = i < room.memberCount;
        // Native member rows are tan plates; slate keeps white names legible.
        p.Box(TEX_ROW, x, y, width, 44.0f,
              occupied ? Color{0.22f, 0.24f, 0.3f, 0.94f} : Color{0.16f, 0.17f, 0.2f, 0.45f});
        if (!occupied) continue;

        const Member& member = room.members[i];
        const float cy = y + 22.0f;
        if (memberFocus_ && i == memberCursor_) {
            const float pulse = SelectionPulse(frame_);
            p.Box(TEX_ROW_SEL, x - 4.0f, y - 3.0f, width + 8.0f, 50.0f, WithAlpha(kWhite, pulse));
        }
        // Small HOST / GUEST tag: rows 6 and 7 of the tag sheet.
        p.Sprite(TEX_TAGS, x + 12.0f, cy, 46.0f, 24.0f,
                 Uv{11.0f / 64.0f, (member.isHost ? 196.0f : 228.0f) / 256.0f, 53.0f / 64.0f,
                    (member.isHost ? 220.0f : 252.0f) / 256.0f},
                 A_ML);
        p.Sprite(IconTex(member.id, member.icon), x + 64.0f, cy, 34.0f, 34.0f, kFullUv, A_ML);
        p.TextFit(member.name, x + 106.0f, cy, 0.78f, 172.0f, A_ML, member.isLocal ? kCyan : kText);

        // Status: BATTLE (row 0) while fighting, WATCH (row 2) while spectating.
        if (member.status == MemberStatus::Fighting || member.status == MemberStatus::Watching) {
            const int tagRow = member.status == MemberStatus::Fighting ? 0 : 2;
            p.Sprite(TEX_TAGS, x + 288.0f, cy, 54.0f, 22.0f,
                     Uv{1.0f / 64.0f, (tagRow * 32 + 4) / 256.0f, 63.0f / 64.0f,
                        (tagRow * 32 + 28) / 256.0f},
                     A_ML);
        } else if (member.status == MemberStatus::Ready) {
            p.Text("READY", x + 288.0f, cy, 0.56f, A_ML, kYellow);
        } else if (member.status == MemberStatus::Seated) {
            p.Text("SEATED", x + 288.0f, cy, 0.56f, A_ML, kDim);
        } else if (member.queueSlot >= 0) {
            // Waiting: which match, and how far from the front.
            char wait[16];
            snprintf(wait, sizeof(wait), "S%d #%d", member.queueSlot + 1, member.queuePos);
            p.Text(wait, x + 288.0f, cy, 0.56f, A_ML, member.queuePos == 1 ? kCyan : kDim);
        }
        if (!member.isLocal) DrawPingWithNumber(p, x + 355.0f, cy, member.pingMs, 0.66f, 24.0f, 25.0f, 0.5f);
    }
}

void LobbyUi::DrawRoomSlots(Canvas& canvas) const
{
    const float t = EaseOut(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, (1.0f - t) * 30.0f);
    const RoomState& room = service_.Room();
    const int slots = room.settings.slotCount;
    if (slots <= 0) return;

    // One or two slots stack full-width; three or four use a 2x2 grid.
    const float areaX = 492.0f, areaY = 110.0f, areaW = 748.0f, areaH = 536.0f;
    const int columns = slots > 2 ? 2 : 1;
    const int rows = slots > 2 ? 2 : slots;
    const float gap = 14.0f;
    const float cardW = (areaW - gap * (columns - 1)) / columns;
    float cardH = (areaH - gap * (rows - 1)) / rows;
    if (cardH > 262.0f) cardH = 262.0f;

    const Member* local = LocalMember();
    for (int slot = 0; slot < slots; ++slot) {
        const float x = areaX + (slot % columns) * (cardW + gap);
        const float y = areaY + (slot / columns) * (cardH + gap);
        const SlotState state = room.slots[slot];
        const bool live = state == SlotState::InProgress;
        // Who is waiting for this slot, front of the line first.
        const Member* nextUp = nullptr;
        int waiting = 0;
        bool localWaiting = false;
        for (int i = 0; i < room.memberCount; ++i) {
            const Member& member = room.members[i];
            if (member.queueSlot != slot) continue;
            ++waiting;
            localWaiting |= member.isLocal;
            if (member.queuePos == 1) nextUp = &member;
        }
        // The slot the local player is in line for stands out in dark blue;
        // one they only watch (no place in its line), in green.
        const bool localWatching = !localWaiting && local && local->slot < 0 &&
                                   service_.WatchIntent() == slot;
        p.Box(TEX_PANEL_DARK, x, y, cardW, cardH,
              localWaiting ? Color{0.5f, 0.68f, 1.0f, 1.0f}
              : localWatching ? Color{0.55f, 1.0f, 0.6f, 1.0f} : kWhite);
        if (localWaiting)
            p.Fill(x + 5.0f, y + 5.0f, cardW - 10.0f, cardH - 10.0f, Color{0.08f, 0.2f, 0.55f, 0.34f});
        else if (localWatching)
            p.Fill(x + 5.0f, y + 5.0f, cardW - 10.0f, cardH - 10.0f, Color{0.08f, 0.42f, 0.16f, 0.34f});

        // Grid cards (three or four slots) are too narrow for names in the
        // header: the queue is a number only there.
        const bool narrowCard = cardW < 500.0f;
        char title[24];
        snprintf(title, sizeof(title), "SETUP %d", slot + 1);
        p.Text(title, x + 26.0f, y + 30.0f, 0.36f, A_ML, kYellow, FONT_HEADING);
        // Right side: the slot's state, then to its left how many are in line.
        float labelLeft = x + cardW - 26.0f;
        if (live) {
            // "MATCH IN PROGRESS" word art, breathing; a size down on grid cards.
            const float artScale = narrowCard ? 0.5f : 0.64f;
            const float pulse = 0.72f + 0.28f * SelectionWave(frame_);
            p.Sprite(TEX_WORDS_B, x + cardW - 24.0f, y + 30.0f, 235.0f * artScale, 37.0f * artScale,
                     Uv{11.0f / 256.0f, 13.0f / 256.0f, 246.0f / 256.0f, 50.0f / 256.0f}, A_MR,
                     WithAlpha(kWhite, pulse));
            labelLeft = x + cardW - 24.0f - 235.0f * artScale;
        } else {
            p.Text(kSlotStateNames[static_cast<int>(state)], x + cardW - 26.0f, y + 30.0f, 0.58f,
                   A_MR, state == SlotState::Starting ? kGreen : kDim);
            labelLeft = x + cardW - 26.0f - 92.0f;
        }
        char count[8];
        snprintf(count, sizeof(count), "%d", waiting);
        const Color countColor = localWaiting ? kCyan : waiting ? kText : kFaint;
        p.Text(count, labelLeft - 14.0f, y + 31.0f, 0.7f, A_MR, countColor);
        p.Sprite(TEX_FRIENDS, labelLeft - 14.0f - (waiting > 9 ? 30.0f : 18.0f), y + 30.0f, 24.0f,
                 24.0f, kFullUv, A_MR, WithAlpha(kWhite, waiting ? 1.0f : 0.45f));
        // Spectators, when there are any: the WATCH tag and how many.
        const int watchers = SlotWatcherCount(slot);
        float headerLeft = x + 150.0f;
        if (watchers > 0) {
            // On a grid card it has to fit between the title and the queue count.
            const float tagScale = narrowCard ? 0.62f : 1.0f;
            const float tagX = narrowCard ? x + 121.0f : x + 150.0f;
            p.Sprite(TEX_TAGS, tagX, y + 30.0f, 54.0f * tagScale, 22.0f * tagScale,
                     Uv{1.0f / 64.0f, 68.0f / 256.0f, 63.0f / 64.0f, 92.0f / 256.0f}, A_ML);
            char watching[8];
            snprintf(watching, sizeof(watching), "%d", watchers);
            p.Text(watching, tagX + 54.0f * tagScale + (narrowCard ? 3.0f : 5.0f), y + 31.0f,
                   narrowCard ? 0.54f : 0.7f, A_ML, kText);
            headerLeft = tagX + 54.0f * tagScale + 34.0f;
        }
        if (nextUp && !narrowCard) {
            char line[96];
            snprintf(line, sizeof(line), "NEXT: %.40s", nextUp->name);
            const float left = headerLeft;
            const float right = labelLeft - 76.0f;
            if (right - left > 80.0f)
                p.TextFit(line, left, y + 31.0f, 0.58f, right - left, A_ML,
                          nextUp->isLocal ? kCyan : kDim);
        }

        const float seatTop = y + 54.0f;
        const float seatH = (cardH - 54.0f - 14.0f - 8.0f) * 0.5f;
        for (int side = 0; side < 2; ++side) {
            const float sy = seatTop + side * (seatH + 8.0f);
            const float cy = sy + seatH * 0.5f;
            const Member* occupant = SeatOccupant(slot, side);
            const bool focused = slot == seatSlot_ && side == seatSide_ && !roomMenuOpen_ &&
                                 !quickChatOpen_ && !musicOpen_ && !memberFocus_ &&
                                 !chatFocus_ && !chatExpanded_;
            const bool mine = occupant && local && occupant->id == local->id;

            // The focus frame goes under the seat so it never veils the text.
            if (focused) {
                const float pulse = SelectionPulse(frame_, 0.4f);
                p.Box(TEX_ROW_SEL, x + 10.0f, sy - 5.0f, cardW - 20.0f, seatH + 10.0f,
                      WithAlpha(kWhite, pulse));
            }
            p.Box(mine ? TEX_PANEL_BLUE : TEX_PANEL_DOT, x + 16.0f, sy, cardW - 32.0f, seatH,
                  WithAlpha(kWhite, occupant ? 1.0f : 0.72f));

            // PLAYER 1 / PLAYER 2 word art: rows 0 and 1.
            p.Sprite(TEX_WORDS_A, x + 32.0f, cy - (occupant && live ? 13.0f : 0.0f), 74.0f, 23.0f,
                     Uv{2.0f / 128.0f, (side * 32 + 4) / 256.0f, 76.0f / 128.0f,
                        (side * 32 + 27) / 256.0f},
                     A_ML, WithAlpha(kWhite, occupant ? 1.0f : 0.6f));
            if (!occupant) {
                if (state < SlotState::Starting)
                    p.Text("OPEN SEAT", x + 122.0f, cy, 0.7f, A_ML, focused ? kText : kFaint);
                continue;
            }

            // Narrow (grid) cards move the READY lamp to the team line so the
            // player name keeps the full first line.
            // Before the match only the name shows: a team on display here
            // would be an invitation to counterpick, and it can still change.
            // Once the match is running the teams are locked and listed.
            const float nameY = live ? cy - 13.0f : cy;
            p.TextFit(occupant->name, x + 122.0f, nameY, 0.84f, cardW - 122.0f - 32.0f - 44.0f, A_ML,
                      mine ? kYellow : kText);
            if (live) {
                // The team as portraits, each with its life bar, as the game's
                // own lobby shows a match in progress.
                const float rowLeft = x + 32.0f;
                const float cell = (cardW - 64.0f - 44.0f) / 3.0f;
                const float icon = 28.0f;
                for (int i = 0; i < 3; ++i) {
                    const float ix = rowLeft + i * cell;
                    const uint8_t health = occupant->health[i];
                    const bool down = health == 0;
                    p.Sprite(CharacterIcon(occupant->team.chars[i]), ix, cy + 17.0f, icon, icon,
                             kFullUv, A_ML, down ? Color{0.35f, 0.35f, 0.38f, 1.0f} : kWhite);
                    const float barX = ix + icon + 6.0f;
                    const float barW = cell - icon - 6.0f - 12.0f;
                    if (barW < 12.0f) continue;
                    // The native lobby's own bar (HP_Bat1P_blk/yel in
                    // net_room_player1): on the 256x32 sheet the black track
                    // is columns 3..204 of rows 5..10 and the yellow fill
                    // columns 4..203 of rows 22..25. The fill is cut, not
                    // squeezed, to the life left.
                    p.Sprite(TEX_HP_BAR, barX, cy + 17.0f, barW, 8.0f,
                             Uv{4.0f / 256.0f, 6.0f / 32.0f, 204.0f / 256.0f, 10.0f / 32.0f}, A_ML);
                    if (health == kHealthUnknown || down) continue;
                    const float ratio = health / 100.0f;
                    // The tint multiplies the sheet's yellow: untouched above
                    // half, then orange, then red.
                    const Color tint = ratio > 0.5f ? kWhite
                                     : ratio > 0.25f ? Color{1.0f, 0.65f, 0.6f, 1.0f}
                                                     : Color{1.0f, 0.3f, 0.3f, 1.0f};
                    p.Sprite(TEX_HP_BAR, barX + 1.0f, cy + 17.0f, (barW - 2.0f) * ratio, 6.0f,
                             Uv{5.0f / 256.0f, 22.5f / 32.0f, (5.0f + 198.0f * ratio) / 256.0f,
                                25.5f / 32.0f},
                             A_ML, tint);
                }
            }
            // This player's games in the set being played.
            char wins[8];
            snprintf(wins, sizeof(wins), "%d", occupant->setWins);
            p.Text(wins, x + cardW - 40.0f, cy, 1.0f, A_MR, live ? kText : kFaint);
        }

        if (state == SlotState::Starting) {
            // "FIGHT!" punches in while the two players connect.
            const float beat = 0.78f + 0.06f * std::sin(frame_ * 0.4f);
            p.Sprite(TEX_WORDS_B, x + cardW * 0.5f, y + 30.0f, 139.0f * beat, 44.0f * beat,
                     Uv{59.0f / 256.0f, 74.0f / 256.0f, 198.0f / 256.0f, 118.0f / 256.0f}, A_MC);
        }
    }
}

void LobbyUi::DrawRoomChat(Canvas& canvas) const
{
    const float t = Clamp01(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, 0.0f);
    // Expanded, the panel grows upward over the player list.
    const float x = kChatX, w = kChatW;
    // `open` runs 0..1 as it slides; the bottom edge stays where it is.
    const float open = EaseOut(chatExpandFrames_ / static_cast<float>(kChatExpandFrames));
    const float y = 516.0f + (110.0f - 516.0f) * open;
    const float h = 130.0f + (536.0f - 130.0f) * open;
    // Space under the last row: the expanded panel keeps a line for the count.
    const float bottomGap = 24.0f + (52.0f - 24.0f) * open;
    if (chatFocus_ && !chatExpanded_) {
        const float pulse = SelectionPulse(frame_);
        p.Box(TEX_ROW_SEL, x - 4.0f, y - 4.0f, w + 8.0f, h + 8.0f, WithAlpha(kWhite, pulse));
    }
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    if (open > 0.0f) // twice over: the room behind must not show through text
        p.Fill(x + 5.0f, y + 5.0f, w - 10.0f, h - 10.0f, Color{0.02f, 0.03f, 0.06f, 0.7f * open});

    const bool typing = textTarget_ == TextTarget::Chat;
    // As many rows as the panel has room for at this point of the slide.
    int rows = static_cast<int>((h - bottomGap - 22.0f) / 21.0f + 0.01f) + 1;
    if (rows < kChatRows) rows = kChatRows;
    if (rows > kChatExpandedRows) rows = kChatExpandedRows;
    const int visible = typing ? rows - 1 : rows;
    const int count = service_.ChatCount();
    const int scroll = chatExpanded_ ? chatScroll_ : 0;

    // Long messages wrap, so the window is counted in display rows. Walk back
    // from the newest message, skipping `scroll` rows, until it is full.
    struct Row {
        int message;
        ChatSpan span;
        bool first; // first row of its message: carries the sender
    };
    Row window[kChatExpandedRows];
    int filled = 0, skipped = 0;
    for (int i = count - 1; i >= 0 && filled < visible; --i) {
        const ChatLine& line = service_.Chat(i);
        ChatSpan spans[kChatMaxWrap];
        const int wrapped = WrapChat(line.text, line.system ? kChatSystemW : kChatTextW, spans);
        for (int r = wrapped - 1; r >= 0 && filled < visible; --r) {
            if (skipped < scroll) {
                ++skipped;
                continue;
            }
            window[filled++] = Row{i, spans[r], r == 0};
        }
    }
    if (chatExpanded_) {
        // The line at the bottom of the view, out of every line there is.
        const int total = ChatRowsFrom(0);
        char position[32];
        snprintf(position, sizeof(position), "%d / %d", total - skipped, total);
        p.Text(position, x + w - 22.0f, y + h - 16.0f, 0.5f, A_MR, WithAlpha(kFaint, kFaint.a * open));
    }
    // `window` runs newest first; the oldest row goes on the top line.
    for (int slot = 0; slot < filled; ++slot) {
        const Row& row = window[filled - 1 - slot];
        const ChatLine& line = service_.Chat(row.message);
        // A full window hangs from the bottom edge, so its lines hold still
        // while the panel slides; a short history stays under the top edge.
        const float rowY = filled == visible
            ? y + h - bottomGap - (rows - 1 - slot) * 21.0f
            : y + 22.0f + slot * 21.0f;
        char text[kChatCapacity + 4];
        snprintf(text, sizeof(text), "%.*s", row.span.length, line.text + row.span.start);
        if (line.system) {
            p.Text(text, x + 22.0f, rowY, kChatScale, A_ML, kYellow);
            continue;
        }
        if (row.first) {
            char sender[48];
            FitText(sender, sizeof(sender), line.sender, 110.0f, kChatScale);
            p.Text(sender, x + 22.0f, rowY, kChatScale, A_ML, kCyan);
        }
        p.Text(text, x + kChatTextX, rowY, kChatScale, A_ML);
    }
    if (count == 0 && !typing)
        p.Text("Say hello to the lobby.", x + 22.0f, y + 22.0f, 0.56f, A_ML, kFaint);
    if (typing) {
        char entry[kChatCapacity + 8];
        snprintf(entry, sizeof(entry), "> %s%s", text_, (frame_ / 20) % 2 ? "_" : " ");
        // Keep the caret visible by showing the tail of long messages.
        const char* shown = entry;
        while (TextWidth(shown) * 0.6f > w - 44.0f && shown[1]) {
            ++shown;
            while ((static_cast<unsigned char>(*shown) & 0xC0) == 0x80) ++shown;
        }
        p.Text(shown, x + 22.0f, y + h - 22.0f, 0.6f, A_ML, kText);
    }

    // Cursor on the chat: its shortcuts pop up on the panel's top edge.
    if (chatHoverFrames_ > 0 && !typing && !quickChatOpen_) {
        const float t = Clamp01(chatHoverFrames_ / 8.0f), u = t - 1.0f;
        const float pop = 1.0f + 2.70158f * u * u * u + 1.70158f * u * u; // ease out, overshooting
        const Prompt prompts[] = {{BTN_Y, "Type"}, {BTN_QUICK_CHAT, "Quick chat"}};
        const int first = usingKeyboard_ ? 0 : 1; // typing needs a keyboard
        const float glyph = 30.0f * pop, scale = 0.56f * pop, gap = 16.0f * pop;
        float tagW = 20.0f - gap;
        for (int i = first; i < 2; ++i) tagW += glyph + 6.0f + TextWidth(prompts[i].label) * scale + gap;
        // Centred just above the top edge, clear of the first line of chat.
        const float right = x + w - 12.0f, cy = y - 6.0f;
        const Painter tag = p.Faded(Clamp01(chatHoverFrames_ / 4.0f));
        tag.Fill(right - tagW, cy - 17.0f * pop, tagW, 34.0f * pop, Color{0.0f, 0.0f, 0.0f, 0.9f});
        float at = right - 10.0f;
        for (int i = 1; i >= first; --i) {
            tag.Text(prompts[i].label, at, cy, scale, A_MR, kText);
            at -= TextWidth(prompts[i].label) * scale + 6.0f;
            DrawGlyph(tag, prompts[i].button, usingKeyboard_, at, cy, glyph);
            at -= glyph + gap;
        }
    }
}

void LobbyUi::DrawListMenu(Canvas& canvas, const char* title, const char* const* items,
                           int count, int cursor) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});
    const float w = 440.0f;
    const float h = 96.0f + count * 44.0f;
    const float x = (kScreenW - w) * 0.5f;
    const float y = (kScreenH - h) * 0.5f - 10.0f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.TextFit(title, x + w * 0.5f, y + 40.0f, 0.38f, w - 60.0f, A_MC, kYellow, FONT_HEADING);
    for (int i = 0; i < count; ++i) {
        const float rowY = y + 88.0f + i * 44.0f;
        if (i == cursor) {
            const float pulse = SelectionPulse(frame_);
            p.Box(TEX_ROW_SEL, x + 22.0f, rowY - 24.0f, w - 44.0f, 48.0f, WithAlpha(kWhite, pulse));
        }
        p.Text(items[i], x + w * 0.5f, rowY, 0.8f, A_MC, i == cursor ? kText : kDim);
    }
}

const char* LobbyUi::DrawRoom(Canvas& canvas) const
{
    const float t = Clamp01(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, 0.0f);
    const RoomState& room = service_.Room();

    // Title bar: room name on the left, population on the right.
    p.Sprite(TEX_TITLE_BAR, 40.0f, 62.0f, 1200.0f, 52.0f,
             Uv{4.0f / 1024.0f, 8.0f / 64.0f, 1020.0f / 1024.0f, 56.0f / 64.0f}, A_ML);
    const float kNameScale = 0.44f, kNameMax = 460.0f;
    p.TextFit(room.settings.name, 64.0f, 60.0f, kNameScale, kNameMax, A_ML, kText, FONT_HEADING);
    // What the host has playing, so a change is not a mystery to the room.
    const int track = FindMusicTrack(room.music);
    char music[96];
    if (room.settings.firstTo == kFirstToEndless)
        snprintf(music, sizeof(music), "ENDLESS   BGM: %s", kMusicTracks[track < 0 ? 0 : track].name);
    else
        snprintf(music, sizeof(music), "FT%d   %s   BGM: %s", room.settings.firstTo,
                 RotationName(room.settings.rotation), kMusicTracks[track < 0 ? 0 : track].name);
    // Shrunk to fit, never cut. It takes whatever the lobby name leaves free,
    // so the long soundtrack titles stay full size beside a short name.
    float nameWidth = TextWidth(room.settings.name, FONT_HEADING) * kNameScale;
    if (nameWidth > kNameMax) nameWidth = kNameMax;
    p.TextShrink(music, 1060.0f, 61.0f, 0.62f, 1060.0f - 64.0f - nameWidth - 28.0f, A_MR, kDim);
    p.Sprite(TEX_FRIENDS, 1098.0f, 62.0f, 40.0f, 40.0f, kFullUv, A_MC);
    char population[24];
    snprintf(population, sizeof(population), "%d / %d", room.memberCount, room.settings.maxMembers);
    p.Text(population, 1216.0f, 61.0f, 0.86f, A_MR);

    // The fully expanded chat covers the list.
    if (!chatExpanded_ || chatExpandFrames_ < kChatExpandFrames) DrawRoomMembers(canvas);
    DrawRoomSlots(canvas);
    DrawRoomChat(canvas);

    // Kept short: the room footer carries the most button prompts.
    const char* help = "Battle begins when both are ready.";
    const Member* local = LocalMember();
    if (textTarget_ == TextTarget::Chat)
        help = "Type a message, then confirm.";
    else if (local && local->queueSlot >= 0)
        help = service_.WatchIntent() == local->queueSlot
            ? "You are in line: you watch the match until it is your turn."
            : "You are in line. You will be seated when it is your turn.";
    else if (local && local->slot < 0 && service_.WatchIntent() >= 0)
        help = "You will watch this setup's matches. Stop Watching in a match ends it.";
    else if (local && local->slot < 0)
        help = "Take a seat.";

    if (roomMenuOpen_) {
        // Guests get a mute toggle where the host has the music picker.
        const char* items[kRoomMenuCount];
        for (int i = 0; i < kRoomMenuCount; ++i) items[i] = kRoomMenuItems[i];
        if (!local || !local->isHost)
            items[1] = prefs_.muteMusic ? "UNMUTE LOBBY MUSIC" : "MUTE LOBBY MUSIC";
        DrawListMenu(canvas, "LOBBY MENU", items, kRoomMenuCount, roomMenuCursor_);
    } else if (slotMenuOpen_) {
        char title[16];
        snprintf(title, sizeof(title), "SETUP %d", seatSlot_ + 1);
        const char* slotItems[3];
        const int slotItemCount = SlotMenuItems(slotItems);
        DrawListMenu(canvas, title, slotItems, slotItemCount, slotMenuCursor_);
    } else if (quickChatOpen_)
        DrawListMenu(canvas, "CHAT", kQuickChat, kQuickChatCount, quickChatCursor_);
    else if (musicOpen_)
        DrawMusicMenu(canvas);
    else if (HandshakeStep() != Handshake::None && textTarget_ == TextTarget::None)
        DrawHandshake(canvas);
    else if (memberMenuOpen_) {
        const Member* self = LocalMember();
        const bool canKick = self && self->isHost && memberMenuId_ != self->id;
        const char* const items[] = {"VIEW STEAM PROFILE", "KICK FROM LOBBY"};
        DrawListMenu(canvas, memberMenuName_, items, canKick ? 2 : 1, memberMenuCursor_);
    } else if (chatExpanded_ && textTarget_ == TextTarget::None)
        help = "Scroll back through the lobby's chat.";
    else if (chatFocus_ && textTarget_ == TextTarget::None)
        help = "Expand the chat to read back through it.";
    else if (memberFocus_ && textTarget_ == TextTarget::None)
        help = "Select a player to view their profile.";
    return roomMenuOpen_ || quickChatOpen_ || musicOpen_ || memberMenuOpen_ ||
                   HandshakeStep() != Handshake::None
               ? nullptr : help;
}

void LobbyUi::DrawMusicMenu(Canvas& canvas) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});
    const float w = 860.0f; // the group label plus the longest soundtrack title
    const float h = 136.0f + kMusicRows * 44.0f;
    const float x = (kScreenW - w) * 0.5f;
    const float y = (kScreenH - h) * 0.5f - 10.0f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Text("LOBBY MUSIC", x + w * 0.5f, y + 40.0f, 0.38f, A_MC, kYellow, FONT_HEADING);

    // Keep the cursor in the middle of the window where the list allows it.
    int first = musicCursor_ - kMusicRows / 2;
    if (first > kMusicTrackCount - kMusicRows) first = kMusicTrackCount - kMusicRows;
    if (first < 0) first = 0;
    const int playing = screen_ == Screen::Room ? service_.Room().music : prefs_.create.music;
    for (int row = 0; row < kMusicRows && first + row < kMusicTrackCount; ++row) {
        const int index = first + row;
        const MusicTrack& track = kMusicTracks[index];
        const float rowY = y + 88.0f + row * 44.0f;
        const bool selected = index == musicCursor_;
        if (selected) {
            const float pulse = SelectionPulse(frame_);
            p.Box(TEX_ROW_SEL, x + 22.0f, rowY - 24.0f, w - 44.0f, 48.0f, WithAlpha(kWhite, pulse));
        }
        const bool current = track.id == playing;
        p.Text(MusicGroupName(track.group), x + 48.0f, rowY, 0.56f, A_ML,
               current ? kCyan : kDim);
        p.TextShrink(track.name, x + 190.0f, rowY, 0.8f, w - 190.0f - 48.0f, A_ML,
                     current ? kCyan : selected ? kText : kDim);
    }
    char position[24];
    snprintf(position, sizeof(position), "%d / %d", musicCursor_ + 1, kMusicTrackCount);
    p.Text(position, x + w - 48.0f, y + h - 30.0f, 0.62f, A_MR, kDim);
}

// A team as the wide cards the game shows before a battle, three across `w`,
// each with its assist named underneath.
constexpr float kBannerGap = 0.0f; // the cards touch: each has its own black edge
float TeamBannerHeight(float w) { return (w - 2.0f * kBannerGap) / 3.0f * kBannerAspect; }

// Names the player typed are drawn in the heading font only when it has
// every character: it is a Latin display face.
Font HeadingFontFor(const char* text)
{
    for (const char* c = text; *c; ++c)
        if (static_cast<unsigned char>(*c) >= 0x7F) return FONT_BODY;
    return FONT_HEADING;
}

void LobbyUi::DrawTeamBanners(Canvas& canvas, float x, float y, float w) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    const float cardW = (w - 2.0f * kBannerGap) / 3.0f, cardH = TeamBannerHeight(w);
    for (int i = 0; i < 3; ++i) {
        const float cardX = x + i * (cardW + kBannerGap);
        const int32_t character = prefs_.team.chars[i];
        const int color = prefs_.team.colors[i];
        // The card's archive is read in the background the first time it is
        // wanted; until it is in, its place is a dark plate.
        p.Fill(cardX, y, cardW, cardH, Color{0.0f, 0.0f, 0.0f, 0.6f});
        if (character == kCharRandom) {
            // No card to show until the match draws one: the RANDOM icon on
            // the plate, and its name where the assist would be.
            const float iconH = cardH * 0.72f;
            p.Sprite(TEX_STAGE_FIRST, cardX + cardW * 0.5f, y + cardH * 0.5f,
                     iconH / kStageIconAspect, iconH, kStageIconUv, A_MC);
            p.Text("RANDOM", cardX + cardW * 0.5f, y + cardH + 16.0f, 0.56f, A_MC, kDim);
            continue;
        }
        p.Sprite(CharacterBanner(character, color), cardX, y, cardW, cardH, kBannerUv, A_TL);
        const char* move = roster_.assistName
            ? roster_.assistName(character, prefs_.team.assists[i]) : "";
        char assist[96];
        snprintf(assist, sizeof(assist), "%s  %s", kAssistLetters[prefs_.team.assists[i] % 3],
                 move ? move : "");
        p.TextShrink(assist, cardX + cardW * 0.5f, y + cardH + 16.0f, 0.56f, cardW - 8.0f, A_MC,
                     kDim);
    }
}

// The pre-match prompt: one panel whose body changes with the step.
void LobbyUi::DrawHandshake(Canvas& canvas) const
{
    const Handshake step = HandshakeStep();
    const Member* local = LocalMember();
    const Member* other = local ? SeatOccupant(local->slot, 1 - local->side) : nullptr;
    if (step == Handshake::None || !local || !other) return;

    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.6f});
    // The start choice shows the team it starts with, so it is taller.
    const float w = 760.0f, h = step == Handshake::Entry ? 404.0f : 300.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 12.0f;
    const float cx = x + w * 0.5f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Sprite(TEX_YELLOW_BAR, x + 30.0f, y + 26.0f, w - 60.0f, 3.0f, Uv{0.1f, 0.2f, 0.9f, 0.8f}, A_TL);

    char line[160];
    snprintf(line, sizeof(line), "VS  %.48s", other->name);
    p.TextFit(line, cx, y + 62.0f, 0.44f, w - 400.0f, A_MC, kYellow, FONT_HEADING);
    // Connection to this opponent, on every step.
    DrawPing(p, x + 44.0f, y + 62.0f, other->pingMs, 0.8f);
    if (other->pingMs >= 0) {
        snprintf(line, sizeof(line), "%d ms", other->pingMs);
        p.Text(line, x + 128.0f, y + 62.0f, 0.62f, A_ML, kDim);
    }
    // What "Auto" comes to against this opponent, so it is never a blind pick.
    const int autoDelay = service_.AutoDelay(other->pingMs);

    const auto option = [&](const char* label, float ox, float oy, float ow, bool selected) {
        if (selected) {
            const float pulse = SelectionPulse(frame_);
            p.Box(TEX_ROW_SEL, ox - ow * 0.5f, oy - 24.0f, ow, 48.0f, WithAlpha(kWhite, pulse));
        }
        p.Text(label, ox, oy, 0.4f, A_MC, selected ? kText : kDim, FONT_HEADING);
    };
    const auto delayName = [autoDelay](int pick, char* out, size_t size) {
        if (pick == kDelayPickNone) snprintf(out, size, "...");
        else if (pick == kDelayPickAuto && autoDelay >= 0) snprintf(out, size, "AUTO (%d)", autoDelay);
        else if (pick == kDelayPickAuto) snprintf(out, size, "AUTO");
        else snprintf(out, size, "%d", pick - 1);
    };
    const auto delayValue = [autoDelay](int pick) { return pick > kDelayPickAuto ? pick - 1 : autoDelay; };
    uint32_t limit = 0;
    uint32_t elapsed = handshakeFrames_;
    switch (step) {
    case Handshake::Delay:
    case Handshake::WaitDelay: {
        p.Text("INPUT DELAY", cx, y + 108.0f, 0.36f, A_MC, kText, FONT_HEADING);
        char mine[24], theirs[24];
        delayName(step == Handshake::Delay ? delayCursor_ : local->delayPick, mine, sizeof(mine));
        // Their locked pick, or until then whatever they have highlighted.
        const bool theirsLocked = other->accepted && other->delayPick != kDelayPickNone;
        delayName(theirsLocked ? other->delayPick : other->delayHover, theirs, sizeof(theirs));
        p.Text("YOU", cx - 180.0f, y + 150.0f, 0.6f, A_MC, kDim);
        p.Text(mine, cx - 180.0f, y + 188.0f, 1.0f, A_MC, step == Handshake::Delay ? kText : kGreen);
        if (step == Handshake::Delay) DrawValueArrows(p, frame_, cx - 180.0f, y + 188.0f, 110.0f);
        p.TextFit(other->name, cx + 180.0f, y + 150.0f, 0.6f, 300.0f, A_MC, kDim);
        p.Text(theirs, cx + 180.0f, y + 188.0f, 1.0f, A_MC, theirsLocked ? kGreen : kText);
        p.Text(step == Handshake::Delay ? "The higher of the two picks is used."
                                        : "Waiting for your opponent to choose.",
               cx, y + 244.0f, 0.62f, A_MC, kDim);
        if (step == Handshake::Delay) limit = kDelayTimeout;
        break;
    }
    case Handshake::Entry: {
        // START and the team it starts with on one row: left and right go
        // through the player's teams, and the three cards follow.
        const float rowW = w - 80.0f, rowY = y + 116.0f;
        const bool start = handshakeCursor_ == 0;
        if (start)
            p.Box(TEX_ROW_SEL, cx - rowW * 0.5f, rowY - 24.0f, rowW, 48.0f,
                  WithAlpha(kWhite, SelectionPulse(frame_)));
        p.Text("START", cx - rowW * 0.5f + 34.0f, rowY, 0.4f, A_ML, start ? kText : kDim,
               FONT_HEADING);
        char name[kPresetNameCapacity];
        SlotName(prefs_.slot, name, sizeof(name));
        const float valueX = cx + 70.0f, arrowSpan = rowW * 0.5f - 90.0f;
        const Font nameFont = HeadingFontFor(name);
        p.TextFit(name, valueX, rowY, nameFont == FONT_HEADING ? 0.4f : 0.9f,
                  2.0f * (arrowSpan - 28.0f), A_MC, start ? kYellow : kDim, nameFont);
        if (start && NextUsedSlot(prefs_.slot, 1) != prefs_.slot)
            DrawValueArrows(p, frame_, valueX, rowY, arrowSpan);
        const float cardsY = y + 152.0f, cardsW = w - 80.0f;
        DrawTeamBanners(canvas, x + 40.0f, cardsY, cardsW);
        const float selectY = cardsY + TeamBannerHeight(cardsW) + 64.0f;
        option("CHARACTER SELECT", cx, selectY, rowW, !start);
        p.Text(start ? "Go into battle with this team." : "Choose or edit your team first.", cx,
               selectY + 44.0f, 0.62f, A_MC, kDim);
        {
            // Both picks are in: the delay the match will run at.
            const int mine = delayValue(local->delayPick), theirs = delayValue(other->delayPick);
            if (mine >= 0 && theirs >= 0) {
                snprintf(line, sizeof(line), "Input delay: %d", mine > theirs ? mine : theirs);
                p.Text(line, cx, selectY + 74.0f, 0.62f, A_MC, kCyan);
            }
        }
        limit = kEntryTimeout;
        elapsed = entryFrames_ - teamFrames_;
        break;
    }
    default:
        p.Text("Ready", cx, y + 130.0f, 0.9f, A_MC, kGreen);
        p.Text("Waiting for opponent.", cx, y + 172.0f, 0.74f, A_MC, kDim);
        if (step == Handshake::WaitStart) {
            // The opponent's choice first. Still waiting after that means
            // they are in character select: then the most that can take.
            limit = entryFrames_ < kEntryTimeout ? kEntryTimeout : kEntryTimeout + kTeamTimeout;
            elapsed = entryFrames_;
        }
        break;
    }
    if (limit) {
        char clock[16];
        snprintf(clock, sizeof(clock), "%u", elapsed < limit ? (limit - elapsed + 59) / 60 : 0);
        p.Text(clock, x + w - 48.0f, y + 62.0f, 0.8f, A_MR, kDim);
    }
}

const char* LobbyUi::DrawTeam(Canvas& canvas) const
{
    const float t = EaseOut(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, (1.0f - t) * 36.0f);

    p.Text("TEAM SETUP", 76.0f, 70.0f, 0.62f, A_ML, kText, FONT_HEADING);

    const float panelX = 60.0f, panelY = 126.0f, panelW = 660.0f, panelH = 520.0f;
    p.Box(TEX_PANEL_DARK, panelX, panelY, panelW, panelH);

    char value[96];
    for (int row = 0; row < kTeamRows; ++row) {
        const float y = panelY + 40.0f + row * 36.5f;
        const bool selected = row == teamCursor_;
        if (selected) {
            const float pulse = SelectionPulse(frame_);
            p.Box(TEX_ROW_SEL, panelX + 20.0f, y - 23.0f, panelW - 40.0f, 46.0f,
                  WithAlpha(kWhite, pulse));
        }
        const float labelX = panelX + 50.0f;
        const float valueX = panelX + 430.0f;
        const char* label = "";
        value[0] = '\0';
        Color valueColor = kText;
        bool randomRow = false; // an assist or color that cannot be set

        if (row == kTeamRowPreset) {
            label = "PRESET";
            SlotName(prefs_.slot, value, sizeof(value));
            valueColor = kYellow;
        } else if (row == kTeamRowIcon) {
            label = "ICON";
            if (prefs_.icon == kIconSteam)
                snprintf(value, sizeof(value), "STEAM PICTURE");
            else
                snprintf(value, sizeof(value), "ICON %d", prefs_.icon);
            valueColor = kYellow;
        } else if (row == kTeamRowStage) {
            label = "PREFERRED STAGE";
            snprintf(value, sizeof(value), "%s", StageName(prefs_.stage));
            valueColor = kYellow;
        } else if (row == kTeamRowDone) {
            p.Text(teamForMatch_ ? "START" : "DONE", panelX + panelW * 0.5f, y, 0.4f, A_MC, kText,
                   FONT_HEADING);
            continue;
        } else {
            const int member = (row - kTeamRowFirstSlot) / 3;
            const int field = (row - kTeamRowFirstSlot) % 3;
            const int32_t character = prefs_.team.chars[member];
            if (field == 0) {
                label = kTeamSlotNames[member];
                snprintf(value, sizeof(value), "%s", CharacterLabel(roster_, character));
            } else if (field == 1) {
                label = "   ASSIST";
                const char* assist = roster_.assistName
                    ? roster_.assistName(character, prefs_.team.assists[member]) : "";
                snprintf(value, sizeof(value), "%s  %s", kAssistLetters[prefs_.team.assists[member] % 3],
                         assist ? assist : "");
                valueColor = kDim;
            } else {
                label = "   COLOR";
                snprintf(value, sizeof(value), "%s", ColorLabel(prefs_.team.colors[member]));
                valueColor = kDim;
            }
            // A random member's assist and color are drawn with it.
            if (field != 0 && character == kCharRandom) {
                snprintf(value, sizeof(value), "-");
                valueColor = kDim;
                randomRow = true;
            }
        }
        const bool header = row == kTeamRowPreset || row == kTeamRowIcon || row == kTeamRowStage ||
                            (row >= kTeamRowFirstSlot && row < kTeamRowIcon &&
                             (row - kTeamRowFirstSlot) % 3 == 0);
        p.Text(label, labelX, y, header ? 0.8f : 0.68f, A_ML,
               selected || header ? kText : kFaint);
        // An assist name is never cut: it gets the width of the panel's
        // value column and, past that, is drawn smaller.
        const bool assistRow = row >= kTeamRowFirstSlot && row < kTeamRowIcon &&
                               (row - kTeamRowFirstSlot) % 3 == 1;
        float arrowSpan = kValueArrowSpan;
        if (assistRow) {
            const float scale = p.TextShrink(value, valueX, y, 0.7f, kAssistTextWidth, A_MC,
                                             selected && !randomRow ? kText : valueColor);
            const float half = TextWidth(value) * scale * 0.5f + 24.0f;
            if (half > arrowSpan) arrowSpan = half;
        } else {
            p.TextFit(value, valueX, y, header ? 0.8f : 0.7f, kValueTextWidth, A_MC,
                      selected && !randomRow ? kText : valueColor);
        }
        // Teams, characters, colors and the icon are picked from a list or
        // grid, not stepped.
        const bool gridRow = row == kTeamRowIcon || row == kTeamRowPreset || row == kTeamRowStage ||
                             (row >= kTeamRowFirstSlot && row < kTeamRowIcon &&
                              (row - kTeamRowFirstSlot) % 3 != 1);
        if (selected && !gridRow && !randomRow) {
            DrawValueArrows(p, frame_, valueX, y, arrowSpan);
        }
    }

    DrawTeamCard(canvas, 752.0f, 126.0f, 470.0f);
    if (teamForMatch_) p.Label("YOUR OPPONENT IS WAITING", 764.0f, 452.0f, 0.62f, A_ML, kYellow);
    // Opened against an opponent: the seconds left before the match starts
    // with the team as it stands, in the title row's right corner. Drawn
    // after the lists and grids so it stays in sight over them.
    const auto matchClock = [&](const char* help) {
        if (!teamForMatch_) return help;
        const uint32_t left = teamFrames_ < kTeamTimeout
            ? (kTeamTimeout - teamFrames_ + 59) / 60 : 0;
        char clock[16];
        snprintf(clock, sizeof(clock), "%u", left);
        const Painter fixed = p.Fixed();
        fixed.Label("MATCH STARTS IN", 1150.0f, 70.0f, 0.62f, A_MR, kDim);
        fixed.Text(clock, 1224.0f, 70.0f, 0.62f, A_MR, left <= 10 ? kRed : kYellow, FONT_HEADING);
        return help;
    };

    if (presetListOpen_) {
        DrawPresetList(canvas);
        if (textTarget_ == TextTarget::PresetName) return matchClock("Type a name, then confirm.");
        return matchClock(SlotUsed(presetListCursor_)
                              ? "Choose the team to use and edit."
                              : "A free slot starts as a copy of your team.");
    }
    if (stagePickerOpen_) {
        DrawStagePicker(canvas);
        return matchClock("Choose the stage you would like to fight on.");
    }
    if (charPickerOpen_) {
        DrawCharPicker(canvas);
        return matchClock("Choose a character.");
    }
    if (colorPickerOpen_) {
        DrawColorPicker(canvas);
        return matchClock("Choose a color.");
    }
    if (iconPickerOpen_) {
        DrawIconPicker(canvas);
        return matchClock("Choose the icon other players see.");
    }

    const char* help = teamForMatch_ ? "Choose your team, then select START."
                                     : "Edit your team.";
    if (teamCursor_ == kTeamRowPreset)
        help = "Choose the team to use, or add, rename and delete teams.";
    else if (teamCursor_ == kTeamRowIcon)
        help = "Your Steam profile picture, or one of the game's icons.";
    else if (teamCursor_ == kTeamRowStage)
        help = "A match is fought on your stage or your opponent's, picked at random.";
    return matchClock(help);
}

// Every team slot: the ten presets, then the game's reserve units under a
// rule, each with its name and its three faces.
void LobbyUi::DrawPresetList(Canvas& canvas) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});
    const int reserve = roster_.presetCount ? roster_.presetCount() : 0;
    const int rows = kTeamPresets + reserve;
    const float rowH = 38.0f, reserveGap = reserve > 0 ? 14.0f : 0.0f;
    const float w = 860.0f, h = 84.0f + rows * rowH + reserveGap + 18.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kFooterY - h) * 0.5f;
    // The panel texture is translucent; the team rows under it must not show.
    p.Fill(x + 4.0f, y + 4.0f, w - 8.0f, h - 8.0f, Color{0.02f, 0.03f, 0.05f, 0.92f});
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Text("TEAM PRESETS", x + w * 0.5f, y + 36.0f, 0.38f, A_MC, kYellow, FONT_HEADING);

    for (int slot = 0; slot < rows; ++slot) {
        const bool unit = slot >= kTeamPresets;
        const float rowY = y + 84.0f + slot * rowH + (unit ? reserveGap : 0.0f);
        if (slot == kTeamPresets)
            p.Sprite(TEX_YELLOW_BAR, x + 30.0f, rowY - rowH * 0.5f - reserveGap * 0.5f - 1.0f,
                     w - 60.0f, 2.0f, Uv{0.1f, 0.2f, 0.9f, 0.8f}, A_TL, WithAlpha(kWhite, 0.5f));
        const bool selected = slot == presetListCursor_;
        if (selected)
            p.Box(TEX_ROW_SEL, x + 22.0f, rowY - 20.0f, w - 44.0f, 40.0f,
                  WithAlpha(kWhite, SelectionPulse(frame_)));
        // No slot number: a preset's default name already carries it.
        const float nameX = x + 56.0f, nameW = 500.0f;
        if (selected && textTarget_ == TextTarget::PresetName) {
            // The caret blinks just past the name being typed.
            char fitted[sizeof(text_)];
            FitText(fitted, sizeof(fitted), text_, nameW, 0.74f);
            p.Text(fitted, nameX, rowY, 0.74f, A_ML, kCyan);
            if ((frame_ / 20) % 2)
                p.Text("_", nameX + TextWidth(fitted) * 0.74f + 2.0f, rowY, 0.74f, A_ML, kCyan);
        }
        Team team;
        if (!SlotTeam(slot, &team)) {
            p.Text("- EMPTY -", nameX, rowY, 0.66f, A_ML, kFaint);
            continue;
        }
        if (!(selected && textTarget_ == TextTarget::PresetName)) {
            char name[kPresetNameCapacity];
            SlotName(slot, name, sizeof(name));
            // The team in use is the one in cyan.
            p.TextFit(name, nameX, rowY, 0.74f, nameW, A_ML,
                      selected ? kText : slot == prefs_.slot ? kCyan : kDim);
        }
        const float face = 48.0f;
        for (int i = 0; i < 3; ++i)
            DrawFace(p, team.chars[i], team.colors[i], x + 600.0f + i * 76.0f,
                     rowY - face * kFaceAspect * 0.5f, face, A_TC);
    }
}

// The stage grid, centred on the screen: the stage select screen's icons,
// its RANDOM icon standing for no preference.
void LobbyUi::DrawStagePicker(Canvas& canvas) const
{
    const int stages = roster_.stageCount && roster_.stageId ? roster_.stageCount() : 0;
    const int count = stages + 1;
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});

    const int rows = (count + kStageGridColumns - 1) / kStageGridColumns;
    const float cell = 150.0f, cellH = 96.0f, icon = 134.0f;
    const float w = kStageGridColumns * cell + 72.0f;
    const float h = rows * cellH + 128.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 20.0f;
    // The panel texture is translucent; the team rows under it must not show.
    p.Fill(x + 4.0f, y + 4.0f, w - 8.0f, h - 8.0f, Color{0.02f, 0.03f, 0.05f, 0.92f});
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Text("PREFERRED STAGE", x + w * 0.5f, y + 36.0f, 0.36f, A_MC, kYellow, FONT_HEADING);

    const float gridX = x + 36.0f, gridY = y + 66.0f;
    const auto stageAt = [&](int index) { return index == 0 ? kStageAny : roster_.stageId(index - 1); };
    for (int index = 0; index < count; ++index) {
        const float cx = gridX + (index % kStageGridColumns) * cell + cell * 0.5f;
        const float cy = gridY + (index / kStageGridColumns) * cellH + cellH * 0.5f;
        const int stage = stageAt(index);
        const bool current = stage == prefs_.stage;
        const bool selected = index == stagePickerCursor_;
        if (selected) {
            p.Box(TEX_ROW_SEL, cx - cell * 0.5f - 3.0f, cy - cellH * 0.5f - 3.0f, cell + 6.0f,
                  cellH + 6.0f);
        } else if (current) {
            p.Box(TEX_ROW_SEL, cx - cell * 0.5f, cy - cellH * 0.5f, cell, cellH,
                  WithAlpha(kCyan, 0.6f));
        }
        const float size = selected ? icon + 6.0f : icon;
        p.Sprite(StageIcon(stage), cx, cy, size, size * kStageIconAspect, kStageIconUv, A_MC);
    }
    const int highlighted = stageAt(stagePickerCursor_ < count ? stagePickerCursor_ : 0);
    p.TextFit(StageName(highlighted), x + w * 0.5f, y + h - 30.0f, 0.8f, w - 120.0f, A_MC,
              highlighted == prefs_.stage ? kCyan : kText);
}

// The character grid, centred on the screen: every character's face, the
// slot's current character in the color it is set to.
void LobbyUi::DrawCharPicker(Canvas& canvas) const
{
    const int count = roster_.characterCount ? roster_.characterCount() : 0;
    if (count <= 0) return;
    const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});

    const bool random = RandomOffered();
    const int rows = (count + kCharGridColumns - 1) / kCharGridColumns + (random ? 1 : 0);
    const float cell = 88.0f, cellH = 68.0f, face = 80.0f;
    const float w = kCharGridColumns * cell + 72.0f;
    const float h = rows * cellH + 128.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 20.0f;
    // The panel texture is translucent; the team rows under it must not show.
    p.Fill(x + 4.0f, y + 4.0f, w - 8.0f, h - 8.0f, Color{0.02f, 0.03f, 0.05f, 0.92f});
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Text(kTeamSlotNames[member], x + w * 0.5f, y + 36.0f, 0.36f, A_MC, kYellow, FONT_HEADING);

    const float gridX = x + 36.0f, gridY = y + 66.0f;
    for (int index = 0; index < count; ++index) {
        const float cx = gridX + (index % kCharGridColumns) * cell + cell * 0.5f;
        const float cy = gridY + (index / kCharGridColumns) * cellH + cellH * 0.5f;
        const int32_t id = roster_.characterId(index);
        const bool current = id == prefs_.team.chars[member];
        const bool selected = index == charPickerCursor_;
        if (selected) {
            // A frame wider than the cell, and the face itself a size up.
            p.Box(TEX_ROW_SEL, cx - cell * 0.5f - 5.0f, cy - cellH * 0.5f - 5.0f, cell + 10.0f,
                  cellH + 10.0f);
        } else if (current) {
            p.Box(TEX_ROW_SEL, cx - cell * 0.5f, cy - cellH * 0.5f, cell, cellH,
                  WithAlpha(kCyan, 0.6f));
        }
        const float size = selected ? face + 8.0f : face;
        p.Sprite(CharacterFace(id, current ? prefs_.team.colors[member] : 0), cx, cy, size,
                 size * kFaceAspect, kFaceUv, A_MC);
    }
    if (random) {
        // RANDOM, alone on the last row and centred under the roster.
        const float cx = x + w * 0.5f, cy = gridY + (rows - 1) * cellH + cellH * 0.5f;
        const bool selected = charPickerCursor_ >= count;
        if (selected) {
            p.Box(TEX_ROW_SEL, cx - cell * 0.5f - 5.0f, cy - cellH * 0.5f - 5.0f, cell + 10.0f,
                  cellH + 10.0f);
        } else if (prefs_.team.chars[member] == kCharRandom) {
            p.Box(TEX_ROW_SEL, cx - cell * 0.5f, cy - cellH * 0.5f, cell, cellH,
                  WithAlpha(kCyan, 0.6f));
        }
        DrawFace(p, kCharRandom, 0, cx, cy, selected ? face + 8.0f : face, A_MC);
    }
    const int32_t highlighted = charPickerCursor_ >= count ? kCharRandom
                                                           : roster_.characterId(charPickerCursor_);
    p.TextFit(CharacterLabel(roster_, highlighted), x + w * 0.5f, y + h - 30.0f, 0.8f, w - 120.0f,
              A_MC, highlighted == prefs_.team.chars[member] ? kCyan : kText);
}

// The color grid, centred on the screen: the slot's character once per color,
// labelled as the game's select screen labels them.
void LobbyUi::DrawColorPicker(Canvas& canvas) const
{
    const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
    const int32_t id = prefs_.team.chars[member];
    const int count = roster_.colorCount ? roster_.colorCount(id) : 1;
    if (count <= 0) return;
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});

    const int columns = count < kColorGridColumns ? count : kColorGridColumns;
    const int rows = (count + kColorGridColumns - 1) / kColorGridColumns;
    const float cell = 116.0f, cellH = 116.0f, face = 100.0f;
    const float w = columns * cell + 72.0f;
    const float h = rows * cellH + 56.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 20.0f;
    // The panel texture is translucent; the team rows under it must not show.
    p.Fill(x + 4.0f, y + 4.0f, w - 8.0f, h - 8.0f, Color{0.02f, 0.03f, 0.05f, 0.92f});
    p.Box(TEX_PANEL_DARK, x, y, w, h);

    const float gridX = x + (w - columns * cell) * 0.5f, gridY = y + 32.0f;
    for (int color = 0; color < count; ++color) {
        const float cx = gridX + (color % kColorGridColumns) * cell + cell * 0.5f;
        const float top = gridY + (color / kColorGridColumns) * cellH;
        const float cy = top + 44.0f;
        const bool current = color == prefs_.team.colors[member];
        const bool selected = color == colorPickerCursor_;
        // Framed like the character grid: around the face, the label below it.
        const float faceH = face * kFaceAspect;
        if (selected) {
            p.Box(TEX_ROW_SEL, cx - face * 0.5f - 9.0f, cy - faceH * 0.5f - 9.0f, face + 18.0f,
                  faceH + 18.0f);
        } else if (current) {
            p.Box(TEX_ROW_SEL, cx - face * 0.5f - 4.0f, cy - faceH * 0.5f - 4.0f, face + 8.0f,
                  faceH + 8.0f, WithAlpha(kCyan, 0.6f));
        }
        const float size = selected ? face + 8.0f : face;
        p.Sprite(CharacterFace(id, color), cx, cy, size, size * kFaceAspect, kFaceUv, A_MC);
        // The game names eight; any further installed color goes by its number.
        char label[8];
        if (color < 8) snprintf(label, sizeof(label), "%s", ColorLabel(color));
        else snprintf(label, sizeof(label), "%d", color + 1);
        p.Text(label, cx, top + cellH - 20.0f, 0.62f, A_MC,
               selected ? kText : current ? kCyan : kDim);
    }
}

// The icon grid, centred on the screen. Cell 0 is the Steam profile picture.
void LobbyUi::DrawIconPicker(Canvas& canvas) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.55f});

    const float cell = 62.0f, icon = 52.0f;
    const float w = kIconGridColumns * cell + 72.0f;
    const float h = kIconGridRows * cell + 128.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 20.0f;
    // The panel texture is translucent; the team rows under it must not show.
    p.Fill(x + 4.0f, y + 4.0f, w - 8.0f, h - 8.0f, Color{0.02f, 0.03f, 0.05f, 0.92f});
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Text("ICON", x + w * 0.5f, y + 36.0f, 0.36f, A_MC, kYellow, FONT_HEADING);

    const int count = kIconMax + 1;
    const float gridX = x + 36.0f, gridY = y + 66.0f;
    for (int row = 0; row < kIconGridRows; ++row) {
        for (int column = 0; column < kIconGridColumns; ++column) {
            const int index = (iconScroll_ + row) * kIconGridColumns + column;
            if (index >= count) break;
            const float cx = gridX + column * cell + cell * 0.5f;
            const float cy = gridY + row * cell + cell * 0.5f;
            const bool selected = index == iconCursor_;
            if (selected) {
                // A frame wider than the cell, and the icon itself a size up.
                const float frame = cell + 10.0f;
                p.Box(TEX_ROW_SEL, cx - frame * 0.5f, cy - frame * 0.5f, frame, frame);
            } else if (index == prefs_.icon) {
                // The icon in use now.
                p.Box(TEX_ROW_SEL, cx - cell * 0.5f, cy - cell * 0.5f, cell, cell,
                      WithAlpha(kCyan, 0.6f));
            }
            const float size = selected ? icon + 8.0f : icon;
            p.Sprite(IconTex(0, index), cx, cy, size, size, kFullUv, A_MC);
        }
    }
    char position[40];
    if (iconCursor_ == kIconSteam)
        snprintf(position, sizeof(position), "STEAM PICTURE");
    else
        snprintf(position, sizeof(position), "ICON %d / %d", iconCursor_, kIconMax);
    p.Text(position, x + w * 0.5f, y + h - 30.0f, 0.62f, A_MC, kDim);
}

bool LobbyUi::DrawModal(Canvas& canvas) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    if (updateActive_) return false; // DrawUpdateProgress has the screen
    const char* message = nullptr;
    if (busy_[0])
        message = busy_;
    else if (notice_[0])
        message = notice_;
    else if (confirm_ == Confirm::LeaveRoom)
        message = "Leave this lobby?";
    else if (confirm_ == Confirm::Update)
        message = updateOffer_;
    char kick[160];
    if (!message && confirm_ == Confirm::DeletePreset) {
        snprintf(kick, sizeof(kick), "Delete %.*s?", kPresetNameCapacity,
                 presetListCursor_ >= 0 && presetListCursor_ < kTeamPresets
                     ? prefs_.presets[presetListCursor_].name : "");
        message = kick;
    }
    if (!message && confirm_ == Confirm::KickMember) {
        snprintf(kick, sizeof(kick), "Kick %.40s?\nThey will not be able to rejoin this lobby.",
                 memberMenuName_);
        message = kick;
    }
    if (!message) return false;

    // The whole message is always shown: a line too wide for the panel is
    // wrapped at a space (or mid-word when it has none), never cut short.
    const float w = 720.0f;
    constexpr float kMessageScale = 0.86f;
    constexpr int kMaxLines = 12;
    const float lineLimit = (w - 80.0f) / kMessageScale;
    char wrapped[768];
    size_t used = 0, lineStart = 0, lastSpace = 0;
    bool haveSpace = false;
    for (const char* c = message; *c && used + 2 < sizeof(wrapped); ++c) {
        if (*c == '\n') {
            wrapped[used++] = '\n';
            lineStart = used;
            haveSpace = false;
            continue;
        }
        if (*c == ' ') {
            lastSpace = used;
            haveSpace = true;
        }
        wrapped[used++] = *c;
        // Only measured at the end of a character, not inside a UTF-8 sequence.
        if ((static_cast<unsigned char>(c[1]) & 0xC0) == 0x80) continue;
        wrapped[used] = '\0';
        if (TextWidth(wrapped + lineStart) <= lineLimit) continue;
        if (haveSpace) {
            wrapped[lastSpace] = '\n';
            lineStart = lastSpace + 1;
        } else {
            // One unbroken run wider than the panel: break before this character.
            size_t at = used - 1;
            while (at > lineStart && (static_cast<unsigned char>(wrapped[at]) & 0xC0) == 0x80) --at;
            if (at == lineStart) continue; // a single character; nothing to break
            memmove(wrapped + at + 1, wrapped + at, used - at);
            wrapped[at] = '\n';
            ++used;
            lineStart = at + 1;
        }
        haveSpace = false;
    }
    wrapped[used] = '\0';
    message = wrapped;

    int lineCount = 1;
    for (const char* c = message; *c; ++c)
        if (*c == '\n') ++lineCount;
    if (lineCount > kMaxLines) lineCount = kMaxLines;
    const bool asking = confirm_ != Confirm::None && !busy_[0] && !notice_[0];

    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.6f});
    // A question grows by a line height for each line past two, so the
    // answers stay clear of the text.
    const float h = asking ? 210.0f + (lineCount > 2 ? (lineCount - 2) * 30.0f : 0.0f)
                           : 170.0f + (lineCount > 3 ? (lineCount - 3) * 30.0f : 0.0f);
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 12.0f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Sprite(TEX_YELLOW_BAR, x + 30.0f, y + 26.0f, w - 60.0f, 3.0f, Uv{0.1f, 0.2f, 0.9f, 0.8f}, A_TL);

    // One line of the wrapped message at a time.
    char line[256];
    const char* cursor = message;
    int lineIndex = 0;
    const float firstY = asking ? y + 76.0f - (lineCount > 1 ? 15.0f : 0.0f)
                                : y + h * 0.5f - (lineCount - 1) * 15.0f;
    while (*cursor && lineIndex < kMaxLines) {
        size_t length = 0;
        while (cursor[length] && cursor[length] != '\n') ++length;
        const size_t copy = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
        memcpy(line, cursor, copy);
        line[copy] = '\0';
        p.Text(line, kScreenW * 0.5f, firstY + lineIndex * 30.0f, kMessageScale, A_MC);
        cursor += length;
        if (*cursor == '\n') ++cursor;
        ++lineIndex;
    }

    if (busy_[0]) {
        // Three travelling dots as a native-style wait indicator.
        for (int i = 0; i < 3; ++i) {
            const float phase = std::sin(frame_ * 0.14f - i * 0.9f);
            p.Fill(kScreenW * 0.5f - 22.0f + i * 18.0f, y + h - 36.0f, 8.0f, 8.0f,
                   WithAlpha(kYellow, 0.35f + 0.65f * Clamp01(phase)));
        }
    } else if (!notice_[0] && confirm_ != Confirm::None) {
        const char* const options[2] = {"YES", "NO"};
        for (int i = 0; i < 2; ++i) {
            const float optionX = kScreenW * 0.5f + (i == 0 ? -110.0f : 110.0f);
            if (i == confirmCursor_) {
                const float pulse = SelectionPulse(frame_);
                p.Box(TEX_ROW_SEL, optionX - 90.0f, y + h - 82.0f, 180.0f, 50.0f,
                      WithAlpha(kWhite, pulse));
            }
            p.Text(options[i], optionX, y + h - 57.0f, 0.4f, A_MC,
                   i == confirmCursor_ ? kText : kDim, FONT_HEADING);
        }
    }
    return true;
}

// The updater's progress: what it is doing, a bar, and the byte counts.
void LobbyUi::DrawUpdateProgress(Canvas& canvas) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.6f});
    const float w = 760.0f, h = 220.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 12.0f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Sprite(TEX_YELLOW_BAR, x + 30.0f, y + 26.0f, w - 60.0f, 3.0f, Uv{0.1f, 0.2f, 0.9f, 0.8f}, A_TL);
    p.Text("TIMESTONE UPDATE", kScreenW * 0.5f, y + 56.0f, 0.42f, A_MC, kYellow, FONT_HEADING);
    p.TextFit(update_.stage, kScreenW * 0.5f, y + 96.0f, 0.8f, w - 80.0f, A_MC);

    const float barX = x + 50.0f, barY = y + 124.0f, barW = w - 100.0f, barH = 22.0f;
    p.Fill(barX - 2.0f, barY - 2.0f, barW + 4.0f, barH + 4.0f, WithAlpha(kFaint, 0.8f));
    p.Fill(barX, barY, barW, barH, Color{0.0f, 0.0f, 0.0f, 0.85f});
    if (update_.fraction >= 0.0f) {
        const float filled = barW * Clamp01(update_.fraction);
        if (filled > 0.0f) {
            p.Fill(barX, barY, filled, barH, kYellow);
            // A highlight sweeping along the filled part keeps it lively.
            const float sweep = std::fmod(frame_ * 6.0f, barW + 120.0f) - 60.0f;
            const float from = std::max(0.0f, sweep), to = std::min(filled, sweep + 60.0f);
            if (to > from) p.Fill(barX + from, barY, to - from, barH, WithAlpha(kWhite, 0.35f));
        }
    } else {
        // Size not known yet: a block travelling back and forth.
        const float block = barW * 0.2f;
        const float t = 0.5f + 0.5f * std::sin(frame_ * 0.06f);
        p.Fill(barX + (barW - block) * t, barY, block, barH, kYellow);
    }
    p.TextFit(update_.status, kScreenW * 0.5f, y + 176.0f, 0.62f, w - 80.0f, A_MC, kDim);
}

} // namespace lobby
