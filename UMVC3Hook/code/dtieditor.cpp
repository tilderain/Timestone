// dtieditor.cpp
// MtDTI type hierarchy inspector and editor — ImGui-based.
// Also captures and displays MtProperty entries via a hook on
// cAtkCtrl__TargetInfo__createProperty_BF60 (0x14000bf60).
#include "../pch.h"
#include "dtieditor.h"
#include "umvc3menu.h"
#include "mk10utils.h"
#include "../imgui/imgui.h"
#include "../MemoryMgr.h"
#include "../utils/Trampoline.h"
#include "propertytable.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <unordered_map>

using namespace Memory::VP;

// HookLog — declared extern in dllmain.cpp
extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------

// Extract runtime address from a RIP-relative LEA instruction at a given VA.
static uintptr_t ReadLeaTarget(uintptr_t instructionVa)
{
    uintptr_t runtimeInst = (uintptr_t)_addr(instructionVa);
    int32_t disp = *(int32_t*)(runtimeInst + 3);
    return (runtimeInst + 7) + (uintptr_t)disp;
}

// 256-entry hash table — extract its runtime address from the LEA inside
// MtDTI__findByHash @ 0x14045d980:  LEA RDX,[rip+disp]
static uintptr_t GetHashTableAddr()
{
    static uintptr_t cached = 0;
    if (!cached) cached = ReadLeaTarget(0x14045d980);
    return cached;
}

// Root sentinel — extract its runtime address from the LEA inside
// MtDTI__MtDTI @ 0x14045d8e7:  LEA RAX,[rip+disp]
static uintptr_t GetRootSentinelAddr()
{
    static uintptr_t cached = 0;
    if (!cached) cached = ReadLeaTarget(0x14045d8e7);
    return cached;
}

// MtCRC__getCRC for computing DTI hashes
static constexpr uintptr_t MTCRC_GETCRC_VA   = 0x1404c4590;

// cAtkCtrl__TargetInfo__createProperty_BF60 — universal property registration
static constexpr uintptr_t CREATE_PROPERTY_HOOK_VA = 0x14000bf60;

// ---------------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------------
static std::vector<MtDTI*> g_allDtis;          // every DTI collected from hash table
static std::vector<DtiEntry> g_filtered;        // filtered flat list for display
static std::vector<CapturedProperty> g_props;   // all captured properties
static char g_filter[256] = {};                 // search filter text
static MtDTI* g_selected = nullptr;             // currently selected DTI
static bool g_initialized = false;              // whether we've done first collection
static bool g_propsInitialized = false;         // whether hooks are installed

// Property filter
static char g_propFilter[256] = {};
static bool g_showPropPane = true;
static bool g_filterLoadedOnly = true;  // only show DTIs with captured properties
static bool g_filterEditableOnly = false; // only show DTIs with editable properties

// Forward declarations for allocation tracking
struct MtDTIDescriptor;
static uint32_t ComputeDtiHash(const char* name);
static void RecordPropertyOwner(const char* className, uintptr_t ownerPtr);
static void InstallAllocationHook();

// ---------------------------------------------------------------------------
// Known singleton instances for manual property triggering
// ---------------------------------------------------------------------------
struct SingletonEntry {
    const char* dtiName;
    uintptr_t instancePtrAddr;    // address that holds the instance pointer (read via _addr)
    uintptr_t createPropertyAddr; // createProperty function VA
};
static const SingletonEntry kKnownSingletons[] = {
    {"sBattleSetting", 0x140d50e58, 0x14024bdf0},
};
static constexpr int kKnownSingletonCount = sizeof(kKnownSingletons) / sizeof(kKnownSingletons[0]);

static const SingletonEntry* FindSingletonEntry(const char* dtiName)
{
    if (!dtiName) return nullptr;
    for (int i = 0; i < kKnownSingletonCount; i++) {
        if (strcmp(kKnownSingletons[i].dtiName, dtiName) == 0)
            return &kKnownSingletons[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// DTI instance tracking via vtable scanning
// ---------------------------------------------------------------------------
struct VtableMapEntry {
    uintptr_t vtableAddr;   // runtime address of the vtable array in .rdata
    const char* className;  // DTI class name from property table
    int instanceCount;      // live instance count (refreshed by scan)
};
static std::vector<VtableMapEntry> g_vtableMap;
static bool g_vtableMapBuilt = false;

// Build vtable→class mapping by scanning .rdata for createProperty function
// pointers.  createProperty can be at any vtable index; we find the actual
// vtable base (vtable[0]) by scanning backward from the match.
static void BuildVtableMap()
{
    if (g_vtableMapBuilt) return;
    g_vtableMap.clear();

    uintptr_t rdataStart = (uintptr_t)_addr(0x140a5b000);
    uintptr_t rdataEnd   = (uintptr_t)_addr(0x140c50fff);
    uintptr_t textStart  = (uintptr_t)_addr(0x140001000);
    uintptr_t textEnd    = (uintptr_t)_addr(0x140a5afff);

    // Build a bloom-filter-like approach: insert all createProperty addrs into a set
    // so we can scan .rdata in one linear pass.
    // Use an std::set for the address lookup.
    struct CreatePropEntry {
        uintptr_t addr;
        const char* name;
        bool operator<(const CreatePropEntry& o) const { return addr < o.addr; }
    };
    std::vector<CreatePropEntry> sortedTable;
    sortedTable.reserve(g_createPropertyTableCount);
    for (size_t i = 0; i < g_createPropertyTableCount; i++) {
        uintptr_t fnAddr = (uintptr_t)_addr(g_createPropertyTable[i].address);
        sortedTable.push_back({fnAddr, g_createPropertyTable[i].className});
    }
    std::sort(sortedTable.begin(), sortedTable.end());

    // Helper: binary search for addr in sortedTable
    auto findInTable = [&](uintptr_t val) -> const char* {
        auto it = std::lower_bound(sortedTable.begin(), sortedTable.end(), CreatePropEntry{val, nullptr},
            [](const CreatePropEntry& a, const CreatePropEntry& b) { return a.addr < b.addr; });
        if (it != sortedTable.end() && it->addr == val) return it->name;
        return nullptr;
    };

    // Linear scan of .rdata
    for (uintptr_t p = rdataStart; p + 32 <= rdataEnd; p += 8) {
        uintptr_t val = *(uintptr_t*)p;
        const char* className = findInTable(val);
        if (!className) continue;

        // Verify: next 3 entries should also be valid .text addresses
        bool validVtable = true;
        for (int i = 1; i <= 3; i++) {
            uintptr_t e = *(uintptr_t*)(p + i * 8);
            if (e < textStart || e >= textEnd) { validVtable = false; break; }
        }
        if (!validVtable) continue;

        // Find the actual vtable base by scanning backward for contiguous
        // .text pointers.  The createProperty slot may be at any index.
        uintptr_t vtableBase = p;
        while (vtableBase >= rdataStart + 8) {
            uintptr_t candidate = vtableBase - 8;
            uintptr_t cval = *(uintptr_t*)candidate;
            if (cval >= textStart && cval < textEnd)
                vtableBase = candidate;
            else
                break;
        }

        // Check for duplicate (inheritance — same vtable, different class)
        bool dup = false;
        for (auto& existing : g_vtableMap) {
            if (existing.vtableAddr == vtableBase) dup = true;
        }
        if (!dup) {
            g_vtableMap.push_back({vtableBase, className, 0});
        }
    }

    g_vtableMapBuilt = true;
    HookLog("DTI Editor: built vtable map with %zu entries\n", g_vtableMap.size());
}

// Scan writable memory for known vtable pointers to count live instances.
// Returns true if the scan completed (may be false if the process heap is locked).
static bool ScanForInstances()
{
    if (!g_vtableMapBuilt) BuildVtableMap();
    if (g_vtableMap.empty()) return false;

    // Reset counts
    for (auto& entry : g_vtableMap) entry.instanceCount = 0;

    // Build hash set for O(1) lookup
    std::unordered_map<uintptr_t, int*> vtableLookup;
    vtableLookup.reserve(g_vtableMap.size() * 2);
    for (auto& entry : g_vtableMap) {
        vtableLookup[entry.vtableAddr] = &entry.instanceCount;
    }

    // 1. Scan .data section (static/global objects — fastest)
    uintptr_t dataStart = (uintptr_t)_addr(0x140c51000);
    uintptr_t dataEnd   = (uintptr_t)_addr(0x140e24047);
    for (uintptr_t p = dataStart; p + 8 <= dataEnd; p += 8) {
        auto it = vtableLookup.find(*(uintptr_t*)p);
        if (it != vtableLookup.end()) (*it->second)++;
    }

    // 2. Scan the default process heap (dynamic objects)
    HANDLE hHeap = GetProcessHeap();
    if (hHeap) {
        PROCESS_HEAP_ENTRY phe = {};
        // HeapWalk can fail mid-iteration in multi-threaded programs — that's ok
        while (HeapWalk(hHeap, &phe)) {
            if ((phe.wFlags & PROCESS_HEAP_ENTRY_BUSY) && phe.lpData) {
                auto it = vtableLookup.find(*(uintptr_t*)phe.lpData);
                if (it != vtableLookup.end()) (*it->second)++;
            }
        }
    }

    return true;
}

// Returns >0 live instances for the given DTI name, or -1 if unknown.
static int GetInstanceCount(const char* dtiName)
{
    if (!dtiName) return -1;
    for (auto& entry : g_vtableMap) {
        if (strcmp(entry.className, dtiName) == 0) return entry.instanceCount;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// CRC32 hashing (same as game's MtCRC__getCRC)
// ---------------------------------------------------------------------------
static uint32_t ComputeDtiHash(const char* name)
{
    typedef uint64_t(__fastcall* MtCRC_t)(const char*, uint64_t);
    MtCRC_t getCRC = (MtCRC_t)_addr(MTCRC_GETCRC_VA);
    if (!getCRC || !name) return 0;
    return (uint32_t)(getCRC(name, 0xFFFFFFFF) & 0x7FFFFFFF);
}

// ---------------------------------------------------------------------------
// Crash-Proof String and Hash Helpers (VirtualQuery-guarded, MinGW-compatible)
// ---------------------------------------------------------------------------

static bool IsReadableCStr(const char* str)
{
    if (!str) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(str, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect;
    if (prot & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (prot & kRead) != 0;
}

// Case-insensitive substring search (no shlwapi dependency).
static bool ContainsInsensitive(const char* str, const char* substr)
{
    if (!str || !substr) return false;
    if (substr[0] == 0) return true;
    for (; *str; ++str) {
        const char* a = str;
        const char* b = substr;
        while (*a && *b && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b)) {
            ++a; ++b;
        }
        if (*b == 0) return true;
    }
    return false;
}

static bool SafeStrStr(const char* str, const char* substr)
{
    if (!IsReadableCStr(str) || !IsReadableCStr(substr)) return false;
    // Case-insensitive search via ContainsInsensitive
    return ContainsInsensitive(str, substr);
}

static const char* SafeStrRChr(const char* str, int ch)
{
    if (!IsReadableCStr(str)) return nullptr;
    return strrchr(str, ch);
}

static uint32_t SafeComputeDtiHash(const char* name)
{
    if (!IsReadableCStr(name)) return 0;
    return ComputeDtiHash(name);
}

static const char* GetSafeClassName(MtDTI* dti)
{
    if (!dti || !IsReadableCStr(dti->className)) return "(corrupt name)";
    return dti->className;
}

// ---------------------------------------------------------------------------
// Walk the global hash table and collect every registered DTI.
// ---------------------------------------------------------------------------
static void CollectAllDtis()
{
    g_allDtis.clear();
    uintptr_t* hashTable = (uintptr_t*)GetHashTableAddr();

    for (int bucket = 0; bucket < 256; bucket++) {
        for (uintptr_t ptr = hashTable[bucket]; ptr; ) {
            MtDTI* dti = (MtDTI*)ptr;
            g_allDtis.push_back(dti);
            ptr = (uintptr_t)dti->hashNext;
        }
    }
}

// ---------------------------------------------------------------------------
// Find a DTI by class name (linear scan — small set, called during init).
// ---------------------------------------------------------------------------
static MtDTI* FindDtiByName(const char* className)
{
    if (!className) return nullptr;
    for (MtDTI* dti : g_allDtis) {
        if (dti->className && strcmp(dti->className, className) == 0)
            return dti;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Look up the createProperty function for a given return address,
// returning the DTI class name.  Uses binary search on the sorted table.
// ---------------------------------------------------------------------------
static const char* FindCreatePropertyClass(uintptr_t returnAddr)
{
    // Binary search: find the largest address <= returnAddr
    if (g_createPropertyTableCount == 0) return nullptr;
    if (returnAddr < g_createPropertyTable[0].address) return nullptr;

    size_t lo = 0, hi = g_createPropertyTableCount - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (g_createPropertyTable[mid].address <= returnAddr)
            lo = mid;
        else
            hi = mid - 1;
    }
    return g_createPropertyTable[lo].className;
}

// ---------------------------------------------------------------------------
// Hook handler for cAtkCtrl__TargetInfo__createProperty_BF60.
// Called for EVERY property registration in the game.
// ---------------------------------------------------------------------------

// Original function type: void(__fastcall*)(void* ctx, void* propDesc)
typedef void(__fastcall* CreatePropertyFn)(void* ctx, void* propDesc);
static CreatePropertyFn g_origCreateProperty = nullptr;

// The real function body is at the target of the JMP at 0x14000bf60.
// Decompilation shows:  JMP 0x14000c630  (tail-call to MtPropertyList__newElement+link)
static constexpr uintptr_t CREATE_PROPERTY_REAL_VA = 0x14000c630;

void __fastcall HookedCreateProperty(void* ctx, void* propDesc)
{
    // Call the original first
    g_origCreateProperty(ctx, propDesc);

    // Now capture the property
    // propDesc layout (+0x00=name, +0x08=type:uint16, +0x10=owner, +0x18=dataPtr)
    uintptr_t* desc = (uintptr_t*)propDesc;
    const char* name = (const char*)desc[0];
    uint16_t type = (uint16_t)(*(uint64_t*)(desc + 1) & 0xFFFF);
    void* owner = (void*)desc[2];

    if (!name) return;

    // Get return address to identify which class this property belongs to
    uintptr_t retAddr = (uintptr_t)__builtin_return_address(0);
    const char* dtiName = FindCreatePropertyClass(retAddr);

    if (dtiName) {
        // Type name for logging
        const char* typeName = "?";
        switch (type) {
        case PROP_CLASS:    typeName = "Class"; break;
        case PROP_CLASSREF: typeName = "ClassRef"; break;
        case PROP_BOOL:     typeName = "Bool"; break;
        case PROP_U32:      typeName = "U32"; break;
        case PROP_S32:      typeName = "S32"; break;
        case PROP_F32:      typeName = "F32"; break;
        case PROP_STRING:   typeName = "String"; break;
        case PROP_MATRIX:   typeName = "Matrix"; break;
        case PROP_VECTOR3:  typeName = "Vector3"; break;
        }

        //HookLog("PROP [%s] %s %s @+0x%llx (this=0x%llx)\n",
        //    dtiName, typeName, name,
        //    (unsigned long long)(desc[3] - (uintptr_t)owner),
        //    (unsigned long long)owner);

        CapturedProperty cp;
        cp.name = name;
        cp.type = type;
        cp.ownerPtr = (uintptr_t)owner;
        cp.dataPtr = (uintptr_t)desc[3];
        cp.nameHash = ComputeDtiHash(dtiName);
        g_props.push_back(cp);

        // Track the owner instance for this class
        RecordPropertyOwner(dtiName, (uintptr_t)owner);
    }
}

// ---------------------------------------------------------------------------
// Check if a DTI has at least one captured property (used by Loaded-only filter).
// ---------------------------------------------------------------------------
static bool DtiHasProperties(MtDTI* dti)
{
    if (!dti) return false;
    uint64_t rootSentinel = GetRootSentinelAddr();

    for (MtDTI* cur = dti; cur; cur = cur->parent) {
        if ((uintptr_t)cur == rootSentinel) break;
        if (!cur->className) continue;
        uint32_t h = SafeComputeDtiHash(cur->className);
        if (h == 0) continue;
        for (auto& p : g_props)
            if (p.nameHash == h) return true;
    }
    return false;
}

// Check if a DTI has at least one editable property.
static bool DtiHasEditableProperties(MtDTI* dti)
{
    if (!dti) return false;
    uint64_t rootSentinel = GetRootSentinelAddr();

    for (MtDTI* cur = dti; cur; cur = cur->parent) {
        if ((uintptr_t)cur == rootSentinel) break;
        if (!cur->className) continue;
        uint32_t h = SafeComputeDtiHash(cur->className);
        if (h == 0) continue;
        for (auto& p : g_props) {
            if (p.nameHash != h) continue;
            switch (p.type) {
            case PROP_BOOL:
            case PROP_U32:
            case PROP_S32:
            case PROP_F32:
            case PROP_VECTOR3:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Recursively add DTI + children to the filtered list in pre-order.
// Returns true if this node or any descendant matched the filter.
// ---------------------------------------------------------------------------
static bool AddToFiltered(MtDTI* dti, int depth)
{
    if (!dti) return false;

    bool matches = (g_filter[0] == 0);
    const char* className = GetSafeClassName(dti);

    if (!matches) {
        if (className && strcmp(className, "(corrupt name)") != 0) {
            matches = SafeStrStr(className, g_filter);

            // Also match short name (after last ::)
            if (!matches) {
                const char* shortName = SafeStrRChr(className, ':');
                if (shortName) {
                    shortName++;
                    matches = SafeStrStr(shortName, g_filter);
                }
            }
        }

        // Match by hex hash
        if (!matches && g_filter[0] == '0' && g_filter[1] == 'x') {
            unsigned int h = 0;
            if (sscanf(g_filter + 2, "%x", &h) == 1)
                matches = (dti->nameHash == h);
        }
    }

    // "Loaded only" filter — use property capture + instance scan
    if (matches && g_filterLoadedOnly) {
        matches = DtiHasProperties(dti);
        if (!matches && className && strcmp(className, "(corrupt name)") != 0) {
            matches = GetInstanceCount(className) > 0;
        }
    }

    // "Editable only" filter — only if matches is still true
    if (matches && g_filterEditableOnly) {
        matches = DtiHasEditableProperties(dti);
    }

    // Check children
    bool childMatches = false;
    for (MtDTI* child = dti->firstChild; child; child = child->nextSibling) {
        if (AddToFiltered(child, depth + 1))
            childMatches = true;
    }

    if (matches || childMatches) {
        DtiEntry entry;
        entry.dti = dti;
        entry.depth = depth;
        g_filtered.push_back(entry);
    }

    return matches || childMatches;
}

// ---------------------------------------------------------------------------
// Build the filtered display list from current g_allDtis and g_filter.
// ---------------------------------------------------------------------------
static void BuildFilteredList()
{
    g_filtered.clear();

    uint64_t rootSentinel = GetRootSentinelAddr();

    // Find root-level DTIs (parent == nullptr or parent == sentinel)
    std::vector<MtDTI*> roots;
    for (MtDTI* dti : g_allDtis) {
        if (!dti->parent || (uintptr_t)dti->parent == rootSentinel) {
            roots.push_back(dti);
        }
    }

    // For each root, recursively add to filtered list
    for (MtDTI* root : roots) {
        AddToFiltered(root, 0);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void DtiEditor_Init()
{
    g_initialized = false;
    g_propsInitialized = false;
    HookLog("DTI Editor initialized.\n");
}

void DtiEditor_InitHooks(Trampoline* tramp)
{
    if (g_propsInitialized) return;

    // cAtkCtrl__TargetInfo__createProperty_BF60 at 0x14000bf60 is a 5-byte
    // JMP thunk (E9 rel32) to the real body at 0x14000c630.
    // We patch the thunk to redirect to our hook.
    uintptr_t thunkAddr = _addr(CREATE_PROPERTY_HOOK_VA);
    uintptr_t realFuncAddr = _addr(CREATE_PROPERTY_REAL_VA);

    // Read the JMP displacement to verify
    if (*(uint8_t*)thunkAddr == 0xE9) {
        int32_t disp = *(int32_t*)(thunkAddr + 1);
        uintptr_t computedTarget = (thunkAddr + 5) + disp;
        if (computedTarget == realFuncAddr) {
            HookLog("DTI Editor: verified JMP thunk at 0x%llx -> 0x%llx\n",
                (unsigned long long)thunkAddr, (unsigned long long)realFuncAddr);
        }
    }

    // Set the original function pointer to the real implementation
    g_origCreateProperty = (CreatePropertyFn)realFuncAddr;

    // Patch the thunk with a JMP to our hook handler
    uintptr_t hookAddr = (uintptr_t)&HookedCreateProperty;
    int64_t rel = (int64_t)(hookAddr - (thunkAddr + 5));
    if (rel < INT32_MIN || rel > INT32_MAX) {
        HookLog("DTI Editor: ERROR - hook too far (rel=%lld), need trampoline!\n",
            (long long)rel);
        // Fallback: use the trampoline
        InjectHook((void*)thunkAddr, tramp->Jump(HookedCreateProperty), PATCH_JUMP);
    } else {
        // Write exactly 5 bytes: E9 + 32-bit signed displacement
        struct JmpPatch { uint8_t op; int32_t disp; } __attribute__((packed));
        JmpPatch patch = { 0xE9, (int32_t)rel };
        DWORD old;
        VirtualProtect((void*)thunkAddr, sizeof(patch), PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)thunkAddr, &patch, sizeof(patch));
        VirtualProtect((void*)thunkAddr, sizeof(patch), old, &old);
    }

    // Install the MtDTI__getSize hook to track allocations
    InstallAllocationHook();

    g_propsInitialized = true;
    HookLog("DTI Editor: property hooks installed.\n");
}

// ---------------------------------------------------------------------------
// Ensure collected data is populated (called on first draw).
// ---------------------------------------------------------------------------
static void DtiEditor_EnsureCollected()
{
    if (g_initialized) return;

    // Diagnostic: try using the game's own findByHash for a sanity check.
    typedef uint64_t(__fastcall* FindByHashFn)(uint32_t hash);
    FindByHashFn findByHash = (FindByHashFn)_addr(0x14045d980);
    uint32_t mtObjHash = ComputeDtiHash("MtObject");
    uint64_t found = findByHash(mtObjHash);
    HookLog("DTI Editor: findByHash(\"MtObject\") = 0x%llx (hash=0x%08x)\n",
        (unsigned long long)found, mtObjHash);

    CollectAllDtis();
    BuildFilteredList();
    BuildVtableMap();
    g_initialized = true;
    HookLog("DTI Editor: collected %zu types, %zu properties, %zu vtable entries.\n",
        g_allDtis.size(), g_props.size(), g_vtableMap.size());
}

// ---------------------------------------------------------------------------
// Temporarily make a page writable, write data, then restore protection.
// ---------------------------------------------------------------------------
static void WriteToMemory(uintptr_t addr, const void* data, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD oldProtect = 0;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return;
    if (mbi.State != MEM_COMMIT) return;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return;

    // Check if already writable
    if (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) {
        memcpy((void*)addr, data, size);
        return;
    }

    // Need to make it writable temporarily
    if (VirtualProtect((void*)addr, size, PAGE_READWRITE, &oldProtect)) {
        memcpy((void*)addr, data, size);
        VirtualProtect((void*)addr, size, oldProtect, &oldProtect); // restore
    }
}

// ---------------------------------------------------------------------------
// Manually trigger property enumeration for a known singleton DTI.
// Calls the class's createProperty function with a minimal context so that
// our hook captures all properties.
// ---------------------------------------------------------------------------
static void TriggerPropertiesForDti(const char* dtiName)
{
    const SingletonEntry* entry = FindSingletonEntry(dtiName);
    if (!entry) {
        HookLog("DTI Editor: no singleton entry for %s\n", dtiName);
        return;
    }

    // Read instance pointer from the known global address
    uintptr_t* instancePtrAddr = (uintptr_t*)_addr(entry->instancePtrAddr);
    if (!instancePtrAddr) {
        HookLog("DTI Editor: _addr(%llx) returned null\n", (unsigned long long)entry->instancePtrAddr);
        return;
    }
    uintptr_t instancePtr = *instancePtrAddr;
    if (!instancePtr) {
        HookLog("DTI Editor: instance for %s is null\n", dtiName);
        return;
    }

    // Create a minimal MtPropertyList context on the stack.
    // Layout: +0x00 = vtable (unused by newElement), +0x08 = linked list head
    struct { uintptr_t unused; uintptr_t head; } ctx = {0, 0};

    // Call the createProperty function directly by address
    typedef void(__fastcall* CreatePropertyDebugFn)(uintptr_t instance, void* ctx);
    CreatePropertyDebugFn fn = (CreatePropertyDebugFn)_addr(entry->createPropertyAddr);

    HookLog("DTI Editor: triggering properties for %s (instance=0x%llx)\n",
        dtiName, (unsigned long long)instancePtr);

    fn(instancePtr, &ctx);

    HookLog("DTI Editor: done, now have %zu total captured properties\n", g_props.size());
}

// ---------------------------------------------------------------------------
// Check if memory is readable (but NOT writable) for display purposes only.
// ---------------------------------------------------------------------------
static bool IsReadableMem(uintptr_t addr, size_t minSize)
{
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// ---------------------------------------------------------------------------
// Vtable-based instance scanning for a specific class
// ---------------------------------------------------------------------------
static uintptr_t FindVtableAddr(const char* className)
{
    if (!className) return 0;
    for (auto& entry : g_vtableMap) {
        if (strcmp(entry.className, className) == 0)
            return entry.vtableAddr;
    }
    return 0;
}

// Scan memory for live instances of a specific class (by vtable address).
// Uses the vtable base address as stored by BuildVtableMap.
// Returns up to maxResults instance addresses.
static std::vector<uintptr_t> ScanForInstancesOfClass(uintptr_t vtableAddr, int maxResults = 1)
{
    std::vector<uintptr_t> results;
    if (!vtableAddr) return results;

    // Scan .data section (static/global objects)
    uintptr_t dataStart = (uintptr_t)_addr(0x140c51000);
    uintptr_t dataEnd   = (uintptr_t)_addr(0x140e24047);
    for (uintptr_t p = dataStart; p + 8 <= dataEnd && (int)results.size() < maxResults; p += 8) {
        if (IsReadableMem(p, 8) && *(uintptr_t*)p == vtableAddr) {
            results.push_back(p);
        }
    }
    if ((int)results.size() >= maxResults) return results;

    // Scan the default process heap (dynamic objects)
    HANDLE hHeap = GetProcessHeap();
    if (hHeap) {
        PROCESS_HEAP_ENTRY phe = {};
        while (HeapWalk(hHeap, &phe) && (int)results.size() < maxResults) {
            if ((phe.wFlags & PROCESS_HEAP_ENTRY_BUSY) && phe.lpData) {
                if (IsReadableMem((uintptr_t)phe.lpData, 8) &&
                    *(uintptr_t*)phe.lpData == vtableAddr) {
                    results.push_back((uintptr_t)phe.lpData);
                }
            }
        }
    }

    return results;
}

// ---------------------------------------------------------------------------
// Look up createProperty function address from the property table by class name.
// ---------------------------------------------------------------------------
static uintptr_t FindCreatePropertyFn(const char* className)
{
    if (!className) return 0;
    for (size_t i = 0; i < g_createPropertyTableCount; i++) {
        if (strcmp(g_createPropertyTable[i].className, className) == 0)
            return (uintptr_t)_addr(g_createPropertyTable[i].address);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Allocation tracking via MtDTI__getSize hook.
// When the game constructs an MtObject, MtDTI__getSize is called to
// determine the object's allocation size.  We intercept this to track
// which DTI classes have been instantiated.
// ---------------------------------------------------------------------------

// DTI descriptor for MtDTI__getSize — same layout as MtDTI but confirmed
// against the binary: +0x34 = size field (not nameHash).
struct MtDTIDescriptor {
    uintptr_t vtable;       // +0x00
    const char* className;  // +0x08
    MtDTI* nextSibling;     // +0x10
    MtDTI* firstChild;      // +0x18
    MtDTI* parent;          // +0x20
    MtDTI* hashNext;        // +0x28
    uint32_t flags;         // +0x30
    uint32_t size;          // +0x34  ← MtDTI__getSize reads this
};

static constexpr uintptr_t MTDTI_GETSIZE_VA = 0x1402a40c0;

// Original MtDTI__getSize function
typedef uint32_t(__fastcall* MtDTIGetSizeFn)(MtDTIDescriptor* dti);
static MtDTIGetSizeFn g_origGetSize = nullptr;

// Tracks which DTI classes have been allocated
static std::unordered_map<uint32_t, const char*> g_allocatedDtis;
// Tracks (classHash → property owner instances) built from HookedCreateProperty
static std::unordered_map<uint32_t, std::vector<uintptr_t>> g_classInstances;

uint32_t __fastcall HookedMtDTIGetSize(MtDTIDescriptor* dti)
{
    uint32_t size = g_origGetSize(dti);

    if (dti && dti->className) {
        uint32_t hash = ComputeDtiHash(dti->className);
        g_allocatedDtis[hash] = dti->className;
    }

    return size;
}

// Install the MtDTI__getSize hook (called from DtiEditor_InitHooks)
static void InstallAllocationHook()
{
    uintptr_t target = _addr(MTDTI_GETSIZE_VA);
    g_origGetSize = (MtDTIGetSizeFn)target;
    // Same JMP patching approach as the property hook
    uintptr_t hookAddr = (uintptr_t)&HookedMtDTIGetSize;
    int64_t rel = (int64_t)(hookAddr - (target + 5));
    if (rel >= INT32_MIN && rel <= INT32_MAX) {
        struct JmpPatch { uint8_t op; int32_t disp; } __attribute__((packed));
        JmpPatch patch = { 0xE9, (int32_t)rel };
        DWORD old;
        VirtualProtect((void*)target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)target, &patch, sizeof(patch));
        VirtualProtect((void*)target, sizeof(patch), old, &old);
    } else {
        HookLog("DTI Editor: MtDTI__getSize hook too far (rel=%lld), skipping\n", (long long)rel);
    }
}

// Record an instance address associated with a class (called from HookedCreateProperty)
static void RecordPropertyOwner(const char* className, uintptr_t ownerPtr)
{
    if (!className || !ownerPtr) return;
    uint32_t hash = ComputeDtiHash(className);
    auto& instances = g_classInstances[hash];
    // Avoid duplicates
    for (auto addr : instances) {
        if (addr == ownerPtr) return;
    }
    instances.push_back(ownerPtr);
}

// Check if a DTI class has a tracked live instance (from allocation + property tracking)
static bool HasTrackedInstance(const char* className)
{
    if (!className) return false;
    uint32_t hash = ComputeDtiHash(className);
    auto it = g_classInstances.find(hash);
    if (it == g_classInstances.end()) return false;
    for (auto addr : it->second) {
        if (IsReadableMem(addr, 8)) return true;
    }
    return false;
}

// Get the number of tracked instances for a class
static int GetTrackedInstanceCount(const char* className)
{
    if (!className) return -1;
    uint32_t hash = ComputeDtiHash(className);
    auto it = g_classInstances.find(hash);
    if (it == g_classInstances.end()) return -1;
    int count = 0;
    for (auto addr : it->second) {
        if (IsReadableMem(addr, 8)) count++;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Manually trigger property enumeration for ANY class via vtable scanning.
// This looks up the class's vtable via the auto-built vtable map, finds live
// instances, then looks up the createProperty function from the property
// table and calls it on each found instance.
// ---------------------------------------------------------------------------
static void TriggerPropertiesForDti_Generic(const char* dtiName)
{
    if (!dtiName) return;

    // Check if we already have captured properties from the passive hook
    uint32_t hash = ComputeDtiHash(dtiName);
    bool hasProps = false;
    for (auto& p : g_props) {
        if (p.nameHash == hash) { hasProps = true; break; }
    }

    HookLog("DTI Editor: properties for %s: %zu captured (hook), %zu tracked instances\n",
        dtiName, g_props.size(), g_classInstances.size());

    if (hasProps) {
        HookLog("DTI Editor: %s already has %zu captured properties via passive hook\n",
            dtiName, std::count_if(g_props.begin(), g_props.end(),
                [hash](auto& p) { return p.nameHash == hash; }));
        return;
    }

    // No captured properties yet. Check for live instances from the tracker.
    uint32_t dtiHash = ComputeDtiHash(dtiName);
    auto instIt = g_classInstances.find(dtiHash);
    if (instIt == g_classInstances.end() || instIt->second.empty()) {
        HookLog("DTI Editor: no tracked instances for %s — enter a match to capture\n", dtiName);
        return;
    }

    // Try each tracked instance
    uintptr_t createPropertyFn = FindCreatePropertyFn(dtiName);
    if (!createPropertyFn) {
        HookLog("DTI Editor: no createProperty entry for %s\n", dtiName);
        return;
    }

    struct { uintptr_t unused; uintptr_t head; } ctx = {0, 0};
    typedef void(__fastcall* CreatePropertyDebugFn)(uintptr_t instance, void* ctx);
    CreatePropertyDebugFn fn = (CreatePropertyDebugFn)createPropertyFn;

    for (auto addr : instIt->second) {
        if (!IsReadableMem(addr, 8)) continue;
        HookLog("DTI Editor: triggering properties for %s via tracked instance 0x%llx\n",
            dtiName, (unsigned long long)addr);
        fn(addr, &ctx);
        break;  // Only need one
    }
}

// ---------------------------------------------------------------------------
// Decode a Shift-JIS (codepage 932) string to UTF-8 for display.
// First checks if the string is already valid UTF-8 — if so, returns as-is.
// Otherwise converts from Shift-JIS to UTF-8 via a wide Char buffer.
// Returns a pointer to a static buffer (subsequent calls overwrite).
// ---------------------------------------------------------------------------
static const char* SjisToUtf8(const char* sjis)
{
    if (!sjis || !*sjis) return sjis;

    // Fast path: if the string is pure ASCII (all bytes < 0x80),
    // it's valid UTF-8 as-is and definitely not SJIS-encoded.
    bool hasHighByte = false;
    for (const char* p = sjis; *p; ++p) {
        if ((unsigned char)*p >= 0x80) { hasHighByte = true; break; }
    }
    if (!hasHighByte) return sjis;

    // Has high bytes — assume Shift-JIS and convert via UTF-16.
    // Use flag 0 (not MB_ERR_INVALID_CHARS) to tolerate vendor extensions.
    int wideLen = MultiByteToWideChar(932, 0, sjis, -1, NULL, 0);
    if (wideLen <= 0) return sjis;

    std::wstring wide(wideLen, L'\0');
    if (MultiByteToWideChar(932, 0, sjis, -1, &wide[0], wideLen) <= 0)
        return sjis;

    // Convert UTF-16 → UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, NULL, 0, NULL, NULL);
    if (utf8Len <= 0) return sjis;

    static std::string s_utf8Buf;
    s_utf8Buf.resize(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &s_utf8Buf[0], utf8Len, NULL, NULL);

    // WideCharToMultiByte includes the null terminator in utf8Len when given -1 length.
    // Pop it so s_utf8Buf.size() reflects the actual string length.
    if (!s_utf8Buf.empty() && s_utf8Buf.back() == '\0')
        s_utf8Buf.pop_back();

    return s_utf8Buf.c_str();
}

// ---------------------------------------------------------------------------
// Draw property details for the currently selected DTI.
// ---------------------------------------------------------------------------
static void DrawPropertiesForDti(MtDTI* dti)
{
    const char* safeName = GetSafeClassName(dti);
    if (strcmp(safeName, "(corrupt name)") == 0) return;

    uint32_t dtiHash = SafeComputeDtiHash(safeName);
    if (dtiHash == 0) return;

    // Collect properties for this class and ALL ancestor classes.
    // In MT Framework, a child class inherits all properties registered by
    // ancestor createProperty functions.  Since the hook at cAtkCtrl captures
    // properties by return address (→ the actual registering class), we need
    // to aggregate across the inheritance chain.
    std::vector<CapturedProperty*> matching;
    uint64_t rootSentinel = GetRootSentinelAddr();

    for (MtDTI* cur = dti; cur; cur = cur->parent) {
        // Stop at root sentinel
        if ((uintptr_t)cur == rootSentinel) break;

        const char* curName = GetSafeClassName(cur);
        if (strcmp(curName, "(corrupt name)") == 0) continue;

        uint32_t curHash = SafeComputeDtiHash(curName);
        if (curHash == 0) continue;

        for (auto& p : g_props) {
            if (p.nameHash == curHash) {
                // Deduplicate by name in case of override
                bool dup = false;
                for (auto* existing : matching) {
                    if (existing->name && p.name &&
                        strcmp(existing->name, p.name) == 0) {
                        dup = true; break;
                    }
                }
                if (!dup) matching.push_back(&p);
            }
        }
    }

    if (matching.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1),
            "No captured properties for this type.");
        return;
    }

    ImGui::Separator();
    ImGui::Text("Properties (%zu):", matching.size());
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    ImGui::InputText("##propFilter", g_propFilter, sizeof(g_propFilter));
    ImGui::PopItemWidth();
    ImGui::Separator();

    // Column headers
    ImGui::Columns(4, "propColumns", true);
    ImGui::Text("Name"); ImGui::NextColumn();
    ImGui::Text("Type"); ImGui::NextColumn();
    ImGui::Text("Value"); ImGui::NextColumn();
    ImGui::Text("Offset"); ImGui::NextColumn();
    ImGui::Separator();
    ImGui::Columns(1);

    ImGui::BeginChild("##propList", ImVec2(0, 200), true);

    for (auto* pp : matching) {
        // Guard against dangling pointers — properties may outlive their source
        if (!IsReadableCStr(pp->name))
            continue;

        // Filter
        if (g_propFilter[0] &&
            !ContainsInsensitive(pp->name, g_propFilter))
            continue;

        ImGui::Columns(4, "propColumns", true);

        // Name (UMVC3 stores names in Shift-JIS)
        ImGui::Text("%s", SjisToUtf8(pp->name)); ImGui::NextColumn();

        // Type
        const char* typeName = "?";
        bool editable = true;
        switch (pp->type) {
        case PROP_CLASS:    typeName = "Class"; editable = false; break;
        case PROP_CLASSREF: typeName = "ClassRef"; editable = false; break;
        case PROP_BOOL:     typeName = "Bool"; break;
        case PROP_U32:      typeName = "U32"; break;
        case PROP_S32:      typeName = "S32"; break;
        case PROP_F32:      typeName = "F32"; break;
        case PROP_STRING:   typeName = "String"; editable = false; break;
        case PROP_MATRIX:   typeName = "Matrix"; editable = false; break;
        case PROP_VECTOR3:  typeName = "Vector3"; break;
        }
        ImGui::Text("%s", typeName); ImGui::NextColumn();

        // Value (editable)
        ImGui::PushID(pp->name);
        if (pp->dataPtr && editable && IsReadableMem(pp->dataPtr, 16)) {
            switch (pp->type) {
            case PROP_BOOL: {
                bool val = *(bool*)pp->dataPtr;
                if (ImGui::Checkbox("##val", &val))
                    WriteToMemory(pp->dataPtr, &val, sizeof(val));
                break;
            }
            case PROP_U32: {
                uint32_t val = *(uint32_t*)pp->dataPtr;
                if (ImGui::InputScalar("##val", ImGuiDataType_U32, &val, NULL, NULL, "%u", ImGuiInputTextFlags_EnterReturnsTrue))
                    WriteToMemory(pp->dataPtr, &val, sizeof(val));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Press Enter to apply");
                break;
            }
            case PROP_S32: {
                int32_t val = *(int32_t*)pp->dataPtr;
                if (ImGui::InputInt("##val", &val, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
                    WriteToMemory(pp->dataPtr, &val, sizeof(val));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Press Enter to apply");
                break;
            }
            case PROP_F32: {
                float val = *(float*)pp->dataPtr;
                if (ImGui::InputFloat("##val", &val, 0.0f, 0.0f, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue))
                    WriteToMemory(pp->dataPtr, &val, sizeof(val));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Press Enter to apply");
                break;
            }
            case PROP_VECTOR3: {
                float v0 = ((float*)pp->dataPtr)[0];
                float v1 = ((float*)pp->dataPtr)[1];
                float v2 = ((float*)pp->dataPtr)[2];
                ImGui::PushItemWidth(60);
                if (ImGui::InputFloat("##vx", &v0, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
                    WriteToMemory(pp->dataPtr, &v0, sizeof(float));
                ImGui::SameLine(); ImGui::Text("X");
                if (ImGui::InputFloat("##vy", &v1, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
                    WriteToMemory(pp->dataPtr + 4, &v1, sizeof(float));
                ImGui::SameLine(); ImGui::Text("Y");
                if (ImGui::InputFloat("##vz", &v2, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue))
                    WriteToMemory(pp->dataPtr + 8, &v2, sizeof(float));
                ImGui::SameLine(); ImGui::Text("Z");
                ImGui::PopItemWidth();
                break;
            }
            default:
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "(read-only)");
                break;
            }
        } else if (!editable) {
            switch (pp->type) {
            case PROP_CLASS:
            case PROP_CLASSREF:
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "(ref)");
                break;
            case PROP_STRING:
                if (pp->dataPtr && *(const char**)pp->dataPtr && IsReadableCStr(*(const char**)pp->dataPtr)) {
                    const char* s = *(const char**)pp->dataPtr;
                    // UMVC3 stores strings in Shift-JIS — decode to UTF-8 for ImGui
                    const char* display = SjisToUtf8(s);
                    // Truncate long strings for display
                    ImGui::TextColored(ImVec4(0.6f,0.6f,1,1), "\"%s\"", display);
                } else {
                    ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "(null)");
                }
                break;
            case PROP_MATRIX:
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "(16 floats)");
                break;
            default:
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "(read-only)");
                break;
            }
        } else {
            ImGui::TextColored(ImVec4(1,0,0,1), "(unwritable)");
        }
        ImGui::PopID();
        ImGui::NextColumn();

        // Offset (dataPtr - ownerPtr)
        ImGui::Text("+0x%llx",
            (unsigned long long)(pp->dataPtr - pp->ownerPtr));
        ImGui::NextColumn();

        ImGui::Columns(1);
    }

    ImGui::EndChild();
    ImGui::Columns(1);
}

// ---------------------------------------------------------------------------
// extern
// ---------------------------------------------------------------------------
extern int g_dbg_peekCalls;
extern int g_dbg_peekKeydowns;
extern int g_dbg_translated;
extern int g_dbg_lastMsg;

// ---------------------------------------------------------------------------
// Draw the DTI inspector content (no Begin/End — caller owns the window).
// ---------------------------------------------------------------------------
void DtiEditor_Draw()
{
    // Collect on first draw
    DtiEditor_EnsureCollected();

    // Debug: PeekMessage hook status (helps diagnose keyboard capture)
    ImGui::TextColored(ImVec4(0.8f,0.8f,0.8f,1),
        "DBG: peek=%d keydown=%d translate=%d  lastMsg=0x%04X  menu=%s ctx=%s",
        g_dbg_peekCalls, g_dbg_peekKeydowns, g_dbg_translated,
        g_dbg_lastMsg,
        TheMenu->GetActiveState() ? "Y" : "N",
        ImGui::GetCurrentContext() ? "Y" : "N");
    ImGui::Separator();

    // --- Filter bar ---
    ImGui::Text("Filter: ");
    ImGui::SameLine();
    ImGui::PushItemWidth(120);
    if (ImGui::InputText("##dtiFilter", g_filter, sizeof(g_filter)))
    {
        BuildFilteredList();
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        g_filter[0] = 0;
        BuildFilteredList();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
        CollectAllDtis();
        BuildFilteredList();
    }
    ImGui::SameLine();
    ImGui::Text("  (%zu types)", g_filtered.size());
    ImGui::SameLine();
    if (ImGui::SmallButton(g_showPropPane ? "Hide Props" : "Show Props")) {
        g_showPropPane = !g_showPropPane;
    }
    ImGui::SameLine();
    ImGui::Text("| %zu props", g_props.size());
    ImGui::SameLine();
    // "Loaded only" checkbox — rebuild tree when toggled
    {
        static bool prev = g_filterLoadedOnly;
        ImGui::Checkbox("Loaded only", &g_filterLoadedOnly);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only show DTIs that have live instances (scans for vtable pointers in memory) or captured properties");
        if (prev != g_filterLoadedOnly) {
            prev = g_filterLoadedOnly;
            if (g_filterLoadedOnly && !g_vtableMapBuilt) {
                BuildVtableMap();
                ScanForInstances();
            }
            BuildFilteredList();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Scan")) {
        BuildVtableMap();
        ScanForInstances();
        BuildFilteredList();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh instance counts by scanning memory for vtable pointers");
    ImGui::SameLine();
    // "Editable only" checkbox — rebuild tree when toggled
    {
        static bool prev = g_filterEditableOnly;
        ImGui::Checkbox("Editable only", &g_filterEditableOnly);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Only show DTIs that have at least one editable property (bool, int, float, vector3)");
        if (prev != g_filterEditableOnly) {
            prev = g_filterEditableOnly;
            BuildFilteredList();
        }
    }
    ImGui::Separator();

    // --- Split: tree view (left) + details (right) ---
    ImGui::BeginChild("##dtiTreePane", ImVec2(400, -1), true);
    for (size_t i = 0; i < g_filtered.size(); i++) {
        MtDTI* dti = g_filtered[i].dti;
        int depth = g_filtered[i].depth;
        if (!dti || !dti->className) continue;

        // Indent by depth
        ImGui::Indent(depth * 12.0f);

        // Leaf vs expandable
        bool hasChildren = (dti->firstChild != nullptr);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (hasChildren) {
            flags = ImGuiTreeNodeFlags_None;
        }
        if (dti == g_selected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        ImGui::PushID((void*)dti);

        bool open = ImGui::TreeNodeEx(GetSafeClassName(dti), flags, "%s", GetSafeClassName(dti));
        if (ImGui::IsItemClicked()) {
            g_selected = dti;
        }
        // Right-click context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Copy name")) {
                ImGui::SetClipboardText(GetSafeClassName(dti));
            }
            if (ImGui::MenuItem("Copy hash")) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%08X", dti->nameHash);
                ImGui::SetClipboardText(buf);
            }
            ImGui::EndPopup();
        }
        if (open && hasChildren) {
            ImGui::TreePop();
        }

        ImGui::PopID();
        ImGui::Unindent(depth * 12.0f);
    }

    ImGui::EndChild(); // tree pane
    ImGui::SameLine();

    // --- Details pane (right side) ---
    ImGui::BeginChild("##dtiDetailPane", ImVec2(0, -1), true);

    if (g_selected) {
        MtDTI* dti = g_selected;

        ImGui::Text("Name: %s", GetSafeClassName(dti));
        ImGui::Text("Address: 0x%llx", (unsigned long long)dti);
        ImGui::Text("Hash: 0x%08X (%u)", dti->nameHash, dti->nameHash);
        ImGui::Text("Flags: 0x%08X", dti->flags);
        ImGui::Separator();

        // Parent
        if (dti->parent) {
            uint64_t rootSentinel = GetRootSentinelAddr();
            if ((uintptr_t)dti->parent == rootSentinel) {
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "Parent: (root)");
            } else {
                ImGui::Text("Parent: %s", GetSafeClassName(dti->parent));
            }
        } else {
            ImGui::Text("Parent: (none)");
        }

        // Sibling count
        int siblingCount = 0;
        for (MtDTI* s = dti->nextSibling; s; s = s->nextSibling) siblingCount++;
        ImGui::Text("Next siblings: %d", siblingCount);

        // Children
        int childCount = 0;
        for (MtDTI* c = dti->firstChild; c; c = c->nextSibling) childCount++;
        ImGui::Text("Children: %d", childCount);

        if (childCount > 0) {
            ImGui::Separator();
            ImGui::Text("Child classes:");
            ImGui::Indent(12.0f);
            for (MtDTI* c = dti->firstChild; c; c = c->nextSibling) {
                    ImGui::BulletText("%s", GetSafeClassName(c));
            }
            ImGui::Unindent(12.0f);
        }

        ImGui::Separator();
        ImGui::Text("Structure layout (relative to MtObject):");
        ImGui::Indent(12.0f);
        ImGui::Text("+0x00: vtable");
        ImGui::Text("+0x08: className = \"%s\"", GetSafeClassName(dti));
        ImGui::Text("+0x10: nextSibling = 0x%llx", (unsigned long long)dti->nextSibling);
        ImGui::Text("+0x18: firstChild = 0x%llx", (unsigned long long)dti->firstChild);
        ImGui::Text("+0x20: parent = 0x%llx", (unsigned long long)dti->parent);
        ImGui::Text("+0x28: hashNext = 0x%llx", (unsigned long long)dti->hashNext);
        ImGui::Text("+0x30: flags = 0x%08X", dti->flags);
        ImGui::Text("+0x34: nameHash = 0x%08X", dti->nameHash);
        ImGui::Unindent(12.0f);

        // CRC verification
        ImGui::Separator();
        const char* safeName = GetSafeClassName(dti);
        if (strcmp(safeName, "(corrupt name)") != 0) {
            uint32_t computed = SafeComputeDtiHash(safeName);
            ImGui::Text("CRC verification: stored=0x%08X computed=0x%08X %s",
                dti->nameHash, computed,
                (computed == dti->nameHash) ? "[MATCH]" : "[MISMATCH]");

            // Instance count from allocation tracking
            int instCnt = GetTrackedInstanceCount(safeName);
            if (instCnt >= 0) {
                ImGui::Text("Tracked instances: %d", instCnt);
            } else {
                ImGui::Text("Tracked instances: (none)");
            }
        }

        // Property info — show captured properties or guide user
        if (strcmp(safeName, "(corrupt name)") != 0) {
            const SingletonEntry* singEntry = FindSingletonEntry(safeName);
            bool hasCapProps = false;
            {
                uint32_t h = SafeComputeDtiHash(safeName);
                for (auto& p : g_props) {
                    if (p.nameHash == h) { hasCapProps = true; break; }
                }
            }

            if (singEntry && hasCapProps) {
                ImGui::Separator();
                if (ImGui::SmallButton("Trigger Properties")) {
                    TriggerPropertiesForDti(safeName);
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f,0.8f,1,1), "Singleton — click to enumerate properties");
            } else if (singEntry) {
                ImGui::Separator();
                if (ImGui::SmallButton("Trigger Properties")) {
                    TriggerPropertiesForDti(safeName);
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.5f,0.8f,1,1), "Singleton");
            } else if (!hasCapProps) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1),
                    "No captured properties for this type.");
                ImGui::TextColored(ImVec4(0.5f,0.8f,1,1),
                    "Enter a match to capture properties naturally.");
            }
        }

        // Properties
        if (g_showPropPane && strcmp(safeName, "(corrupt name)") != 0) {
            DrawPropertiesForDti(dti);
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "Select a type from the tree");
        if (g_allDtis.empty()) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1,0.5f,0,1), "No DTIs found in hash table.");
            ImGui::Text("Hash table (from LEA): 0x%llX", (unsigned long long)GetHashTableAddr());
            ImGui::Text("Game base: 0x%llX",
                (unsigned long long)GetMKXEntryPoint());
            ImGui::TextColored(ImVec4(1,1,0,1), "Try pressing the Refresh button above.");
        }
    }

    ImGui::EndChild(); // detail pane
}
