#pragma once

#include <cstdint>

void Rollback_ClearStaleMaterialPointers();
void Rollback_RemoveStaleUiUnits();
void Rollback_SyncActiveCharacterUi();
void Rollback_SyncActiveCharacterVisibility();
void Rollback_RepairOrphanedCutinRenderState();
