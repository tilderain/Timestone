#pragma once

// RoomService implementation on Steam lobbies (room mode of steam_lobby.cpp).
//
// There is no room server. Lobby data holds the room settings, every member
// publishes its own seat/ready/team as lobby member data, and all clients
// derive the same room state from that. Seat conflicts resolve
// deterministically (lowest SteamID keeps the seat), so the room survives the
// owner leaving: Steam migrates ownership and nothing else has to move.

#include "lobby_model.h"

#include <cstdint>

namespace lobby {

// A pairing the local player is part of: both seats of one slot are ready.
struct MatchPairing {
    int slot;
    int localSide;           // 0 = player 1 (ENet host), 1 = player 2
    uint64_t opponentId;
    char opponentName[kNameCapacity];
    Team localTeam;
    Team opponentTeam;
    int localDelayPick;    // see kDelayPickAuto in lobby_model.h
    int opponentDelayPick;
    bool bothReady;        // both players finished the pre-match steps
};

class SteamRoomService final : public RoomService {
public:
    SteamRoomService();

    // Call once per frame on the game thread.
    void Pump();

    bool Available() const override { return available_; }
    const char* StatusText() const override;
    const char* LocalName() const override;
    RoomBusy Busy() const override;

    void Search() override;
    int ResultCount() const override { return resultCount_; }
    const SearchResult& Result(int index) const override;

    void Create(const RoomSettings& settings) override;
    void Join(uint64_t roomId) override;
    void Leave() override;
    const RoomState& Room() const override { return room_; }

    void Sit(int slot, int side) override;
    void Stand() override;
    void JoinQueue(int slot) override;
    void LeaveQueue() override;
    void SetWatchIntent(int slot) override { watchIntent_ = slot; }
    int WatchIntent() const override { return watchIntent_; }
    void Watch(int slot) override;
    void StopWatching() override;
    void AcceptMatch() override;
    void PickDelay(int pick) override;
    void HoverDelay(int pick) override;
    int AutoDelay(int pingMs) const override;
    void SetReady(bool ready) override;
    void SetTeam(const Team& team) override;
    void SetIcon(int icon) override;
    Tex AvatarTex(uint64_t memberId) const override;
    void InviteFriends() override;
    void CopyLobbyLink() override;
    void SetMusic(int32_t trackId) override;
    void ViewProfile(uint64_t memberId) override;
    void Kick(uint64_t memberId) override;

    void SendChat(const char* utf8) override;
    int ChatCount() const override { return chatCount_; }
    const ChatLine& Chat(int index) const override;

    bool TakeError(char* message, size_t capacity) override;

    // --- Match flow (lobby_flow.cpp) ---
    // True once both seats of the local player's slot are taken and no match
    // is running there. The connection is made from that point, before
    // either has readied up, so the prompts can show a measured ping;
    // `bothReady` says when the match may start.
    bool FindPairing(MatchPairing& pairing) const;
    // Publishes the local status shown to the room (Fighting while in battle).
    void SetLocalStatus(MemberStatus status);
    // After a match: clear ready, back to Seated, bump the room record.
    void FinishMatch(bool counted, bool localWon);
    // During a set: the local player's games won so far, shown to the room.
    void ReportSetScore(int localGamesWon);
    // During a match: the local team's life, percent per member in team order.
    void ReportHealth(const uint8_t percent[3]);
    // Ping measured on the live connection to a member; shown in place of
    // the room's estimate for them until the next FinishMatch.
    void SetMeasuredPing(uint64_t memberId, int rttMs);
    // While watching: the member the local spectator's feed comes from (0 for
    // none yet). Published so that member, and everyone choosing a feed of
    // their own, can see it.
    void SetUpstream(uint64_t memberId);
    void PostSystemLine(const char* text);
    // A Steam invite accepted while in another room moves the player to the
    // invited one. While held (a match is starting or running) the move
    // waits, and RoomChangeWaiting says that it does.
    void HoldRoomChange(bool hold) { holdRoomChange_ = hold; }
    bool RoomChangeWaiting() const { return holdRoomChange_ && invitedRoom_ != 0; }

private:
    struct LocalState {
        int slot = -1;
        int side = 0;
        bool ready = false;
        MemberStatus status = MemberStatus::Idle;
        bool accepted = false;
        int delayPick = kDelayPickNone;
        int delayHover = kDelayPickNone;
        Team team = {};
        int icon = kIconSteam;
        int wins = 0;
        int losses = 0;
        int setWins = 0;
        int queueSlot = -1;
        uint32_t queueSeq = 0;
        uint8_t health[3] = {kHealthUnknown, kHealthUnknown, kHealthUnknown};
        int watchSlot = -1;
        uint32_t watchSeq = 0;
        uint64_t upstream = 0;
    };

    void PublishLocal();
    void PublishHostIcon();
    void RefreshRoom();
    void RefreshResults();
    void DrainChat();
    void AddChat(const char* sender, const char* text, bool system);
    void SetError(const char* text);

    bool available_ = false;
    uint32_t frame_ = 0;
    uint32_t nextInitAttempt_ = 0;

    SearchResult results_[kMaxSearchResults] = {};
    int resultCount_ = 0;

    RoomState room_ = {};
    LocalState local_;
    bool joining_ = false;
    bool creating_ = false;
    uint64_t invitedRoom_ = 0; // room to move to (see HoldRoomChange)
    bool holdRoomChange_ = false;
    bool publishedOnce_ = false;
    bool liveAdvertised_ = false;
    // Host icon last written to the lobby's data, and for which room.
    int hostIconPublished_ = -1;
    uint64_t hostIconRoom_ = 0;
    int watchIntent_ = -1;      // setup whose matches the player wants to watch
    int32_t pendingMusic_ = -1; // track to publish once the created room exists
    int pendingFirstTo_ = -1;   // likewise the set length
    int pendingVisibility_ = -1; // likewise who can find the room
    int pendingRotation_ = -1;  // and the rotation rule
    void RunQueue();
    int QueueLength(int slot) const;
    uint64_t handshakeOpponent_ = 0; // who the pre-match steps were taken with
    uint64_t measuredPeer_ = 0;
    int measuredPing_ = -1;
    void ClearHandshake();
    uint64_t knownMembers_[kMaxMembers] = {};
    char knownNames_[kMaxMembers][kNameCapacity] = {};
    int knownMemberCount_ = 0;

    ChatLine chat_[kMaxChatLines] = {};
    int chatCount_ = 0;
    int chatHead_ = 0; // index of the oldest retained line (ring)
    char error_[256] = {};
};

} // namespace lobby
