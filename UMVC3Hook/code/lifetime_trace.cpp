#include "../pch.h"
#include "lifetime_trace.h"
#include "gameplay_hooks.h"
#include "mk10utils.h"
#include "rollback.h"

#include <atomic>
#include <cstring>
#include <initializer_list>

#if defined(_MSC_VER)
#include <intrin.h>
#define LIFE_CALLER() reinterpret_cast<uintptr_t>(_ReturnAddress())
#else
#define LIFE_CALLER() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#endif

extern void HookLog(const char* fmt, ...);

namespace {
constexpr size_t RegistrySize = 4096;
constexpr size_t ProbeLimit = 32;
constexpr size_t EventCapacity = 8192;
constexpr uint64_t CaptureLimit = 65536;
constexpr uint64_t UnknownSampleLimit = 256;
constexpr size_t QuarantineLimit = 128;
thread_local uintptr_t g_caller = 0;
struct CallerScope {
    uintptr_t previous = g_caller;
    explicit CallerScope(uintptr_t caller) { g_caller = caller; }
    ~CallerScope() { g_caller = previous; }
};
enum class Kind : uint8_t { Shot, Effect };
enum class Occupancy : uint8_t { Empty, Live, Tombstone, Quarantined, Collecting };
struct Identity {
    void* object = nullptr;
    uint64_t id = 0;
    uintptr_t birthVtable = 0;
    Kind kind = Kind::Shot;
    Occupancy occupancy = Occupancy::Empty;
    uint32_t snapshots = 0;
    uint64_t revision = 0;
};
static_assert(ROLLBACK_WINDOW > 0 && ROLLBACK_WINDOW <= 32);
struct RetiringEffectState {
    void* object = nullptr;
    uint64_t id = 0, revision = 0;
    void* previous = nullptr;
    void* next = nullptr;
    uint64_t previousId = 0, nextId = 0;
    void* resource = nullptr;
    uint32_t category = 0;
    uint8_t control[0x10]{}; // cUnit +08..+17: rno, unitParam, packed flags, drawMode.
    float deltaTime = 0;
};
struct SnapshotManifest {
    bool valid = false;
    int32_t frame = -1;
    size_t count = 0;
    Identity members[RegistrySize];
    bool retiringValid = false;
    void* unitManager = nullptr;
    uint32_t thread = 0;
    size_t retiringCount = 0;
    RetiringEffectState retiring[QuarantineLimit];
};
SnapshotManifest g_manifests[ROLLBACK_WINDOW];
struct Event {
    const char* name = nullptr;
    uint64_t sequence = 0, id = 0, tick = 0, context = 0, epoch = 0;
    uintptr_t object = 0, birthVtable = 0, vtable = 0;
    uintptr_t caller = 0;
    uint64_t a = 0, b = 0, c = 0, d = 0;
    uint32_t thread = 0, flags = 0;
    Kind kind = Kind::Shot;
    bool untracked = false;
    uint32_t snapshots = 0;
    uint64_t revision = 0;
};
SRWLOCK g_lock = SRWLOCK_INIT;
Identity g_registry[RegistrySize];
Event g_events[EventCapacity];
size_t g_read = 0, g_count = 0;
uint64_t g_nextId = 0, g_sequence = 0, g_epoch = 0;
uint64_t g_dropped = 0, g_registryMisses = 0, g_untracked = 0, g_reusedLive = 0;
uint64_t g_unknownSamples = 0;
uint64_t g_graphSamples = 0;
uint64_t g_reportedDropped = 0, g_reportedMisses = 0, g_reportedUntracked = 0;
uint64_t g_reportedReused = 0;
bool g_reportedStopped = false;
std::atomic<bool> g_recording{false};
std::atomic<bool> g_finishRequested{false};
std::atomic<uint64_t> g_context{uint64_t(uint32_t(-1)) << 1};
size_t g_quarantined = 0;
size_t g_reportedQuarantined = 0;
bool g_retentionOpen = false;
void* (__fastcall* oDeleteEffect)(void*, uint32_t) = nullptr;
void CollectQuarantine();

template<class T> T Read(void* object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<uint8_t*>(object) + offset, sizeof(value));
    return value;
}

template<class T> void Write(void* object, size_t offset, T value)
{
    std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}

// Native sUnit critical section precedes the trace lock, matching native callbacks.
struct UnitGuard {
    void* manager = nullptr;
    UnitGuard() {
        if constexpr (RETIRING_EMPTY_EFFECT_RESTORE_ENABLED && EMPTY_EFFECT_QUARANTINE_ENABLED) {
            manager = Read<void*>((void*)_addr(0x140E17698), 0);
            if (manager) EnterCriticalSection((LPCRITICAL_SECTION)((uint8_t*)manager + 8));
        }
    }
    ~UnitGuard() {
        if (manager) LeaveCriticalSection((LPCRITICAL_SECTION)((uint8_t*)manager + 8));
    }
};

// Caller holds g_lock. Fixed probe budget even for untracked shared cUnit calls.
Identity* Find(void* object, bool insert)
{
    const size_t start = ((uintptr_t(object) >> 4) * 11400714819323198485ull) & (RegistrySize - 1);
    Identity* vacant = nullptr;
    for (size_t probe = 0; probe < ProbeLimit; ++probe) {
        Identity& entry = g_registry[(start + probe) & (RegistrySize - 1)];
        const bool occupied = entry.occupancy != Occupancy::Empty && entry.occupancy != Occupancy::Tombstone;
        if (occupied && entry.object == object)
            return &entry;
        if (!occupied && !vacant) vacant = &entry;
        if (entry.occupancy == Occupancy::Empty) break;
    }
    return insert ? vacant : nullptr;
}

void Push(const char* name, const Identity* identity, void* object,
          uint64_t a, uint64_t b, uint64_t c, uint64_t d, bool inspect, bool untracked = false)
{
    if (!g_recording.load(std::memory_order_relaxed)) return;
    const uint64_t sequence = ++g_sequence;
    if (g_count == EventCapacity) {
        ++g_dropped;
    } else {
        Event event{};
        event.name = name;
        event.sequence = sequence;
        event.id = identity ? identity->id : 0;
        event.kind = identity ? identity->kind : Kind::Shot;
        event.birthVtable = identity ? identity->birthVtable : 0;
        event.snapshots = identity ? identity->snapshots : 0;
        event.revision = identity ? identity->revision : 0;
        event.object = uintptr_t(object);
        event.caller = g_caller;
        event.untracked = untracked;
        event.context = g_context.load(std::memory_order_relaxed);
        event.epoch = g_epoch;
        event.thread = GetCurrentThreadId();
        event.tick = GetTickCount64();
        event.a = a; event.b = b; event.c = c; event.d = d;
        if (inspect && object) {
            event.vtable = Read<uintptr_t>(object, 0);
            event.flags = Read<uint32_t>(object, 0x10);
        }
        g_events[(g_read + g_count++) % EventCapacity] = event;
    }
    if (sequence >= CaptureLimit) g_recording.store(false, std::memory_order_relaxed);
}

void Birth(void* object, Kind kind)
{
    if (!g_recording.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!object) {
        Push("birth_null", nullptr, nullptr, uint64_t(kind), 0, 0, 0, false);
    } else {
        Identity* entry = Find(object, true);
        if (entry) {
            if (entry->occupancy == Occupancy::Live) ++g_reusedLive;
            *entry = {object, ++g_nextId, Read<uintptr_t>(object, 0), kind, Occupancy::Live};
        } else {
            ++g_registryMisses;
        }
        Push("birth", entry, object, uint64_t(kind), 0, 0, 0, true);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

uint64_t Record(const char* name, void* object, uint64_t a = 0, uint64_t b = 0,
                uint64_t c = 0, uint64_t d = 0, bool reportUnknown = true)
{
    if (!g_recording.load(std::memory_order_relaxed)) return 0;
    AcquireSRWLockExclusive(&g_lock);
    Identity* entry = Find(object, false);
    const uint64_t id = entry ? entry->id : 0;
    if (entry) Push(name, entry, object, a, b, c, d, true);
    else if (reportUnknown) {
        // Verified uUiEffectBase, outside gameplay lifetime coverage. Do not
        // classify other unknown effect subclasses as UI by category or address.
        if (object && Read<uintptr_t>(object, 0) == uintptr_t(_addr(0x140B213D0))) {
            ReleaseSRWLockExclusive(&g_lock);
            return 0;
        }
        ++g_untracked;
        if (g_unknownSamples < UnknownSampleLimit) {
            ++g_unknownSamples;
            // Only typed shot/effect boundaries opt in. Never adopt an unknown identity.
            Push(name, nullptr, object, a, b, c, d, true, true);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return id;
}

void Destroyed(const char* name, void* object, uint64_t id)
{
    if (!id) return;
    AcquireSRWLockExclusive(&g_lock);
    Identity* entry = Find(object, false);
    if (entry && entry->id == id) {
        // Never read object storage after native teardown or attribute a reused address.
        Push(name, entry, object, 0, 0, 0, 0, false);
        entry->occupancy = Occupancy::Tombstone;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Potentially destructive boundaries, not a claim that every call frees storage.
// 1=base teardown, 2=graph release, 3=collision release, 4=resource bind, 5=graph create.
void Mutation(void* object, uint64_t reason)
{
    if (!g_recording.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_lock);
    Identity* entry = Find(object, false);
    if (entry) {
        if (entry->snapshots)
            Push("snapshot_hazard", entry, object, reason, entry->snapshots, entry->revision, 0, false);
        ++entry->revision;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void* (__fastcall* oCreateShot)(void*) = nullptr;
void* __fastcall CreateShot(void* resource)
{
    const CallerScope caller(LIFE_CALLER());
    void* result = oCreateShot(resource);
    Birth(result, Kind::Shot);
    return result;
}
void* (__fastcall* oConstructEffect)(void*) = nullptr;
void* __fastcall ConstructEffect(void* effect)
{
    const CallerScope caller(LIFE_CALLER());
    void* result = oConstructEffect(effect);
    // Includes direct new+ctor paths. Do not also trace the factory as a birth.
    Birth(result, Kind::Effect);
    return result;
}

// No destructor body has executed here. Native normal/headless sUnit have already
// detached the object; reset and other direct deleting calls are not normalized.
bool EmptyEffectGraph(void* effect)
{
    return Read<uintptr_t>(effect, 0) == uintptr_t(_addr(0x140A8E180)) &&
        Read<uint64_t>(effect, 0x204) == 0 &&
        Read<uintptr_t>(effect, 0x218) == 0 && Read<uintptr_t>(effect, 0x220) == 0 &&
        Read<uintptr_t>(effect, 0x228) == 0 &&
        (Read<uint32_t>(effect, 0x130) & 0x01000000u) == 0;
}

// Retirement freezes these CPU-owned allocations. No state-1/2 simulation is
// restored here; GPU contexts, child retirement, and audio requests are excluded.
bool RetiringEffectGraph(void* effect)
{
    if (EmptyEffectGraph(effect)) return true;
    if constexpr (!RETIRING_CPU_BILLBOARD_ENABLED) return false;
    if (Read<uintptr_t>(effect, 0) != uintptr_t(_addr(0x140A8E180)) ||
        !(Read<uint32_t>(effect, 0x130) & 0x01000000u) ||
        (Read<uint32_t>(effect, 0x100) & 0x0F080000u) ||
        Read<void*>(effect, 0x1F0) || Read<void*>(effect, 0x228)) return false;
    const size_t nodes = Read<uint16_t>(effect, 0x204);
    const size_t joints = Read<uint16_t>(effect, 0x208);
    const uintptr_t storage = Read<uintptr_t>(effect, 0x220);
    if (!nodes || nodes > 32 || !joints || joints > 32 || !storage) return false;
    // Reconstruct the ordinary allocation extent; +210 is aggregate accounting.
    size_t bytes = joints * 0x150;
    size_t auxiliary = 0;
    void* node = Read<void*>(effect, 0x218);
    for (size_t i = 0; i < nodes; ++i) {
        if (!node || Read<uintptr_t>(node, 0) != uintptr_t(_addr(0x140BC4CB0)) ||
            Read<void*>(node, 0x10) != effect || Read<void*>(node, 0x48) ||
            (Read<uint32_t>(node, 0x20) & 0x70u) ||
            Read<int32_t>(node, 0xD4) != -1 ||
            (Read<uint32_t>(node, 0x12C) & 0x400u)) return false;
        void* parameter = Read<void*>(node, 0x58);
        if (!parameter || (Read<uint8_t>(parameter, 0x18) & 0xF0)) return false;
        const uintptr_t joint = Read<uintptr_t>(node, 0x28);
        if (joint < storage || joint - storage >= joints * 0x150 ||
            (joint - storage) % 0x150) return false;
        const uintptr_t pool = Read<uintptr_t>(node, 0x38);
        const size_t poolBytes = Read<uint32_t>(node, 0x34);
        const size_t capacity = Read<uint16_t>(node, 0x110);
        if (pool != storage + bytes || capacity * 0xD0 > poolBytes ||
            poolBytes > 256 * 1024 || bytes + poolBytes > 256 * 1024) return false;
        // Only initialized active records may contain meaningful child pointers.
        void* particle = Read<void*>(node, 0xE0);
        size_t active = 0;
        while (particle) {
            const uintptr_t address = uintptr_t(particle);
            if (++active > capacity || address < pool ||
                address - pool >= capacity * 0xD0 || (address - pool) % 0xD0 ||
                Read<void*>(particle, 0x60)) return false;
            particle = Read<void*>(particle, 8);
        }
        const size_t extra = Read<uint32_t>(node, 0xC4);
        if (extra > 256 * 1024 || (extra && !Read<void*>(node, 0x210))) return false;
        auxiliary += extra;
        bytes += poolBytes;
        node = Read<void*>(node, 0x18);
    }
    if (node) return false; // Count mismatch or cycle, never traverse indefinitely.
    for (size_t i = 0; i < joints; ++i) {
        void* joint = (void*)(storage + i * 0x150);
        const size_t extra = Read<uint32_t>(joint, 0x128);
        if (Read<void*>(joint, 0x100) != effect || extra > 0x50 ||
            (extra && !Read<void*>(joint, 0x130))) return false;
        auxiliary += extra;
    }
    return bytes + auxiliary + nodes * 0x240 + 0x2C0 <= 256 * 1024;
}

bool RetainableDetachedEffect(void* effect)
{
    return RetiringEffectGraph(effect) &&
        (Read<uint32_t>(effect, 0x10) & 0x3FFu) == 0x3F8u &&
        Read<uintptr_t>(effect, 0x18) == 0 && Read<uintptr_t>(effect, 0x20) == 0 &&
        Read<uint64_t>(effect, 0x30) == 0;
}

uint32_t EmptyEffectRejections(void* effect)
{
    return (Read<uintptr_t>(effect, 0) != uintptr_t(_addr(0x140A8E180)) ? 1u : 0u) |
        (Read<uint64_t>(effect, 0x204) ? 2u : 0u) |
        ((Read<uintptr_t>(effect, 0x218) || Read<uintptr_t>(effect, 0x220) ||
          Read<uintptr_t>(effect, 0x228)) ? 4u : 0u) |
        ((Read<uint32_t>(effect, 0x130) & 0x01000000u) ? 8u : 0u) |
        ((Read<uint32_t>(effect, 0x10) & 0x3FFu) != 0x3F8u ? 16u : 0u) |
        ((Read<uintptr_t>(effect, 0x18) || Read<uintptr_t>(effect, 0x20)) ? 32u : 0u) |
        (Read<uint64_t>(effect, 0x30) ? 64u : 0u);
}

void CaptureRetiringEffects(SnapshotManifest& manifest, void* manager)
{
    manifest.retiringValid = manager != nullptr;
    manifest.unitManager = manager;
    manifest.thread = GetCurrentThreadId();
    manifest.retiringCount = 0;
    if (!manager) return;
    const uint32_t categories = Read<uint32_t>(manager, 0xC38);
    if (categories > 64) { manifest.retiringValid = false; return; }
    for (const auto& identity : g_registry) {
        if (identity.occupancy != Occupancy::Live || identity.kind != Kind::Effect) continue;
        void* object = identity.object;
        const uint32_t flags = Read<uint32_t>(object, 0x10);
        const uint32_t state = flags & 7;
        if ((state != 3 && state != 4) || Read<uint64_t>(object, 0x30) || !RetiringEffectGraph(object)) continue;
        const uint32_t category = (flags >> 3) & 0x7F;
        if (category >= categories || manifest.retiringCount == QuarantineLimit) {
            manifest.retiringValid = false;
            return;
        }
        RetiringEffectState saved{};
        saved.object = object; saved.id = identity.id; saved.revision = identity.revision;
        saved.previous = Read<void*>(object, 0x20);
        saved.next = Read<void*>(object, 0x18);
        saved.resource = Read<void*>(object, 0x108);
        saved.category = category;
        void* line = static_cast<uint8_t*>(manager) + 0x50 + 0x30 * category;
        for (bool previous : {true, false}) {
            void* neighbour = previous ? saved.previous : saved.next;
            if (!neighbour) {
                if (Read<void*>(line, previous ? 0 : 8) != object) manifest.retiringValid = false;
                continue;
            }
            const Identity* anchor = Find(neighbour, false);
            if (!anchor || anchor->occupancy != Occupancy::Live ||
                ((Read<uint32_t>(neighbour, 0x10) >> 3) & 0x7F) != category ||
                Read<void*>(neighbour, previous ? 0x18 : 0x20) != object) {
                manifest.retiringValid = false;
                continue;
            }
            if (previous) saved.previousId = anchor->id;
            else saved.nextId = anchor->id;
        }
        std::memcpy(saved.control, static_cast<uint8_t*>(object) + 8, sizeof(saved.control));
        saved.deltaTime = Read<float>(object, 0x28);
        manifest.retiring[manifest.retiringCount++] = saved;
    }
}

void* __fastcall DeleteEffect(void* effect, uint32_t flags)
{
    const CallerScope caller(LIFE_CALLER());
    if constexpr (EMPTY_EFFECT_QUARANTINE_ENABLED) {
        AcquireSRWLockExclusive(&g_lock);
        Identity* entry = Find(effect, false);
        if (entry && entry->occupancy == Occupancy::Quarantined) {
            Push("quarantine_duplicate_delete", entry, effect, flags, 0, 0, 0, false);
            ReleaseSRWLockExclusive(&g_lock);
            return effect;
        }
        if (entry && entry->occupancy == Occupancy::Live && entry->snapshots &&
            g_retentionOpen && g_recording.load(std::memory_order_relaxed) && flags == 1) {
            if (g_quarantined < QuarantineLimit && RetainableDetachedEffect(effect)) {
                entry->occupancy = Occupancy::Quarantined;
                ++g_quarantined;
                Push("quarantine_hold", entry, effect, entry->snapshots, g_quarantined,
                     Read<uint16_t>(effect, 0x204), 0, true);
                ReleaseSRWLockExclusive(&g_lock);
                return effect;
            }
            Push("quarantine_skip", entry, effect, g_quarantined >= QuarantineLimit ? 2 : 1,
                 flags, EmptyEffectRejections(effect), Read<uint32_t>(effect, 0x130), true);
            if (g_graphSamples < 64) {
                void* node = Read<void*>(effect, 0x218);
                // Sample native live nodes before any destructor runs, never historical pointers.
                for (unsigned i = 0; node && i < 8 && g_graphSamples < 64; ++i) {
                    ++g_graphSamples;
                    Push("quarantine_graph", entry, effect, Read<uintptr_t>(node, 0),
                         Read<uintptr_t>(node, 0x48), Read<uint32_t>(node, 0x20),
                         uintptr_t(node), false);
                    node = Read<void*>(node, 0x18);
                }
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return oDeleteEffect(effect, flags);
}

void CollectQuarantine()
{
    if constexpr (!EMPTY_EFFECT_QUARANTINE_ENABLED) return;
    AcquireSRWLockExclusive(&g_lock);
    const bool empty = g_quarantined == 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (empty) return;
    // Only called at main-tick entry or an explicit history/epoch-close boundary.
    // Originals run outside the trace lock; base teardown re-enters the tracer.
    for (size_t i = 0; i < RegistrySize; ++i) {
        AcquireSRWLockExclusive(&g_lock);
        Identity& entry = g_registry[i];
        if (entry.occupancy != Occupancy::Quarantined || entry.snapshots) {
            ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        const uint64_t id = entry.id;
        void* object = entry.object;
        entry.occupancy = Occupancy::Collecting;
        --g_quarantined;
        Push("quarantine_collect", &entry, object, 0, 0, 0, 0, false);
        ReleaseSRWLockExclusive(&g_lock);
        oDeleteEffect(object, 1);
        AcquireSRWLockExclusive(&g_lock);
        if (entry.id == id && entry.occupancy == Occupancy::Collecting)
            entry.occupancy = Occupancy::Tombstone; // Also works after trace capture stops.
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void (__fastcall* oActionFinal)(void*) = nullptr;
void __fastcall ActionFinal(void* action)
{
    const CallerScope caller(LIFE_CALLER());
    LifetimeTrace_ClearSnapshots();
    oActionFinal(action);
}
void (__fastcall* oMainDtor)(void*) = nullptr;
void __fastcall MainDtor(void* main)
{
    const CallerScope caller(LIFE_CALLER());
    LifetimeTrace_ClearSnapshots();
    oMainDtor(main);
}
void (__fastcall* oManagerDtor)(void*) = nullptr;
void __fastcall ManagerDtor(void* manager)
{
    const CallerScope caller(LIFE_CALLER());
    LifetimeTrace_ClearSnapshots();
    oManagerDtor(manager);
}
void (__fastcall* oPublishShot)(void*, uint32_t, void*) = nullptr;
void __fastcall PublishShot(void* manager, uint32_t team, void* shot)
{
    const CallerScope caller(LIFE_CALLER());
    Record("shot_publish_begin", shot, team);
    oPublishShot(manager, team, shot);
    Record("shot_publish_end", shot, team);
}
void (__fastcall* oKill)(void*) = nullptr;
void __fastcall Kill(void* unit)
{
    const CallerScope caller(LIFE_CALLER());
    const uint64_t id = Record("kill_begin", unit, 0, 0, 0, 0, false);
    oKill(unit);
    if (id) Record("kill_end", unit, id, 0, 0, 0, false);
}
bool (__fastcall* oRemoveUnit)(void*, void*) = nullptr;
bool __fastcall RemoveUnit(void* manager, void* unit)
{
    const CallerScope caller(LIFE_CALLER());
    const uint64_t id = Record("unlink_begin", unit, 0, 0, 0, 0, false);
    const bool result = oRemoveUnit(manager, unit);
    if (id) Record("unlink_end", unit, result, 0, 0, 0, false);
    return result;
}
void (__fastcall* oShotDtor)(void*) = nullptr;
void __fastcall ShotDtor(void* shot)
{
    const CallerScope caller(LIFE_CALLER());
    const uint64_t id = Record("shot_base_begin", shot);
    Mutation(shot, 1);
    oShotDtor(shot);
    Destroyed("shot_base_end", shot, id);
}
void (__fastcall* oEffectDtor)(void*) = nullptr;
void __fastcall EffectDtor(void* effect)
{
    const CallerScope caller(LIFE_CALLER());
    const uint64_t id = Record("game_effect_base_begin", effect);
    Mutation(effect, 1);
    oEffectDtor(effect);
    Destroyed("game_effect_base_end", effect, id);
}
void (__fastcall* oShotResource)(void*, void*) = nullptr;
void __fastcall ShotResource(void* shot, void* resource)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(shot, 4);
    if (g_recording.load(std::memory_order_relaxed))
        Record("shot_resource_begin", shot, Read<uintptr_t>(shot, 0x1F68), uintptr_t(resource));
    oShotResource(shot, resource);
    Record("shot_resource_end", shot, uintptr_t(resource));
}
void (__fastcall* oReleaseCollision)(void*, uint32_t, uint8_t) = nullptr;
void __fastcall ReleaseCollision(void* shot, uint32_t slot, uint8_t full)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(shot, 3);
    Record("collision_release_begin", shot, slot, full);
    if (g_recording.load(std::memory_order_relaxed))
        Record("collision_roots", shot, Read<uintptr_t>(shot, 0x2058),
               Read<uintptr_t>(shot, 0x2060), Read<uintptr_t>(shot, 0x2068), Read<uintptr_t>(shot, 0x2070));
    oReleaseCollision(shot, slot, full);
    Record("collision_release_end", shot, slot, full);
}
void (__fastcall* oEffectResource)(void*, void*) = nullptr;
void __fastcall EffectResource(void* effect, void* resource)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(effect, 4);
    if (g_recording.load(std::memory_order_relaxed))
        Record("effect_resource_begin", effect, Read<uintptr_t>(effect, 0x108), uintptr_t(resource));
    oEffectResource(effect, resource);
    Record("effect_resource_end", effect, uintptr_t(resource));
}
void (__fastcall* oReleaseGraph)(void*) = nullptr;
void __fastcall ReleaseGraph(void* effect)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(effect, 2);
    if (g_recording.load(std::memory_order_relaxed))
        Record("graph_release_begin", effect, Read<uintptr_t>(effect, 0x218),
               Read<uintptr_t>(effect, 0x220), Read<uintptr_t>(effect, 0x108), Read<uint32_t>(effect, 0x204));
    oReleaseGraph(effect);
    Record("graph_release_end", effect);
}
bool (__fastcall* oCreateGraph)(void*) = nullptr;
bool __fastcall CreateGraph(void* effect)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(effect, 5);
    if (g_recording.load(std::memory_order_relaxed)) {
        void* resource = Read<void*>(effect, 0x108);
        Record("graph_create_begin", effect, uintptr_t(resource),
               resource ? Read<uint32_t>(resource, 0x54) : 0,
               Read<uint32_t>(effect, 0x130), Read<uintptr_t>(effect, 0x218));
    }
    const bool result = oCreateGraph(effect);
    if (g_recording.load(std::memory_order_relaxed))
        Record("graph_create_end", effect, result, Read<uintptr_t>(effect, 0x218),
               Read<uintptr_t>(effect, 0x220), Read<uint32_t>(effect, 0x204));
    return result;
}
void (__fastcall* oMainReset)(void*) = nullptr;
void __fastcall MainReset(void* main)
{
    const CallerScope caller(LIFE_CALLER());
    LifetimeTrace_ClearSnapshots();
    AcquireSRWLockExclusive(&g_lock);
    ++g_epoch;
    Push("main_reset_begin", nullptr, nullptr, uintptr_t(main), 0, 0, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
    oMainReset(main);
    LifetimeTrace_Marker("main_reset_end", -1);
}
} // namespace

void LifetimeTrace_DropSnapshot(int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (!g_recording.load(std::memory_order_relaxed)) return;
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    AcquireSRWLockExclusive(&g_lock);
    auto& manifest = g_manifests[slot];
    if (manifest.valid)
        Push("snapshot_drop", nullptr, nullptr, uint32_t(manifest.frame), slot, manifest.count, 0, false);
    manifest.valid = false;
    manifest.count = 0;
    manifest.retiringValid = false;
    manifest.retiringCount = 0;
    for (auto& identity : g_registry) identity.snapshots &= ~(uint32_t(1) << slot);
    ReleaseSRWLockExclusive(&g_lock);
}

void LifetimeTrace_SaveSnapshot(int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (!g_recording.load(std::memory_order_relaxed)) return;
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    UnitGuard unitGuard;
    AcquireSRWLockExclusive(&g_lock);
    auto& manifest = g_manifests[slot];
    // Also supports replacing an already-saved same-frame observation atomically.
    if (manifest.valid)
        Push("snapshot_drop", nullptr, nullptr, uint32_t(manifest.frame), slot, manifest.count, 0, false);
    manifest.valid = true;
    manifest.frame = frame;
    manifest.count = 0;
    g_retentionOpen = true;
    for (auto& identity : g_registry) {
        identity.snapshots &= ~(uint32_t(1) << slot);
        if (identity.occupancy != Occupancy::Live) continue;
        identity.snapshots |= uint32_t(1) << slot;
        manifest.members[manifest.count++] = identity;
    }
    CaptureRetiringEffects(manifest, unitGuard.manager);
    Push("snapshot_manifest", nullptr, nullptr, uint32_t(frame), slot, manifest.count,
         g_registryMisses || g_untracked || g_reusedLive || g_dropped, false);
    ReleaseSRWLockExclusive(&g_lock);
}

void LifetimeTrace_CheckSnapshot(int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (!g_recording.load(std::memory_order_relaxed)) return;
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    AcquireSRWLockExclusive(&g_lock);
    const auto& manifest = g_manifests[slot];
    if (!manifest.valid || manifest.frame != frame) {
        Push("snapshot_manifest_absent", nullptr, nullptr, uint32_t(frame), slot, 0, 0, false);
    } else {
        size_t missing = 0, changed = 0, held = 0, samples = 0;
        for (size_t i = 0; i < manifest.count; ++i) {
            const Identity& saved = manifest.members[i];
            const Identity* current = Find(saved.object, false);
            const bool gone = !current || current->id != saved.id;
            const bool revised = !gone && current->revision != saved.revision;
            held += !gone && current->occupancy == Occupancy::Quarantined;
            missing += gone;
            changed += revised;
            if ((gone || revised) && samples++ < 16)
                Push(gone ? "snapshot_identity_missing" : "snapshot_identity_changed", &saved,
                     saved.object, uint32_t(frame), current ? current->id : 0,
                     saved.revision, current ? current->revision : 0, false);
        }
        Push("snapshot_manifest_check", nullptr, nullptr, uint32_t(frame), manifest.count, missing, changed, false);
        if (held)
            Push("quarantine_restore_pending", nullptr, nullptr, uint32_t(frame), held, 0, 0, false);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void LifetimeTrace_RestoreRetiringEffects(int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED || !EMPTY_EFFECT_QUARANTINE_ENABLED ||
                  !RETIRING_EMPTY_EFFECT_RESTORE_ENABLED) return;
    if (!g_recording.load(std::memory_order_relaxed)) return;
    UnitGuard unitGuard;
    AcquireSRWLockExclusive(&g_lock);
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    const auto& manifest = g_manifests[slot];
    unsigned failure = 0;
    if (!manifest.valid || manifest.frame != frame || !manifest.retiringValid) failure = 1;
    else if (unitGuard.manager != manifest.unitManager || manifest.thread != GetCurrentThreadId()) failure = 2;
    if (failure) {
        Push("retiring_restore_skip", nullptr, nullptr, uint32_t(frame), failure, 0, 0, false);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    const size_t count = manifest.retiringCount;
    if (!count) { ReleaseSRWLockExclusive(&g_lock); return; }
    Identity* identities[QuarantineLimit]{};
    bool wasHeld[QuarantineLimit]{}, covered[QuarantineLimit]{};
    size_t held = 0;
    const auto indexOf = [&](void* object) -> int {
        if (!object) return -1;
        for (size_t i = 0; i < count; ++i)
            if (manifest.retiring[i].object == object) return int(i);
        return -1;
    };
    const uint32_t categories = Read<uint32_t>(unitGuard.manager, 0xC38);
    if (categories > 64) failure = 2;
    // Validate identity before dereferencing any saved object or anchor address.
    for (size_t i = 0; !failure && i < count; ++i) {
        const auto& saved = manifest.retiring[i];
        Identity* current = Find(saved.object, false);
        if (!current || current->id != saved.id || current->revision != saved.revision ||
            !(current->snapshots & (uint32_t(1) << slot)) || saved.category >= categories ||
            (current->occupancy != Occupancy::Live && current->occupancy != Occupancy::Quarantined)) {
            failure = 3; break;
        }
        if (!RetiringEffectGraph(saved.object) || Read<uint64_t>(saved.object, 0x30) ||
            Read<void*>(saved.object, 0x108) != saved.resource) { failure = 3; break; }
        identities[i] = current;
        wasHeld[i] = current->occupancy == Occupancy::Quarantined;
        held += wasHeld[i];
        if (wasHeld[i]) {
            if (!RetainableDetachedEffect(saved.object)) failure = 3;
        } else {
            const uint32_t flags = Read<uint32_t>(saved.object, 0x10);
            if (((flags >> 3) & 0x7F) != saved.category || ((flags & 7) != 3 && (flags & 7) != 4)) failure = 3;
            void* line = static_cast<uint8_t*>(unitGuard.manager) + 0x50 + 0x30 * saved.category;
            for (bool previous : {true, false}) {
                void* neighbour = Read<void*>(saved.object, previous ? 0x20 : 0x18);
                if (!neighbour) {
                    if (Read<void*>(line, previous ? 0 : 8) != saved.object) failure = 4;
                } else {
                    const Identity* anchor = Find(neighbour, false);
                    if (!anchor || anchor->occupancy != Occupancy::Live ||
                        ((Read<uint32_t>(neighbour, 0x10) >> 3) & 0x7F) != saved.category ||
                        Read<void*>(neighbour, previous ? 0x18 : 0x20) != saved.object) failure = 4;
                }
            }
        }
        for (bool previous : {true, false}) {
            void* neighbour = previous ? saved.previous : saved.next;
            const uint64_t id = previous ? saved.previousId : saved.nextId;
            if (!neighbour) continue;
            const Identity* anchor = Find(neighbour, false);
            const int index = indexOf(neighbour);
            if (!anchor || anchor->id != id) { failure = 4; continue; }
            if (index >= 0) {
                const auto& adjacent = manifest.retiring[index];
                if (adjacent.category != saved.category ||
                    (previous ? adjacent.next : adjacent.previous) != saved.object) failure = 4;
            } else if (anchor->occupancy != Occupancy::Live ||
                       ((Read<uint32_t>(neighbour, 0x10) >> 3) & 0x7F) != saved.category) failure = 4;
        }
    }
    // Compare each saved segment's external anchors against the current list
    // with admitted nodes omitted. New occupants in a gap cause rejection.
    const auto skipAdmitted = [&](void* node, size_t offset) -> void* {
        for (size_t steps = 0; steps <= count; ++steps) {
            const int index = indexOf(node);
            if (index < 0) return node;
            if (identities[index]->occupancy != Occupancy::Live) return (void*)uintptr_t(1);
            node = Read<void*>(node, offset);
        }
        return (void*)uintptr_t(1); // Cycle: cannot match a validated anchor.
    };
    for (size_t i = 0; !failure && i < count; ++i) {
        const auto& first = manifest.retiring[i];
        if (indexOf(first.previous) >= 0) continue;
        size_t tail = i;
        for (;;) {
            if (covered[tail]) { failure = 4; break; }
            covered[tail] = true;
            const int next = indexOf(manifest.retiring[tail].next);
            if (next < 0) break;
            tail = size_t(next);
        }
        if (failure) break;
        const auto& last = manifest.retiring[tail];
        void* line = static_cast<uint8_t*>(unitGuard.manager) + 0x50 + 0x30 * first.category;
        void* forward = first.previous ? Read<void*>(first.previous, 0x18) : Read<void*>(line, 0);
        void* backward = last.next ? Read<void*>(last.next, 0x20) : Read<void*>(line, 8);
        if (skipAdmitted(forward, 0x18) != last.next || skipAdmitted(backward, 0x20) != first.previous)
            failure = 4;
    }
    for (size_t i = 0; !failure && i < count; ++i) if (!covered[i]) failure = 4;
    if (held > g_quarantined) failure = 3;
    if (failure) {
        Push("retiring_restore_skip", nullptr, nullptr, uint32_t(frame), failure, count, held, false);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    // No native callback, allocator or destructor is invoked in the commit phase.
    for (size_t i = 0; i < count; ++i) {
        if (wasHeld[i]) continue;
        const auto& saved = manifest.retiring[i];
        void* previous = Read<void*>(saved.object, 0x20);
        void* next = Read<void*>(saved.object, 0x18);
        void* line = static_cast<uint8_t*>(unitGuard.manager) + 0x50 + 0x30 * saved.category;
        if (previous) Write(previous, 0x18, next); else Write(line, 0, next);
        if (next) Write(next, 0x20, previous); else Write(line, 8, previous);
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& saved = manifest.retiring[i];
        std::memcpy(static_cast<uint8_t*>(saved.object) + 8, saved.control, sizeof(saved.control));
        Write(saved.object, 0x28, saved.deltaTime);
        Write(saved.object, 0x18, saved.next);
        Write(saved.object, 0x20, saved.previous);
        if (wasHeld[i]) { --g_quarantined; identities[i]->occupancy = Occupancy::Live; }
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& saved = manifest.retiring[i];
        void* line = static_cast<uint8_t*>(unitGuard.manager) + 0x50 + 0x30 * saved.category;
        if (indexOf(saved.previous) < 0) {
            if (saved.previous) Write(saved.previous, 0x18, saved.object); else Write(line, 0, saved.object);
        }
        if (indexOf(saved.next) < 0) {
            if (saved.next) Write(saved.next, 0x20, saved.object); else Write(line, 8, saved.object);
        }
        Push("retiring_state_restored", identities[i], saved.object, uint32_t(frame), wasHeld[i],
             saved.category, Read<uint32_t>(saved.object, 0x10) & 7, true);
    }
    Push("retiring_restore_applied", nullptr, nullptr, uint32_t(frame), count, held, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
}

void LifetimeTrace_ClearSnapshots()
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    AcquireSRWLockExclusive(&g_lock);
    g_retentionOpen = false;
    for (auto& manifest : g_manifests) {
        manifest.valid = false; manifest.count = 0;
        manifest.retiringValid = false; manifest.retiringCount = 0;
    }
    for (auto& identity : g_registry) identity.snapshots = 0;
    Push("snapshot_manifests_clear", nullptr, nullptr, 0, 0, 0, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
    CollectQuarantine();
}

void LifetimeTrace_SetFrame(int32_t frame, bool resimulating)
{
    if constexpr (LIFETIME_TRACE_ENABLED)
        g_context.store((uint64_t(uint32_t(frame)) << 1) | uint64_t(resimulating), std::memory_order_relaxed);
}

void LifetimeTrace_Marker(const char* event, int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (!g_recording.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_lock);
    Push(event, nullptr, nullptr, uint32_t(frame), 0, 0, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
}

bool LifetimeTrace_FinishCapture()
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return true;
    AcquireSRWLockExclusive(&g_lock);
    const bool complete = !g_recording.load(std::memory_order_relaxed) &&
        g_quarantined == 0 && g_count == 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (!complete) g_finishRequested.store(true, std::memory_order_release);
    return complete;
}

void LifetimeTrace_Flush()
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (g_finishRequested.exchange(false, std::memory_order_acq_rel)) {
        LifetimeTrace_ClearSnapshots();
        LifetimeTrace_Marker("capture_finish", int32_t(g_context.load(std::memory_order_relaxed) >> 1));
        g_recording.store(false, std::memory_order_relaxed);
    }
    // Retention must never depend on continued event recording. Reaching the
    // capture limit closes the experiment and drains even the still-leased set.
    if constexpr (EMPTY_EFFECT_QUARANTINE_ENABLED) {
        if (!g_recording.load(std::memory_order_relaxed)) {
            AcquireSRWLockExclusive(&g_lock);
            const bool close = g_retentionOpen || g_quarantined != 0;
            ReleaseSRWLockExclusive(&g_lock);
            if (close) LifetimeTrace_ClearSnapshots();
        } else CollectQuarantine();
    }
    // Fixed output budget. Native callbacks only append; no logging while holding g_lock.
    Event batch[256];
    AcquireSRWLockExclusive(&g_lock);
    const size_t count = g_count < 256 ? g_count : 256;
    for (size_t i = 0; i < count; ++i) batch[i] = g_events[(g_read + i) % EventCapacity];
    g_read = (g_read + count) % EventCapacity;
    g_count -= count;
    const auto dropped = g_dropped, misses = g_registryMisses, unknown = g_untracked, reused = g_reusedLive;
    const auto samples = g_unknownSamples;
    const auto quarantined = g_quarantined;
    const bool stopped = !g_recording.load(std::memory_order_relaxed) && g_sequence >= CaptureLimit;
    ReleaseSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < count; ++i) {
        const Event& e = batch[i];
        HookLog("LIFE seq=%llu ms=%llu thread=%u frame=%d resim=%u epoch=%llu event=%s id=%llu kind=%s obj=0x%llX birthvt=0x%llX vt=0x%llX flags=0x%X a=0x%llX b=0x%llX c=0x%llX d=0x%llX untracked=%u caller=0x%llX snapshots=0x%X revision=%llu\n",
                e.sequence, e.tick, e.thread, int32_t(e.context >> 1), unsigned(e.context & 1), e.epoch,
                e.name, e.id, e.id ? (e.kind == Kind::Shot ? "shot" : "effect") : "none",
                uint64_t(e.object), uint64_t(e.birthVtable), uint64_t(e.vtable), e.flags, e.a, e.b, e.c, e.d,
                unsigned(e.untracked), uint64_t(e.caller), e.snapshots, e.revision);
    }
    if (dropped != g_reportedDropped || misses != g_reportedMisses || unknown != g_reportedUntracked ||
        reused != g_reportedReused || stopped != g_reportedStopped || quarantined != g_reportedQuarantined) {
        HookLog("LIFESTATS dropped=%llu registry_misses=%llu untracked=%llu reused_live=%llu stopped=%d unknown_samples=%llu quarantined=%zu\n",
                dropped, misses, unknown, reused, stopped, samples, quarantined);
        g_reportedDropped = dropped; g_reportedMisses = misses; g_reportedUntracked = unknown;
        g_reportedReused = reused; g_reportedStopped = stopped;
        g_reportedQuarantined = quarantined;
    }
}

void LifetimeTrace_Install()
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    const struct { uintptr_t address; void* detour; void** original; } hooks[] = {
        {0x14029F7F0, (void*)&CreateShot, (void**)&oCreateShot},
        {0x140139D30, (void*)&ConstructEffect, (void**)&oConstructEffect},
        {0x140150AD0, (void*)&PublishShot, (void**)&oPublishShot},
        {0x140016F20, (void*)&Kill, (void**)&oKill},
        {0x14051B9F0, (void*)&RemoveUnit, (void**)&oRemoveUnit},
        {0x1401C9FD0, (void*)&ShotDtor, (void**)&oShotDtor},
        {0x140139DF0, (void*)&EffectDtor, (void**)&oEffectDtor},
        {0x1401CDAE0, (void*)&ShotResource, (void**)&oShotResource},
        {0x1401C6870, (void*)&ReleaseCollision, (void**)&oReleaseCollision},
        {0x1405E6C40, (void*)&EffectResource, (void**)&oEffectResource},
        {0x1405E6890, (void*)&ReleaseGraph, (void**)&oReleaseGraph},
        {0x1405E27C0, (void*)&CreateGraph, (void**)&oCreateGraph},
        {0x140259990, (void*)&MainReset, (void**)&oMainReset},
    };
    bool complete = true;
    if constexpr (EMPTY_EFFECT_QUARANTINE_ENABLED) {
        const struct { uintptr_t address; void* detour; void** original; } retentionHooks[] = {
            {0x140139E80, (void*)&DeleteEffect, (void**)&oDeleteEffect},
            {0x140202DE0, (void*)&ActionFinal, (void**)&oActionFinal},
            {0x140257D80, (void*)&MainDtor, (void**)&oMainDtor},
            {0x14025A070, (void*)&ManagerDtor, (void**)&oManagerDtor},
        };
        // Deleting-hook enable is deferred until ALL trace/epoch hooks exist below.
        for (const auto& hook : retentionHooks) {
            const auto status = GameplayHooks_Create((void*)_addr(hook.address), hook.detour, hook.original);
            if (status != MH_OK) {
                complete = false;
                HookLog("LifetimeTrace: retention hook creation failed va=0x%llX status=%d\n",
                        uint64_t(hook.address), int(status));
            }
        }
    }
    for (const auto& hook : hooks) {
        const auto status = GameplayHooks_Create((void*)_addr(hook.address), hook.detour, hook.original);
        if (status != MH_OK) {
            complete = false;
            HookLog("LifetimeTrace: hook creation failed va=0x%llX status=%d\n", uint64_t(hook.address), int(status));
        }
    }
    if (!complete) return; // No partial trace is enabled when creation failed.
    if constexpr (EMPTY_EFFECT_QUARANTINE_ENABLED) {
        for (uintptr_t address : {uintptr_t(0x140139E80), uintptr_t(0x140202DE0),
                                  uintptr_t(0x140257D80), uintptr_t(0x14025A070)})
            complete &= GameplayHooks_QueueEnable((void*)_addr(address)) == MH_OK;
    }
    for (const auto& hook : hooks)
        complete &= GameplayHooks_QueueEnable((void*)_addr(hook.address)) == MH_OK;
    g_recording.store(complete, std::memory_order_relaxed);
    HookLog("LifetimeTrace: queued=%d image_base=0x%llX limit=%llu ring=%zu registry=%zu empty_effect_quarantine=%d quarantine_limit=%zu retiring_restore=%d cpu_billboard_retention=%d; no active-effect rewind\n",
            complete, uint64_t(_addr(0x140000000)), CaptureLimit, EventCapacity, RegistrySize,
            EMPTY_EFFECT_QUARANTINE_ENABLED, QuarantineLimit, RETIRING_EMPTY_EFFECT_RESTORE_ENABLED,
            RETIRING_CPU_BILLBOARD_ENABLED);
}
