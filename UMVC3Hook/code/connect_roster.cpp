#include "connect_roster.h"
#include "../IniSafe.h"

#include <windows.h>
#include <string>
#include <vector>

ConnectRoster ConnectRoster_Load(const char* path, const ConnectRoster& defaults)
{
    ConnectRoster roster = defaults;
    for (int i = 0; i < 3; ++i) {
        const auto slot = std::to_string(i + 1);
        roster.chars[i] = GetPrivateProfileIntA("ConnectRoster", ("Slot" + slot).c_str(), defaults.chars[i], path);
        roster.colors[i] = GetPrivateProfileIntA("ConnectRoster", ("Color" + slot).c_str(), defaults.colors[i], path);
        roster.assists[i] = GetPrivateProfileIntA("ConnectRoster", ("Assist" + slot).c_str(), defaults.assists[i], path);
    }
    roster.simpleMode = GetPrivateProfileIntA("ConnectRoster", "SimpleMode", defaults.simpleMode, path) != 0;
    roster.stageId = GetPrivateProfileIntA("ConnectRoster", "StageId", defaults.stageId, path);
    return roster;
}

bool ConnectRoster_Save(const char* path, const ConnectRoster& roster)
{
    std::vector<IniSafe_Entry> entries;
    entries.reserve(11);
    for (int i = 0; i < 3; ++i) {
        const auto slot = std::to_string(i + 1);
        entries.push_back({"ConnectRoster", "Slot" + slot, std::to_string(roster.chars[i])});
        entries.push_back({"ConnectRoster", "Color" + slot, std::to_string(roster.colors[i])});
        entries.push_back({"ConnectRoster", "Assist" + slot, std::to_string(roster.assists[i])});
    }
    entries.push_back({"ConnectRoster", "SimpleMode", roster.simpleMode ? "1" : "0"});
    entries.push_back({"ConnectRoster", "StageId", std::to_string(roster.stageId)});
    return IniSafe_WriteBatch(entries.data(), entries.size(), path);
}

void ConnectRoster_UpdateLocalTeam(ConnectRoster& roster, bool isHost,
    const int32_t chars[2][3], const int32_t colors[2][3],
    const int32_t assists[2][3], const bool simpleMode[2])
{
    const int team = isHost ? 0 : 1;
    for (int i = 0; i < 3; ++i) {
        roster.chars[i] = chars[team][i];
        roster.colors[i] = colors[team][i];
        roster.assists[i] = assists[team][i];
    }
    roster.simpleMode = simpleMode[team];
}
