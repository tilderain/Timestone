#pragma once
// Skips recopying snapshot regions whose bytes are already in the destination.
//
// Most of a snapshot is animation work memory that does not change from one
// frame to the next: background characters, idle motion slots, and material
// jobs that are not running. Each such region carries a version that is bumped
// whenever a cheap signal says it may have changed. The version last written
// is remembered per destination buffer, so a save into a buffer that already
// holds the same region at the same version moves no bytes. The buffer layout
// is unchanged: restore and the dump recorder read complete snapshots as before.
//
// The signals are observed correlations (see doc/snapshot_reuse.md), not proofs.
// Validate mode still compares every skipped region and repairs a mismatch.

#include <cstddef>
#include <cstdint>

enum class SnapshotReuseMode {
    Off,       // Copy every region.
    Validate,  // Compare each region a signal calls unchanged; log and copy on a mismatch.
    Trust,     // Skip without comparing.
};
inline constexpr SnapshotReuseMode SNAPSHOT_REUSE_MODE = SnapshotReuseMode::Validate;

enum class SnapshotReuseKind : uint8_t {
    None,           // Always copied.
    ActorBody,      // uActorGameModel-derived body: always copied, owns the regions below.
    Prefix,         // Unchanged while the owner's actor prefix is unchanged.
    MotionBlend,    // Unchanged while the owner's motion slot record `index` is unchanged.
    MaterialTable,  // Material job array `index`: unchanged while no job in it runs.
};

struct SnapshotReuseRegion {
    const uint8_t* ptr = nullptr;
    size_t size = 0;
    uint64_t ownerSlot = 0;  // Address of the owner's pointer field, for the kinds with an owner.
    SnapshotReuseKind kind = SnapshotReuseKind::None;
    uint8_t index = 0;
};

struct SnapshotReuseStats {
    uint64_t saves = 0;
    uint64_t copiedBytes = 0;
    uint64_t reusedBytes = 0;
    uint64_t mismatches = 0;  // Validate mode: regions a signal wrongly called unchanged.
};

// The region list changed. Every region is treated as changed at the next save.
void SnapshotReuse_SetPlan(const SnapshotReuseRegion* regions, int count);
// A restore rewrites live region memory behind the save's back. Between Begin
// and End, report each region (by index in the saved list) whose live bytes
// were set to exactly the saved bytes. Those regions take the version they had
// in the restored buffer, so other buffers saved at that version still match;
// every other region is treated as changed. Without End, all are.
void SnapshotReuse_BeginRestore();
void SnapshotReuse_RegionRestored(int index);
void SnapshotReuse_EndRestore(const uint8_t* buffer);
// `buffer` is being freed or was written by someone else.
void SnapshotReuse_ForgetBuffer(const uint8_t* buffer);
// Write the planned regions, in order and contiguously, into `buffer`.
// Returns the snapshot size in bytes.
size_t SnapshotReuse_Save(uint8_t* buffer);
const SnapshotReuseStats& SnapshotReuse_Stats();
