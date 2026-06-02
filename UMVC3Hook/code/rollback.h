#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Rollback Snapshot / Restore (GGPO Callbacks)
//
// The snapshot system captures all necessary game memory regions for GGPO's
// rollback engine.  Rollback_SaveGameState/LoadGameState serialise/deserialise
// the region set.  Rollback_AdvanceFrame runs one frame of game logic with
// timing patches applied for fast re-simulation.
// ---------------------------------------------------------------------------

// Snapshot ring-buffer capacity.  Must be >= rollback frames so old
// snapshots survive long enough for any pending re-sim to complete.
static constexpr int ROLLBACK_WINDOW = 16;

// Global indicator for re-simulation mode.  External hooks check this to
// decide whether to bypass render / timing / sound operations.
extern bool g_isResimulating;

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------
void Rollback_InitHooks();
void Rollback_HeadlessTick(uint64_t sMvc3Main);

// ---------------------------------------------------------------------------
// GGPO callbacks
// ---------------------------------------------------------------------------
bool Rollback_SaveGameState(unsigned char** buffer, int* len, int* checksum, int frame);
bool Rollback_LoadGameState(unsigned char* buffer, int len);
bool Rollback_AdvanceFrame(int flags);
void Rollback_FreeSavedStates();

// ---------------------------------------------------------------------------
// Timing / render patches  (used during re-simulation)
// ---------------------------------------------------------------------------
void Rollback_PatchSleep(bool enable);
void Rollback_PatchPresent(bool enable);
void Rollback_PatchSwapchain(bool enable);
void Rollback_PatchDrawCalls(bool enable);

// Resynchronise the game's QPC / frame-timing fields after re-sim so the
// first post-re-sim real frame sees a well-formed time delta.
void Rollback_ResyncMainTiming(uint64_t sMain);
