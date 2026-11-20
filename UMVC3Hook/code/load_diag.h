#pragma once
// Temporary timing of the game's asynchronous resource loads; see load_diag.cpp.

void LoadDiag_Install();
// Once a frame, on the game thread.
void LoadDiag_Tick();
