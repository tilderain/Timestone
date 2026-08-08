#pragma once

struct RollbackSnapshotSlot;

void Effect_InitHooks();
void Effect_CaptureRecipes(RollbackSnapshotSlot& slot);
int Effect_RestoreRecipes(RollbackSnapshotSlot& slot);
void Effect_PrepareUnitRemoval(void* unit);
void Effect_FinishUnitRemoval(void* unit);
void Effect_ReleaseOwnedAllocations(void* effect);
int Effect_RetireCurrent(void* sUnit);
bool Effect_RetireGameEffectModel(void* sUnit, void* effect);
void Effect_SanitizeRetired(void* sUnit);
void Effect_Reset();
