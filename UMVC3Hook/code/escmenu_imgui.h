#pragma once

#include <cstdint>

// Game thread, right after sPad's once-per-frame poll: reads the decoded pad
// masks for the menu (Start toggles it) and, while it is open, clears them so
// neither the match nor netplay sees menu input.
void EscMenuImGui_OnPadPoll(uint64_t sPad);
// Overlay thread, before ImGui::NewFrame: gamepad navigation for the menu.
void EscMenuImGui_FeedGamepad();
void EscMenuImGui_Update();
void EscMenuImGui_Draw();
void EscMenuImGui_DrawDisplaySettings();
bool EscMenuImGui_IsOpen();
