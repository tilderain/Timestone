#pragma once

#include <cstdint>
#include <enet/enet.h>

#include "steam_api_compat.h"

bool SteamTransport_CreateBridge(bool isHost, ISteamNetworkingSockets* sockets,
                                 HSteamNetConnection connection,
                                 ENetHost** outGameHost, ENetPeer** outGamePeer);
void SteamTransport_Pump();
bool SteamTransport_IsActive();
uint32_t SteamTransport_GetRTT();
float SteamTransport_GetPacketLoss();
float SteamTransport_GetRemotePacketLoss();
int32_t SteamTransport_GetJitter();
void SteamTransport_Shutdown();
