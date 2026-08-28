#pragma once

#include <Windows.h>

struct CompactLaunchMenuControlIds {
    int search;
    int create;
    int join;
    int type;
    int name;
    int status;
    int list;
    int normal;
    int advanced;
};

void CreateCompactLaunchMenuControls(HWND window,
                                     const CompactLaunchMenuControlIds& ids,
                                     int lobbyType,
                                     const char* lobbyName);
