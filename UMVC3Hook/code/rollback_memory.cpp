#include "../pch.h"

#include "rollback_memory.h"
#include "mk10utils.h"

namespace {
constexpr uintptr_t DTI_CUNIT = 0x140E173D0;
constexpr uintptr_t DTI_UEFFECT = 0x140E18698;
constexpr uintptr_t DTI_UBASEMODEL = 0x140E17D48;

bool IsObjectKindOf(const void* object, uintptr_t expectedDti)
{
    uint64_t dti = GetObjectDti((uint64_t)object);
    const uint64_t targetDti = _addr(expectedDti);
    for (int depth = 0; dti && depth < 128; ++depth) {
        if (dti == targetDti) return true;
        uint64_t parent = *(const uint64_t*)(dti + 0x20);
        if (parent == dti) return false;
        dti = parent;
    }
    return false;
}
} // namespace

uint64_t GetObjectDti(uint64_t object)
{
    if (!HasDtiVirtual((const void*)object)) return 0;
    uint64_t vtable = *(const uint64_t*)object;
    auto getDti = (uint64_t(__fastcall*)(uint64_t))(*(const uint64_t*)(vtable + 0x20));
    return getDti(object);
}

uint32_t DecodeDtiSize(uint64_t dti)
{
    if (!dti) return 0;
    return ((*(uint32_t*)(dti + 0x30) & 0x7FFFFF) << 2);
}

bool IsLikelyPointer(uint64_t value)
{
    return value >= 0x10000 && value < 0x0000800000000000ull && (value & 7) == 0;
}

bool HasDtiVirtual(const void* object)
{
    if (!object) return false;
    uint64_t vtable = *(const uint64_t*)object;
    return vtable && *(const uint64_t*)(vtable + 0x20) != 0;
}

bool IsUnitObject(const void* object)
{
    return IsObjectKindOf(object, DTI_CUNIT);
}

bool IsEffectObject(const void* object)
{
    return IsObjectKindOf(object, DTI_UEFFECT);
}

bool IsBaseModelObject(const void* object)
{
    return IsObjectKindOf(object, DTI_UBASEMODEL);
}
