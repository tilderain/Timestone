// dtieditor.cpp
// MtDTI type hierarchy inspector and editor — ImGui-based.
#include "../pch.h"
#include "dtieditor.h"
#include "mk10utils.h"
#include "../imgui/imgui.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

// HookLog — declared extern in dllmain.cpp
extern void HookLog(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------

// Extract runtime address from a RIP-relative LEA instruction at a given VA.
// This is more reliable than _addr(VA) for data-section addresses in
// unpacked/rehacked binaries where the data section may not map 1:1 from
// the original PE layout.  We read the displacement directly from the
// instruction bytes at runtime and compute: RIP_after_instruction + disp.
static uintptr_t ReadLeaTarget(uintptr_t instructionVa)
{
    // LEA r64,[rip+disp] is encoded as:
    //   REX.W (48) + Opcode (8D) + ModRM (xx) + signed32 displacement
    // Total 7 bytes.  Displacement is at offset +3.
    // RIP points to the byte after the instruction = instructionVa + 7.
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

// ---------------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------------
static std::vector<MtDTI*> g_allDtis;          // every DTI collected from hash table
static std::vector<DtiEntry> g_filtered;        // filtered flat list for display
static char g_filter[256] = {};                 // search filter text
static MtDTI* g_selected = nullptr;             // currently selected DTI
static bool g_initialized = false;              // whether we've done first collection

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
// Recursively add DTI + children to the filtered list in pre-order.
// Returns true if this node or any descendant matched the filter.
// ---------------------------------------------------------------------------
static bool AddToFiltered(MtDTI* dti, int depth)
{
    if (!dti) return false;

    // Check if this node matches the filter
    bool matches = (g_filter[0] == 0);
    if (!matches && dti->className) {
        matches = (strstr(dti->className, g_filter) != nullptr);
        // Also match short name (after last ::)
        if (!matches) {
            const char* shortName = strrchr(dti->className, ':');
            if (shortName) { shortName++; matches = (strstr(shortName, g_filter) != nullptr); }
        }
        // Match by hex hash
        if (!matches && g_filter[0] == '0' && g_filter[1] == 'x') {
            unsigned int h = 0;
            if (sscanf(g_filter + 2, "%x", &h) == 1)
                matches = (dti->nameHash == h);
        }
    }

    // Check children
    bool childMatches = false;
    for (MtDTI* child = dti->firstChild; child; child = child->nextSibling) {
        if (AddToFiltered(child, depth + 1))
            childMatches = true;
    }

    // Add this node if it matches, or if any child matches (parent placeholder)
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
    // Defer first collection to first Draw() so the hash table has maximal
    // content (some DTIs may be registered after our DLL init).
    g_initialized = false;

    HookLog("DTI Editor initialized.\n");
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
    HookLog("DTI Editor: collected %zu types from hash table.\n", g_allDtis.size());
}

// ---------------------------------------------------------------------------
// Draw the DTI inspector content (no Begin/End — caller owns the window).
// ---------------------------------------------------------------------------
void DtiEditor_Draw()
{
    // Collect on first draw (allows DTIs registered after DLL init to appear).
    DtiEditor_EnsureCollected();

    // --- Filter bar ---
    ImGui::Text("Filter: ");
    ImGui::SameLine();
    if (ImGui::InputText("##dtiFilter", g_filter, sizeof(g_filter),
                         ImGuiInputTextFlags_AutoSelectAll))
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

        // Use a unique ID to avoid conflicts
        ImGui::PushID((void*)dti);

        bool open = ImGui::TreeNodeEx(dti->className, flags, "%s", dti->className);
        if (ImGui::IsItemClicked()) {
            g_selected = dti;
        }
        // Right-click context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Copy name")) {
                ImGui::SetClipboardText(dti->className);
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

        ImGui::Text("Name: %s", dti->className ? dti->className : "(null)");
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
                ImGui::Text("Parent: %s", dti->parent->className ? dti->parent->className : "(null)");
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
                if (c->className) {
                    ImGui::BulletText("%s", c->className);
                }
            }
            ImGui::Unindent(12.0f);
        }

        ImGui::Separator();
        ImGui::Text("Structure layout (relative to MtObject):");
        ImGui::Indent(12.0f);
        ImGui::Text("+0x00: vtable");
        ImGui::Text("+0x08: className = \"%s\"", dti->className ? dti->className : "?");
        ImGui::Text("+0x10: nextSibling = 0x%llx", (unsigned long long)dti->nextSibling);
        ImGui::Text("+0x18: firstChild = 0x%llx", (unsigned long long)dti->firstChild);
        ImGui::Text("+0x20: parent = 0x%llx", (unsigned long long)dti->parent);
        ImGui::Text("+0x28: hashNext = 0x%llx", (unsigned long long)dti->hashNext);
        ImGui::Text("+0x30: flags = 0x%08X", dti->flags);
        ImGui::Text("+0x34: nameHash = 0x%08X", dti->nameHash);
        ImGui::Unindent(12.0f);

        // CRC verification
        ImGui::Separator();
        if (dti->className) {
            uint32_t computed = ComputeDtiHash(dti->className);
            ImGui::Text("CRC verification: stored=0x%08X computed=0x%08X %s",
                dti->nameHash, computed,
                (computed == dti->nameHash) ? "[MATCH]" : "[MISMATCH]");
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
