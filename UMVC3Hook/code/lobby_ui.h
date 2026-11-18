#pragma once

// Screen logic for the in-game lobby. Pure state machine over RoomService and
// Canvas: it owns no game or Steam state, so the preview tool can drive it.

#include "lobby_canvas.h"
#include "lobby_model.h"

namespace lobby {

enum Sfx : uint32_t {
    SFX_MOVE = 1u << 0,
    SFX_CONFIRM = 1u << 1,
    SFX_CANCEL = 1u << 2,
    // A match is waiting on the local player: the stage-select cue, and the
    // game window is brought to the front.
    SFX_MATCH_FOUND = 1u << 3,
};

// A Timestone update being downloaded, drawn as a modal with a progress bar.
struct UpdateView {
    char stage[160];  // what the updater is doing ("Downloading...")
    char status[160]; // byte counts and speed, or empty
    float fraction;   // 0..1, or negative while the size is unknown
    bool cancellable; // nothing installed yet, cancel stops it
};

// Preferences the caller loads before constructing the UI and saves whenever
// TakePrefsDirty() reports a change.
struct UiPrefs {
    Team team;
    int presetIndex; // saved-roster preset in use, or -1 for a custom team
    RoomSettings create;
};

class LobbyUi {
public:
    LobbyUi(RoomService& service, const RosterSource& roster, const UiPrefs& prefs);

    void Update(const Input& input);
    void Draw(Canvas& canvas);

    uint32_t TakeSfx();
    // True once after the player chose to leave the lobby for the main menu.
    bool TakeMainMenuRequested();
    bool TakePrefsDirty();
    const UiPrefs& Prefs() const { return prefs_; }

    // True while keyboard characters should be routed to Input::typed.
    bool TextEntryActive() const { return textTarget_ != TextTarget::None; }

    // Track (lobby_music.h id) to audition while the create screen's music
    // list is open, or -1: the one under the cursor.
    int32_t PreviewMusic() const;

    // Modal message dismissed with confirm.
    void ShowNotice(const char* text);
    // Non-dismissable progress overlay (e.g. "Connecting to opponent...").
    // Pass nullptr to clear.
    void SetBusy(const char* text);
    // Asks whether to download and install a newer Timestone release.
    void OfferUpdate(const char* installed, const char* available);
    // True once after the player said yes to OfferUpdate.
    bool TakeUpdateAccepted();
    // Shows (or with nullptr, clears) the update progress modal. It takes all
    // input; cancel is reported by TakeUpdateCancelled when allowed.
    void SetUpdateProgress(const UpdateView* view);
    bool TakeUpdateCancelled();
    // Re-enter the room screen after a match without replaying transitions.
    void ReturnToRoom();
    // Replays the current screen's slide-in, holding it back for `delayFrames`
    // first. The lobby calls this when it actually becomes visible: frames
    // tick before the native backend can draw, and under the fade from black.
    void PlayEntrance(uint32_t delayFrames);

private:
    enum class Screen : uint8_t { Top, Browse, Create, Room, Team };
    enum class TextTarget : uint8_t { None, RoomName, Chat };
    enum class Confirm : uint8_t { None, LeaveRoom, MainMenu, KickMember, Update };
    // Pre-match steps shown as a modal once both seats of the local player's
    // match slot are taken. Each is answered by both players in turn.
    enum class Handshake : uint8_t {
        None,
        Delay,      // choosing an input delay, which also readies up
        WaitDelay,  // picked, the other player has not
        Entry,      // character select or start immediately
        WaitStart,  // done, waiting for the other player's team
    };

    void GoTo(Screen screen);
    void Play(uint32_t sfx) { sfx_ |= sfx; }
    bool MoveCursor(int& cursor, int count, const Input& input, bool wrap = true);

    void UpdateTop(const Input& input);
    void UpdateBrowse(const Input& input);
    void UpdateCreate(const Input& input);
    void UpdateRoom(const Input& input);
    void UpdateRoomMenu(const Input& input);
    void UpdateSlotMenu(const Input& input);
    void UpdateQuickChat(const Input& input);
    void UpdateMusicMenu(const Input& input);
    void UpdateMemberList(const Input& input);
    Handshake HandshakeStep() const;
    bool UpdateHandshake(const Input& input); // true while it owns the input
    void DrawHandshake(Canvas& canvas) const;
    void UpdateMemberMenu(const Input& input);
    void UpdateChatFocus(const Input& input);
    void UpdateTeam(const Input& input);
    void UpdateCharPicker(const Input& input);
    void UpdateText(const Input& input);
    void SyncWithService();

    void DrawBackdrop(Canvas& canvas) const;
    void DrawFooter(Canvas& canvas, const char* help) const;
    const char* DrawTop(Canvas& canvas) const; // returns footer help
    const char* DrawBrowse(Canvas& canvas) const; // returns footer help
    const char* DrawCreate(Canvas& canvas) const; // returns footer help
    const char* DrawRoom(Canvas& canvas) const; // returns footer help
    void DrawRoomMembers(Canvas& canvas) const;
    void DrawRoomSlots(Canvas& canvas) const;
    void DrawRoomChat(Canvas& canvas) const;
    int ChatRowsFrom(int firstMessage) const; // display rows of messages [first, count)
    const char* DrawTeam(Canvas& canvas) const; // returns footer help
    void DrawTeamCard(Canvas& canvas, float x, float y, float w) const;
    void DrawCharPicker(Canvas& canvas, float centerX, float rowY) const;
    void DrawListMenu(Canvas& canvas, const char* title, const char* const* items,
                      int count, int cursor) const;
    void DrawMusicMenu(Canvas& canvas) const;
    bool DrawModal(Canvas& canvas) const; // true when a modal layer was drawn
    void DrawUpdateProgress(Canvas& canvas) const;

    const Member* LocalMember() const;
    const Member* SeatOccupant(int slot, int side) const;
    int SlotQueueLength(int slot) const;
    int SlotWatcherCount(int slot) const;
    void ApplyPreset(int index);
    void CommitTeam();
    void StepTeamField(int delta);
    void SetTeamCharacter(int member, int32_t id);
    void BeginText(TextTarget target, const char* initial);
    void EndText(bool accept);

    RoomService& service_;
    RosterSource roster_;
    UiPrefs prefs_;

    Screen screen_ = Screen::Top;
    Screen teamReturn_ = Screen::Top;
    uint32_t frame_ = 0;
    uint32_t screenFrame_ = 0; // frames since the current screen was entered
    uint32_t entranceDelay_ = 0;
    uint32_t sfx_ = 0;
    bool prefsDirty_ = false;
    bool mainMenuRequested_ = false;
    bool wasInRoom_ = false;
    bool leftByChoice_ = false;
    bool usingKeyboard_ = false;

    int topCursor_ = 0;
    int browseCursor_ = 0;
    int browseScroll_ = 0;
    int createCursor_ = 0;
    int seatSlot_ = 0;
    int seatSide_ = 0;
    int teamCursor_ = 0;
    bool charPickerOpen_ = false; // character dropdown on a team member row
    int charPickerCursor_ = 0;    // roster index highlighted in it

    bool roomMenuOpen_ = false;
    int roomMenuCursor_ = 0;
    bool slotMenuOpen_ = false; // line up, watch or both, for a setup that is taken
    int slotMenuCursor_ = 0;
    bool quickChatOpen_ = false;
    int quickChatCursor_ = 0; // kept between openings, like the last phrase used
    bool memberFocus_ = false;    // cursor is in the player list, not on a seat
    int memberCursor_ = 0;
    bool memberMenuOpen_ = false; // actions for the selected player
    bool chatFocus_ = false;      // cursor is on the chat panel, below the player list
    bool chatExpanded_ = false;   // chat covers the player list and scrolls
    int chatScroll_ = 0;          // display rows scrolled back from the newest
    int chatSeenCount_ = 0;       // messages there were when the scroll was last set
    uint32_t chatHoverFrames_ = 0; // frames the cursor has been on the chat panel
    int memberMenuCursor_ = 0;
    uint64_t memberMenuId_ = 0;   // who the menu / kick confirmation is about
    char memberMenuName_[kNameCapacity] = {};
    Handshake handshake_ = Handshake::None; // step shown last frame
    uint32_t handshakeFrames_ = 0;          // frames spent on it
    int handshakeCursor_ = 0;
    int delayCursor_ = 0;                   // delay pick being chosen
    bool teamForMatch_ = false;             // team screen opened from the Entry step
    bool musicOpen_ = false; // host's lobby music picker
    int musicCursor_ = 0;

    Confirm confirm_ = Confirm::None;
    int confirmCursor_ = 1; // default to "No"
    char notice_[256] = {};
    char updateOffer_[192] = {}; // the Confirm::Update question
    bool updateAccepted_ = false;
    bool updateActive_ = false;  // progress modal shown
    bool updateCancelled_ = false;
    UpdateView update_ = {};
    char busy_[128] = {};

    TextTarget textTarget_ = TextTarget::None;
    char text_[kChatCapacity] = {};
    size_t textCapacity_ = 0;
};

} // namespace lobby
