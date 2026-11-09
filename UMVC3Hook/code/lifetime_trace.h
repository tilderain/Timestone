#pragma once

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <memory>

// Development diagnostics. Native lifetime changes are controlled separately below.
inline constexpr bool LIFETIME_TRACE_ENABLED = true;
// Master retention switch (historical name), including intact shot/model owners.
// Snapshot ownership outlives trace recording.
inline constexpr bool EMPTY_EFFECT_QUARANTINE_ENABLED = true;
// One transaction for saved lifecycle, CPU graph state and scheduler membership.
inline constexpr bool EFFECT_RESTORE_ENABLED = true;
// Active graph snapshots use the same owned-allocation descriptor as retirement.
inline constexpr bool EFFECT_SIMULATION_SNAPSHOTS_ENABLED = true;
// Common ordinary-generator ownership, using native DTI sizes and pool strides.
inline constexpr bool ORDINARY_CPU_EFFECT_GRAPHS_ENABLED = true;

void LifetimeTrace_Install();
void LifetimeTrace_SetFrame(int32_t frame, bool resimulating);
// Event names must have static storage (the bounded queue retains the pointer).
void LifetimeTrace_Marker(const char* event, int32_t frame);
void LifetimeTrace_Flush();
// Crash-only, nonblocking queue snapshot. Does not inspect native object memory.
void LifetimeTrace_WritePending(FILE* file);
// Request final drain at main-tick entry; poll until retained objects and logs drain.
bool LifetimeTrace_FinishCapture();
// Snapshot census, generation leases, and shared graph/scheduler restoration.
void LifetimeTrace_DropSnapshot(int32_t frame);
struct LifetimeSnapshot;
// Owned by RollbackSnapshotSlot; snapdump shares the same capture, not its leases.
void LifetimeTrace_SaveSnapshot(int32_t frame, std::shared_ptr<LifetimeSnapshot>& storage);
void LifetimeTrace_CheckSnapshot(int32_t frame);
void LifetimeTrace_ClearSnapshots();
void LifetimeTrace_RestoreEffects(int32_t frame);

struct LifetimeSnapshotCounts {
    uint32_t shots = 0, effects = 0, shotModels = 0;
};
using LifetimeSnapshotVisitor = void (*)(void* context, const char* name,
    const void* address, uint64_t ownerSlot, uint64_t vtable,
    const void* bytes, size_t size);
// Dump-time only, at a quiescent fence. Reads snapshot-owned bytes without
// dereferencing historical native pointers. Optional shot/model observations
// describe the dump-time world, not the requested snapshot frame.
LifetimeSnapshotCounts LifetimeTrace_VisitSnapshot(int32_t frame, const LifetimeSnapshot* snapshot,
    bool observeShots, LifetimeSnapshotVisitor visitor, void* context);
