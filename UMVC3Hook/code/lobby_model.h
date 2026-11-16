#pragma once

// Data model shared by the lobby screens, the Steam room service and the
// host-side preview harness. Nothing here touches the game or Steam.

#include <cstddef>
#include <cstdint>

namespace lobby {

constexpr int kMaxMembers = 8;       // matches the native lobby's eight rows
constexpr int kMaxSlots = 4;         // match slots ("cabinets") per room
constexpr int kMaxSearchResults = 48;
constexpr int kMaxChatLines = 32;
constexpr int kNameCapacity = 64;
constexpr int kChatCapacity = 128;
// Set lengths a room can be created with ("first to N"). 0 is endless: the
// two players rematch until one of them leaves, and nothing is recorded.
constexpr int kFirstToEndless = 0;
constexpr int kFirstToChoices[] = {1, 2, 3, 5, 7, 10, kFirstToEndless};
constexpr int kFirstToChoiceCount = 7;
constexpr int kMaxFirstTo = 10;

struct Team {
    int32_t chars[3];
    int32_t assists[3]; // 0 = alpha, 1 = beta, 2 = gamma
    int32_t colors[3];  // zero-based archive variant
};

// Input delay choices offered before a match. A pick is stored as an index:
// kDelayPickNone until chosen, kDelayPickAuto to follow the measured ping,
// otherwise delay frames + 1.
constexpr int kDelayPickNone = -1;
constexpr int kDelayPickAuto = 0;
constexpr int kMaxDelayPick = 7; // delay 6

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
    int8_t delayPick;
    int8_t delayHover; // what they have highlighted while still choosing
    Team team;
    int16_t wins;   // sets won in this room
    int16_t losses;
    int8_t setWins; // games won in the set being played right now
    int8_t queueSlot;   // match slot this player is waiting for, -1 if none
    uint32_t queueSeq;  // order of joining that queue (lower waits longer)
    int8_t queuePos;    // 1 = next up; 0 when not queued (derived)
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
    int firstTo;   // games needed to win a set (0 = endless), fixed at creation
    Rotation rotation; // likewise fixed
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
    int members;
    int maxMembers;
    int slotCount;
    int pingMs;        // -1 when not yet estimated
    bool compatible;   // same Timestone protocol version
    bool friendLobby;
    bool matchInProgress;
    int firstTo;
};

struct ChatLine {
    char sender[kNameCapacity];
    char text[kChatCapacity];
    bool system; // join/leave/match notices
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
    // Pre-match steps, see Member. They are forgotten whenever the opposite
    // seat changes hands or empties.
    virtual void AcceptMatch() = 0;
    virtual void PickDelay(int pick) = 0;
    // The delay currently highlighted, shown to the other player as it moves.
    virtual void HoverDelay(int pick) = 0;
    // The input delay an "Auto" pick stands for at this one-way ping, or -1
    // if the ping is not known yet.
    virtual int AutoDelay(int pingMs) const = 0;
    virtual void SetReady(bool ready) = 0;
    virtual void SetTeam(const Team& team) = 0;
    virtual void InviteFriends() = 0;
    // Host only: the room's music, a lobby_music.h track id. Everyone in the
    // room sees it in RoomState::music.
    virtual void SetMusic(int32_t trackId) = 0;
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
    int (*presetCount)();
    bool (*preset)(int index, Team* team);
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
    bool usingKeyboard;       // last input source, selects footer glyphs
    // Text typed this frame while a text field is active (UTF-8, may be empty).
    const char* typed;
    bool backspace;
};

inline bool IsValidSeat(const RoomSettings& settings, int slot, int side)
{
    return slot >= 0 && slot < settings.slotCount && (side == 0 || side == 1);
}

} // namespace lobby
