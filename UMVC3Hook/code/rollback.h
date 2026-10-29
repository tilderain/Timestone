#pragma once
#include <cstddef>
#include <cstdint>

// Snapshot ring capacity, not the configured rollback depth.
// Keep aligned with NET_HISTORY_SIZE and large enough for retained corrections.
static constexpr int ROLLBACK_WINDOW = 16;

// Resimulation state used by input overrides, hooks, and diagnostics.
extern bool    g_isResimulating;
extern int32_t g_resimulateCurrentFrame;
extern int32_t g_resimReentranceDepth;

// Post-resimulation live-frame marker set by the main move hook; -1 when inactive.
extern int32_t g_postResimFrame;

// Requested snapshot frame, consumed by the main move hook; -1 when idle.
extern int32_t g_restoreRequestedFrame;

// F7 rollback diagnostic state machine.
extern int g_f7State;
extern int32_t g_f7StartFrame;
extern int g_f7Remaining;

// Schedule desync capture through frame + 15; ignore an already pending trigger.
extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame);

// Cancel the pending desync capture.
extern "C" void Rollback_ClearPendingDesyncCapture();

// Process capture on the game thread. Returns true while waiting or retrying.
extern "C" bool Rollback_ProcessDesyncDelay(int32_t currentFrame);

// Force native online/replay fixed-timing and job-control setup.
bool Rollback_PatchNativeNetplayDeterminism();
// Apply native job-control settings; HitSolver also needs the inline patch.
void Rollback_DisableGameplayParallelism();
// Apply fixed timing and job controls at the game-thread netplay boundary.
void Rollback_ApplyNetplayDeterminism();

// Override the online predicate only for selected callers during custom netplay.
void Rollback_InitOnlineBattleHook();

// Clear or flush model-bounds work deferred during resimulation.
void Rollback_ClearDeferredModelBounds();
void Rollback_FlushDeferredModelBounds();

// Queue, consume, or cancel resimulation requests for the main game thread.
void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame);
bool Rollback_ConsumeReSimRequest(int32_t& startFrame, int32_t& endFrame);
void Rollback_ClearPendingReSimulation();
void Rollback_SetMainInstance(uint64_t main);

// Install rollback movement hooks.
void Rollback_InitHooks();
// Force HitSolver's in-place coalesce/sort path instead of worker buckets.
bool Rollback_PatchHitSolverAlwaysInline();
// Skip native UI initialization blocks when their allocations fail.
void Rollback_PatchBattleUiAllocationFailure();
// Install native-object guards, resimulation filters, and exception recovery.
void Rollback_InitNativeNullGuards();

// Capture the configured regions and subsystem state for this frame.
void Rollback_SaveSnapshot(int32_t frame);

// Replace a saved frame's remote input when a late packet arrives.
void Rollback_UpdateRemoteInput(int32_t frame, uint32_t input);

// Restore the snapshot saved for this frame.
void Rollback_RestoreSnapshot(int32_t frame);

// Restore startFrame, then run headless ticks for [startFrame, endFrame).
// Multi-resim refreshes corrected snapshots, reusing the start snapshot when valid.
void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame);

// Release rollback slot storage and reset tracking; preserve snapdump history.
void Rollback_FreeSnapshots();

// Run the selected gameplay pipeline without native rendering or frame pacing.
void Rollback_HeadlessTick(uint64_t sMvc3Main);

// Currently a no-op; the Sleep call-site patching body is disabled.
void Rollback_PatchSleep(bool enable);

// Currently a no-op; the Present call-site patching body is disabled.
void Rollback_PatchPresent(bool enable);

// Currently a no-op; the swapchain call-site patching body is disabled.
void Rollback_PatchSwapchain(bool enable);

// Currently a no-op; the draw call-site patching body is disabled.
void Rollback_PatchDrawCalls(bool enable);
