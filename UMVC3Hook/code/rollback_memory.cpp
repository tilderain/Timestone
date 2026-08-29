#include "../pch.h"

#include "rollback_memory.h"
#include "mk10utils.h"

#include <vector>

namespace {

constexpr uintptr_t DTI_CUNIT = 0x140E173D0;
constexpr uintptr_t DTI_UEFFECT = 0x140E18698;
constexpr uintptr_t DTI_UBASEMODEL = 0x140E17D48;
constexpr size_t MAX_CACHED_MEMORY_REGIONS = 256;
constexpr size_t MAX_GAME_EXECUTABLE_RANGES = 16;

struct AddressRange {
    uint64_t start;
    uint64_t end;
};

struct GameExecutableRanges {
    AddressRange ranges[MAX_GAME_EXECUTABLE_RANGES];
    size_t count;
};

struct CachedMemoryRegion {
    uint64_t start;
    uint64_t end;
    bool accessible;
    bool executable;
};

thread_local std::vector<CachedMemoryRegion> g_memoryRegions;
thread_local LONG g_memoryCacheEpochSeen = 0;
thread_local size_t g_lastMemoryRegion = SIZE_MAX;
thread_local size_t g_nextMemoryRegionReplacement = 0;
volatile LONG g_memoryCacheEpoch = 1;

bool IsGameImageRange(uint64_t address, size_t size)
{
    static const uint64_t imageBase = (uint64_t)GetMKXEntryPoint();
    static const uint64_t imageEnd = []() {
        const auto* dos = (const IMAGE_DOS_HEADER*)GetMKXEntryPoint();
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return (uint64_t)0;
        const auto* nt = (const IMAGE_NT_HEADERS64*)(
            (const uint8_t*)dos + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return (uint64_t)0;
        return (uint64_t)dos + nt->OptionalHeader.SizeOfImage;
    }();
    return address >= imageBase && address <= imageEnd &&
        size <= imageEnd - address;
}

bool IsGameExecutableAddress(uint64_t address)
{
    static const GameExecutableRanges executableRanges = []() {
        GameExecutableRanges result = {};
        const auto* dos = (const IMAGE_DOS_HEADER*)GetMKXEntryPoint();
        if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return result;
        const auto* nt = (const IMAGE_NT_HEADERS64*)(
            (const uint8_t*)dos + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return result;

        const uint64_t imageBase = (uint64_t)dos;
        const uint64_t imageSize = nt->OptionalHeader.SizeOfImage;
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections &&
                         result.count < MAX_GAME_EXECUTABLE_RANGES; ++i) {
            if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const uint64_t start = section[i].VirtualAddress;
            const uint64_t size = std::max<uint64_t>(
                section[i].Misc.VirtualSize, section[i].SizeOfRawData);
            if (start >= imageSize || size == 0) continue;
            result.ranges[result.count++] = {
                imageBase + start,
                imageBase + std::min(imageSize, start + size),
            };
        }
        return result;
    }();

    for (size_t i = 0; i < executableRanges.count; ++i) {
        const AddressRange& range = executableRanges.ranges[i];
        if (address >= range.start && address < range.end) return true;
    }
    return false;
}

bool IsGameDti(uint64_t dti)
{
    return IsGameImageRange(dti, 0x38) &&
        IsGameImageRange(*(const uint64_t*)dti, 0x10);
}

void SynchronizeMemoryCacheEpoch()
{
    LONG epoch = InterlockedCompareExchange(&g_memoryCacheEpoch, 0, 0);
    if (g_memoryCacheEpochSeen == epoch) return;
    g_memoryRegions.clear();
    g_lastMemoryRegion = SIZE_MAX;
    g_nextMemoryRegionReplacement = 0;
    g_memoryCacheEpochSeen = epoch;
}

const CachedMemoryRegion* GetCachedMemoryRegion(uint64_t address)
{
    SynchronizeMemoryCacheEpoch();
    if (g_lastMemoryRegion < g_memoryRegions.size()) {
        const CachedMemoryRegion& region = g_memoryRegions[g_lastMemoryRegion];
        if (address >= region.start && address < region.end) return &region;
    }
    for (size_t i = 0; i < g_memoryRegions.size(); ++i) {
        const CachedMemoryRegion& region = g_memoryRegions[i];
        if (address >= region.start && address < region.end) {
            g_lastMemoryRegion = i;
            return &region;
        }
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) return nullptr;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const bool accessible = mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    CachedMemoryRegion queriedRegion = {
        (uint64_t)mbi.BaseAddress,
        (uint64_t)mbi.BaseAddress + mbi.RegionSize,
        accessible,
        accessible && (mbi.Protect & executable) != 0,
    };
    if (g_memoryRegions.size() < MAX_CACHED_MEMORY_REGIONS) {
        g_memoryRegions.push_back(queriedRegion);
        g_lastMemoryRegion = g_memoryRegions.size() - 1;
    } else {
        g_lastMemoryRegion = g_nextMemoryRegionReplacement;
        g_memoryRegions[g_lastMemoryRegion] = queriedRegion;
        g_nextMemoryRegionReplacement =
            (g_nextMemoryRegionReplacement + 1) % MAX_CACHED_MEMORY_REGIONS;
    }
    return &g_memoryRegions[g_lastMemoryRegion];
}

bool IsObjectKindOf(const void* object, uintptr_t expectedDti)
{
    if (!HasCallableDtiVirtual(object)) return false;

    uint64_t dti = GetObjectDti((uint64_t)object);
    const uint64_t targetDti = _addr(expectedDti);
    for (int depth = 0; dti && depth < 128; ++depth) {
        if (!IsGameDti(dti)) return false;
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
    uint64_t vtable = *(uint64_t*)object;
    if (IsGameImageRange(vtable, 0x28)) {
        auto getDti = (uint64_t(__fastcall*)(uint64_t))(*(uint64_t*)(vtable + 0x20));
        const uint64_t getDtiAddress = (uint64_t)getDti;
        if (getDti && (IsGameExecutableAddress(getDtiAddress) ||
                       IsExecutableAddress(getDtiAddress))) {
            uint64_t dti = getDti(object);
            return IsGameDti(dti) ? dti : 0;
        }
    }
    return 0;
}

uint32_t DecodeDtiSize(uint64_t dti)
{
    if (!dti || dti < 0x140000000) return 0;
    return ((*(uint32_t*)(dti + 0x30) & 0x7FFFFF) << 2);
}

bool IsLikelyPointer(uint64_t value)
{
    return value >= 0x10000 && value < 0x0000800000000000ull && (value & 7) == 0;
}

bool IsAccessiblePointer(uint64_t value)
{
    if (!IsLikelyPointer(value)) return false;
    if (const CachedMemoryRegion* region = GetCachedMemoryRegion(value))
        return region->accessible;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)value, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

bool IsAccessibleAddress(uint64_t value)
{
    if (value < 0x10000 || value >= 0x0000800000000000ull) return false;
    if (const CachedMemoryRegion* region = GetCachedMemoryRegion(value))
        return region->accessible;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)value, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

bool IsExecutableAddress(uint64_t value)
{
    if (value < 0x10000 || value >= 0x0000800000000000ull) return false;
    if (const CachedMemoryRegion* region = GetCachedMemoryRegion(value))
        return region->executable;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)value, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executable) != 0;
}

bool IsAccessibleRange(uint64_t value, size_t size)
{
    if (value < 0x10000 || value >= 0x0000800000000000ull || size == 0 ||
        size - 1 > UINT64_MAX - value) {
        return false;
    }

    if (const CachedMemoryRegion* region = GetCachedMemoryRegion(value))
        return region->accessible && value + size <= region->end;

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)value, &mbi, sizeof(mbi)) == 0 ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }

    const uint64_t regionEnd = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    return value + size <= regionEnd;
}

bool HasCallableDtiVirtual(const void* object)
{
    uint64_t address = (uint64_t)object;
    if (!IsAccessibleRange(address, sizeof(uint64_t))) return false;

    uint64_t vtable = *(const uint64_t*)object;
    if (!IsAccessibleRange(vtable, 0x28)) return false;

    uint64_t getDti = *(const uint64_t*)(vtable + 0x20);
    return getDti && IsExecutableAddress(getDti);
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

void InvalidateMemoryQueryCache()
{
    InterlockedIncrement(&g_memoryCacheEpoch);
}

ScopedMemoryQueryCache::ScopedMemoryQueryCache()
{
    InvalidateMemoryQueryCache();
}

ScopedMemoryQueryCache::~ScopedMemoryQueryCache()
{
    InvalidateMemoryQueryCache();
}
