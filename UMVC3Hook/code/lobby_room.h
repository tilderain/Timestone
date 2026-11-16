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
    void SetReady(bool ready) override;
    void SetTeam(const Team& team, EntryMode mode) override;
    void InviteFriends() override;
    void SetMusic(int32_t trackId) override;

    void SendChat(const char* utf8) override;
    int ChatCount() const override { return chatCount_; }
    const ChatLine& Chat(int index) const override;

    bool TakeError(char* message, size_t capacity) override;

    // --- Match flow (lobby_flow.cpp) ---
    // True while the local player and the opponent at the same slot are both
    // seated and ready and no match is running there yet.
    bool FindPairing(MatchPairing& pairing) const;
    // Publishes the local status shown to the room (Fighting while in battle).
    void SetLocalStatus(MemberStatus status);
    // After a match: clear ready, back to Seated, bump the room record.
    void FinishMatch(bool counted, bool localWon);
    void PostSystemLine(const char* text);

private:
    struct LocalState {
        int slot = -1;
        int side = 0;
        bool ready = false;
        MemberStatus status = MemberStatus::Idle;
        EntryMode mode = EntryMode::Preset;
        Team team = {};
        int wins = 0;
        int losses = 0;
    };

    void PublishLocal();
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
    bool publishedOnce_ = false;
    bool liveAdvertised_ = false;
    int32_t pendingMusic_ = -1; // track to publish once the created room exists
    uint64_t knownMembers_[kMaxMembers] = {};
    char knownNames_[kMaxMembers][kNameCapacity] = {};
    int knownMemberCount_ = 0;

    ChatLine chat_[kMaxChatLines] = {};
    int chatCount_ = 0;
    char error_[256] = {};
};

} // namespace lobby
