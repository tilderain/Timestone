#pragma once

#include <cstddef>
#include <cstdint>

struct SavedRoster {
    int32_t chars[3];
    int32_t colors[3];
    int32_t assists[3];
};

size_t Savedata_LoadRosters(SavedRoster* rosters, size_t capacity);
