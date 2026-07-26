#pragma once

#include <cstddef>
#include <cstdint>

int Rollback_ResolveDllPath(char* destination, size_t destinationLength,
                            const char* relativePath);
uint64_t Rollback_Fnv1a64(const void* data, size_t length);
