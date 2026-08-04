#pragma once

struct RollbackSnapshotSlot;

void Effect_InitHooks();
void Effect_CaptureRecipes(RollbackSnapshotSlot& slot);
int Effect_RestoreRecipes(RollbackSnapshotSlot& slot);
void Effect_PrepareUnitRemoval(void* unit);
void Effect_FinishUnitRemoval(void* unit);
int Effect_RetireCurrent(void* sUnit);
void Effect_SanitizeRetired(void* sUnit);
void Effect_Reset();
