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
// Property system types
// ---------------------------------------------------------------------------

// Property type codes (from MtProperty__create* functions)
enum MtPropertyType : uint16_t {
    PROP_CLASS     = 1,   // Sub-class embed
    PROP_CLASSREF  = 2,   // Reference to another class instance
    PROP_BOOL      = 3,   // Boolean
    PROP_U32       = 6,   // Unsigned 32-bit integer
    PROP_S32       = 10,  // Signed 32-bit integer
    PROP_F32       = 12,  // 32-bit float
    PROP_STRING    = 14,  // String (char*)
    PROP_MATRIX    = 19,  // 4x4 matrix
    PROP_VECTOR3   = 20,  // 3-component vector
};

// A property captured at runtime via the hook
struct CapturedProperty {
    const char* name;       // property name string
    uint16_t    type;       // MtPropertyType
    uintptr_t   ownerPtr;   // this pointer (class instance)
    uintptr_t   dataPtr;    // pointer to the actual field in the instance
    uint32_t    nameHash;   // DTI name hash this property belongs to
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void DtiEditor_Init();
void DtiEditor_Draw();
void DtiEditor_InitHooks(class Trampoline* tramp);
