#pragma once

#include <cstddef>
#include <cstdint>

void Rollback_RestoreCollisionBody(void* destination, const uint8_t* source, size_t size);
void Rollback_RestoreExternalAnimBlock(void* destination, const uint8_t* source, size_t size);
