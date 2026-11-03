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
