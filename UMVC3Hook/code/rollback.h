#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Rollback Snapshot / Restore
// ---------------------------------------------------------------------------

// Maximum rollback window in frames. Keep in sync with NET_HISTORY_SIZE (16).
static constexpr int ROLLBACK_WINDOW = 16;

// Global indicators for input override during re-simulation.
// External input hooks (e.g. controller.cpp) check these to decide whether
// to inject historical buffer inputs instead of live controller reads.
extern bool    g_isResimulating;
extern int32_t g_resimulateCurrentFrame;

// Hook setup — installs the sMvc3Main__move hook for re-simulation stepping.
void Rollback_InitHooks();

// Save a full snapshot of all MUST regions, keyed by |frame|.
void Rollback_SaveSnapshot(int32_t frame);

// Restore the snapshot previously saved for |frame|.
void Rollback_RestoreSnapshot(int32_t frame);

// Re-simulate frames [startFrame, endFrame):
//   1. Restore snapshot at startFrame
//   2. Call the full sMvc3Main__move pipeline for each frame
//   3. Save corrected snapshot after each frame
void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame);

// Free all snapshot memory. Call on netplay disconnect / match end.
void Rollback_FreeSnapshots();
