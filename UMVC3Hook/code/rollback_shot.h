#pragma once

#include <cstdint>
#include <vector>

struct RollbackShotFlightState;
struct RollbackShotRecipe;
struct RollbackSnapshotSlot;

void Shot_InitHooks();
bool Shot_HooksReady();
void Shot_CaptureRecipes(RollbackSnapshotSlot& slot);
bool Shot_GetRecipe(void* shot, RollbackShotRecipe& recipe);
void Shot_CollectCurrent(std::vector<void*>& shots);
void* Shot_SpawnOriginal(void* manager, const RollbackShotRecipe& recipe,
                         void* objectRef1, void* objectRef2);
void Shot_TrackReconstructed(void* shot, const RollbackShotRecipe& recipe);
void Shot_Reset();

void Shot_CaptureFlight(void* shot, RollbackShotFlightState& state);
void Shot_ApplyFlight(void* shot, const RollbackShotFlightState& state);
void Shot_RestoreArmState(void* shot, const RollbackShotRecipe& recipe);
void Shot_DetachEmbeddedEffect(void* shot);
void Shot_ReleaseFreshEmbeddedEffectGraph(void* shot);
int Shot_SanitizeCollisionSlotsForDestruction(void* shot);
int Shot_SanitizeUnitCategoryLists(void* sUnit, uint32_t firstCategory,
                                   uint32_t lastCategory);
void Shot_RemoveFromAllMultiMaskArrays(void* sUnit, void* unit);
