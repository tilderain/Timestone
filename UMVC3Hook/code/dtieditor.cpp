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
#include <algorithm>

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

static bool SafeStrStr(const char* str, const char* substr)
{
    if (!IsReadableCStr(str) || !IsReadableCStr(substr)) return false;
    return strstr(str, substr) != nullptr;
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
    }
}

// ---------------------------------------------------------------------------
// Check if a DTI has at least one captured property (used by Loaded-only filter).
// ---------------------------------------------------------------------------
static bool DtiHasProperties(MtDTI* dti)
{
    if (!dti || !dti->className) return false;
    uint32_t h = SafeComputeDtiHash(dti->className);
    if (h == 0) return false;

    for (auto& p : g_props)
        if (p.nameHash == h) return true;
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

    if (!matches) {
        const char* className = GetSafeClassName(dti);
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

    // "Loaded only" filter
    if (matches && g_filterLoadedOnly) {
        matches = DtiHasProperties(dti);
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
    g_initialized = true;
    HookLog("DTI Editor: collected %zu types, %zu properties.\n",
        g_allDtis.size(), g_props.size());
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

    // Collect matching properties
    std::vector<CapturedProperty*> matching;
    for (auto& p : g_props) {
        if (p.nameHash == dtiHash) {
            matching.push_back(&p);
        }
    }

    // Also check parent DTI chain (parent class properties)
    // and check for exact class name match
    // More accurate: filter by exact class name hash match only
    // In MT Framework, children inherit parent's createProperty, but the
    // return address will point to the child's function when child-specific
    // properties are being registered.

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
    ImGui::Text("Offset"); ImGui::NextColumn();
    ImGui::Text("Owner"); ImGui::NextColumn();
    ImGui::Separator();
    ImGui::Columns(1);

    ImGui::BeginChild("##propList", ImVec2(0, 200), true);

    for (auto* pp : matching) {
        // Filter
        if (g_propFilter[0] &&
            !strstr(pp->name, g_propFilter))
            continue;

        ImGui::Columns(4, "propColumns", true);

        // Name
        ImGui::Text("%s", pp->name); ImGui::NextColumn();

        // Type
        const char* typeName = "?";
        switch (pp->type) {
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
        ImGui::Text("%s", typeName); ImGui::NextColumn();

        // Offset (dataPtr - ownerPtr)
        ImGui::Text("+0x%llx",
            (unsigned long long)(pp->dataPtr - pp->ownerPtr));
        ImGui::NextColumn();

        // Owner address
        ImGui::Text("0x%llx", (unsigned long long)pp->ownerPtr);
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
    if (ImGui::InputText("##dtiFilter", g_filter, sizeof(g_filter)))
    {
        BuildFilteredList();
    }
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
            ImGui::SetTooltip("Only show DTIs that have at least one captured property");
        if (prev != g_filterLoadedOnly) {
            prev = g_filterLoadedOnly;
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
