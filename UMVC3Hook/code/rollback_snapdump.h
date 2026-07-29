#pragma once

#include <cstdint>

// Code-only diagnostic toggle. Set to 0 for a build with the recorder and every
// call site preprocessed out, producing no runtime or memory overhead.
#define ROLLBACK_SNAPDUMP_ENABLED 1

#if ROLLBACK_SNAPDUMP_ENABLED

struct RollbackSnapshotSlot;

void SnapDump_CaptureSnapshot(const RollbackSnapshotSlot& slot);
void SnapDump_UpdateInputs(int32_t frame, uint32_t localInput, uint32_t remoteInput);
bool SnapDump_WriteHistory(int32_t triggerFrame);
void SnapDump_Reset();

#endif
