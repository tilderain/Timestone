#pragma once

#include <cstdint>
#include <cstdio>

struct RollbackSnapshotSlot;

inline constexpr bool EFFECT_LIFECYCLE_TRACE_ENABLED = false;

void Effect_InitHooks();
bool Effect_DumpCrashTrace(FILE* file, uint64_t object);
void Effect_CaptureRecipes(RollbackSnapshotSlot& slot);
void Effect_ReleaseSnapshotResources(RollbackSnapshotSlot& slot);
int Effect_RestoreRecipes(RollbackSnapshotSlot& slot);
void Effect_RestoreCharacterStates(RollbackSnapshotSlot& slot);
void Effect_RestoreCharacterReferences(RollbackSnapshotSlot& slot);
// Requires a validated native unit/vtable (e.g. a live sUnit list entry).
bool Effect_IsExAtkSpaceUnit(const void* unit);
// Headless dispatch may omit the boundary check for a live native-owned resource.
bool Effect_IsExAtkSpaceTransitionEffect(const void* effect, bool validateResource = true);
// Restore-only: requires the quiescent main-move snapshot boundary.
void Effect_FlushTrackedForRestore(void* manager);
void Effect_PrepareUnitRemoval(void* unit);
void Effect_FinishUnitRemoval(void* unit);
void Effect_ReleaseOwnedAllocations(void* effect);
bool Effect_RetireOne(void* sUnit, void* effect);
int Effect_RetireCurrent(void* sUnit);
bool Effect_RetireGameEffectModel(void* sUnit, void* effect);
void Effect_SanitizeRetired(void* sUnit);
void Effect_Reset();
