#pragma once

// Data model shared by the lobby screens, the Steam room service and the
// host-side preview harness. Nothing here touches the game or Steam.

#include "lobby_canvas.h"

#include <cstddef>
#include <cstdint>

namespace lobby {

constexpr int kMaxMembers = 16;
// What a new lobby is created with: the native lobby's eight rows. The player
// list packs its rows closer once a room holds more than this.
constexpr int kDefaultMembers = 8;
constexpr int kMaxSlots = 4;         // match slots ("cabinets") per room
constexpr int kMaxSearchResults = 48;
constexpr int kMaxChatLines = 5000; // history kept for the expanded chat
constexpr int kNameCapacity = 64;
constexpr int kChatCapacity = 128;
// Set lengths a room can be created with ("first to N"). 0 is endless: the
// two players rematch until one of them leaves, and nothing is recorded.
constexpr int kFirstToEndless = 0;
constexpr int kFirstToChoices[] = {1, 2, 3, 5, 7, 10, kFirstToEndless};
constexpr int kFirstToChoiceCount = 7;
constexpr int kMaxFirstTo = 10;

// A team member left to chance: the character, its color and its assist are
// drawn when a match is launched (once for the whole set) and are not known
// before. Only a player's own presets can hold one; the game's reserve units
// cannot.
constexpr int32_t kCharRandom = -1;
// The same, but drawn again for every game of the set instead of once.
constexpr int32_t kCharRandomAll = -2;
inline bool IsRandomMember(int32_t id) { return id == kCharRandom || id == kCharRandomAll; }

struct Team {
    int32_t chars[3];   // a character id, kCharRandom or kCharRandomAll
    int32_t assists[3]; // 0 = alpha, 1 = beta, 2 = gamma
    int32_t colors[3];  // zero-based archive variant
};

// The teams a player keeps: ten of their own, named, in Timestone.ini, and
// the game's three reserve units, which live in its save. A slot index runs
// over both: 0..9 the presets, 10..12 reserve units A..C.
constexpr int kTeamPresets = 10;
constexpr int kReserveUnits = 3;
constexpr int kTeamSlots = kTeamPresets + kReserveUnits;
// Names are limited in characters; the buffers hold that many at three bytes
// each (the menu font has nothing beyond the BMP's three-byte range).
constexpr int kPresetNameChars = 23;
constexpr int kPresetNameCapacity = kPresetNameChars * 3 + 1;
// A room name also has to fit kNameCapacity bytes, here and for every client.
constexpr int kRoomNameChars = 40;

struct TeamPreset {
    bool used;
    char name[kPresetNameCapacity];
    Team team;
};

// A player's preferred stage when they have none: any stage will do.
constexpr int kStageAny = -1;

// Input delay choices offered before a match. A pick is stored as an index:
// kDelayPickNone until chosen, kDelayPickAuto to follow the measured ping,
// otherwise delay frames + 1.
constexpr int kDelayPickNone = -1;
constexpr int kDelayPickAuto = 0;
constexpr int kMaxDelayPick = 7; // delay 6
constexpr uint8_t kHealthUnknown = 255;

// What happens to the two players when a set ends and others are queued for
// their match slot. With nobody queued, both simply stay. An endless room
// (firstTo 0) has no set end, so nobody is ever rotated there: a queued
// player waits for someone to leave.
enum class Rotation : uint8_t {
    WinnerStays = 1,  // the loser goes to the back of the queue
    WinnerLeaves = 2, // the winner goes to the back of the queue
};

enum class Visibility : uint8_t {
    Public = 0,
    FriendsOnly = 1,
    Private = 2,
};

enum class MemberStatus : uint8_t {
    Idle,      // in the room, not seated
    Seated,    // sitting at a match slot
    Ready,     // seated and ready
    Fighting,  // match in progress
    Watching,  // spectating a slot
};

struct Member {
    uint64_t id;
    char name[kNameCapacity];
    bool isHost;
    bool isLocal;
    bool ready;
    int8_t slot;   // -1 when not seated or watching
    int8_t side;   // 0 = player 1, 1 = player 2, 2 = spectator
    MemberStatus status;
    // Pre-match steps with the player in the opposite seat, in order:
    // accepted the match, picked an input delay, then `ready` (team settled).
    bool accepted;
    // On the shared character select with the player in the opposite seat
    // (native_charsel.h). A slot whose two players are both there counts as
    // starting, which is what lets the room's spectators follow them in.
    bool selecting;
    int8_t delayPick;
    int8_t delayHover; // what they have highlighted while still choosing
    Team team;
    // Lobby icon: kIconSteam for the Steam profile picture, else the id of
    // the character whose portrait they picked (lobby_canvas.h).
    int16_t icon;
    // The stage they would like to fight on: a game stage id, or kStageAny.
    int16_t stage;
    int16_t wins;   // sets won in this room
    int16_t losses;
    int8_t setWins; // games won in the set being played right now
    // That set's length, as its firstTo + 1; 0 when not in a set. A set keeps
    // the length it started with when the host changes the room's (SetFirstTo).
    int8_t setFormat;
    int8_t queueSlot;   // match slot this player is waiting for, -1 if none
    uint32_t queueSeq;  // order of joining that queue (lower waits longer)
    int8_t queuePos;    // 1 = next up; 0 when not queued (derived)
    // While fighting: each team member's life as a percentage, in team
    // order. kHealthUnknown until the first report of a match.
    uint8_t health[3];
    // Spectating: the match slot being watched (-1 if none), the order of
    // starting to watch it, and the member this one's feed comes from. The
    // last two only matter to the feed itself (spectate.h).
    int8_t watchSlot;
    uint32_t watchSeq;
    uint64_t upstream;
    int16_t pingMs; // estimated ping to the local player, -1 when unknown
};

enum class SlotState : uint8_t {
    Open,        // nobody seated
    Waiting,     // one player seated
    Standby,     // two players seated, not both ready
    Starting,    // both ready: connecting / handshaking
    InProgress,  // match running
};

struct RoomSettings {
    char name[kNameCapacity];
    int maxMembers;      // 2..kMaxMembers
    int slotCount;       // 1..kMaxSlots
    Visibility visibility;
    int32_t music; // lobby_music.h track id the room starts with
    // Games needed to win a set (0 = endless) and who makes way after one.
    // The host can change both later (RoomService::ApplySettings); a set already
    // being played finishes under the rules it started with.
    int firstTo;
    Rotation rotation;
};

struct RoomState {
    bool inRoom;
    uint64_t id;
    RoomSettings settings;
    int memberCount;
    Member members[kMaxMembers];
    SlotState slots[kMaxSlots];
    int32_t music; // lobby_music.h track id playing now (the host's choice)
};

struct SearchResult {
    uint64_t id;
    char name[kNameCapacity];
    char host[kNameCapacity];
    // The host's Steam id (0 if the lobby does not say) and lobby icon, as
    // Member::icon.
    uint64_t hostId;
    int16_t hostIcon;
    int members;
    int maxMembers;
    int slotCount;
    int pingMs;        // -1 when not yet estimated
    bool compatible;   // same Timestone protocol version
    // For a lobby that is not: what to tell the player who picks it.
    char versionNotice[160];
    int8_t versionOrder; // its version is older (-1) or newer (1) than ours; 0 unknown
    bool friendLobby;
    bool friendsOnly;  // the host made it visible to friends only
    bool matchInProgress;
    int firstTo;
};

struct ChatLine {
    char sender[kNameCapacity];
    char text[kChatCapacity];
    bool system; // join/leave/match notices
    bool settings; // a system line saying the host changed the lobby's settings
};

enum class RoomBusy : uint8_t { None, Searching, Creating, Joining };

// Everything the screens need from the networking layer. Implemented by
// SteamRoomService in the plugin and by a scripted fake in the preview tool.
class RoomService {
public:
    virtual ~RoomService() = default;

    virtual bool Available() const = 0;
    virtual const char* StatusText() const = 0;
    virtual const char* LocalName() const = 0;
    virtual RoomBusy Busy() const = 0;

    virtual void Search() = 0;
    virtual int ResultCount() const = 0;
    virtual const SearchResult& Result(int index) const = 0;

    virtual void Create(const RoomSettings& settings) = 0;
    virtual void Join(uint64_t roomId) = 0;
    virtual void Leave() = 0;
    virtual const RoomState& Room() const = 0;

    virtual void Sit(int slot, int side) = 0;
    virtual void Stand() = 0;
    // Wait for a seat at a match slot. The player at the head of a slot's
    // queue is seated by their own client as soon as a seat there opens.
    virtual void JoinQueue(int slot) = 0;
    virtual void LeaveQueue() = 0;
    // The setup whose matches the player wants to watch (-1 for none): a
    // standing wish, kept from match to match until it is withdrawn, the
    // player takes a seat, or (if they were also queued there) leaves the
    // queue. The lobby flow acts on it.
    virtual void SetWatchIntent(int slot) = 0;
    virtual int WatchIntent() const = 0;
    // Start / stop watching the match at a slot now (possible while queued,
    // not while seated). Publishes the watch slot; the flow finds a feed and
    // boots the battle as a spectator.
    virtual void Watch(int slot) = 0;
    virtual void StopWatching() = 0;
    // Pre-match steps, see Member. They are forgotten whenever the opposite
    // seat changes hands or empties.
    virtual void AcceptMatch() = 0;
    virtual void PickDelay(int pick) = 0;
    // The delay currently highlighted, shown to the other player as it moves.
    virtual void HoverDelay(int pick) = 0;
    // The input delay an "Auto" pick stands for at this round-trip ping, or -1
    // if the ping is not known yet.
    virtual int AutoDelay(int pingMs) const = 0;
    virtual void SetReady(bool ready) = 0;
    virtual void SetTeam(const Team& team) = 0;
    // The local player's lobby icon (see Member::icon), shown to the room.
    virtual void SetIcon(int icon) = 0;
    // The local player's preferred stage (see Member::stage). A match is
    // fought on one of its two players' preferred stages, picked at random.
    virtual void SetStage(int stage) = 0;
    // Texture holding a member's Steam profile picture (0 = the local
    // player); the default player icon until one is available.
    virtual Tex AvatarTex(uint64_t memberId) const = 0;
    virtual void InviteFriends() = 0;
    // Puts a link that joins the room on the clipboard, to paste to anyone.
    virtual void CopyLobbyLink() = 0;
    // Host only: the room's music, a lobby_music.h track id. Everyone in the
    // room sees it in RoomState::music.
    virtual void SetMusic(int32_t trackId) = 0;
    // Host only: gives the open room this name, visibility, set length and
    // rotation rule (its size, setups and music are not touched). The set
    // rules hold for sets that start from now on. What changed is announced
    // in everyone's chat.
    virtual void ApplySettings(const RoomSettings& settings) = 0;
    // Opens the player's Steam profile in the overlay.
    virtual void ViewProfile(uint64_t memberId) = 0;
    // Host only: removes the player and keeps them out of this lobby for as
    // long as it exists.
    virtual void Kick(uint64_t memberId) = 0;

    virtual void SendChat(const char* utf8) = 0;
    virtual int ChatCount() const = 0;
    virtual const ChatLine& Chat(int index) const = 0; // 0 = oldest retained

    // One-shot error text for a modal notice (join failed, version mismatch).
    virtual bool TakeError(char* message, size_t capacity) = 0;
};

// Roster data the team screen needs. Provided by the plugin from its existing
// character tables and by a stub table in the preview tool.
struct RosterSource {
    int (*characterCount)();
    int32_t (*characterId)(int index);            // selectable roster order
    const char* (*characterName)(int32_t id);
    const char* (*assistName)(int32_t id, int assist);
    int (*colorCount)(int32_t id);
    // The game's reserve units, 0..presetCount-1. `preset` fails for one
    // that holds no usable team.
    int (*presetCount)();
    bool (*preset)(int index, Team* team);
    // Puts a team in a reserve unit; the provider sees to the game's save.
    bool (*setPreset)(int index, const Team& team);
    // The stages a player can prefer, in the game's order.
    int (*stageCount)();
    int32_t (*stageId)(int index);
    const char* (*stageName)(int32_t id);
};

// One frame of menu input, already merged across pads and keyboard.
// Directions auto-repeat; everything else is a rising edge.
struct Input {
    bool up, down, left, right;
    bool confirm, cancel;
    bool actionX; // secondary action (pad X / keyboard mapped button)
    bool actionY; // tertiary action (pad Y)
    bool menu;    // Start
    bool pageLeft, pageRight; // shoulder buttons
    bool quickChat;           // RB on a pad, X on the keyboard
    bool spectate;            // X on a pad, Page Up on the keyboard
    bool usingKeyboard;       // last input source, selects footer glyphs
    bool unfocused;           // the game's window is not the one in front
    // Text typed this frame while a text field is active (UTF-8, may be empty).
    const char* typed;
    // What an input method is still composing (UTF-8, may be empty or null):
    // shown after the text, not yet part of it.
    const char* composing;
    bool backspace;
};

// Length of the set a fighting member is in: the room's unless the host has
// changed it since that set began.
inline int SetFirstTo(const Member& member, int roomFirstTo)
{
    return member.setFormat > 0 ? member.setFormat - 1 : roomFirstTo;
}

inline bool IsValidSeat(const RoomSettings& settings, int slot, int side)
{
    return slot >= 0 && slot < settings.slotCount && (side == 0 || side == 1);
}

} // namespace lobby
