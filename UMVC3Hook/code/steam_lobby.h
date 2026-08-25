#pragma once

#include <cstddef>
#include <cstdint>
#include <enet/enet.h>

bool SteamLobby_Initialize();
void SteamLobby_Pump();
void SteamLobby_Search();
void SteamLobby_Create();
void SteamLobby_Join(uint64_t lobbyId);
void SteamLobby_Shutdown(bool preserveConnection);

const char* SteamLobby_GetStatus();
uint64_t SteamLobby_GetPeerSteamId();
const char* SteamLobby_GetPeerUsername();
bool SteamLobby_HasLobby();
size_t SteamLobby_GetResultCount();
uint64_t SteamLobby_GetResult(size_t index);
int SteamLobby_GetResultEstimatedPing(size_t index);
const char* SteamLobby_GetResultName(size_t index);
const char* SteamLobby_GetResultUsername(size_t index);
bool SteamLobby_ArePingEstimatesAvailable();
bool SteamLobby_TakeResultsChanged();
bool SteamLobby_TakeVersionMismatch(char* message, size_t messageSize);
bool SteamLobby_ResetHostPeer();
bool SteamLobby_TakeNetworkHandles(ENetHost** outHost, ENetPeer** outPeer,
                                   bool* outIsHost);
