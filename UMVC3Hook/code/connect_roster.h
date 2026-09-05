#pragma once

#include <cstdint>

// Personal preferences, never the combined host/client match configuration.
struct ConnectRoster {
    int32_t chars[3] = {1, 1, 1};
    int32_t colors[3] = {};
    int32_t assists[3] = {};
    bool simpleMode = false;
    int32_t stageId = 0;
};

ConnectRoster ConnectRoster_Load(const char* path, const ConnectRoster& defaults);
bool ConnectRoster_Save(const char* path, const ConnectRoster& roster);

// Stage is intentionally excluded: the match stage may belong to the host,
// not to this player's preferences. Only an explicit local stage edit changes it.
void ConnectRoster_UpdateLocalTeam(ConnectRoster& roster, bool isHost,
    const int32_t chars[2][3], const int32_t colors[2][3],
    const int32_t assists[2][3], const bool simpleMode[2]);
