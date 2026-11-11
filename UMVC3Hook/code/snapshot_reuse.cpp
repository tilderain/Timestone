#include "snapshot_reuse.h"

#include <cstring>
#include <unordered_map>
#include <vector>

extern void HookLog(const char* fmt, ...);

namespace {

// uActorGameModel layout shared by characters and weapons.
constexpr size_t ActorPrefixSize = 0x1450;
constexpr size_t MotionSlotBase = 0x560, MotionSlotStride = 0x180, MotionSlotCount = 8;
constexpr size_t MotionSlotEnd = MotionSlotBase + MotionSlotStride * MotionSlotCount;
constexpr size_t MotionBlendPointer = 0x160;  // Within a motion slot record.
constexpr size_t MaterialTableBase = 0x1408;  // Seven array pointers.
constexpr uint32_t MaterialStrides[] = {0xBB0, 0xBB0, 0xB90, 0xB90, 0xBB0, 0xBB0, 0xB80};
// A job is a uMaterialControlEx unit followed by its own state. Watched per job:
// the unit header up to the controller arrays at +0x50, which holds the unit
// flags and uMaterialControl::mpTarget (+0x40; rebound or cleared by the base
// move with the job inactive), and the block from +0xB40 with the owner, timers,
// endpoints and the active flag (bit 0 of +0xB64). The controller arrays between
// them are written by an active job's move and by controller setup.
constexpr size_t MaterialHeaderSize = 0x50;
constexpr size_t MaterialControlOffset = 0xB40, MaterialActiveOffset = 0xB64;

struct Owner {
    const uint8_t* body = nullptr;
    int32_t parent = -1;  // The body this one is attached to; precedes it in g_owners.
    bool changed = true;  // Own prefix, or the parent's.
    uint8_t changedSlots = 0xFF;
    uint8_t shadow[ActorPrefixSize];
};

struct Item {
    const uint8_t* ptr = nullptr;
    uint32_t size = 0;
    uint32_t offset = 0;
    SnapshotReuseKind kind = SnapshotReuseKind::None;
    uint8_t index = 0;
    int32_t owner = -1;
    uint32_t stride = 0;        // MaterialTable entry size.
    uint32_t shadowOffset = 0;  // MaterialTable control-block copies in g_controlShadow.
    bool wasActive = true;
    uint64_t version = 0;       // 0: never reused.
};

struct Tag {
    const uint8_t* ptr = nullptr;
    uint32_t offset = 0, size = 0;
    uint64_t version = 0;
};

std::vector<Item> g_items;
std::vector<Owner> g_owners;
std::vector<uint8_t> g_controlShadow;
std::unordered_map<const uint8_t*, std::vector<Tag>> g_tags;
// Versions are unique across plans, so a tag from an old plan never matches.
uint64_t g_nextVersion = 0;
bool g_allChanged = true;
std::vector<uint8_t> g_restoredExactly;  // By region index, for the restore in progress.
size_t g_total = 0;
SnapshotReuseStats g_stats;

bool Reusable(SnapshotReuseKind kind)
{
    return kind == SnapshotReuseKind::Prefix || kind == SnapshotReuseKind::MotionBlend ||
           kind == SnapshotReuseKind::MaterialTable;
}

// Compare one span of the owner's prefix with its copy from the previous save,
// and bring the copy up to date.
bool SpanChanged(Owner& owner, size_t begin, size_t end)
{
    if (!std::memcmp(owner.shadow + begin, owner.body + begin, end - begin)) return false;
    std::memcpy(owner.shadow + begin, owner.body + begin, end - begin);
    return true;
}

void UpdateOwners()
{
    for (Owner& owner : g_owners) {
        if (g_allChanged) {
            std::memcpy(owner.shadow, owner.body, ActorPrefixSize);
            owner.changed = true; owner.changedSlots = 0xFF;
            continue;
        }
        owner.changedSlots = 0;
        for (size_t slot = 0; slot < MotionSlotCount; ++slot) {
            const size_t begin = MotionSlotBase + slot * MotionSlotStride;
            if (SpanChanged(owner, begin, begin + MotionSlotStride)) owner.changedSlots |= uint8_t(1u << slot);
        }
        const bool head = SpanChanged(owner, 0, MotionSlotBase);
        const bool tail = SpanChanged(owner, MotionSlotEnd, ActorPrefixSize);
        // A weapon's pose follows the hand that holds it: its joints move with
        // the character even when nothing in the weapon's own prefix does.
        owner.changed = head || tail || owner.changedSlots ||
            (owner.parent >= 0 && g_owners[owner.parent].changed);
    }
}

// A material job array changed if a job runs now, ran at the previous save, or
// a job's header or control block differs from the previous save.
bool MaterialTableChanged(Item& item)
{
    bool active = false, control = false;
    const size_t controlSize = item.stride - MaterialControlOffset;
    uint8_t* shadow = g_controlShadow.data() + item.shadowOffset;
    const auto watch = [&](const uint8_t* live, size_t size) {
        if (g_allChanged || std::memcmp(shadow, live, size)) {
            std::memcpy(shadow, live, size);
            control = true;
        }
        shadow += size;
    };
    for (size_t entry = 0; entry + item.stride <= item.size; entry += item.stride) {
        active |= (item.ptr[entry + MaterialActiveOffset] & 1) != 0;
        watch(item.ptr + entry, MaterialHeaderSize);
        watch(item.ptr + entry + MaterialControlOffset, controlSize);
    }
    const bool changed = active || item.wasActive || control;
    item.wasActive = active;
    return changed;
}

void UpdateVersions()
{
    UpdateOwners();
    for (Item& item : g_items) {
        bool changed = g_allChanged;
        switch (item.kind) {
        case SnapshotReuseKind::Prefix:
            changed |= g_owners[item.owner].changed;
            break;
        case SnapshotReuseKind::MotionBlend:
            changed |= (g_owners[item.owner].changedSlots >> item.index & 1) != 0;
            break;
        case SnapshotReuseKind::MaterialTable:
            changed |= MaterialTableChanged(item);  // Always run: it maintains the shadow.
            break;
        default:
            continue;
        }
        if (changed) item.version = ++g_nextVersion;
    }
    g_allChanged = false;
}

[[gnu::noinline, gnu::cold]] void ReportMismatch(const Item& item, const uint8_t* saved, const char* stage)
{
    ++g_stats.mismatches;
    if (g_stats.mismatches > 32 && g_stats.mismatches % 1024) return;
    size_t at = 0;
    while (at < item.size && saved[at] == item.ptr[at]) ++at;
    HookLog("SnapshotReuse: MISMATCH #%llu at %s kind=%d index=%u region=%p size=%u owner=%p first_diff=0x%zX "
            "(expected unchanged; treated as changed)\n",
            (unsigned long long)g_stats.mismatches, stage, int(item.kind), unsigned(item.index),
            (const void*)item.ptr, item.size,
            item.owner >= 0 ? (const void*)g_owners[item.owner].body : nullptr, at);
}

} // namespace

void SnapshotReuse_SetPlan(const SnapshotReuseRegion* regions, int count)
{
    g_items.clear(); g_owners.clear(); g_controlShadow.clear();
    g_allChanged = true;
    g_total = 0;
    if (!regions || count <= 0) return;
    g_items.reserve(size_t(count));
    int32_t root = -1;
    for (int i = 0; i < count; ++i) {
        Item item;
        item.ptr = regions[i].ptr;
        item.size = uint32_t(regions[i].size);
        item.offset = uint32_t(g_total);
        g_total += regions[i].size;
        if (regions[i].kind == SnapshotReuseKind::ActorBody && regions[i].size >= ActorPrefixSize) {
            Owner& owner = g_owners.emplace_back();
            owner.body = regions[i].ptr;
            if (regions[i].index) owner.parent = root; else root = int32_t(g_owners.size() - 1);
        } else if (regions[i].kind == SnapshotReuseKind::ActorBody && !regions[i].index) {
            root = -1;  // A root too small to track: its attachments have no known parent.
        }
        g_items.push_back(item);
    }
    if constexpr (SNAPSHOT_REUSE_MODE == SnapshotReuseMode::Off) return;
    // A region is reusable only if its owner field is where its kind says it is;
    // anything else stays an ordinary, always-copied region.
    for (int i = 0; i < count; ++i) {
        const SnapshotReuseRegion& region = regions[i];
        Item& item = g_items[size_t(i)];
        if (!Reusable(region.kind) || !region.size) continue;
        int32_t owner = -1;
        for (size_t candidate = 0; candidate < g_owners.size(); ++candidate) {
            const uint64_t body = uint64_t(uintptr_t(g_owners[candidate].body));
            if (region.ownerSlot >= body && region.ownerSlot - body < ActorPrefixSize) owner = int32_t(candidate);
        }
        if (owner < 0) continue;
        const size_t field = size_t(region.ownerSlot - uint64_t(uintptr_t(g_owners[owner].body)));
        if (region.kind == SnapshotReuseKind::MotionBlend) {
            if (region.index >= MotionSlotCount ||
                field != MotionSlotBase + region.index * MotionSlotStride + MotionBlendPointer) continue;
        } else if (region.kind == SnapshotReuseKind::MaterialTable) {
            if (region.index >= sizeof(MaterialStrides) / sizeof(MaterialStrides[0]) ||
                field != MaterialTableBase + region.index * sizeof(uint64_t)) continue;
            item.stride = MaterialStrides[region.index];
            if (region.size % item.stride) continue;
            item.shadowOffset = uint32_t(g_controlShadow.size());
            g_controlShadow.resize(g_controlShadow.size() +
                region.size / item.stride * (MaterialHeaderSize + item.stride - MaterialControlOffset));
        }
        item.kind = region.kind; item.index = region.index; item.owner = owner;
    }
}

void SnapshotReuse_BeginRestore()
{
    g_allChanged = true;  // Stands if the restore does not reach EndRestore.
    g_restoredExactly.assign(g_items.size(), 0);
}

void SnapshotReuse_RegionRestored(int index)
{
    if (index >= 0 && size_t(index) < g_restoredExactly.size()) g_restoredExactly[size_t(index)] = 1;
}

void SnapshotReuse_EndRestore(const uint8_t* buffer)
{
    if constexpr (SNAPSHOT_REUSE_MODE == SnapshotReuseMode::Off) return;
    if (g_restoredExactly.size() != g_items.size()) return;  // Plan changed mid-restore: all changed.
    const auto found = g_tags.find(buffer);
    const std::vector<Tag>* tags = found == g_tags.end() ? nullptr : &found->second;
    // The signals compare against the previous save; from here that is the restored state.
    for (Owner& owner : g_owners) std::memcpy(owner.shadow, owner.body, ActorPrefixSize);
    for (size_t i = 0; i < g_items.size(); ++i) {
        Item& item = g_items[i];
        if (!Reusable(item.kind)) continue;
        bool same = false;
        if (tags && i < tags->size() && g_restoredExactly[i]) {
            const Tag& tag = (*tags)[i];
            same = tag.version && tag.ptr == item.ptr && tag.offset == item.offset && tag.size == item.size;
            if constexpr (SNAPSHOT_REUSE_MODE == SnapshotReuseMode::Validate) {
                // Something later in the restore may have written the region again.
                if (same && std::memcmp(buffer + item.offset, item.ptr, item.size)) {
                    ReportMismatch(item, buffer + item.offset, "restore");
                    same = false;
                }
            }
            if (same) item.version = tag.version;
        }
        if (!same) item.version = ++g_nextVersion;
        if (item.kind == SnapshotReuseKind::MaterialTable) {
            g_allChanged = true;          // Refresh the control-block copies; the result is not used.
            MaterialTableChanged(item);   // Leaves wasActive as the restored frame's state.
        }
    }
    g_allChanged = false;
}

void SnapshotReuse_ForgetBuffer(const uint8_t* buffer)
{
    if (buffer) g_tags.erase(buffer);
}

size_t SnapshotReuse_Save(uint8_t* buffer)
{
    ++g_stats.saves;
    if constexpr (SNAPSHOT_REUSE_MODE == SnapshotReuseMode::Off) {
        for (const Item& item : g_items) std::memcpy(buffer + item.offset, item.ptr, item.size);
        g_stats.copiedBytes += g_total;
        return g_total;
    }
    UpdateVersions();
    std::vector<Tag>& tags = g_tags[buffer];
    tags.resize(g_items.size());
    size_t reused = 0;
    for (size_t i = 0; i < g_items.size(); ++i) {
        const Item& item = g_items[i];
        Tag& tag = tags[i];
        uint8_t* saved = buffer + item.offset;
        if (item.version && tag.version == item.version && tag.ptr == item.ptr &&
            tag.offset == item.offset && tag.size == item.size) {
            const bool validate = SNAPSHOT_REUSE_MODE == SnapshotReuseMode::Validate;
            if ((!validate && item.kind != SnapshotReuseKind::MaterialTable) ||
                !std::memcmp(saved, item.ptr, item.size)) { reused += item.size; continue; }
            if (validate) ReportMismatch(item, saved, "save");
        }
        std::memcpy(saved, item.ptr, item.size);
        tag = {item.ptr, item.offset, item.size, item.version};
    }
    g_stats.reusedBytes += reused;
    g_stats.copiedBytes += g_total - reused;
    // About once a minute of play.
    if (g_stats.saves % 4096 == 0) {
        const uint64_t all = g_stats.copiedBytes + g_stats.reusedBytes;
        HookLog("SnapshotReuse: saves=%llu reused=%llu%% mismatches=%llu mode=%s\n",
                (unsigned long long)g_stats.saves, (unsigned long long)(all ? g_stats.reusedBytes * 100 / all : 0),
                (unsigned long long)g_stats.mismatches,
                SNAPSHOT_REUSE_MODE == SnapshotReuseMode::Validate ? "validate" : "trust");
    }
    return g_total;
}

const SnapshotReuseStats& SnapshotReuse_Stats()
{
    return g_stats;
}
