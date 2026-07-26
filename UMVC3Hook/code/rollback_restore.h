#pragma once

#include <cstddef>
#include <cstdint>

struct RollbackSnapshotSlot;

void Rollback_RestoreCollisionBody(void* destination, const uint8_t* source, size_t size);
void Rollback_RestoreExternalAnimBlock(void* destination, const uint8_t* source, size_t size);
void Rollback_CaptureCollisionNodeClones(RollbackSnapshotSlot& slot);
void Rollback_RestoreCollisionNodeClones(RollbackSnapshotSlot& slot);
void Rollback_DestroyCollisionNodeClones(RollbackSnapshotSlot& slot);
