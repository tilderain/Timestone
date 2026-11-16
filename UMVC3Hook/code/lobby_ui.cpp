#include "lobby_ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace lobby {

namespace {

// ---------------------------------------------------------------------------
// Palette and shared geometry (1280x720 UI space)
// ---------------------------------------------------------------------------
constexpr Color kText = kWhite;
constexpr Color kDim = Rgb(0xBAC4D2);
constexpr Color kFaint = Rgb(0x7E8A9A);
constexpr Color kYellow = Rgb(0xFFD63A);
constexpr Color kCyan = Rgb(0x5CE4FF);
constexpr Color kRed = Rgb(0xFF5A4C);
constexpr Color kGreen = Rgb(0x86F25E);
constexpr Color kInk = Rgb(0x0C1118);

constexpr float kScreenW = 1280.0f;
constexpr float kScreenH = 720.0f;
constexpr float kSafeLeft = 56.0f;
constexpr float kSafeRight = 1224.0f;
constexpr float kFooterY = 664.0f;

constexpr int kBrowseRows = 7;
constexpr int kChatRows = 5;
constexpr uint32_t kTransitionFrames = 9;

// Button glyph cells inside the 4x4 glyph sheets (mai_hlp03 / mai_hlp04).
struct GlyphCell { uint8_t col, row; };
enum Button : uint8_t { BTN_CONFIRM, BTN_CANCEL, BTN_X, BTN_Y, BTN_MENU, BTN_DPAD, BTN_COUNT };
constexpr GlyphCell kPadGlyphs[BTN_COUNT] = {{1, 1}, {0, 1}, {2, 1}, {3, 1}, {1, 3}, {0, 0}};
// The game's fixed keyboard menu mapping: Enter, Backspace, X, Y, Esc (Start).
constexpr GlyphCell kKeyGlyphs[BTN_COUNT] = {{1, 1}, {0, 1}, {2, 1}, {3, 1}, {1, 3}, {0, 0}};

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

const char* const kRoomMenuItems[] = {"TEAM SETUP", "INVITE FRIENDS", "LEAVE LOBBY"};
constexpr int kRoomMenuCount = 3;

const char* const kQuickChat[] = {
    "Type a message...", "Good fight!", "One more?", "Nice!", "Hello!",
    "Good game.", "Be right back.", "Thanks for the games!",
};
constexpr int kQuickChatCount = 8;

const char* const kVisibilityNames[] = {"PUBLIC", "FRIENDS ONLY", "PRIVATE"};
const char* const kSlotStateNames[] = {"OPEN", "WAITING", "STANDBY", "", ""};
const char* const kTeamSlotNames[] = {"POINT", "SECOND", "ANCHOR"};
const char* const kAssistLetters[] = {"A", "B", "Y"};

// Create-screen rows.
enum CreateRow { CREATE_NAME, CREATE_SIZE, CREATE_SLOTS, CREATE_VISIBILITY, CREATE_GO, CREATE_ROWS };
// Team-screen rows: preset selector, 3 x (character, assist, colour), entry, done.
constexpr int kTeamRowPreset = 0;
constexpr int kTeamRowFirstSlot = 1;
constexpr int kTeamRowEntry = 10;
constexpr int kTeamRowDone = 11;
constexpr int kTeamRows = 12;

float Clamp01(float value) { return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value; }
float EaseOut(float t) { t = 1.0f - Clamp01(t); return 1.0f - t * t * t; }

int PingLevel(int pingMs)
{
    if (pingMs < 0) return 0;
    if (pingMs < 50) return 5;
    if (pingMs < 90) return 4;
    if (pingMs < 130) return 3;
    if (pingMs < 180) return 2;
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
              Color color = kText, Font font = FONT_BODY) const
    {
        if (!text || !text[0]) return;
        canvas_.Text(text, x + dx_, y, scale, anchor, WithAlpha(color, alpha_), font);
    }

    void TextFit(const char* text, float x, float y, float scale, float maxWidth,
                 Anchor anchor, Color color = kText, Font font = FONT_BODY) const
    {
        char fitted[192];
        FitText(fitted, sizeof(fitted), text, maxWidth, scale, font);
        Text(fitted, x, y, scale, anchor, color, font);
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

void DrawPing(const Painter& p, float x, float y, int pingMs, float scale = 1.0f)
{
    // Six 32px rows; the bars occupy columns 35..222 of the 256px sheet.
    p.Sprite(TEX_PING, x, y, 94.0f * scale, 16.0f * scale,
             Uv{35.0f / 256.0f, (PingLevel(pingMs) * 32 + 4) / 256.0f, 222.0f / 256.0f,
                (PingLevel(pingMs) * 32 + 28) / 256.0f},
             A_ML);
}

void TeamSummary(const RosterSource& roster, const Team& team, char* out, size_t size)
{
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < 3; ++i) {
        const char* name = roster.characterName ? roster.characterName(team.chars[i]) : "";
        if (!name || !name[0]) name = "?";
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
}

void LobbyUi::GoTo(Screen screen)
{
    screen_ = screen;
    screenFrame_ = 0;
    roomMenuOpen_ = false;
    quickChatOpen_ = false;
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

void LobbyUi::ShowNotice(const char* text)
{
    snprintf(notice_, sizeof(notice_), "%s", text ? text : "");
}

void LobbyUi::SetBusy(const char* text)
{
    snprintf(busy_, sizeof(busy_), "%s", text ? text : "");
}

void LobbyUi::ReturnToRoom()
{
    if (!service_.Room().inRoom) return;
    screen_ = Screen::Room;
    screenFrame_ = kTransitionFrames;
    roomMenuOpen_ = false;
    quickChatOpen_ = false;
    confirm_ = Confirm::None;
    wasInRoom_ = true;
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
        service_.SetTeam(prefs_.team, prefs_.entryMode);
        seatSlot_ = 0;
        seatSide_ = 0;
        leftByChoice_ = false;
        GoTo(Screen::Room);
    } else if (!inRoom && wasInRoom_) {
        if (screen_ == Screen::Room || (screen_ == Screen::Team && teamReturn_ == Screen::Room)) {
            GoTo(Screen::Top);
            if (!leftByChoice_) ShowNotice("Lobby has been closed.");
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
    if (screenFrame_ < 100000) ++screenFrame_;
    usingKeyboard_ = input.usingKeyboard;

    SyncWithService();

    // Modal layers take input in priority order.
    if (busy_[0]) return;
    if (notice_[0]) {
        if (input.confirm || input.cancel) {
            notice_[0] = '\0';
            Play(SFX_CONFIRM);
        }
        return;
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
            } else if (action == Confirm::MainMenu) {
                mainMenuRequested_ = true;
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
        // Backing out by accident is easy, so only this path asks first.
        confirm_ = Confirm::MainMenu;
        confirmCursor_ = 1;
        Play(SFX_CANCEL);
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
            ShowNotice("This lobby is running a different Timestone version.");
        } else if (result.members >= result.maxMembers) {
            Play(SFX_CANCEL);
            ShowNotice("LOBBY IS FULL");
        } else {
            Play(SFX_CONFIRM);
            service_.Join(result.id);
        }
    }
}

void LobbyUi::UpdateCreate(const Input& input)
{
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
    if (roomMenuOpen_) {
        UpdateRoomMenu(input);
        return;
    }
    if (quickChatOpen_) {
        UpdateQuickChat(input);
        return;
    }

    const RoomState& room = service_.Room();
    const Member* local = LocalMember();
    const int seatCount = room.settings.slotCount * 2;
    if (seatCount <= 0) return;

    // Seats form a list: up/down walks seats, left/right jumps whole slots.
    int index = seatSlot_ * 2 + seatSide_;
    int next = index;
    if (input.up) --next;
    if (input.down) ++next;
    if (input.left) next -= 2;
    if (input.right) next += 2;
    next = ((next % seatCount) + seatCount) % seatCount;
    if (next != index) {
        seatSlot_ = next / 2;
        seatSide_ = next % 2;
        Play(SFX_MOVE);
    }

    const bool seated = local && local->slot >= 0 && local->side < 2;
    const bool onOwnSeat = seated && local->slot == seatSlot_ && local->side == seatSide_;

    if (input.menu) {
        roomMenuOpen_ = true;
        roomMenuCursor_ = 0;
        Play(SFX_CONFIRM);
        return;
    }
    if (input.actionY) {
        quickChatOpen_ = true;
        quickChatCursor_ = 0;
        Play(SFX_CONFIRM);
        return;
    }
    if (input.actionX && seated) {
        service_.Stand();
        Play(SFX_CANCEL);
        return;
    }
    if (input.cancel) {
        Play(SFX_CANCEL);
        if (seated && local->ready) {
            service_.SetReady(false);
        } else if (seated) {
            service_.Stand();
        } else {
            confirm_ = Confirm::LeaveRoom;
            confirmCursor_ = 1;
        }
        return;
    }
    if (!input.confirm) return;

    if (onOwnSeat) {
        if (!local->ready && prefs_.entryMode == EntryMode::CharacterSelect) {
            // "Character select" entry: pick the team first, Done readies up.
            Play(SFX_CONFIRM);
            teamReturn_ = Screen::Room;
            teamCursor_ = kTeamRowFirstSlot;
            GoTo(Screen::Team);
            return;
        }
        service_.SetReady(!local->ready);
        Play(SFX_CONFIRM);
        return;
    }
    if (SeatOccupant(seatSlot_, seatSide_) || room.slots[seatSlot_] >= SlotState::Starting) {
        Play(SFX_CANCEL);
        return;
    }
    service_.Sit(seatSlot_, seatSide_);
    Play(SFX_CONFIRM);
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
    case 1:
        service_.InviteFriends();
        break;
    default:
        confirm_ = Confirm::LeaveRoom;
        confirmCursor_ = 1;
        break;
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
    textCapacity_ = target == TextTarget::RoomName ? 40 : kChatCapacity - 1;
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

void LobbyUi::ApplyPreset(int index)
{
    Team team;
    if (index >= 0 && roster_.preset && roster_.preset(index, &team)) {
        prefs_.team = team;
        prefs_.presetIndex = index;
    } else {
        prefs_.presetIndex = -1;
    }
    prefsDirty_ = true;
}

void LobbyUi::CommitTeam()
{
    prefsDirty_ = true;
    if (service_.Room().inRoom) service_.SetTeam(prefs_.team, prefs_.entryMode);
}

void LobbyUi::StepTeamField(int delta)
{
    if (teamCursor_ == kTeamRowPreset) {
        const int presets = roster_.presetCount ? roster_.presetCount() : 0;
        // Cycle CUSTOM (-1), PRESET 1..n.
        int index = prefs_.presetIndex + delta;
        if (index < -1) index = presets - 1;
        if (index >= presets) index = -1;
        ApplyPreset(index);
    } else if (teamCursor_ == kTeamRowEntry) {
        prefs_.entryMode = prefs_.entryMode == EntryMode::Preset ? EntryMode::CharacterSelect
                                                                 : EntryMode::Preset;
    } else if (teamCursor_ >= kTeamRowFirstSlot && teamCursor_ < kTeamRowEntry) {
        const int member = (teamCursor_ - kTeamRowFirstSlot) / 3;
        const int field = (teamCursor_ - kTeamRowFirstSlot) % 3;
        Team& team = prefs_.team;
        if (field == 0) {
            const int count = roster_.characterCount ? roster_.characterCount() : 0;
            if (count <= 0) return;
            int index = 0;
            for (int i = 0; i < count; ++i)
                if (roster_.characterId(i) == team.chars[member]) index = i;
            index = ((index + delta) % count + count) % count;
            team.chars[member] = roster_.characterId(index);
            const int colors = roster_.colorCount ? roster_.colorCount(team.chars[member]) : 1;
            if (team.colors[member] >= colors) team.colors[member] = 0;
        } else if (field == 1) {
            team.assists[member] = ((team.assists[member] + delta) % 3 + 3) % 3;
        } else {
            const int colors = roster_.colorCount ? roster_.colorCount(team.chars[member]) : 1;
            if (colors > 0)
                team.colors[member] = ((team.colors[member] + delta) % colors + colors) % colors;
        }
        prefs_.presetIndex = -1; // edited by hand: no longer the saved preset
    } else {
        return;
    }
    Play(SFX_MOVE);
    CommitTeam();
}

void LobbyUi::UpdateTeam(const Input& input)
{
    MoveCursor(teamCursor_, kTeamRows, input);
    const int delta = (input.right ? 1 : 0) - (input.left ? 1 : 0);
    if (delta != 0) StepTeamField(delta);

    const bool done = input.cancel || (input.confirm && teamCursor_ == kTeamRowDone);
    if (done) {
        const bool readyUp = input.confirm && teamReturn_ == Screen::Room &&
                             prefs_.entryMode == EntryMode::CharacterSelect;
        Play(input.confirm ? SFX_CONFIRM : SFX_CANCEL);
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
    if (input.confirm) StepTeamField(1);
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
    const bool modal = DrawModal(canvas);
    DrawFooter(canvas, modal ? nullptr : help);
}

void LobbyUi::DrawBackdrop(Canvas& canvas) const
{
    // The title area keeps animating its key art behind the lobby; it is
    // pushed far back so panels and text read. (The native lobby's own comic
    // plate is a DXT1 texture that the cloned HQ sprite does not draw.)
    canvas.Sprite(TEX_WHITE, 0.0f, 0.0f, kScreenW, kScreenH, Uv{0.25f, 0.25f, 0.75f, 0.75f}, A_TL,
                  Color{0.02f, 0.03f, 0.07f, 0.74f});
    canvas.Sprite(TEX_WHITE, 0.0f, 0.0f, kScreenW, 132.0f, Uv{0.25f, 0.25f, 0.75f, 0.75f}, A_TL,
                  Color{0.0f, 0.0f, 0.0f, 0.38f});
}

void LobbyUi::DrawFooter(Canvas& canvas, const char* help) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    p.Fill(0.0f, kFooterY - 4.0f, kScreenW, 60.0f, Color{0.0f, 0.0f, 0.0f, 0.62f});
    p.Sprite(TEX_YELLOW_BAR, 0.0f, kFooterY - 4.0f, kScreenW, 3.0f, Uv{0.1f, 0.2f, 0.9f, 0.8f}, A_TL,
             WithAlpha(kWhite, 0.9f));

    // Context prompts, laid out right-to-left.
    Prompt prompts[6];
    int count = 0;
    const auto add = [&](Button button, const char* label) {
        if (count < 6) prompts[count++] = Prompt{button, label};
    };

    const bool modal = notice_[0] || confirm_ != Confirm::None;
    if (busy_[0]) {
        // No input is accepted while a blocking operation runs.
    } else if (textTarget_ != TextTarget::None) {
        add(BTN_CONFIRM, "OK");
        add(BTN_CANCEL, "Cancel");
    } else if (modal) {
        add(BTN_CONFIRM, "OK");
        if (confirm_ != Confirm::None) add(BTN_CANCEL, "Back");
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
        add(BTN_DPAD, "Change");
        add(BTN_CANCEL, "Back");
    } else if (screen_ == Screen::Team) {
        add(BTN_DPAD, "Change");
        add(BTN_CANCEL, "Done");
    } else if (roomMenuOpen_ || quickChatOpen_) {
        add(BTN_CONFIRM, "Select");
        add(BTN_CANCEL, "Back");
    } else {
        const Member* local = LocalMember();
        const bool seated = local && local->slot >= 0 && local->side < 2;
        const bool onOwnSeat = seated && local->slot == seatSlot_ && local->side == seatSide_;
        if (onOwnSeat)
            add(BTN_CONFIRM, local->ready ? "Not ready" : "Ready");
        else if (!SeatOccupant(seatSlot_, seatSide_))
            add(BTN_CONFIRM, "Sit");
        if (seated) add(BTN_X, "Leave seat");
        add(BTN_Y, "Chat");
        add(BTN_MENU, "Menu");
        add(BTN_CANCEL, seated ? "Back" : "Leave");
    }

    const Tex sheet = usingKeyboard_ ? TEX_KEY_GLYPHS : TEX_PAD_GLYPHS;
    const GlyphCell* cells = usingKeyboard_ ? kKeyGlyphs : kPadGlyphs;
    float x = kSafeRight;
    for (int i = count - 1; i >= 0; --i) {
        const float labelWidth = TextWidth(prompts[i].label) * 0.72f;
        p.Text(prompts[i].label, x, kFooterY + 22.0f, 0.72f, A_MR, kText);
        x -= labelWidth + 6.0f;
        const GlyphCell cell = cells[prompts[i].button];
        p.Sprite(sheet, x, kFooterY + 22.0f, 34.0f, 34.0f,
                 Uv{cell.col * 0.25f, cell.row * 0.25f, (cell.col + 1) * 0.25f,
                    (cell.row + 1) * 0.25f},
                 A_MR);
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
    p.Sprite(TEX_USER_ICON, x + 34.0f, y + 58.0f, 56.0f, 56.0f, kFullUv, A_ML);
    p.TextFit(service_.LocalName(), x + 104.0f, y + 46.0f, 0.95f, w - 130.0f, A_ML);
    p.Text(prefs_.presetIndex >= 0 ? "SAVED PRESET" : "CUSTOM TEAM", x + 104.0f, y + 74.0f, 0.62f,
           A_ML, kYellow);
    p.Sprite(TEX_LINE, x + 28.0f, y + 100.0f, w - 56.0f, 3.0f, Uv{0.1f, 0.55f, 0.9f, 0.95f}, A_TL,
             WithAlpha(kCyan, 0.8f));
    for (int i = 0; i < 3; ++i) {
        const float rowY = y + 132.0f + i * 40.0f;
        p.Text(kTeamSlotNames[i], x + 34.0f, rowY, 0.6f, A_ML, kFaint);
        const char* name = roster_.characterName ? roster_.characterName(prefs_.team.chars[i]) : "";
        p.TextFit(name, x + 130.0f, rowY, 0.84f, w - 250.0f, A_ML);
        char assist[48];
        snprintf(assist, sizeof(assist), "ASSIST %s", kAssistLetters[prefs_.team.assists[i] % 3]);
        p.Text(assist, x + w - 34.0f, rowY, 0.62f, A_MR, kDim);
    }
    p.Text("BATTLE ENTRY", x + 34.0f, y + 262.0f, 0.6f, A_ML, kFaint);
    p.Text(prefs_.entryMode == EntryMode::Preset ? "QUICK START" : "CHARACTER SELECT",
           x + w - 34.0f, y + 262.0f, 0.7f, A_MR, kCyan);
}

const char* LobbyUi::DrawTop(Canvas& canvas) const
{
    const float t = EaseOut(screenFrame_ / static_cast<float>(kTransitionFrames));
    const Painter p(canvas, t, (1.0f - t) * 36.0f);

    // "LOBBY" title art (row 0 of ga_room_subtit2), slightly enlarged.
    p.Sprite(TEX_SUBTITLES2, 76.0f, 68.0f, 68.0f * 1.7f, 30.0f * 1.7f,
             Uv{0.0f, 1.0f / 64.0f, 68.0f / 256.0f, 31.0f / 64.0f}, A_ML);
    p.Label("TIMESTONE ROLLBACK", 204.0f, 76.0f, 0.62f, A_ML, kYellow);

    DrawTeamCard(canvas, 72.0f, 168.0f, 470.0f);
    p.TextFit(service_.StatusText(), 84.0f, 494.0f, 0.62f, 450.0f, A_ML,
              service_.Available() ? kDim : kRed);

    for (int i = 0; i < kTopCount; ++i) {
        const float y = 214.0f + i * 82.0f;
        const float shift = i * 14.0f; // native main-menu stagger
        const bool selected = i == topCursor_;
        if (selected) {
            // The white bar carries a round ornament near its right end; the
            // label stops just short of it, as on the native main menu.
            const float pulse = 0.88f + 0.12f * std::sin(frame_ * 0.16f);
            p.Sprite(TEX_MENU_SEL, 1296.0f + shift, y, 640.0f, 72.0f,
                     Uv{12.0f / 512.0f, 10.0f / 64.0f, 1.0f, 58.0f / 64.0f}, A_MR,
                     WithAlpha(kWhite, pulse));
            p.Text(kTopItems[i], 1104.0f + shift, y - 2.0f, 0.5f, A_MR, kInk, FONT_HEADING);
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
    p.Label("HOST", 590.0f, 122.0f, 0.6f, A_ML, kYellow);
    p.Label("PLAYERS", 846.0f, 122.0f, 0.6f, A_ML, kYellow);
    p.Label("CONNECTION", 1066.0f, 122.0f, 0.6f, A_ML, kYellow);

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
        r.Box(TEX_SLANT_ROW, kSafeLeft, y - 29.0f, rowWidth, 58.0f,
              selected ? Color{0.42f, 0.44f, 0.5f, 0.96f} : Color{0.2f, 0.22f, 0.28f, 0.9f});
        if (selected) {
            const float pulse = 0.5f + 0.22f * std::sin(frame_ * 0.18f);
            p.Box(TEX_SLANT_SEL, kSafeLeft, y - 29.0f, rowWidth, 58.0f, WithAlpha(kWhite, pulse));
        }
        r.Sprite(TEX_USER_ICON, 104.0f, y, 38.0f, 38.0f, kFullUv, A_ML);
        r.TextFit(result.name, 152.0f, y, 0.86f, 420.0f, A_ML);
        r.TextFit(result.host, 590.0f, y, 0.74f, 240.0f, A_ML, kDim);

        // One head per seat: host first, then members, then empty seats.
        for (int seat = 0; seat < result.maxMembers && seat < kMaxMembers; ++seat) {
            const int cell = seat == 0 ? 0 : seat < result.members ? 1 : 2;
            r.Sprite(TEX_MEMBER_ICONS, 846.0f + seat * 19.0f, y, 20.0f, 40.0f,
                     Uv{cell * 0.125f, 0.0f, (cell + 1) * 0.125f, 1.0f}, A_ML);
        }
        char players[16];
        snprintf(players, sizeof(players), "%d/%d", result.members, result.maxMembers);
        r.Text(players, 1042.0f, y, 0.74f, A_MR,
               result.members >= result.maxMembers ? kRed : kText);

        if (!result.compatible) {
            p.Text("VERSION MISMATCH", 1066.0f, y, 0.5f, A_ML, kRed);
        } else {
            DrawPing(r, 1066.0f, y - (result.matchInProgress ? 8.0f : 0.0f), result.pingMs);
            if (result.matchInProgress)
                r.Text("MATCH IN PROGRESS", 1066.0f, y + 13.0f, 0.46f, A_ML, kYellow);
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

    const float panelX = 170.0f, panelY = 150.0f, panelW = 940.0f, panelH = 440.0f;
    p.Box(TEX_PANEL_DARK, panelX, panelY, panelW, panelH);
    p.Text("LOBBY SETTING", panelX + 44.0f, panelY + 44.0f, 0.4f, A_ML, kYellow, FONT_HEADING);

    const RoomSettings& draft = prefs_.create;
    char value[kChatCapacity + 8];
    const char* labels[CREATE_ROWS] = {"LOBBY NAME", "LOBBY SIZE", "MATCH SLOTS", "VISIBILITY", ""};
    for (int row = 0; row < CREATE_ROWS; ++row) {
        const float y = panelY + 112.0f + row * 62.0f;
        const bool selected = row == createCursor_;
        if (selected) {
            const float pulse = 0.72f + 0.28f * std::sin(frame_ * 0.18f);
            p.Box(TEX_ROW_SEL, panelX + 26.0f, y - 29.0f, panelW - 52.0f, 58.0f,
                  WithAlpha(kWhite, pulse));
        }
        if (row == CREATE_GO) {
            p.Text(service_.Busy() == RoomBusy::Creating ? "CREATING LOBBY..." : "CREATE LOBBY",
                   panelX + panelW * 0.5f, y, 0.46f, A_MC, selected ? kInk : kText, FONT_HEADING);
            continue;
        }
        p.Text(labels[row], panelX + 64.0f, y, 0.86f, A_ML, selected ? kInk : kDim);
        switch (row) {
        case CREATE_NAME:
            if (textTarget_ == TextTarget::RoomName)
                snprintf(value, sizeof(value), "%s%s", text_, (frame_ / 20) % 2 ? "_" : " ");
            else
                snprintf(value, sizeof(value), "%s", draft.name);
            break;
        case CREATE_SIZE: snprintf(value, sizeof(value), "%d PLAYERS", draft.maxMembers); break;
        case CREATE_SLOTS: snprintf(value, sizeof(value), "%d", draft.slotCount); break;
        default:
            snprintf(value, sizeof(value), "%s", kVisibilityNames[static_cast<int>(draft.visibility) % 3]);
            break;
        }
        const float valueX = panelX + 660.0f;
        p.TextFit(value, valueX, y, 0.86f, 440.0f, A_MC,
                  row == CREATE_NAME && textTarget_ == TextTarget::RoomName ? Rgb(0x0A3A66)
                  : selected ? kInk : kText);
        if (selected && row != CREATE_NAME) {
            // Reuse the menu cursor triangle for the left/right value arrows.
            const float bob = 3.0f * std::sin(frame_ * 0.22f);
            const Uv cursorUv{16.0f / 64.0f, 2.0f / 64.0f, 47.0f / 64.0f, 62.0f / 64.0f};
            p.Sprite(TEX_CURSOR, valueX + 232.0f + bob, y, 14.0f, 26.0f, cursorUv, A_MC);
            p.Sprite(TEX_CURSOR, valueX - 232.0f - bob, y, 14.0f, 26.0f,
                     Uv{cursorUv.u1, cursorUv.v0, cursorUv.u0, cursorUv.v1}, A_MC);
        }
    }

    const char* const help[CREATE_ROWS] = {
        "Set your lobby title.",
        "Set the maximum number of players in your lobby.",
        "Match slots let several pairs fight at the same time.",
        "Choose who can find and join this lobby.",
        "Create a lobby.",
    };
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
        // Small HOST / GUEST tag: rows 6 and 7 of the tag sheet.
        p.Sprite(TEX_TAGS, x + 12.0f, cy, 46.0f, 24.0f,
                 Uv{11.0f / 64.0f, (member.isHost ? 196.0f : 228.0f) / 256.0f, 53.0f / 64.0f,
                    (member.isHost ? 220.0f : 252.0f) / 256.0f},
                 A_ML);
        p.Sprite(TEX_USER_ICON, x + 64.0f, cy, 34.0f, 34.0f, kFullUv, A_ML);
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
        }
        if (!member.isLocal) DrawPing(p, x + 356.0f, cy, member.pingMs, 0.66f);
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
        p.Box(TEX_PANEL_DARK, x, y, cardW, cardH);

        char title[24];
        snprintf(title, sizeof(title), "MATCH %d", slot + 1);
        p.Text(title, x + 26.0f, y + 30.0f, 0.36f, A_ML, kYellow, FONT_HEADING);
        if (live) {
            // "MATCH IN PROGRESS" word art, breathing.
            const float pulse = 0.72f + 0.28f * std::sin(frame_ * 0.1f);
            p.Sprite(TEX_WORDS_B, x + cardW - 24.0f, y + 30.0f, 235.0f * 0.64f, 37.0f * 0.64f,
                     Uv{11.0f / 256.0f, 13.0f / 256.0f, 246.0f / 256.0f, 50.0f / 256.0f}, A_MR,
                     WithAlpha(kWhite, pulse));
        } else {
            p.Text(kSlotStateNames[static_cast<int>(state)], x + cardW - 26.0f, y + 30.0f, 0.58f,
                   A_MR, state == SlotState::Starting ? kGreen : kDim);
        }

        const float seatTop = y + 54.0f;
        const float seatH = (cardH - 54.0f - 14.0f - 8.0f) * 0.5f;
        for (int side = 0; side < 2; ++side) {
            const float sy = seatTop + side * (seatH + 8.0f);
            const float cy = sy + seatH * 0.5f;
            const Member* occupant = SeatOccupant(slot, side);
            const bool focused = slot == seatSlot_ && side == seatSide_ && !roomMenuOpen_ &&
                                 !quickChatOpen_;
            const bool mine = occupant && local && occupant->id == local->id;

            // The focus frame goes under the seat so it never veils the text.
            if (focused) {
                const float pulse = 0.7f + 0.3f * std::sin(frame_ * 0.2f);
                p.Box(TEX_ROW_SEL, x + 10.0f, sy - 5.0f, cardW - 20.0f, seatH + 10.0f,
                      WithAlpha(kWhite, pulse));
            }
            p.Box(mine ? TEX_PANEL_BLUE : TEX_PANEL_DOT, x + 16.0f, sy, cardW - 32.0f, seatH,
                  WithAlpha(kWhite, occupant ? 1.0f : 0.72f));

            // PLAYER 1 / PLAYER 2 word art: rows 0 and 1.
            p.Sprite(TEX_WORDS_A, x + 32.0f, cy - (occupant ? 13.0f : 0.0f), 74.0f, 23.0f,
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
            const bool narrow = cardW < 500.0f;
            const float readyW = narrow ? 76.0f : 92.0f;
            const float lampSpace = live ? 0.0f : readyW + 16.0f;
            p.TextFit(occupant->name, x + 122.0f, cy - 13.0f, 0.84f,
                      cardW - 122.0f - 32.0f - (narrow ? 0.0f : lampSpace), A_ML,
                      mine ? kYellow : kText);
            char summary[160];
            TeamSummary(roster_, occupant->team, summary, sizeof(summary));
            p.TextFit(summary, x + 32.0f, cy + 15.0f, 0.6f, cardW - 64.0f - lampSpace, A_ML,
                      mine ? kText : kDim);

            if (!live) {
                // READY lamp: row 4 lit, row 5 dim.
                const int readyRow = occupant->ready ? 4 : 5;
                p.Sprite(TEX_WORDS_A, x + cardW - 32.0f, narrow ? cy + 14.0f : cy, readyW,
                         readyW * 0.25f,
                         Uv{1.0f / 128.0f, (readyRow * 32 + 1) / 256.0f, 127.0f / 128.0f,
                            (readyRow * 32 + 31) / 256.0f},
                         A_MR);
            }
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
    const float x = 40.0f, y = 516.0f, w = 430.0f, h = 130.0f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);

    const bool typing = textTarget_ == TextTarget::Chat;
    const int visible = typing ? kChatRows - 1 : kChatRows;
    const int count = service_.ChatCount();
    const int first = count > visible ? count - visible : 0;
    for (int i = first; i < count; ++i) {
        const ChatLine& line = service_.Chat(i);
        const float rowY = y + 22.0f + (i - first) * 21.0f;
        if (line.system) {
            p.TextFit(line.text, x + 22.0f, rowY, 0.56f, w - 44.0f, A_ML, kYellow);
            continue;
        }
        char sender[48];
        FitText(sender, sizeof(sender), line.sender, 110.0f, 0.56f);
        p.Text(sender, x + 22.0f, rowY, 0.56f, A_ML, kCyan);
        p.TextFit(line.text, x + 140.0f, rowY, 0.56f, w - 162.0f, A_ML);
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
    p.Text(title, x + w * 0.5f, y + 40.0f, 0.38f, A_MC, kYellow, FONT_HEADING);
    for (int i = 0; i < count; ++i) {
        const float rowY = y + 88.0f + i * 44.0f;
        if (i == cursor) {
            const float pulse = 0.72f + 0.28f * std::sin(frame_ * 0.18f);
            p.Box(TEX_ROW_SEL, x + 22.0f, rowY - 24.0f, w - 44.0f, 48.0f, WithAlpha(kWhite, pulse));
        }
        p.Text(items[i], x + w * 0.5f, rowY, 0.8f, A_MC, i == cursor ? kInk : kDim);
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
    p.TextFit(room.settings.name, 64.0f, 60.0f, 0.44f, 760.0f, A_ML, kText, FONT_HEADING);
    p.Sprite(TEX_FRIENDS, 1098.0f, 62.0f, 40.0f, 40.0f, kFullUv, A_MC);
    char population[24];
    snprintf(population, sizeof(population), "%d / %d", room.memberCount, room.settings.maxMembers);
    p.Text(population, 1216.0f, 61.0f, 0.86f, A_MR);

    DrawRoomMembers(canvas);
    DrawRoomSlots(canvas);
    DrawRoomChat(canvas);

    // Kept short: the room footer carries the most button prompts.
    const char* help = "Battle begins when both are ready.";
    const Member* local = LocalMember();
    if (textTarget_ == TextTarget::Chat)
        help = "Type a message, then confirm.";
    else if (local && local->slot < 0)
        help = "Take a seat to challenge a player.";

    if (roomMenuOpen_)
        DrawListMenu(canvas, "LOBBY MENU", kRoomMenuItems, kRoomMenuCount, roomMenuCursor_);
    else if (quickChatOpen_)
        DrawListMenu(canvas, "CHAT", kQuickChat, kQuickChatCount, quickChatCursor_);
    return roomMenuOpen_ || quickChatOpen_ ? nullptr : help;
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
        const float y = panelY + 40.0f + row * 39.0f;
        const bool selected = row == teamCursor_;
        if (selected) {
            const float pulse = 0.72f + 0.28f * std::sin(frame_ * 0.18f);
            p.Box(TEX_ROW_SEL, panelX + 20.0f, y - 23.0f, panelW - 40.0f, 46.0f,
                  WithAlpha(kWhite, pulse));
        }
        const float labelX = panelX + 50.0f;
        const float valueX = panelX + 430.0f;
        const char* label = "";
        value[0] = '\0';
        Color valueColor = kText;

        if (row == kTeamRowPreset) {
            label = "TEAM";
            if (prefs_.presetIndex >= 0)
                snprintf(value, sizeof(value), "PRESET %d", prefs_.presetIndex + 1);
            else
                snprintf(value, sizeof(value), "CUSTOM");
            valueColor = kYellow;
        } else if (row == kTeamRowEntry) {
            label = "BATTLE ENTRY";
            snprintf(value, sizeof(value), "%s",
                     prefs_.entryMode == EntryMode::Preset ? "QUICK START" : "CHARACTER SELECT");
            valueColor = kCyan;
        } else if (row == kTeamRowDone) {
            p.Text("DONE", panelX + panelW * 0.5f, y, 0.4f, A_MC, selected ? kInk : kText,
                   FONT_HEADING);
            continue;
        } else {
            const int member = (row - kTeamRowFirstSlot) / 3;
            const int field = (row - kTeamRowFirstSlot) % 3;
            const int32_t character = prefs_.team.chars[member];
            if (field == 0) {
                label = kTeamSlotNames[member];
                snprintf(value, sizeof(value), "%s",
                         roster_.characterName ? roster_.characterName(character) : "");
            } else if (field == 1) {
                label = "   ASSIST";
                const char* assist = roster_.assistName
                    ? roster_.assistName(character, prefs_.team.assists[member]) : "";
                snprintf(value, sizeof(value), "%s  %s", kAssistLetters[prefs_.team.assists[member] % 3],
                         assist ? assist : "");
                valueColor = kDim;
            } else {
                label = "   COLOR";
                snprintf(value, sizeof(value), "%d", prefs_.team.colors[member] + 1);
                valueColor = kDim;
            }
        }
        const bool header = row == kTeamRowPreset || row == kTeamRowEntry ||
                            (row >= kTeamRowFirstSlot && (row - kTeamRowFirstSlot) % 3 == 0);
        p.Text(label, labelX, y, header ? 0.8f : 0.68f, A_ML,
               selected ? kInk : header ? kText : kFaint);
        p.TextFit(value, valueX, y, header ? 0.8f : 0.7f, 330.0f, A_MC, selected ? kInk : valueColor);
        if (selected) {
            const float bob = 3.0f * std::sin(frame_ * 0.22f);
            const Uv cursorUv{16.0f / 64.0f, 2.0f / 64.0f, 47.0f / 64.0f, 62.0f / 64.0f};
            p.Sprite(TEX_CURSOR, valueX + 184.0f + bob, y, 12.0f, 22.0f, cursorUv, A_MC);
            p.Sprite(TEX_CURSOR, valueX - 184.0f - bob, y, 12.0f, 22.0f,
                     Uv{cursorUv.u1, cursorUv.v0, cursorUv.u0, cursorUv.v1}, A_MC);
        }
    }

    DrawTeamCard(canvas, 752.0f, 126.0f, 470.0f);
    const float noteY = 452.0f;
    p.Label("QUICK START", 764.0f, noteY, 0.62f, A_ML, kYellow);
    p.Label("Ready up and go straight into battle with this team.", 764.0f, noteY + 24.0f, 0.56f,
            A_ML, kDim);
    p.Label("CHARACTER SELECT", 764.0f, noteY + 60.0f, 0.62f, A_ML, kYellow);
    p.Label("Choose your team each time before you ready up.", 764.0f, noteY + 84.0f, 0.56f,
            A_ML, kDim);

    const char* help = "Edit your team.";
    if (teamCursor_ == kTeamRowPreset)
        help = "Use one of your saved preset teams, or build a custom team.";
    else if (teamCursor_ == kTeamRowEntry)
        help = "Choose how you enter battle once both players are seated.";
    return help;
}

bool LobbyUi::DrawModal(Canvas& canvas) const
{
    const Painter p(canvas, 1.0f, 0.0f);
    const char* message = nullptr;
    if (busy_[0])
        message = busy_;
    else if (notice_[0])
        message = notice_;
    else if (confirm_ == Confirm::LeaveRoom)
        message = "Leave this lobby?";
    else if (confirm_ == Confirm::MainMenu)
        message = "Return to the main menu?";
    if (!message) return false;

    p.Fill(0.0f, 0.0f, kScreenW, kScreenH, Color{0.0f, 0.0f, 0.0f, 0.6f});
    const float w = 720.0f, h = confirm_ != Confirm::None && !busy_[0] && !notice_[0] ? 210.0f : 170.0f;
    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 12.0f;
    p.Box(TEX_PANEL_DARK, x, y, w, h);
    p.Sprite(TEX_YELLOW_BAR, x + 30.0f, y + 26.0f, w - 60.0f, 3.0f, Uv{0.1f, 0.2f, 0.9f, 0.8f}, A_TL);

    // Break the message at newlines; each line is fitted to the panel.
    char line[160];
    const char* cursor = message;
    int lineIndex = 0;
    int lineCount = 1;
    for (const char* c = message; *c; ++c)
        if (*c == '\n') ++lineCount;
    const float firstY = y + (busy_[0] || notice_[0] || confirm_ == Confirm::None ? h * 0.5f : 76.0f) -
                         (lineCount - 1) * 15.0f;
    while (*cursor && lineIndex < 5) {
        size_t length = 0;
        while (cursor[length] && cursor[length] != '\n') ++length;
        const size_t copy = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
        memcpy(line, cursor, copy);
        line[copy] = '\0';
        p.TextFit(line, kScreenW * 0.5f, firstY + lineIndex * 30.0f, 0.86f, w - 80.0f, A_MC);
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
                const float pulse = 0.72f + 0.28f * std::sin(frame_ * 0.18f);
                p.Box(TEX_ROW_SEL, optionX - 90.0f, y + h - 82.0f, 180.0f, 50.0f,
                      WithAlpha(kWhite, pulse));
            }
            p.Text(options[i], optionX, y + h - 57.0f, 0.4f, A_MC,
                   i == confirmCursor_ ? kInk : kDim, FONT_HEADING);
        }
    }
    return true;
}

} // namespace lobby
