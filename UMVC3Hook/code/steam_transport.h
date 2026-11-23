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
// "Direct", "Relay (SDR)", ... for the route Steam is carrying the match on.
const char* SteamTransport_GetRouteName();
// Highest jitter Steam reported since the previous call, in microseconds, or
// -1 if it reported none. For the periodic log line, which owns the reset.
int32_t SteamTransport_TakeLogJitterPeakUsec();
// Steam's estimate of the round trip to the peer through its relay network,
// in milliseconds, or -1 when the peer's location is not known. This is the
// only figure available for the relay route while the match runs direct.
void SteamTransport_SetRelayPingEstimate(int32_t pingMs);
int32_t SteamTransport_GetRelayPingEstimate();
void SteamTransport_Shutdown();
