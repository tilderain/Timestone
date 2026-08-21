#pragma once
#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Rollback Snapshot / Restore
//
// The snapshot ring buffer (ROLLBACK_WINDOW = 16) is the storage capacity.
// The active rollback window (g_rollbackFrames, default 4 in controller.cpp)
// controls how many frames we actually re-simulate on a late-packet conflict.
// ---------------------------------------------------------------------------

// Snapshot ring-buffer capacity.  Must be >= rollback frames so old
// snapshots survive long enough for any pending re-sim to complete.
// Keep in sync with NET_HISTORY_SIZE (16).
static constexpr int ROLLBACK_WINDOW = 16;

// Global indicators for input override during re-simulation.
// External input hooks (e.g. controller.cpp) check these to decide whether
// to inject historical buffer inputs instead of live controller reads.
extern bool    g_isResimulating;
extern int32_t g_resimulateCurrentFrame;
extern int32_t g_resimReentranceDepth;

// When true, send a ResimNotify packet every frame (for NDJSON debugging),
// not only when a rollback conflict triggers re-simulation.
extern bool g_resimNotifyEveryFrame;

// Non-negative after Rollback_ReSimulate completes, holding the frame number
// of the first post-resim live tick.  hkprocessFrameInput checks this to log
// diagnostic info at the resim→live boundary.  Cleared after first use.
extern int32_t g_postResimFrame;

// Snapshot restore is deferred to the main game thread to avoid mid-frame
// mutation from the frame-stepper hook.
extern int32_t g_restoreRequestedFrame;

// F7 rollback diagnostic state machine and active NDJSON session.
extern int g_f7State;
extern int32_t g_f7StartFrame;
extern int g_f7Remaining;
extern int g_f7DiagSession;

// Master switch for all NDJSON serialization, buffering, and file output.
extern bool g_ndjsonLoggingEnabled;

// When true, include direction history fields in NDJSON output.
extern bool g_ndjsonIncludeDirHistory;

// Flush the rolling netplay NDJSON history when desync is triggered.
extern "C" void Rollback_NotifyDesyncTriggered(int32_t frame);

// Cancel a delayed desync capture when its battle generation is reset.
extern "C" void Rollback_ClearPendingDesyncCapture();

// Advance the delayed desync capture state from the game thread. Returns true
// while the two-frame post-trigger capture window is still active.
extern "C" bool Rollback_ProcessDesyncDelay(int32_t currentFrame);

// Flush buffered netplay diagnostics during shutdown or peer disconnect.
extern "C" void Rollback_FlushNetplayLive(int32_t frame);

// Write the current rolling history without stopping subsequent capture.
void Rollback_DumpNetplayHistory(int32_t frame);
bool Rollback_ExportNetplayHistory(int32_t frame, const char* path);
void Rollback_ResetNetplayHistoryBuffers();

// Best-effort flush of already-buffered NDJSON from the crash handler.
// Does not initialize logging, capture game state, or mutate the rolling buffers.
bool Rollback_FlushNDJSONOnCrash();


// Start a fresh rolling netplay NDJSON log for a restarted match.
void Rollback_ResetNetplayLiveLog();

// Copies the current nplive session directory into |path| when initialized.
bool Rollback_GetNetplayLiveDirectory(char* path, size_t pathSize);

// Reproduce the native online battle's gameplay serialization switches.
bool Rollback_PatchNativeNetplayDeterminism();
void Rollback_ApplyNetplayDeterminism();

// Enable selected deterministic and combat online branches during custom ENet
// netplay without enabling native session, netpad, or lifecycle behavior.
void Rollback_InitOnlineBattleHook();

// Request a re-simulate from the controller layer — consumed by hkMvc3Main_move.
// Called when late-arriving remote input differs from what was predicted.
void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame);
bool Rollback_ConsumeReSimRequest(int32_t& startFrame, int32_t& endFrame);
void Rollback_ClearPendingReSimulation();
void Rollback_SetMainInstance(uint64_t main);

// Hook setup — installs the sMvc3Main__move hook for re-simulation stepping.
void Rollback_InitHooks();
void Rollback_PatchBattleUiAllocationFailure();
void Rollback_InitNativeNullGuards();

// Save a full snapshot of all MUST regions, keyed by |frame|.
void Rollback_SaveSnapshot(int32_t frame);

// Update the remote input stored in an existing snapshot slot.
// Called from the packet-drain loop when a late-arriving input packet
// provides the remote input for a frame that was already snapshotted.
void Rollback_UpdateRemoteInput(int32_t frame, uint32_t input);

// Restore the snapshot previously saved for |frame|.
void Rollback_RestoreSnapshot(int32_t frame);

// Re-simulate frames [startFrame, endFrame):
//   1. Restore snapshot at startFrame
//   2. Call the full sMvc3Main__move pipeline for each frame
//   3. In multi-resim mode, replace each frame snapshot with its corrected
//      state while retaining the inputs used for that frame
void Rollback_ReSimulate(int32_t startFrame, int32_t endFrame);

struct ResimNotifyPacket;
// Write received resim notify data to the netplay NDJSON log.
// Called on the receiver side when a PACKET_RESIM_NOTIFY arrives.
void Rollback_LogResimNotifyNDJSON(const ResimNotifyPacket& pkt);

// Queue a re-simulation frame's post-state for paired NDJSON logging.
// In offline (F7) mode, writes directly instead of queueing.
void Rollback_QueuePendingResimDump(int32_t frame);

// Build and send a GroundTruthPacket for [startFrame, endFrame) using
// the local g_liveRef history.  Called on the non-simulating side.
extern "C" void Rollback_SendGroundTruth(int32_t startFrame, int32_t endFrame);

// Process all received GroundTruthPackets — match frames against the
// pending resim queue and write paired ground_truth + resim records
// to rdiag.ndjson.
extern "C" void Rollback_ProcessInboundGroundTruth();

// Free all snapshot memory. Call on netplay disconnect / match end.
void Rollback_FreeSnapshots();

// Full C++ replication of sMvc3Main__move at 0x1402594A0. It preserves the
// native call order, repeated singleton lookups, render calls, and Alt+F4 path.
void Rollback_HeadlessTick(uint64_t sMvc3Main);

// Patch/unpatch the Sleep() calls inside sMain__move (0x140521df0).
// enable=true → overwrites both call sites with NOPs for fast re-simulation.
// enable=false → restores original bytes.
void Rollback_PatchSleep(bool enable);

// Patch/unpatch the Present() call inside the post-render sync function
// sRender__beginFrame (call site 0x14053709c). During re-sim Present blocks for ~16ms
// under DXVK; NOPping it lets the function run normally without the wait.
void Rollback_PatchPresent(bool enable);

// Patch/unpatch the vtable+0x50 swapchain calls in sMvc3Main__move's tail
// and inside sRender__beginFrame (3 sites total). These expect a Present to
// have been submitted; with the Present NOP'd they may do unnecessary
// D3D9/Vulkan work during re-sim.
void Rollback_PatchSwapchain(bool enable);

// Patch/unpatch every remaining render/draw call site inside
// sMvc3Main__move.  Comment out entries in the sites[] array inside
// the function body to test which ones are safe to skip during re-sim.
void Rollback_PatchDrawCalls(bool enable);
