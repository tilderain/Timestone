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
    // The main menu's own cues: an item decided on it, and a menu under it
    // closing back to it. They win over the plain cues in the same frame.
    SFX_MENU_ENTER = 1u << 4,
    SFX_MENU_EXIT = 1u << 5,
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
    Team team;       // the team in use: a copy of the slot's
    int slot;        // which of the kTeamSlots it is: a preset or a reserve unit
    TeamPreset presets[kTeamPresets];
    int icon;        // lobby icon: kIconSteam or a character id (lobby_canvas.h)
    int stage;       // preferred stage: a game stage id or kStageAny
    RoomSettings create;
    bool muteMusic;  // in someone else's lobby: do not play its music
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
    // The end of the text being typed, in UI space (valid while it is drawn).
    void TextCaret(float* x, float* y) const { *x = caretX_; *y = caretY_; }

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
    // Asks whether to show the player's activity on Discord. Shown only on the
    // lobby's top screen with nothing else open; false when it was not shown.
    bool AskDiscordSetup();
    // True once after the question was answered; *yes is the answer. Backing out
    // of it is a no.
    bool TakeDiscordSetupAnswer(bool* yes);
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
    // The reserve units were read again: if the team is one of them, take
    // its current contents (or move to another team if it is gone).
    void RefreshPreset();
    // True while the team screen is open or a match is being agreed.
    bool EditingTeam() const;

    // The game's own character select can be opened for a match (the flow
    // says so). Without it CHARACTER SELECT in the start prompt opens the
    // team screen instead.
    void SetNativeCharSelect(bool available) { nativeCharSel_ = available; }
    // True once after the player chose CHARACTER SELECT in the start prompt.
    bool TakeCharSelectRequested();
    // The preset list on its own, drawn over another screen (the character
    // select). Update returns true once, with the team that was picked, which
    // also becomes the team in use; cancel closes it.
    void OpenPresetPicker();
    bool PresetPickerOpen() const { return presetPicker_; }
    void ClosePresetPicker() { presetPicker_ = false; }
    bool UpdatePresetPicker(const Input& input, Team* picked);
    void DrawPresetPicker(Canvas& canvas) const;

private:
    enum class Screen : uint8_t { Top, Browse, Create, Room, Team };
    enum class TextTarget : uint8_t { None, RoomName, Chat, PresetName };
    enum class Confirm : uint8_t { None, LeaveRoom, LeaveSeat, KickMember, Update, DeletePreset, DiscordSetup };
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
    int RoomMenuActions(int* actions) const;
    void UpdateMemberList(const Input& input);
    Handshake HandshakeStep() const;
    bool UpdateHandshake(const Input& input); // true while it owns the input
    void DrawHandshake(Canvas& canvas) const;
    void UpdateMemberMenu(const Input& input);
    void UpdateChatFocus(const Input& input);
    void UpdateTeam(const Input& input);
    void UpdateCharPicker(const Input& input);
    void UpdateColorPicker(const Input& input);
    // Texture for a player's lobby icon: their Steam picture or a portrait.
    Tex IconTex(uint64_t memberId, int icon) const;
    void SetIcon(int icon);
    void SetStage(int stage);
    void UpdateStagePicker(const Input& input);
    void DrawStagePicker(Canvas& canvas) const;
    const char* StageName(int stage) const;
    void UpdateIconPicker(const Input& input);
    void DrawIconPicker(Canvas& canvas) const;
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
    void DrawCharPicker(Canvas& canvas) const;
    void DrawColorPicker(Canvas& canvas) const;
    void DrawListMenu(Canvas& canvas, const char* title, const char* const* items,
                      int count, int cursor) const;
    void DrawMusicMenu(Canvas& canvas) const;
    bool DrawModal(Canvas& canvas) const; // true when a modal layer was drawn
    void DrawUpdateProgress(Canvas& canvas) const;

    const Member* LocalMember() const;
    const Member* SeatOccupant(int slot, int side) const;
    int SlotQueueLength(int slot) const;
    int SlotWatcherCount(int slot) const;
    // Team slots: the player's presets, then the game's reserve units.
    bool SlotUsed(int slot) const;
    bool SlotTeam(int slot, Team* team) const;
    void SlotName(int slot, char* out, size_t size) const;
    int NextUsedSlot(int from, int delta) const;
    void SelectSlot(int slot);   // makes a used slot the team in use
    void StepSlot(int delta);    // the next used slot in that direction
    void StoreTeam();            // writes the team in use back to its slot
    void DeletePreset(int slot);
    void UpdatePresetList(const Input& input);
    void DrawPresetList(Canvas& canvas) const;
    void DrawTeamBanners(Canvas& canvas, float x, float y, float w) const;
    void CommitTeam();
    void StepTeamField(int delta);
    void SetTeamCharacter(int member, int32_t id);
    bool RandomOffered() const;
    int32_t PickerCharacter(int cell) const;
    void BeginText(TextTarget target, const char* initial);
    void EndText(bool accept);

    RoomService& service_;
    RosterSource roster_;
    UiPrefs prefs_;

    Screen screen_ = Screen::Top;
    Screen teamReturn_ = Screen::Top;
    uint32_t frame_ = 0;
    uint32_t screenFrame_ = 0; // frames since the current screen was entered
    uint32_t roomBackdrop_ = 0; // fade of the space backdrop (room, team setup), 0..kRoomBackdropFrames
    uint32_t entranceDelay_ = 0;
    uint32_t sfx_ = 0;
    bool prefsDirty_ = false;
    bool mainMenuRequested_ = false;
    bool nativeCharSel_ = false;
    bool charSelRequested_ = false;
    bool presetPicker_ = false; // the preset list is up on its own
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
    int charPickerCursor_ = 0;    // roster index highlighted in it; the roster's count is RANDOM
    int charPickerColumn_ = 0;    // column the cursor left the roster from, to go back up to
    bool colorPickerOpen_ = false; // every color at once, on a team member's COLOR row
    int colorPickerCursor_ = 0;    // color highlighted in it
    bool iconPickerOpen_ = false; // icon grid on the team screen's ICON row
    int iconCursor_ = 0;          // icon value highlighted in it (0 = Steam picture)
    int iconScroll_ = 0;          // first grid row on screen
    bool stagePickerOpen_ = false; // stage icons on the team screen's STAGE row
    int stagePickerCursor_ = 0;    // 0 = any stage, then the roster's stages in order
    bool presetListOpen_ = false; // every team slot, on the team screen's PRESET row
    int presetListCursor_ = 0;    // slot highlighted in it

    bool roomMenuOpen_ = false;
    int roomMenuCursor_ = 0;
    bool slotMenuOpen_ = false; // line up, watch or both, for a setup that is taken
    int slotMenuCursor_ = 0;
    bool slotMenuQueued_ = false; // opened on the setup the player is in line for
    // The setup menu's entries for how it was opened; returns how many.
    int SlotMenuItems(const char* items[3]) const;
    bool quickChatOpen_ = false;
    int quickChatCursor_ = 0; // kept between openings, like the last phrase used
    bool memberFocus_ = false;    // cursor is in the player list, not on a seat
    int memberCursor_ = 0;
    bool memberMenuOpen_ = false; // actions for the selected player
    bool chatFocus_ = false;      // cursor is on the chat panel, below the player list
    bool chatExpanded_ = false;   // chat covers the player list and scrolls
    int chatExpandFrames_ = 0;    // 0 collapsed .. kChatExpandFrames fully open
    int chatScroll_ = 0;          // display rows scrolled back from the newest
    int chatSeenCount_ = 0;       // messages there were when the scroll was last set
    uint32_t chatHoverFrames_ = 0; // frames the cursor has been on the chat panel
    int memberMenuCursor_ = 0;
    uint64_t memberMenuId_ = 0;   // who the menu / kick confirmation is about
    char memberMenuName_[kNameCapacity] = {};
    Handshake handshake_ = Handshake::None; // step shown last frame
    uint32_t handshakeFrames_ = 0;          // frames spent on it
    uint32_t readyUpHold_ = 0;              // frames the ready-up still takes no input for
    uint32_t entryFrames_ = 0;              // frames since both delay picks were in
    uint32_t teamFrames_ = 0;               // of those, frames spent in character select
    int handshakeCursor_ = 0;
    int delayCursor_ = 0;                   // delay pick being chosen
    bool teamForMatch_ = false;             // team screen opened from the Entry step
    bool musicOpen_ = false; // host's lobby music picker
    int musicCursor_ = 0;
    // The host changing an open lobby's settings on the create screen: the
    // values being chosen, applied to the lobby on its last row.
    bool editingRoom_ = false;
    RoomSettings roomDraft_ = {};
    RoomSettings& CreateDraft() { return editingRoom_ ? roomDraft_ : prefs_.create; }
    const RoomSettings& CreateDraft() const { return editingRoom_ ? roomDraft_ : prefs_.create; }
    void CloseRoomSettings();

    Confirm confirm_ = Confirm::None;
    int confirmCursor_ = 1; // default to "No"
    char notice_[512] = {};
    char updateOffer_[192] = {}; // the Confirm::Update question
    bool updateAccepted_ = false;
    bool discordAnswered_ = false; // the Confirm::DiscordSetup question was answered
    bool discordYes_ = false;
    bool updateActive_ = false;  // progress modal shown
    bool updateCancelled_ = false;
    UpdateView update_ = {};
    char busy_[128] = {};

    TextTarget textTarget_ = TextTarget::None;
    char text_[kChatCapacity] = {};
    size_t textCapacity_ = 0; // bytes
    size_t textMaxChars_ = 0; // characters
    char composing_[kChatCapacity] = {}; // the input method's unfinished text
    // Where the text being typed ends on screen (UI space), for the input
    // method's candidate list. Set as the field is drawn.
    mutable float caretX_ = 0.0f, caretY_ = 0.0f;
};

} // namespace lobby
