#pragma once

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <memory>

// Development diagnostics. Native lifetime changes are controlled separately below.
inline constexpr bool ROLLBACK_UNIT_ENABLED = true;
// Writes the per-event LIFE and LIFESTATS lines to the hook log. Off silences
// only that output: events are still recorded and drained, so restoration,
// failure reports and the crash report's pending events are unaffected.
inline constexpr bool ROLLBACK_UNIT_LOG_ENABLED = false;
// Master retention switch (historical name), including intact shot/model owners.
// Snapshot ownership outlives trace recording.
inline constexpr bool EMPTY_EFFECT_QUARANTINE_ENABLED = true;
// One transaction for saved lifecycle, CPU graph state and scheduler membership.
inline constexpr bool EFFECT_RESTORE_ENABLED = true;
// Active graph snapshots use the same owned-allocation descriptor as retirement.
inline constexpr bool EFFECT_SIMULATION_SNAPSHOTS_ENABLED = true;
// Common ordinary-generator ownership, using native DTI sizes and pool strides.
inline constexpr bool ORDINARY_CPU_EFFECT_GRAPHS_ENABLED = true;

// Rewinds shots that exist in the restore target, in place: retained storage,
// common and class-specific bytes, constraint links, registration and scheduling.
// Requires shot payload capture on every gameplay snapshot.
inline constexpr bool SAVED_SHOT_RESTORE_ENABLED = true;

void RollbackUnit_Install();
void RollbackUnit_SetFrame(int32_t frame, bool resimulating);
// Event names must have static storage (the bounded queue retains the pointer).
void RollbackUnit_Marker(const char* event, int32_t frame);
void RollbackUnit_Flush();
// Crash-only, nonblocking queue snapshot. Does not inspect native object memory.
void RollbackUnit_WritePending(FILE* file);
// Request final drain at main-tick entry; poll until retained objects and logs drain.
bool RollbackUnit_FinishCapture();
// Snapshot census, generation leases, and shared graph/scheduler restoration.
void RollbackUnit_DropSnapshot(int32_t frame);
struct RollbackUnitSnapshot;
// Owned by RollbackSnapshotSlot; snapdump shares the same capture, not its leases.
void RollbackUnit_SaveSnapshot(int32_t frame, std::shared_ptr<RollbackUnitSnapshot>& storage);
void RollbackUnit_CheckSnapshot(int32_t frame);
void RollbackUnit_ClearSnapshots();
void RollbackUnit_RestoreEffects(int32_t frame);
void RollbackUnit_WaitForRenderWorker();
// Withdraw abandoned future shots without kill callbacks or physical destruction.
void RollbackUnit_RetireFutureShots(int32_t frame);
// Rewind every shot in the target frame, after withdrawal and effect restore.
// An unsupported or inconsistent case is a fatal lifetime failure, not a skip.
void RollbackUnit_RestoreShots(int32_t frame);

// A weak unit reference held outside the tracked objects (a character's shot or
// effect slot) across a restore to `frame`. Returns the value to store:
// `saved` when it names a unit of that frame's snapshot that is live again,
// null when `live` names a unit that is now withdrawn or held, otherwise `live`.
// Identity comes from the registry; no address is ever dereferenced.
void* RollbackUnit_ResolveUnitReference(int32_t frame, void* saved, void* live);

struct RollbackUnitSnapshotCounts {
    uint32_t shots = 0, effects = 0, shotModels = 0;
};
using RollbackUnitSnapshotVisitor = void (*)(void* context, const char* name,
    const void* address, uint64_t ownerSlot, uint64_t vtable,
    const void* bytes, size_t size);
// Dump-time only, at a quiescent fence. Reads snapshot-owned bytes without
// dereferencing historical native pointers. Optional shot/model observations
// describe the dump-time world, not the requested snapshot frame.
RollbackUnitSnapshotCounts RollbackUnit_VisitSnapshot(int32_t frame, const RollbackUnitSnapshot* snapshot,
    bool observeShots, RollbackUnitSnapshotVisitor visitor, void* context);
