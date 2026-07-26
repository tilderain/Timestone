#pragma once

#include <cstddef>
#include <cstdint>

uint64_t GetObjectDti(uint64_t object);
uint32_t DecodeDtiSize(uint64_t dti);

bool IsLikelyPointer(uint64_t value);
bool IsAccessiblePointer(uint64_t value);
bool IsAccessibleAddress(uint64_t value);
bool IsExecutableAddress(uint64_t value);
bool IsAccessibleRange(uint64_t value, size_t size);
bool HasCallableDtiVirtual(const void* object);

bool IsUnitObject(const void* object);
bool IsEffectObject(const void* object);
bool IsBaseModelObject(const void* object);
