#pragma once
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// MtDTI — MT Framework Dynamic Type Information
// ---------------------------------------------------------------------------
struct alignas(8) MtDTI {
    uintptr_t vtable;       // +0x00
    const char* className;  // +0x08
    MtDTI* nextSibling;     // +0x10
    MtDTI* firstChild;      // +0x18
    MtDTI* parent;          // +0x20
    MtDTI* hashNext;        // +0x28
    uint32_t flags;         // +0x30 (packed: size + type flags)
    uint32_t nameHash;      // +0x34 (CRC32 & 0x7FFFFFFF)
};

// Flat entry used for filtered display
struct DtiEntry {
    MtDTI* dti;
    int depth;              // computed depth in hierarchy (0 = root)
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void DtiEditor_Init();
void DtiEditor_Draw();
