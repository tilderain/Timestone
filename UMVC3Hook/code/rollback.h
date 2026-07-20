#pragma once
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

// Re-sim request flags, set by FrameStepper_Process (HookUpdate thread),
// consumed by hkMvc3Main_move (main game thread) so the re-sim runs on
// the correct thread for D3D/render safety.  -1 means no request pending.
extern int32_t g_resimRequestedStart;
extern int32_t g_resimRequestedEnd;

// When true, send a ResimNotify packet every frame (for NDJSON debugging),
// not only when a rollback conflict triggers re-simulation.
extern bool g_resimNotifyEveryFrame;

// Snapshot-restore request flag — same deferral pattern.
// Set by FrameStepper_Process (F6 key), consumed by hkMvc3Main_move at the
// next frame boundary to avoid mid-frame memory corruption.
extern int32_t g_restoreRequestedFrame;

// Non-negative after Rollback_ReSimulate completes, holding the frame number
// of the first post-resim live tick.  hkprocessFrameInput checks this to log
// diagnostic info at the resim→live boundary.  Cleared after first use.
extern int32_t g_postResimFrame;

// When true (default), include direction history fields in NDJSON output.
extern bool g_ndjsonIncludeDirHistory;

// F7 re-sim test state machine:
//   0 = idle
//   1 = save snapshot on next frame boundary, then enter state 2
//   2 = counting down g_f7Remaining frames of normal play
//       when g_f7Remaining hits 0 → restore snapshot + re-sim
extern int      g_f7State;
extern int32_t  g_f7StartFrame;
extern int      g_f7Remaining;

// Diag session for per-frame live capture during F7 recording phase.
// Set >=0 during recording; Rollback_ReSimulate reuses this session ID
// so live and resim dumps appear side by side in the same files.
extern int      g_f7DiagSession;

// Request a re-simulate from the controller layer — consumed by hkMvc3Main_move.
// Called when late-arriving remote input differs from what was predicted.
void Rollback_RequestReSimulate(int32_t startFrame, int32_t endFrame);

// Hook setup — installs the sMvc3Main__move hook for re-simulation stepping.
void Rollback_InitHooks();

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
//      (snapshots are NOT re-saved during re-sim — the next normal frame
//       boundary save handles the corrected state)
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

// Headless frame tick — runs game-logic sub-functions from sMvc3Main__move
// but SKIPS: Sleep (frame pacing), all vtable+0x48 calls (draw/render),
// all sRender/sPrimitive calls, the Present/swap at 0x140537080.
// Safe to call many times per real frame for re-simulation.
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
