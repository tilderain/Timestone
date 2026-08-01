#pragma once

#include <cstdint>

void Rollback_ClearStalePointers();
void Rollback_DetachDrStrangeEffects();
void Rollback_RemoveStaleUiUnits();
void Rollback_ClearGameEffectSlots();
void Rollback_SyncActiveCharacterUi();
void Rollback_SyncActiveCharacterVisibility();
void Rollback_RepairOrphanedCutinRenderState();
