#pragma once

#include <cstdint>
#include <cstdio>

struct RollbackSnapshotSlot;

inline constexpr bool EFFECT_LIFECYCLE_TRACE_ENABLED = false;

void Effect_InitHooks();
bool Effect_DumpCrashTrace(FILE* file, uint64_t object);
void Effect_CaptureRecipes(RollbackSnapshotSlot& slot);
int Effect_RestoreRecipes(RollbackSnapshotSlot& slot);
void Effect_RestoreCharacterStates(RollbackSnapshotSlot& slot);
void Effect_PrepareUnitRemoval(void* unit);
void Effect_FinishUnitRemoval(void* unit);
void Effect_ReleaseOwnedAllocations(void* effect);
bool Effect_RetireOne(void* sUnit, void* effect);
int Effect_RetireCurrent(void* sUnit);
bool Effect_RetireGameEffectModel(void* sUnit, void* effect);
void Effect_SanitizeRetired(void* sUnit);
void Effect_Reset();
