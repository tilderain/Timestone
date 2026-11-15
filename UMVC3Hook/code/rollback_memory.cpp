#include "../pch.h"

#include "rollback_memory.h"
#include "mk10utils.h"

#include <algorithm>

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

bool IsDtiKindOf(uint64_t dti, uintptr_t expectedDti)
{
    const uint64_t targetDti = _addr(expectedDti);
    for (int depth = 0; dti && depth < 128; ++depth) {
        if (dti == targetDti) return true;
        uint64_t parent = *(const uint64_t*)(dti + 0x20);
        if (parent == dti) return false;
        dti = parent;
    }
    return false;
}

namespace {
constexpr uintptr_t PTR_SSTAGE = 0x140d4f718;
constexpr uint32_t SSTAGE_CURRENT_STAGE_OFFSET = 0x2050;
constexpr uint32_t USTAGE_OBJECT_ARRAY_OFFSET = 0x14E0; // MtArray: count +8, data +0x18
constexpr uint32_t UGAMEEFFECT_OWNER_OFFSET = 0x280;
constexpr uint32_t MAX_STAGE_OWNERS = 255;

struct StageOwnerSet {
    uint64_t stage = 0;
    uint64_t data = 0;
    uint32_t count = 0;
    uint32_t size = 0;
    uint64_t owners[MAX_STAGE_OWNERS + 1]; // sorted
};
StageOwnerSet g_stageOwners;

const StageOwnerSet& CurrentStageOwners()
{
    StageOwnerSet& set = g_stageOwners;
    uint64_t sStage = *(uint64_t*)_addr(PTR_SSTAGE);
    uint64_t stage = sStage ? *(uint64_t*)(sStage + SSTAGE_CURRENT_STAGE_OFFSET) : 0;
    uint32_t count = stage ? *(uint32_t*)(stage + USTAGE_OBJECT_ARRAY_OFFSET + 0x08) : 0;
    uint64_t data = stage ? *(uint64_t*)(stage + USTAGE_OBJECT_ARRAY_OFFSET + 0x18) : 0;
    if (!data || count > MAX_STAGE_OWNERS) count = 0;
    if (stage == set.stage && data == set.data && count == set.count) return set;

    set.stage = stage;
    set.data = data;
    set.count = count;
    set.size = 0;
    if (stage) set.owners[set.size++] = stage;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t object = *(uint64_t*)(data + (uint64_t)i * sizeof(uint64_t));
        if (object) set.owners[set.size++] = object;
    }
    std::sort(set.owners, set.owners + set.size);
    return set;
}
} // namespace

bool IsStageOwnedEffect(uint64_t effect)
{
    const uint64_t owner = *(uint64_t*)(effect + UGAMEEFFECT_OWNER_OFFSET);
    if (!owner) return false;
    const StageOwnerSet& set = CurrentStageOwners();
    return std::binary_search(set.owners, set.owners + set.size, owner);
}
