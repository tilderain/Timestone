#pragma once

// Local test lobby (developer builds, kLobbyTestBuild below): the in-game
// lobby without Steam, between game processes on one PC. See lobby_local.cpp. These mirror the
// SteamLobby_/SteamRoom_ calls the in-game lobby makes; steam_lobby.cpp
// forwards to them while the mode is on.

#include <cstddef>
#include <cstdint>
#include <enet/enet.h>

// Call before the lobby starts. `instance` is 0 for the game the player
// launched and 1, 2... for the ones it spawned.
void LocalLobby_Enable(int instance);
// Set to true and rebuild for a lobby test build: the game then always boots
// into the in-game lobby against the local stand-in and opens a second
// window. Leave it false in anything that ships; while it is false no forward
// in steam_lobby.cpp is ever taken and nothing can switch the mode on.
constexpr bool kLobbyTestBuild = false;
constexpr bool LocalLobby_Enabled() { return kLobbyTestBuild; }
// Extra game windows a lobby test build opens: 1 for a match, 2 or more to
// have players waiting in a queue (each is a full copy of the game).
constexpr int kLobbyTestExtraInstances = 2;
bool LocalLobby_IsChild();
// Starts the extra game processes (first instance only, once).
void LocalLobby_SpawnSecondInstance();

bool LocalLobby_Initialize();
void LocalLobby_Pump();
const char* LocalLobby_GetStatus();
const char* LocalLobby_GetLocalUsername();
uint64_t LocalLobby_GetLocalId();

void LocalLobby_Search();
bool LocalLobby_TakeResultsChanged();
size_t LocalLobby_GetResultCount();
uint64_t LocalLobby_GetResult();
const char* LocalLobby_GetResultName();
const char* LocalLobby_GetResultUsername();
int LocalLobby_GetResultMemberCount();
int LocalLobby_GetResultMemberLimit();
int LocalLobby_GetResultSlotCount();
bool LocalLobby_IsResultInMatch();

bool LocalLobby_IsActive();
uint64_t LocalLobby_GetLobbyId();
void LocalLobby_Create(int maxMembers, int slotCount, const char* name);
void LocalLobby_Join(uint64_t lobbyId);
void LocalLobby_Leave();
int LocalLobby_GetMemberCount();
uint64_t LocalLobby_GetMember(int index);
uint64_t LocalLobby_GetOwner();
int LocalLobby_GetMemberLimit();
const char* LocalLobby_GetMemberName(uint64_t memberId);
const char* LocalLobby_GetMemberData(uint64_t memberId, const char* key);
void LocalLobby_SetLocalData(const char* key, const char* value);
const char* LocalLobby_GetData(const char* key);
const char* LocalLobby_GetLobbyData(uint64_t lobbyId, const char* key);
void LocalLobby_SetData(const char* key, const char* value);

bool LocalLobby_SendChat(const void* data, int size);
bool LocalLobby_PollChat(uint64_t* sender, char* buffer, int capacity, int* size);

bool LocalLobby_ConnectPeer(uint64_t memberId, bool asHost);
bool LocalLobby_TakeNetworkHandles(ENetHost** outHost, ENetPeer** outPeer, bool* outIsHost);
void LocalLobby_DisconnectPeer();
bool LocalLobby_TakePeerFailed();
const char* LocalLobby_GetPeerUsername();

// Spectator links (see steam_lobby.h): ENet on localhost, one listening
// socket per process on a port derived from its id.
void LocalLobby_SpecOpen(uint64_t memberId);
void LocalLobby_SpecClose(uint64_t memberId);
void LocalLobby_SpecCloseAll();
int LocalLobby_SpecState(uint64_t memberId);
bool LocalLobby_SpecSend(uint64_t memberId, const void* data, int size);
bool LocalLobby_SpecReceive(uint64_t* memberId, void* buffer, int capacity, int* size);
void LocalLobby_SpecPump();
