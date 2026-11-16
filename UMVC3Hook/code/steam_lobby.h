#pragma once

#include <cstddef>
#include <cstdint>
#include <enet/enet.h>

enum class SteamLobbyType : int {
    Private = 0,
    FriendsOnly = 1,
    Public = 2,
};

bool SteamLobby_Initialize();
bool SteamLobby_IsRelayOnly();
void SteamLobby_Pump();
void SteamLobby_Search();
void SteamLobby_SetName(const char* name);
void SteamLobby_Create(SteamLobbyType type);
void SteamLobby_Join(uint64_t lobbyId);
void SteamLobby_Shutdown(bool preserveConnection);

const char* SteamLobby_GetStatus();
const char* SteamLobby_GetConnectionType();
const char* SteamLobby_GetLocalUsername();
uint64_t SteamLobby_GetPeerSteamId();
const char* SteamLobby_GetPeerUsername();
bool SteamLobby_GetPeerAvatarRGBA(uint8_t* rgba, size_t capacity,
                                  uint32_t* width, uint32_t* height);
bool SteamLobby_HasPeerProfile();
bool SteamLobby_OpenPeerProfile();
bool SteamLobby_HasLobby();
bool SteamLobby_IsHost();
bool SteamLobby_IsCreating();
bool SteamLobby_IsHostedLobby(uint64_t lobbyId);
bool SteamLobby_GetHostedJoinLink(char* link, size_t capacity);
bool SteamLobby_IsFriendLobby(uint64_t lobbyId);
bool SteamLobby_IsLobbyVersionCompatible(uint64_t lobbyId);
size_t SteamLobby_GetResultCount();
uint64_t SteamLobby_GetResult(size_t index);
int SteamLobby_GetResultEstimatedPing(size_t index);
const char* SteamLobby_GetResultName(size_t index);
const char* SteamLobby_GetResultUsername(size_t index);
bool SteamLobby_ArePingEstimatesAvailable();
bool SteamLobby_TakeResultsChanged();
bool SteamLobby_TakeVersionMismatch(char* message, size_t messageSize);
bool SteamLobby_TakeReplacementJoin(uint64_t* lobbyId);
bool SteamLobby_ResetHostPeer();
bool SteamLobby_TakeNetworkHandles(ENetHost** outHost, ENetPeer** outPeer,
                                   bool* outIsHost);

// Ends the current peer session. In room mode this only closes the match
// connection and keeps the player in their room; otherwise it is the full
// SteamLobby_Shutdown(false).
void SteamLobby_EndSession();
// 0 = idle, 1 = searching, 2 = creating, 3 = joining.
int SteamLobby_GetPendingKind();
int SteamLobby_GetResultMemberCount(size_t index);
int SteamLobby_GetResultMemberLimit(size_t index);
int SteamLobby_GetResultSlotCount(size_t index);
bool SteamLobby_IsResultInMatch(size_t index);

// ---------------------------------------------------------------------------
// Room mode: multi-player lobbies for the in-game lobby (see lobby_room.cpp).
// A room is a Steam lobby advertised under "TimestoneRoom". Joining it does
// not connect anyone; SteamRoom_ConnectPeer opens the match connection between
// the two players seated at a match slot.
// ---------------------------------------------------------------------------
void SteamRoom_SetMode(bool enabled);
bool SteamRoom_IsActive();
uint64_t SteamRoom_GetLobbyId();
uint64_t SteamRoom_GetLocalId();
void SteamRoom_Create(SteamLobbyType type, int maxMembers, int slotCount, const char* name);
void SteamRoom_Leave();
int SteamRoom_GetMemberCount();
uint64_t SteamRoom_GetMember(int index);
uint64_t SteamRoom_GetOwner();
int SteamRoom_GetMemberLimit();
const char* SteamRoom_GetMemberName(uint64_t memberId);
const char* SteamRoom_GetMemberData(uint64_t memberId, const char* key);
void SteamRoom_SetLocalData(const char* key, const char* value);
const char* SteamRoom_GetData(const char* key);
void SteamRoom_SetData(const char* key, const char* value); // owner only
int SteamRoom_EstimateMemberPing(uint64_t memberId);
bool SteamRoom_SendChat(const void* data, int size);
// Returns the next received lobby chat entry (including our own), or false.
bool SteamRoom_PollChat(uint64_t* sender, char* buffer, int capacity, int* size);
void SteamRoom_InviteFriends();
// Opens the Steam overlay on a player's profile. False if it is unavailable.
bool SteamRoom_ViewProfile(uint64_t memberId);
// Data of any lobby Steam has told us about (search results, the current room).
const char* SteamRoom_GetLobbyData(uint64_t lobbyId, const char* key);
// Opens (and keeps retrying) the match connection to another room member.
// asHost selects the ENet server role; the two players must disagree.
bool SteamRoom_ConnectPeer(uint64_t memberId, bool asHost);
// Closes the match connection and transport bridge; the room is kept.
void SteamRoom_DisconnectPeer();
bool SteamRoom_TakePeerFailed();

// ---------------------------------------------------------------------------
// Spectator links: reliable message links between room members, apart from
// the match connection (their own virtual port). A link exists once both
// ends have opened it; who opens which is derived from the room's published
// state (spectate.cpp), so both always do.
// `dial` says which end this is: the one that calls (a spectator, towards
// whoever it is or may be fed by) or the one that answers. Only one end
// dials, so two connection attempts never meet half-way.
void SteamRoom_SpecOpen(uint64_t memberId, bool dial);  // idempotent
void SteamRoom_SpecClose(uint64_t memberId);
void SteamRoom_SpecCloseAll();
// 0 = not open, 1 = connecting, 2 = connected, 3 = failed.
int SteamRoom_SpecState(uint64_t memberId);
bool SteamRoom_SpecSend(uint64_t memberId, const void* data, int size);
bool SteamRoom_SpecReceive(uint64_t* memberId, void* buffer, int capacity, int* size);
// Once per frame: connection progress and retries.
void SteamRoom_SpecPump();
