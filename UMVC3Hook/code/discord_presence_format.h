#pragma once
#include <cstdint>
#include <string>

// Payloads for the Discord desktop client's local IPC pipe. Nothing in this
// file touches Windows: it is compiled into the ASI and, on its own, into a
// host test (tools/test_discord_presence.cpp).

enum class PresenceActivity : uint8_t {
    None,         // publish nothing (clears the activity)
    LobbyMenu,    // in-game lobby screens, not in a room
    InRoom,       // in a room, not playing or watching
    InMatch,      // room match (lobby phase Launching or InMatch)
    Spectating,   // watching a room match
    OfflineMenu,  // released from the lobby to the native main menu
    Training,     // launcher training, no netplay
    DualTraining, // launcher dual training, host process only
    DirectMatch,  // launcher direct-IP netplay
};

constexpr int kPresenceNameBytes = 64;
constexpr int kPresenceCharBytes = 24;

struct PresenceSnapshot {
    PresenceActivity activity = PresenceActivity::None;
    uint64_t lobbyId = 0;
    char roomName[kPresenceNameBytes] = {};
    int memberCount = 0;
    int memberLimit = 0;
    bool roomPublic = false;     // neither "private" nor friends-only
    bool startingMatch = false;  // lobby phase Connecting or Handshake
    int seat = -1;               // 0-based match slot the local player sits at, -1 none
    int queuePos = -1;           // 1-based queue position, -1 not queued
    char opponent[kPresenceNameBytes] = {};
    char localTeam[3][kPresenceCharBytes] = {};
    char opponentTeam[3][kPresenceCharBytes] = {};
    int firstTo = 0;             // 0 = endless set
    int winsLocal = 0;
    int winsOpponent = 0;
    char watchP1[kPresenceNameBytes] = {};
    char watchP2[kPresenceNameBytes] = {};
    int64_t startUnix = 0;       // 0 = no timer
    char version[16] = {};
};

bool DiscordPresence_IsJoinable(const PresenceSnapshot& s);
std::string DiscordPresence_ActivityJson(const PresenceSnapshot& s); // "" for None
std::string DiscordPresence_HandshakeJson(const char* clientId);
std::string DiscordPresence_SetActivityJson(const std::string& activityJson, uint32_t pid, uint32_t nonce);
std::string DiscordPresence_SubscribeJson(const char* evt, uint32_t nonce);
std::string DiscordPresence_JoinReplyJson(const std::string& userId, bool accept, uint32_t nonce);
std::string DiscordPresence_Frame(uint32_t opcode, const std::string& body);
bool DiscordPresence_JsonString(const std::string& json, const char* key, std::string& out, size_t from = 0);
bool DiscordPresence_JsonInt(const std::string& json, const char* key, long long& out);
