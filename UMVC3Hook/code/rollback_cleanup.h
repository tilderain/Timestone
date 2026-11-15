#pragma once

#include <cstdint>

struct RollbackSnapshotSlot;

void Rollback_ClearStaleMaterialPointers();
void Rollback_RemoveStaleUiUnits();
void Rollback_SyncActiveCharacterUi();
void Rollback_SyncActiveCharacterVisibility();

// uCutIn rollback: capture registered cut-ins with a snapshot, rewind them
// before the character render-state restore, then end the cut-in render state
// of participants whose cut-in no longer exists.
bool Rollback_HasActiveCutin();
void Rollback_CaptureCutins(RollbackSnapshotSlot& slot);
void Rollback_RewindCutins(const RollbackSnapshotSlot& slot);
void Rollback_RepairCutinRenderState(const RollbackSnapshotSlot& slot);
