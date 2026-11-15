#pragma once

#include <cstddef>
#include <cstdint>

uint64_t GetObjectDti(uint64_t object);
uint32_t DecodeDtiSize(uint64_t dti);

// Snapshot word classification only; this does not validate memory or lifetime.
bool IsLikelyPointer(uint64_t value);
// Tests callback presence only. The caller owns the object's lifetime.
bool HasDtiVirtual(const void* object);

bool IsUnitObject(const void* object);
bool IsEffectObject(const void* object);
bool IsBaseModelObject(const void* object);

// DTI ancestry test from a DTI pointer (parent at +0x20).
bool IsDtiKindOf(uint64_t dti, uintptr_t expectedDti);

// True when a uGameEffect's owner (+0x280) is the current uStage or one of its
// layout stage objects (uStage+0x14E0). The owner pointer is only compared,
// never dereferenced. Main thread only.
bool IsStageOwnedEffect(uint64_t effect);
