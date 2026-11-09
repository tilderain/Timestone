#include "../pch.h"
#include "lifetime_trace.h"
#include "gameplay_hooks.h"
#include "mk10utils.h"
#include "rollback.h"
#include "rollback_memory.h"
#include "rollback_character_restore.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#include <algorithm>
#include <vector>
#include <functional>
#include <array>
#include <utility>
#include <unordered_map>

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
constexpr size_t PayloadCapacity = 1024 * 1024;
constexpr size_t SpanCapacity = 2048;
constexpr size_t GraphSpanLimit = 97; // Combined block, 32 nodes, and 64 auxiliaries.
constexpr size_t GenerationCapacity = 512;
struct GeneratorLayout {
    uintptr_t vtable = 0;
    uint32_t size = 0;
    bool ordinary = false;
};
GeneratorLayout g_generatorLayouts[64];
size_t g_generatorLayoutCount = 0;
thread_local uintptr_t g_caller = 0;
struct CallerScope {
    uintptr_t previous = g_caller;
    explicit CallerScope(uintptr_t caller) { g_caller = caller; }
    ~CallerScope() { g_caller = previous; }
};
enum class Kind : uint8_t { Shot, Effect, Unit };
enum class Occupancy : uint8_t { Empty, Live, Tombstone, Quarantined, Collecting };
using Deleting = void* (__fastcall*)(void*, uint32_t);
struct Identity {
    void* object = nullptr;
    uint64_t id = 0;
    uintptr_t birthVtable = 0;
    Kind kind = Kind::Shot;
    Occupancy occupancy = Occupancy::Empty;
    uint32_t snapshots = 0;
    uint32_t dependencies = 0;
    uint64_t revision = 0;
    int graph = -1;
    int resourceReference = -1;
    uint64_t restoreTag = 0;
    bool restoreOwned = false;
    Deleting deleting = nullptr;
    bool shotLinked = false, shotRegistrationReleased = false;
    bool futureShot = false;
    uint64_t dti = 0;
    uint32_t extent = 0;
};
static_assert(ROLLBACK_WINDOW > 0 && ROLLBACK_WINDOW <= 32);
struct GraphSpan {
    void* address = nullptr;
    uint32_t size = 0, offset = 0;
};
struct GraphGeneration {
    uint64_t id = 0, ownerId = 0;
    void* owner = nullptr;
    void* resource = nullptr;
    uint32_t snapshots = 0;
    bool attached = false, collecting = false;
    int reference = -1;
    size_t spanCount = 0;
    GraphSpan spans[GraphSpanLimit];
    uint8_t root[0x2C0]{};
};
GraphGeneration g_generations[GenerationCapacity];
struct ResourceReference {
    void* resource = nullptr;
    uint64_t ownerId = 0;
    size_t generations = 0;
    bool onOwner = false;
};
ResourceReference g_resourceReferences[GenerationCapacity];
uint64_t g_nextGeneration = 0;
size_t g_generationHighWater = 0;
struct EffectState {
    void* object = nullptr;
    uint64_t id = 0, revision = 0;
    void* resource = nullptr;
    uint32_t category = 0;
    uint8_t control[0x10]{}; // cUnit +08..+17: rno, unitParam, packed flags, drawMode.
    float deltaTime = 0;
    uint64_t groups = 0;
    bool simulation = false;
    uint32_t spanBegin = 0, spanCount = 0;
    int graph = -1;
    uint64_t generation = 0;
    uint8_t root[0x2C0]{};
};
struct ShotSpan {
    const char* name = nullptr;
    GraphSpan memory;
    uint64_t ownerSlot = 0, vtable = 0;
    uint16_t group = UINT16_MAX;
    uint32_t element = UINT32_MAX;
};
struct ShotState {
    uint64_t id = 0;
    std::vector<ShotSpan> spans;
    std::vector<uint8_t> payload;
    struct GenerationRef { size_t index; uint64_t id; };
    std::vector<GenerationRef> generations;
};
} // namespace

struct LifetimeSnapshot {
    bool valid = false;
    int32_t frame = -1;
    size_t count = 0;
    std::vector<Identity> members;
    void* unitManager = nullptr;
    uint32_t thread = 0;
    uint64_t birthWatermark = 0;
    std::vector<void*> primary[64];
    std::vector<void*> groups[64];
    size_t effectCount = 0;
    std::vector<EffectState> effects;
    size_t spanCount = 0, payloadBytes = 0;
    std::vector<GraphSpan> spans;
    std::vector<uint8_t> payload;
    std::vector<ShotState> shots;
    void* shotManager = nullptr;
    uint32_t shotCursor = 0;
    void* shotConfig = nullptr;
    uint32_t shotSlotFlags = 0;
};

namespace {
using SnapshotManifest = LifetimeSnapshot;
// Non-owning lease index only. Captured state belongs to the snapshot storage.
SnapshotManifest* g_liveSnapshots[ROLLBACK_WINDOW]{};
std::vector<void*> g_groupScratch[64];
std::vector<void*> g_primaryScratch[64];
std::vector<void*> g_membershipLive, g_membershipSaved, g_membershipOrder;
uint64_t g_restoreTag = 0;
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
std::atomic<bool> g_lifetimeEnabled{false};
std::atomic<bool> g_finishRequested{false};
std::atomic<uint64_t> g_context{uint64_t(uint32_t(-1)) << 1};
size_t g_quarantined = 0;
size_t g_reportedQuarantined = 0;
bool g_retentionOpen = false;
void* (__fastcall* oDeleteEffect)(void*, uint32_t) = nullptr;
void CollectQuarantine();
void CollectGraphGenerations();
bool HasGraphGenerations(const Identity& owner);
void (__fastcall* oReleaseGraph)(void*) = nullptr;
void (__fastcall* releaseResourceRef)(void*) = nullptr;
void (__fastcall* oUnlinkShot)(void*, uint32_t, void*) = nullptr;
void (__fastcall* oReleaseShotId)(void*, uint32_t) = nullptr;
void (__fastcall* oRemoveGroups)(void*, void*) = nullptr;
bool (__fastcall* oRemoveUnit)(void*, void*) = nullptr;
bool (__fastcall* oAddUnit)(void*, uint32_t, void*, uint64_t) = nullptr;
void (__fastcall* oEffectOwner)(void*, void*) = nullptr;
thread_local void* g_collectingShot = nullptr;
thread_local void* g_shotBase = nullptr;
void RegisterUnit(void* unit);
Deleting RegisterDeleting(void* object);
void CollectShotGenerations();
bool HasRetiredShotGenerations(uint64_t owner);
void ForgetCurrentShotGenerations(uint64_t owner, void* address);
void CaptureShotGenerations(const Identity& shot, uint32_t bit, ShotState& saved);
void DropShotGenerations(const LifetimeSnapshot* saved, uint32_t bit);
size_t ShotKeyframeBytes(const Identity& shot);
void FenceFutureShotCollection();
void FenceRetiredShotCollisions();

template<class T> T Read(const void* object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(value));
    return value;
}

template<class T> void Write(void* object, size_t offset, T value)
{
    std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}

// Typed CPU allocation census only. This is capture, not permission to restore
// ownership-bearing pointers: replacement-generation adapters must precede restore.
bool VisitShotMemory(const Identity& shot, LifetimeSnapshotVisitor visitor, void* context)
{
    const auto dti = shot.dti ? shot.dti : GetObjectDti(uint64_t(shot.object));
    const size_t size = shot.extent ? shot.extent : DecodeDtiSize(dti);
    if (size < 0x2140 || size > 1024 * 1024) return false;
    visitor(context, "body", shot.object, 0, shot.birthVtable, shot.object, size);
    auto buffer = [&](const char* name, size_t offset, size_t bytes) {
        void* address = Read<void*>(shot.object, offset);
        if (address && bytes) visitor(context, name, address,
            uint64_t(shot.object) + offset, 0, address, bytes);
    };
    const auto* joints = Read<uint8_t*>(shot.object, 0x538);
    const size_t count = joints ? Read<uint32_t>(joints - 0x10, 0) : 0;
    if (count && count <= 65536) {
        buffer("model.joints", 0x538, count * 0xC0);
        buffer("model.depths", 0x548, count);
        buffer("model.interpolation", 0x11B0, count * 0x40);
        const size_t workCount = std::min(count, size_t(Read<uint32_t>(shot.object, 0x530)));
        const size_t blends = Read<uint32_t>(shot.object, 0x554);
        static const char* names[] = {
            "model.motion.0.joints", "model.motion.1.joints", "model.motion.2.joints", "model.motion.3.joints",
            "model.motion.4.joints", "model.motion.5.joints", "model.motion.6.joints", "model.motion.7.joints"};
        for (size_t i = 0; i < std::min(blends, size_t(8)); ++i)
            buffer(names[i], 0x6C0 + i * 0x180, workCount * 0x68);
    }
    buffer("model.keyframes", 0x6D8, ShotKeyframeBytes(shot));
    return true;
}

// Native sUnit critical section precedes the trace lock, matching native callbacks.
struct UnitGuard {
    void* manager = nullptr;
    UnitGuard() {
        if constexpr (EFFECT_RESTORE_ENABLED && EMPTY_EFFECT_QUARANTINE_ENABLED) {
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
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!object) {
        Push("birth_null", nullptr, nullptr, uint64_t(kind), 0, 0, 0, false);
    } else {
        Identity* entry = Find(object, true);
        if (entry) {
            if (HasGraphGenerations(*entry)) std::abort();
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
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return 0;
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
    if (entry && entry->id == id && entry->kind == Kind::Shot) {
        ReleaseSRWLockExclusive(&g_lock);
        ForgetCurrentShotGenerations(id, object);
        AcquireSRWLockExclusive(&g_lock);
        entry = Find(object, false);
    }
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
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return;
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
    if constexpr (EMPTY_EFFECT_QUARANTINE_ENABLED) {
        if (result && g_lifetimeEnabled.load(std::memory_order_relaxed)) RegisterUnit(result);
    }
    return result;
}

void __fastcall UnlinkShot(void* manager, uint32_t team, void* shot)
{
    if (g_collectingShot && shot == g_collectingShot) return;
    oUnlinkShot(manager, team, shot);
    AcquireSRWLockExclusive(&g_lock);
    if (Identity* identity = Find(shot, false)) identity->shotLinked = false;
    ReleaseSRWLockExclusive(&g_lock);
}

void __fastcall ReleaseShotId(void* manager, uint32_t id)
{
    // The ID alone cannot identify an allocation. Scope suppression to the
    // exact collecting shot's base destructor, including nested destruction.
    if (g_collectingShot && g_shotBase == g_collectingShot) return;
    oReleaseShotId(manager, id);
}

struct UnitDeleteHook { void* target = nullptr; Deleting original = nullptr; };
std::array<UnitDeleteHook, 256> g_unitDeleteHooks;
size_t g_unitDeleteHookCount = 0;

void* DeleteUnit(void* shot, uint32_t flags, Deleting original, uintptr_t nativeCaller)
{
    const CallerScope caller(nativeCaller);
    UnitGuard unitGuard;
    AcquireSRWLockExclusive(&g_lock);
    Identity* identity = Find(shot, false);
    if (identity && identity->occupancy == Occupancy::Quarantined) {
        Push("quarantine_duplicate_delete", identity, shot, flags, 0, 0, 0, false);
        ReleaseSRWLockExclusive(&g_lock);
        return shot;
    }
    if (identity && identity->snapshots && g_retentionOpen && flags != 1) std::abort();
    if (identity && identity->occupancy == Occupancy::Live &&
        (identity->snapshots || (identity->kind == Kind::Shot && HasRetiredShotGenerations(identity->id))) &&
        g_retentionOpen && flags == 1) {
        identity->occupancy = Occupancy::Quarantined;
        ++g_quarantined;
        const bool linked = identity->shotLinked;
        identity->shotLinked = false;
        ReleaseSRWLockExclusive(&g_lock);
        // Normal sUnit teardown already detached the shot. Direct deleting
        // calls must produce the same non-simulating postimage without kill.
        if (!unitGuard.manager && (Read<uint64_t>(shot, 0x30) ||
            ((Read<uint32_t>(shot, 0x10) >> 3) & 0x7F) != 0x7F)) std::abort();
        if (Read<uint64_t>(shot, 0x30)) oRemoveGroups(unitGuard.manager, shot);
        if (((Read<uint32_t>(shot, 0x10) >> 3) & 0x7F) != 0x7F)
            oRemoveUnit(unitGuard.manager, shot);
        Write(shot, 0x10, (Read<uint32_t>(shot, 0x10) & ~0x3FFu) | 0x3F8u);
        void* manager = Read<void*>((void*)_addr(0x140D47F40), 0);
        if (manager && identity->kind == Kind::Shot) {
            if (linked) oUnlinkShot(manager, Read<uint32_t>(shot, 0x1F54), shot);
            oReleaseShotId(manager, Read<uint32_t>(shot, 0x1F50));
        }
        if (identity->kind == Kind::Shot) {
            Write(shot, 0x1458, (void*)nullptr);
            Write(shot, 0x1460, (void*)nullptr);
        }
        AcquireSRWLockExclusive(&g_lock);
        identity->shotRegistrationReleased = identity->kind == Kind::Shot && manager != nullptr;
        Push("quarantine_hold", identity, shot, identity->snapshots, g_quarantined, 0, 0, true);
        ReleaseSRWLockExclusive(&g_lock);
        return shot;
    }
    const uint64_t id = identity ? identity->id : 0;
    const bool generic = identity && identity->kind == Kind::Unit;
    if (generic) Push("unit_dispose_begin", identity, shot, flags, 0, 0, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
    if (identity && identity->kind == Kind::Shot) {
        if (HasRetiredShotGenerations(id)) std::abort();
        ForgetCurrentShotGenerations(id, shot);
    }
    void* result = original(shot, flags);
    if (generic) Destroyed("unit_dispose_end", shot, id);
    return result;
}

template<size_t Index> void* __fastcall UnitDeleting(void* shot, uint32_t flags)
{
    return DeleteUnit(shot, flags, g_unitDeleteHooks[Index].original, LIFE_CALLER());
}

template<size_t... Indices> constexpr auto UnitDeleteDetours(std::index_sequence<Indices...>)
{
    return std::array<Deleting, sizeof...(Indices)>{&UnitDeleting<Indices>...};
}
constexpr auto g_unitDeleteDetours = UnitDeleteDetours(std::make_index_sequence<256>{});

Deleting RegisterDeleting(void* shot)
{
    // The live allocation has its final vtable. Share the factory/adoption path
    // and register once per deleting target, not per allocation or whitelist.
    void* target = Read<void*>(Read<void*>(shot, 0), 0);
    size_t index = 0;
    while (index < g_unitDeleteHookCount && g_unitDeleteHooks[index].target != target) ++index;
    if (index == g_unitDeleteHookCount) {
        if (index == g_unitDeleteHooks.size()) std::abort();
        auto& hook = g_unitDeleteHooks[index];
        hook.target = target;
        if (GameplayHooks_Create(target, (void*)g_unitDeleteDetours[index], (void**)&hook.original) != MH_OK)
            std::abort();
        ++g_unitDeleteHookCount;
        if (GameplayHooks_QueueEnable(target) != MH_OK || !GameplayHooks_Apply()) std::abort();
    }
    return g_unitDeleteHooks[index].original;
}

void RegisterUnit(void* shot)
{
    const auto deleting = RegisterDeleting(shot);
    const auto dti = GetObjectDti(uint64_t(shot));
    const auto extent = DecodeDtiSize(dti);
    AcquireSRWLockExclusive(&g_lock);
    Identity* identity = Find(shot, false);
    if (!identity) std::abort();
    identity->deleting = deleting;
    identity->dti = dti;
    identity->extent = extent;
    ReleaseSRWLockExclusive(&g_lock);
}

#include "lifetime_shot_generations.inl"
#include "lifetime_shot_world.inl"

bool __fastcall AddUnit(void* manager, uint32_t line, void* unit, uint64_t groups)
{
    AcquireSRWLockExclusive(&g_lock);
    const Identity* identity = Find(unit, false);
    const uint64_t retired = identity && identity->occupancy == Occupancy::Quarantined ? identity->id : 0;
    const uint32_t before = retired ? Read<uint32_t>(unit, 0x10) : 0;
    ReleaseSRWLockExclusive(&g_lock);
    const bool result = oAddUnit(manager, line, unit, groups);
    if (retired)
        HookLog("LifetimeTrace: retired unit registered id=%llu obj=%p line=%u flags_before=0x%X flags_after=0x%X\n",
                retired, unit, line, before, Read<uint32_t>(unit, 0x10));
    return result;
}

void __fastcall EffectOwner(void* effect, void* owner)
{
    oEffectOwner(effect, owner);
    AcquireSRWLockExclusive(&g_lock);
    const Identity* identity = Find(effect, false);
    const bool retired = identity && identity->occupancy == Occupancy::Quarantined;
    const uint64_t id = identity ? identity->id : 0;
    if (identity) Push("effect_owner_bind", identity, effect, uintptr_t(owner), 0, 0, 0, true);
    ReleaseSRWLockExclusive(&g_lock);
    if (retired)
        HookLog("LifetimeTrace: retired effect owner rebound id=%llu obj=%p owner=%p flags=0x%X\n",
                id, effect, owner, Read<uint32_t>(effect, 0x10));
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

const GeneratorLayout* GeneratorLayoutFor(void* node)
{
    const uintptr_t vtable = Read<uintptr_t>(node, 0);
    for (size_t i = 0; i < g_generatorLayoutCount; ++i)
        if (g_generatorLayouts[i].vtable == vtable) return &g_generatorLayouts[i];
    return nullptr;
}

void RegisterGeneratorLayouts(void* effect)
{
    void* node = Read<void*>(effect, 0x218);
    const uint16_t count = Read<uint16_t>(effect, 0x204);
    for (uint16_t i = 0; node && i < count; ++i, node = Read<void*>(node, 0x18)) {
        AcquireSRWLockExclusive(&g_lock);
        const bool known = GeneratorLayoutFor(node) != nullptr;
        ReleaseSRWLockExclusive(&g_lock);
        if (known) continue;
        // Native getDTI is called once per concrete class, outside the recorder
        // lock. Capture then uses cached native extent and base ancestry.
        const uint64_t dti = GetObjectDti(uintptr_t(node));
        GeneratorLayout layout{Read<uintptr_t>(node, 0), DecodeDtiSize(dti), false};
        bool aggregate = false;
        uint64_t parent = dti;
        for (unsigned depth = 0; parent && depth < 128; ++depth) {
            aggregate |= parent == _addr(0x140E1E168); // Child-unit generator, not an ordinary pool.
            if (parent == _addr(0x140E1E120)) { layout.ordinary = !aggregate; break; }
            parent = Read<uint64_t>((void*)parent, 0x20);
        }
        AcquireSRWLockExclusive(&g_lock);
        if (!GeneratorLayoutFor(node)) {
            if (g_generatorLayoutCount == std::size(g_generatorLayouts)) std::abort();
            g_generatorLayouts[g_generatorLayoutCount++] = layout;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
}

// Allocation-ownership descriptor shared by active and retiring snapshots.
// GPU contexts, child ownership, and pending external requests need other adapters.
bool OwnedEffectGraph(void* effect)
{
    if (EmptyEffectGraph(effect)) return true;
    if constexpr (!ORDINARY_CPU_EFFECT_GRAPHS_ENABLED) return false;
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
    size_t objects = 0;
    void* node = Read<void*>(effect, 0x218);
    for (size_t i = 0; i < nodes; ++i) {
        if (!node) return false;
        const auto* layout = GeneratorLayoutFor(node);
        if (!layout || !layout->ordinary || layout->size < 0x240 || layout->size > 256 * 1024 ||
            Read<void*>(node, 0x10) != effect || Read<void*>(node, 0x48) ||
            (Read<uint32_t>(node, 0x20) & 0x70u) ||
            Read<int32_t>(node, 0xD4) != -1 ||
            (Read<uint32_t>(node, 0x12C) & 0x400u)) return false;
        objects += layout->size;
        const uintptr_t joint = Read<uintptr_t>(node, 0x28);
        if (joint < storage || joint - storage >= joints * 0x150 ||
            (joint - storage) % 0x150) return false;
        const uintptr_t pool = Read<uintptr_t>(node, 0x38);
        const size_t poolBytes = Read<uint32_t>(node, 0x34);
        const size_t capacity = Read<uint16_t>(node, 0x110);
        const size_t stride = Read<uint16_t>(node, 0x116);
        if ((capacity && stride < 0x68) || pool != storage + bytes || capacity * stride > poolBytes ||
            poolBytes > 256 * 1024 || bytes + poolBytes > 256 * 1024) return false;
        // Only initialized active records may contain meaningful child pointers.
        void* particle = Read<void*>(node, 0xE0);
        size_t active = 0;
        while (particle) {
            const uintptr_t address = uintptr_t(particle);
            if (++active > capacity || address < pool ||
                address - pool >= capacity * stride || (address - pool) % stride ||
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
    return bytes + auxiliary + objects + 0x2C0 <= 256 * 1024;
}

bool RetainableDetachedEffect(void* effect)
{
    return OwnedEffectGraph(effect) &&
        (Read<uint32_t>(effect, 0x10) & 0x3FFu) == 0x3F8u &&
        Read<uintptr_t>(effect, 0x18) == 0 && Read<uintptr_t>(effect, 0x20) == 0 &&
        Read<uint64_t>(effect, 0x30) == 0;
}

size_t DescribeOwnedGraph(void* effect, GraphSpan (&spans)[GraphSpanLimit])
{
    size_t count = 0;
    const auto append = [&](void* address, size_t size) {
        if (size) spans[count++] = {address, uint32_t(size), 0};
    };
    size_t combined = size_t(Read<uint16_t>(effect, 0x208)) * 0x150;
    for (void* node = Read<void*>(effect, 0x218); node; node = Read<void*>(node, 0x18)) {
        append(node, GeneratorLayoutFor(node)->size);
        append(Read<void*>(node, 0x210), Read<uint32_t>(node, 0xC4));
        combined += Read<uint32_t>(node, 0x34);
    }
    void* storage = Read<void*>(effect, 0x220);
    for (size_t i = 0; i < Read<uint16_t>(effect, 0x208); ++i) {
        void* joint = static_cast<uint8_t*>(storage) + i * 0x150;
        append(Read<void*>(joint, 0x130), Read<uint32_t>(joint, 0x128));
    }
    append(storage, combined);
    return count;
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

bool HasGraphGenerations(const Identity& owner)
{
    for (size_t i = 0; i < g_generationHighWater; ++i)
        if (g_generations[i].id && g_generations[i].ownerId == owner.id) return true;
    return false;
}

// Caller holds the recorder lock and the native simulation is quiescent.
int OwnCurrentGraph(Identity& owner, const GraphSpan* spans, size_t count)
{
    owner.restoreOwned = true;
    if (owner.graph >= 0) return owner.graph;
    size_t slot = 0;
    while (slot < GenerationCapacity && g_generations[slot].id) ++slot;
    if (slot == GenerationCapacity) return -1;
    if (slot >= g_generationHighWater) g_generationHighWater = slot + 1;
    auto& graph = g_generations[slot];
    graph.id = ++g_nextGeneration;
    graph.ownerId = owner.id;
    graph.owner = owner.object;
    graph.resource = Read<void*>(owner.object, 0x108);
    graph.snapshots = 0;
    graph.attached = true;
    graph.collecting = false;
    graph.spanCount = count;
    for (size_t i = 0; i < count; ++i) graph.spans[i] = spans[i];
    std::memcpy(graph.root, owner.object, sizeof(graph.root));
    owner.graph = int(slot);
    // The object already owns this reference. Snapshot leases do not add refs.
    if (graph.resource) {
        if (owner.resourceReference < 0) {
            size_t reference = 0;
            while (reference < GenerationCapacity && g_resourceReferences[reference].resource) ++reference;
            if (reference == GenerationCapacity) std::abort();
            g_resourceReferences[reference] = {graph.resource, owner.id, 0, true};
            owner.resourceReference = int(reference);
        }
        auto& reference = g_resourceReferences[owner.resourceReference];
        if (!reference.onOwner || reference.resource != graph.resource || reference.ownerId != owner.id) std::abort();
        ++reference.generations;
        graph.reference = owner.resourceReference;
    }
    Push("graph_owned", &owner, owner.object, graph.id, count, uintptr_t(graph.resource), 0, false);
    return int(slot);
}

void TransferOwnerResource(Identity& owner)
{
    if (owner.resourceReference < 0) return;
    auto& reference = g_resourceReferences[owner.resourceReference];
    if (reference.resource != Read<void*>(owner.object, 0x108) || !reference.onOwner) std::abort();
    reference.onOwner = false;
    Push("graph_resource_transfer", &owner, owner.object, owner.resourceReference, 0, 0, 0, false);
    owner.resourceReference = -1;
}

void DetachGraph(Identity& owner)
{
    auto& graph = g_generations[owner.graph];
    std::memcpy(graph.root, owner.object, sizeof(graph.root));
    graph.attached = false;
    owner.graph = -1;
    // Exact postimage of uEffect::releaseGenerator, without destroying storage.
    Write(owner.object, 0x218, (void*)nullptr);
    Write(owner.object, 0x220, (void*)nullptr);
    Write(owner.object, 0x228, (void*)nullptr);
    Write(owner.object, 0x204, uint64_t(0));
    Write(owner.object, 0x130, Read<uint32_t>(owner.object, 0x130) & ~0x01000000u);
    Write(owner.object, 0x1E0, uint32_t(0));
    Write(owner.object, 0x12C, uint16_t(0));
    Push("graph_detached", &owner, owner.object, graph.id, graph.snapshots, 0, 0, false);
}

void CollectGraphGenerations()
{
    AcquireSRWLockExclusive(&g_lock);
    const size_t highWater = g_generationHighWater;
    ReleaseSRWLockExclusive(&g_lock);
    if (!highWater) return;
    UnitGuard unitGuard;
    for (size_t slot = 0; slot < highWater; ++slot) {
        auto& graph = g_generations[slot];
        AcquireSRWLockExclusive(&g_lock);
        if (!graph.id || graph.snapshots || graph.collecting) {
            ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        Identity* owner = Find(graph.owner, false);
        if (!owner || owner->id != graph.ownerId) std::abort(); // Ownership invariant, not a restore miss.
        graph.collecting = true;
        const bool detached = !graph.attached;
        if (graph.attached) owner->graph = -1;
        Push("graph_collect", owner, graph.owner, graph.id, detached, 0, 0, false);
        ReleaseSRWLockExclusive(&g_lock);
        if (detached) {
            // Nodes retain the original owner address. Dispose using that owner,
            // then put its current generation back; never use a surrogate owner.
            uint8_t current[0x280];
            std::memcpy(current, static_cast<uint8_t*>(graph.owner) + 0x40, sizeof(current));
            std::memcpy(static_cast<uint8_t*>(graph.owner) + 0x40, graph.root + 0x40, sizeof(current));
            oReleaseGraph(graph.owner);
            std::memcpy(static_cast<uint8_t*>(graph.owner) + 0x40, current, sizeof(current));
        }
        AcquireSRWLockExclusive(&g_lock);
        void* release = nullptr;
        if (graph.reference >= 0) {
            auto& reference = g_resourceReferences[graph.reference];
            if (!reference.generations || reference.resource != graph.resource) std::abort();
            if (--reference.generations == 0) {
                if (reference.onOwner) owner->resourceReference = -1;
                else release = reference.resource;
                reference = {};
            }
        }
        graph = {};
        ReleaseSRWLockExclusive(&g_lock);
        if (release) releaseResourceRef(release);
    }
    AcquireSRWLockExclusive(&g_lock);
    while (g_generationHighWater && !g_generations[g_generationHighWater - 1].id) --g_generationHighWater;
    ReleaseSRWLockExclusive(&g_lock);
}

// Caller holds g_lock at the quiescent capture fence. These are typed borrowed
// cUnit links, not ownership inferred by scanning address-looking byte values.
void LeaseEffectDependencies(SnapshotManifest& manifest, const EffectState& effect, uint32_t bit)
{
    for (size_t offset : {size_t(0x40), size_t(0x138), size_t(0x1F0),
                          size_t(0x278), size_t(0x280), size_t(0x288)}) {
        void* object = Read<void*>(effect.root, offset);
        if (!object || object == effect.object) continue;
        Identity* identity = Find(object, false);
        if (!identity) {
            ReleaseSRWLockExclusive(&g_lock);
            const uint64_t dti = GetObjectDti(uintptr_t(object));
            bool unit = false, shot = false;
            uint64_t parent = dti;
            for (unsigned depth = 0; parent && depth < 128; ++depth) {
                shot |= parent == uintptr_t(_addr(0x140D4D960));
                if (parent == uintptr_t(_addr(0x140E173D0))) { unit = true; break; }
                parent = Read<uint64_t>((void*)parent, 0x20);
            }
            bool linked = false;
            if (unit) {
                const bool effectDeleting = Read<uintptr_t>(Read<void*>(object, 0), 0) ==
                    uintptr_t(_addr(0x140139E80));
                Birth(object, effectDeleting ? Kind::Effect : (shot ? Kind::Shot : Kind::Unit));
                if (!effectDeleting) RegisterUnit(object);
                if (shot) {
                    // Adoption can discover shots outside the normal factory
                    // hook. Their stale own links do not establish membership.
                    void* head = (void*)(_addr(0x140D47F90) + 0x28 * Read<uint32_t>(object, 0x1F54) + 8);
                    for (void* node = Read<void*>(head, 8); node && node != head; node = Read<void*>(node, 8)) {
                        if (node == static_cast<uint8_t*>(object) + 0x1450) { linked = true; break; }
                    }
                }
            }
            AcquireSRWLockExclusive(&g_lock);
            if (!unit) continue; // Non-cUnit links require their owner's allocation adapter.
            identity = Find(object, false);
            if (!identity) std::abort();
            if (shot) identity->shotLinked = linked;
        }
        const bool first = identity->dependencies == 0;
        identity->dependencies |= bit;
        if (!(identity->snapshots & bit)) {
            identity->snapshots |= bit;
            if (manifest.count == RegistrySize) std::abort();
            manifest.members.push_back(*identity);
            ++manifest.count;
        }
        if (first) Push("snapshot_dependency", identity, object,
                        effect.id, offset, bit, identity->occupancy == Occupancy::Quarantined, false);
    }
}

void CaptureEffects(SnapshotManifest& manifest, void* manager)
{
    manifest.unitManager = manager;
    manifest.thread = GetCurrentThreadId();
    manifest.effectCount = 0;
    manifest.effects.clear();
    manifest.spanCount = manifest.payloadBytes = 0;
    manifest.spans.clear();
    manifest.payload.clear();
    for (auto& entries : manifest.primary) entries.clear();
    for (auto& entries : manifest.groups) entries.clear();
    if (!manager) return;
    const uint32_t categories = Read<uint32_t>(manager, 0xC38);
    if (categories > 64) std::abort();
    for (uint32_t category = 0; category < categories; ++category) {
        auto& entries = manifest.primary[category];
        entries.clear();
        for (void* unit = Read<void*>(manager, 0x50 + 0x30 * category); unit;
             unit = Read<void*>(unit, 0x18)) entries.push_back(unit);
    }
    const uint32_t groups = Read<uint32_t>(manager, 0x1840);
    if (groups > 64) std::abort();
    for (uint32_t group = 0; group < groups; ++group) {
        void* descriptor = static_cast<uint8_t*>(manager) + 0xC40 + group * 0x30;
        const uint32_t size = Read<uint32_t>(descriptor, 0x18);
        auto& entries = manifest.groups[group];
        entries.clear();
        if (size) {
            void** data = Read<void**>(descriptor, 0x28);
            entries.assign(data, data + size);
        }
    }
    size_t activeSeen = 0, activeSaved = 0;
    uint32_t rejectionMask = 0;
    for (auto& identity : g_registry) {
        if (identity.occupancy != Occupancy::Live || identity.kind != Kind::Effect) continue;
        void* object = identity.object;
        const uint32_t flags = Read<uint32_t>(object, 0x10);
        const uint32_t state = flags & 7;
        GraphSpan spans[GraphSpanLimit]{};
        size_t spanCount = 0;
        const bool simulation = state == 2 && EFFECT_SIMULATION_SNAPSHOTS_ENABLED;
        if (simulation) {
            ++activeSeen;
            if (!OwnedEffectGraph(object)) {
                rejectionMask |= 2;
                continue;
            }
        } else if (!OwnedEffectGraph(object)) continue;
        if (Read<void*>(object, 0x218)) spanCount = DescribeOwnedGraph(object, spans);
        const uint32_t category = (flags >> 3) & 0x7F;
        if ((category >= categories && category != 0x7F) || manifest.effectCount == QuarantineLimit) std::abort();
        EffectState saved{};
        saved.object = object; saved.id = identity.id; saved.revision = identity.revision;
        saved.resource = Read<void*>(object, 0x108);
        saved.category = category;
        saved.groups = Read<uint64_t>(object, 0x30);
        saved.simulation = simulation;
        std::memcpy(saved.control, static_cast<uint8_t*>(object) + 8, sizeof(saved.control));
        saved.deltaTime = Read<float>(object, 0x28);
        {
            size_t bytes = 0;
            for (size_t i = 0; i < spanCount; ++i) bytes += spans[i].size;
            if (spanCount > SpanCapacity - manifest.spanCount ||
                bytes > PayloadCapacity - manifest.payloadBytes) {
                Push("effect_capture_overflow", nullptr, nullptr, manifest.effectCount,
                     manifest.payloadBytes, spanCount, bytes, false);
                std::abort();
            }
            saved.graph = OwnCurrentGraph(identity, spans, spanCount);
            if (saved.graph < 0) std::abort();
            auto& graph = g_generations[saved.graph];
            saved.generation = graph.id;
            graph.snapshots |= uint32_t(1) << unsigned((manifest.frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
            saved.spanBegin = uint32_t(manifest.spanCount);
            saved.spanCount = uint32_t(spanCount);
            std::memcpy(saved.root, object, sizeof(saved.root));
            for (size_t i = 0; i < spanCount; ++i) {
                spans[i].offset = uint32_t(manifest.payloadBytes);
                const auto* data = static_cast<const uint8_t*>(spans[i].address);
                manifest.payload.insert(manifest.payload.end(), data, data + spans[i].size);
                manifest.payloadBytes += spans[i].size;
                manifest.spans.push_back(spans[i]);
                ++manifest.spanCount;
            }
            activeSaved += simulation;
        }
        manifest.effects.push_back(saved);
        ++manifest.effectCount;
    }
    const uint32_t bit = uint32_t(1) << unsigned((manifest.frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    for (size_t i = 0; i < manifest.effectCount; ++i)
        LeaseEffectDependencies(manifest, manifest.effects[i], bit);
    if (activeSeen)
        Push("effect_capture", nullptr, nullptr, activeSeen, activeSaved, rejectionMask,
             manifest.payloadBytes, false);
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
        if (entry && (HasGraphGenerations(*entry) || entry->dependencies)) {
            // A leased generation also owns its original outer object. Capacity
            // pressure or trace exhaustion can never turn this into native free.
            if (flags != 1 || (Read<uint32_t>(effect, 0x10) & 0x3FFu) != 0x3F8u ||
                Read<void*>(effect, 0x18) || Read<void*>(effect, 0x20) ||
                Read<uint64_t>(effect, 0x30)) std::abort();
            const bool disposeUnownedGraph = entry->graph < 0 && !entry->dependencies;
            entry->occupancy = Occupancy::Quarantined;
            ++g_quarantined;
            Push("quarantine_hold", entry, effect, entry->snapshots, g_quarantined,
                 Read<uint16_t>(effect, 0x204), 0, true);
            ReleaseSRWLockExclusive(&g_lock);
            // A newer, unreferenced graph still receives native logical cleanup
            // now. The old leased graphs and the outer object remain untouched.
            if (disposeUnownedGraph) oReleaseGraph(effect);
            return effect;
        }
        if (entry && entry->occupancy == Occupancy::Live && entry->snapshots &&
            g_retentionOpen && g_lifetimeEnabled.load(std::memory_order_relaxed) && flags == 1) {
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
    UnitGuard unitGuard;
    FenceFutureShotCollection();
    // Only called at main-tick entry or an explicit history/epoch-close boundary.
    // Originals run outside the trace lock; base teardown re-enters the tracer.
    // Effect teardown may still consult its borrowed model/shot owner. Dispose
    // eligible effects before those outer owners at the same collection fence.
    for (bool effects : {true, false}) for (size_t i = 0; i < RegistrySize; ++i) {
        AcquireSRWLockExclusive(&g_lock);
        Identity& entry = g_registry[i];
        if ((entry.kind == Kind::Effect) != effects ||
            entry.occupancy != Occupancy::Quarantined || entry.snapshots ||
            (entry.futureShot && HasLiveUnitReference(entry.object)) ||
            (entry.kind == Kind::Shot && HasRetiredShotGenerations(entry.id)) ||
            (entry.kind == Kind::Effect && HasGraphGenerations(entry))) {
            ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        const uint64_t id = entry.id;
        void* object = entry.object;
        if (entry.futureShot) {
            ReleaseSRWLockExclusive(&g_lock);
            const bool borrowed = HasCharacterShotReference(unitGuard.manager, object);
            AcquireSRWLockExclusive(&g_lock);
            if (borrowed) { ReleaseSRWLockExclusive(&g_lock); continue; }
            if (entry.id != id || entry.occupancy != Occupancy::Quarantined) std::abort();
        }
        entry.occupancy = Occupancy::Collecting;
        --g_quarantined;
        Push("quarantine_collect", &entry, object, 0, 0, 0, 0, false);
        if (entry.kind == Kind::Unit) Push("unit_dispose_begin", &entry, object, 0, 0, 0, 0, false);
        ReleaseSRWLockExclusive(&g_lock);
        if (entry.kind != Kind::Effect) {
            if (entry.kind == Kind::Shot) ForgetCurrentShotGenerations(entry.id, object);
            void* previous = g_collectingShot;
            g_collectingShot = entry.shotRegistrationReleased ? object : nullptr;
            entry.deleting(object, 1);
            g_collectingShot = previous;
        } else oDeleteEffect(object, 1);
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
void (__fastcall* oUnitReset)(void*) = nullptr;
void __fastcall UnitReset(void* manager)
{
    LifetimeTrace_ClearSnapshots();
    oUnitReset(manager);
}
void (__fastcall* oPublishShot)(void*, uint32_t, void*) = nullptr;
void __fastcall PublishShot(void* manager, uint32_t team, void* shot)
{
    const CallerScope caller(LIFE_CALLER());
    Record("shot_publish_begin", shot, team);
    oPublishShot(manager, team, shot);
    AcquireSRWLockExclusive(&g_lock);
    if (Identity* identity = Find(shot, false)) identity->shotLinked = true;
    ReleaseSRWLockExclusive(&g_lock);
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
    AcquireSRWLockExclusive(&g_lock);
    const auto* owner = Find(shot, false);
    const bool missedDeletingBoundary = owner && owner->snapshots && g_retentionOpen &&
        g_shotDomains.count(owner->id);
    ReleaseSRWLockExclusive(&g_lock);
    if (missedDeletingBoundary) std::abort(); // A base destructor cannot retain an already-torn-down derived object.
    const uint64_t id = Record("shot_base_begin", shot);
    Mutation(shot, 1);
    void* previous = g_shotBase;
    g_shotBase = shot;
    oShotDtor(shot);
    g_shotBase = previous;
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
    GenerationResourceMove transfer(shot, ShotDomain::ShotResource, true);
    Mutation(shot, 4);
    if (g_recording.load(std::memory_order_relaxed))
        Record("shot_resource_begin", shot, Read<uintptr_t>(shot, 0x1F68), uintptr_t(resource));
    oShotResource(shot, resource);
    Record("shot_resource_end", shot, uintptr_t(resource));
}
void (__fastcall* oReleaseCollision)(void*, uint32_t, uint8_t) = nullptr;
void __fastcall ReleaseCollision(void* shot, uint32_t slot, uint8_t full)
{
    UnitGuard fence;
    const CallerScope caller(LIFE_CALLER());
    Mutation(shot, 3);
    Record("collision_release_begin", shot, slot, full);
    if (g_recording.load(std::memory_order_relaxed))
        Record("collision_roots", shot, Read<uintptr_t>(shot, 0x2058),
               Read<uintptr_t>(shot, 0x2060), Read<uintptr_t>(shot, 0x2068), Read<uintptr_t>(shot, 0x2070));
    AcquireSRWLockExclusive(&g_lock);
    const Identity* tracked = Find(shot, false);
    const Identity owner = tracked ? *tracked : Identity{};
    ReleaseSRWLockExclusive(&g_lock);
    if (!tracked || !RetireAttack(owner, slot, full != 0)) oReleaseCollision(shot, slot, full);
    Record("collision_release_end", shot, slot, full);
}
void (__fastcall* oEffectResource)(void*, void*) = nullptr;
void (__fastcall* oBaseEffectResource)(void*, void*) = nullptr;
void __fastcall BaseEffectResource(void* effect, void* resource)
{
    AcquireSRWLockExclusive(&g_lock);
    Identity* owner = Find(effect, false);
    if (owner && owner->resourceReference >= 0) {
        // uEffect::setEffectList releases the graph before this base setter.
        // Suppress only its old-reference decrement, transferring that existing
        // reference to the retained graph. Native code still acquires the new one.
        if (owner->graph >= 0) std::abort();
        TransferOwnerResource(*owner);
        Write(effect, 0x108, (void*)nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
    oBaseEffectResource(effect, resource);
}

void __fastcall EffectResource(void* effect, void* resource)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(effect, 4);
    if (g_recording.load(std::memory_order_relaxed))
        Record("effect_resource_begin", effect, Read<uintptr_t>(effect, 0x108), uintptr_t(resource));
    oEffectResource(effect, resource);
    Record("effect_resource_end", effect, uintptr_t(resource));
}
void __fastcall ReleaseGraph(void* effect)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(effect, 2);
    AcquireSRWLockExclusive(&g_lock);
    Identity* owner = Find(effect, false);
    if (owner && owner->graph >= 0) {
        DetachGraph(*owner);
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (g_recording.load(std::memory_order_relaxed))
        Record("graph_release_begin", effect, Read<uintptr_t>(effect, 0x218),
               Read<uintptr_t>(effect, 0x220), Read<uintptr_t>(effect, 0x108), Read<uint32_t>(effect, 0x204));
    oReleaseGraph(effect);
    Record("graph_release_end", effect);
}
bool (__fastcall* oCreateGraph)(void*) = nullptr;
void (__fastcall* oRestartGraph)(void*) = nullptr;
void __fastcall RestartGraph(void* effect)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(effect, 6); // Restart can rebind parameters without replacing allocations.
    oRestartGraph(effect);
}

bool __fastcall CreateGraph(void* effect)
{
    const CallerScope caller(LIFE_CALLER());
    Mutation(effect, 5);
    AcquireSRWLockExclusive(&g_lock);
    if (Identity* owner = Find(effect, false); owner && owner->graph >= 0) DetachGraph(*owner);
    ReleaseSRWLockExclusive(&g_lock);
    if (g_recording.load(std::memory_order_relaxed)) {
        void* resource = Read<void*>(effect, 0x108);
        Record("graph_create_begin", effect, uintptr_t(resource),
               resource ? Read<uint32_t>(resource, 0x54) : 0,
               Read<uint32_t>(effect, 0x130), Read<uintptr_t>(effect, 0x218));
    }
    const bool result = oCreateGraph(effect);
    if (result) RegisterGeneratorLayouts(effect);
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
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    AcquireSRWLockExclusive(&g_lock);
    const auto* manifest = g_liveSnapshots[slot];
    DropShotGenerations(manifest, uint32_t(1) << slot);
    if (manifest && manifest->valid)
        Push("snapshot_drop", nullptr, nullptr, uint32_t(manifest->frame), slot, manifest->count, 0, false);
    g_liveSnapshots[slot] = nullptr;
    for (auto& identity : g_registry) {
        identity.snapshots &= ~(uint32_t(1) << slot);
        identity.dependencies &= ~(uint32_t(1) << slot);
    }
    for (auto& graph : g_generations) graph.snapshots &= ~(uint32_t(1) << slot);
    ReleaseSRWLockExclusive(&g_lock);
}

void LifetimeTrace_SaveSnapshot(int32_t frame, std::shared_ptr<LifetimeSnapshot>& storage)
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return;
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    UnitGuard unitGuard;
    // Allocation/capacity is reused by the owning snapshot/history bank. A
    // frame jump must not overwrite a capture still indexed by another lease.
    if (!storage || std::any_of(std::begin(g_liveSnapshots), std::end(g_liveSnapshots),
        [&](auto* saved) { return saved == storage.get() && saved != g_liveSnapshots[slot]; }))
        storage = std::make_shared<LifetimeSnapshot>();
    AcquireSRWLockExclusive(&g_lock);
    const auto* previous = g_liveSnapshots[slot];
    DropShotGenerations(previous, uint32_t(1) << slot);
    auto& manifest = *storage;
    // Also supports replacing an already-saved same-frame observation atomically.
    if (previous && previous->valid)
        Push("snapshot_drop", nullptr, nullptr, uint32_t(previous->frame), slot, previous->count, 0, false);
    g_liveSnapshots[slot] = storage.get();
    manifest.valid = false;
    manifest.frame = frame;
    manifest.birthWatermark = g_nextId;
    manifest.count = 0;
    manifest.members.clear();
    g_retentionOpen = true;
    for (auto& graph : g_generations) graph.snapshots &= ~(uint32_t(1) << slot);
    for (auto& identity : g_registry) {
        identity.snapshots &= ~(uint32_t(1) << slot);
        identity.dependencies &= ~(uint32_t(1) << slot);
        if (identity.occupancy != Occupancy::Live) continue;
        identity.snapshots |= uint32_t(1) << slot;
        manifest.members.push_back(identity);
        ++manifest.count;
    }
    CaptureEffects(manifest, unitGuard.manager);
    manifest.shotManager = Read<void*>((void*)_addr(0x140D47F40), 0);
    manifest.shotCursor = manifest.shotManager ? Read<uint32_t>(manifest.shotManager, 0x438) : 0;
    manifest.shotConfig = Read<void*>((void*)_addr(0x140D50F88), 0);
    manifest.shotSlotFlags = manifest.shotConfig ? Read<uint32_t>(manifest.shotConfig, 0x4A89C) : 0;
    const size_t shotCount = unitGuard.manager ? std::count_if(manifest.members.begin(), manifest.members.end(),
        [](const auto& saved) { return saved.kind == Kind::Shot; }) : 0;
    manifest.shots.resize(shotCount);
    ReleaseSRWLockExclusive(&g_lock);
    // Reuse the snapshot-owned storage and copy each native CPU span once.
    // Dynamic DTI dispatch, when needed, stays outside the recorder lock.
    size_t shotIndex = 0;
    for (const auto& identity : manifest.members) {
        if (!unitGuard.manager || identity.kind != Kind::Shot) continue;
        auto& saved = manifest.shots[shotIndex++];
        saved.id = identity.id;
        saved.spans.clear(); saved.payload.clear();
        saved.generations.clear();
        if (VisitShotMemory(identity,
            [](void* context, const char* name, const void* address, uint64_t ownerSlot,
               uint64_t vtable, const void* bytes, size_t size) {
                auto& saved = *static_cast<ShotState*>(context);
                if (size > UINT32_MAX || saved.payload.size() > UINT32_MAX - size) std::abort();
                saved.spans.push_back({name,
                    {const_cast<void*>(address), uint32_t(size), uint32_t(saved.payload.size())}, ownerSlot, vtable});
                const auto* data = static_cast<const uint8_t*>(bytes);
                saved.payload.insert(saved.payload.end(), data, data + size);
            }, &saved)) CaptureShotGenerations(identity, uint32_t(1) << slot, saved);
    }
    AcquireSRWLockExclusive(&g_lock);
    manifest.birthWatermark = g_nextId; // Include pre-existing owners adopted during capture.
    manifest.valid = true;
    Push("snapshot_manifest", nullptr, nullptr, uint32_t(frame), slot, manifest.count,
         g_registryMisses || g_untracked || g_reusedLive || g_dropped, false);
    ReleaseSRWLockExclusive(&g_lock);
}

LifetimeSnapshotCounts LifetimeTrace_VisitSnapshot(int32_t frame, const LifetimeSnapshot* snapshot,
    bool observeShots, LifetimeSnapshotVisitor visitor, void* context)
{
    LifetimeSnapshotCounts counts;
    if (!visitor || frame < 0) return counts;
    UnitGuard unitGuard;
    std::vector<Identity> shots;
    AcquireSRWLockExclusive(&g_lock);
    const bool available = snapshot && snapshot->valid && snapshot->frame == frame;
    const int64_t capture[] = {frame, available ? 1 : 0,
        observeShots ? int64_t(int32_t(g_context.load(std::memory_order_relaxed) >> 1)) : -1};
    visitor(context, "diagnostic.lifetime", nullptr, 0, 0, capture, sizeof(capture));
    std::vector<const EffectState*> effects;
    for (size_t i = 0; available && i < snapshot->effectCount; ++i) effects.push_back(&snapshot->effects[i]);
    std::sort(effects.begin(), effects.end(), [](auto a, auto b) { return a->id < b->id; });
    for (const auto* saved : effects) {
        char name[64];
        std::snprintf(name, sizeof(name), "lifetime.effect.%llu.root", (unsigned long long)saved->id);
        visitor(context, name, saved->object, 0, Read<uint64_t>(saved->root, 0),
                saved->root, sizeof(saved->root));
        for (size_t i = 0; i < saved->spanCount; ++i) {
            const auto& span = snapshot->spans[saved->spanBegin + i];
            std::snprintf(name, sizeof(name), "lifetime.effect.%llu.graph.%zu",
                          (unsigned long long)saved->id, i);
            visitor(context, name, span.address, 0, 0,
                    snapshot->payload.data() + span.offset, span.size);
        }
        ++counts.effects;
    }
    for (size_t i = 0; available && i < snapshot->count; ++i) {
        const auto& saved = snapshot->members[i];
        if (saved.kind != Kind::Shot) continue;
        char name[64];
        std::snprintf(name, sizeof(name), "lifetime.shot.%llu.identity", (unsigned long long)saved.id);
        const uint64_t metadata[] = {saved.id, saved.revision, uint64_t(saved.occupancy), saved.birthVtable};
        visitor(context, name, nullptr, 0, saved.birthVtable, metadata, sizeof(metadata));
        ++counts.shots;
    }
    if (available) for (const auto& shot : snapshot->shots) {
        for (const auto& span : shot.spans) {
            char name[64];
            if (span.element != UINT32_MAX)
                std::snprintf(name, sizeof(name), "lifetime.shot.%llu.%s.%u.%u",
                    (unsigned long long)shot.id, span.name, unsigned(span.group), span.element);
            else if (span.group != UINT16_MAX)
                std::snprintf(name, sizeof(name), "lifetime.shot.%llu.%s.%u",
                    (unsigned long long)shot.id, span.name, unsigned(span.group));
            else std::snprintf(name, sizeof(name), "lifetime.shot.%llu.%s",
                    (unsigned long long)shot.id, span.name);
            visitor(context, name, span.memory.address, span.ownerSlot, span.vtable,
                    shot.payload.data() + span.memory.offset, span.memory.size);
        }
    }
    if (observeShots)
        for (const auto& saved : g_registry)
            if (saved.kind == Kind::Shot && (saved.occupancy == Occupancy::Live ||
                saved.occupancy == Occupancy::Quarantined)) shots.push_back(saved);
    ReleaseSRWLockExclusive(&g_lock);

    // Native DTI virtual dispatch must be outside the recorder lock. The unit
    // fence keeps the observed owners intact during this dump-time copy.
    std::sort(shots.begin(), shots.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const auto& shot : shots) {
        const auto dti = shot.dti ? shot.dti : GetObjectDti(uint64_t(shot.object));
        const size_t size = shot.extent ? shot.extent : DecodeDtiSize(dti);
        const uint64_t metadata[] = {shot.id, shot.revision, uint64_t(shot.occupancy), dti, size};
        char name[64];
        std::snprintf(name, sizeof(name), "observed.shot.%llu.identity", (unsigned long long)shot.id);
        visitor(context, name, nullptr, 0, shot.birthVtable, metadata, sizeof(metadata));
        struct Observation {
            uint64_t id;
            LifetimeSnapshotVisitor visitor;
            void* context;
        } observation{shot.id, visitor, context};
        if (VisitShotMemory(shot,
            [](void* context, const char* suffix, const void* address, uint64_t ownerSlot,
               uint64_t vtable, const void* bytes, size_t size) {
                const auto& out = *static_cast<const Observation*>(context);
                char name[64];
                std::snprintf(name, sizeof(name), "observed.shot.%llu.%s",
                              (unsigned long long)out.id, suffix);
                out.visitor(out.context, name, address, ownerSlot, vtable, bytes, size);
            }, &observation)) ++counts.shotModels;
    }
    return counts;
}

void LifetimeTrace_CheckSnapshot(int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (!g_recording.load(std::memory_order_relaxed)) return;
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    AcquireSRWLockExclusive(&g_lock);
    const auto* snapshot = g_liveSnapshots[slot];
    if (!snapshot || !snapshot->valid || snapshot->frame != frame) {
        Push("snapshot_manifest_absent", nullptr, nullptr, uint32_t(frame), slot, 0, 0, false);
    } else {
        const auto& manifest = *snapshot;
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

void LifetimeTrace_RestoreEffects(int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED || !EMPTY_EFFECT_QUARANTINE_ENABLED ||
                  !EFFECT_RESTORE_ENABLED) return;
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return;
    UnitGuard unitGuard;
    AcquireSRWLockExclusive(&g_lock);
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    const auto* snapshot = g_liveSnapshots[slot];
    if (!snapshot) std::abort();
    const auto& manifest = *snapshot;
    // A published snapshot is a restore contract, not a best-effort batch.
    if (!manifest.valid || manifest.frame != frame || !manifest.unitManager ||
        unitGuard.manager != manifest.unitManager || manifest.thread != GetCurrentThreadId()) std::abort();
    const size_t count = manifest.effectCount;
    Identity* identities[QuarantineLimit]{};
    bool wasHeld[QuarantineLimit]{};
    void* releasedResources[QuarantineLimit]{};
    size_t held = 0;
    const uint64_t tag = ++g_restoreTag;
    const uint32_t categories = Read<uint32_t>(unitGuard.manager, 0xC38);
    if (categories > 64) std::abort();
    for (size_t i = 0; i < count; ++i) {
        const auto& saved = manifest.effects[i];
        Identity* current = Find(saved.object, false);
        if (!current || current->id != saved.id ||
            !(current->snapshots & (uint32_t(1) << slot)) ||
            (saved.category >= categories && saved.category != 0x7F) ||
            (current->occupancy != Occupancy::Live && current->occupancy != Occupancy::Quarantined)) std::abort();
        if (saved.graph >= 0) {
            // These are ownership assertions, not content/admission checks. A
            // referenced generation cannot expire or have its allocations reused.
            if (size_t(saved.graph) >= GenerationCapacity) std::abort();
            const auto& graph = g_generations[saved.graph];
            if (graph.id != saved.generation || graph.ownerId != saved.id ||
                !(graph.snapshots & (uint32_t(1) << slot)) ||
                graph.spanCount != saved.spanCount || saved.spanBegin > manifest.spanCount ||
                saved.spanCount > manifest.spanCount - saved.spanBegin) std::abort();
            for (size_t j = 0; j < saved.spanCount; ++j) {
                const auto& span = manifest.spans[saved.spanBegin + j];
                if (graph.spans[j].address != span.address || graph.spans[j].size != span.size ||
                    span.offset > manifest.payloadBytes || span.size > manifest.payloadBytes - span.offset)
                    std::abort();
            }
            if (current->graph < 0 && (Read<void*>(saved.object, 0x218) ||
                Read<void*>(saved.object, 0x220) || Read<void*>(saved.object, 0x228))) {
                // No generation owns this newer graph. Dispose it normally,
                // rather than allocating a generation merely to throw it away.
                ReleaseSRWLockExclusive(&g_lock);
                oReleaseGraph(saved.object);
                AcquireSRWLockExclusive(&g_lock);
            }
        }
        identities[i] = current;
        current->restoreTag = tag;
        wasHeld[i] = current->occupancy == Occupancy::Quarantined;
        held += wasHeld[i];
    }
    // Membership is state to restore, not a requirement that live masks match.
    // Keep native array storage, retain non-participating units, and restore the
    // captured members in their saved group order without registration callbacks.
    uint64_t affectedGroups = 0;
    for (size_t i = 0; i < count; ++i)
        affectedGroups |= manifest.effects[i].groups | Read<uint64_t>(manifest.effects[i].object, 0x30);
    const auto isCaptured = [&](void* object) {
        const Identity* identity = Find(object, false);
        return identity && identity->restoreTag == tag;
    };
    // Remove the abandoned branch without running kill callbacks against the
    // restored characters. Other snapshots may still own these exact objects.
    for (auto& identity : g_registry) {
        if (identity.occupancy != Occupancy::Live || identity.kind != Kind::Effect ||
            identity.restoreTag == tag ||
            (!identity.restoreOwned &&
             (identity.id <= manifest.birthWatermark || !OwnedEffectGraph(identity.object)))) continue;
        affectedGroups |= Read<uint64_t>(identity.object, 0x30);
        identity.occupancy = Occupancy::Quarantined;
        ++g_quarantined;
        Write(identity.object, 0x30, uint64_t(0));
        Write(identity.object, 0x10, (Read<uint32_t>(identity.object, 0x10) & ~0x3FFu) | 0x3F8u);
        Push("quarantine_hold", &identity, identity.object, identity.snapshots, g_quarantined,
             Read<uint16_t>(identity.object, 0x204), 1, false);
    }
    const auto omitted = [&](void* object) {
        const Identity* identity = Find(object, false);
        return identity && (identity->restoreTag == tag || identity->occupancy == Occupancy::Quarantined);
    };
    const auto reconcile = [&](const std::vector<void*>& saved, std::vector<void*>& entries) {
        entries.clear();
        g_membershipOrder = g_membershipLive;
        std::sort(g_membershipLive.begin(), g_membershipLive.end(), std::less<void*>{});
        for (void* unit : saved) {
            if (isCaptured(unit) || std::binary_search(g_membershipLive.begin(), g_membershipLive.end(),
                                                      unit, std::less<void*>{})) entries.push_back(unit);
        }
        // Only current foreign addresses are safe to use. Preserve new foreign
        // entries in their native order, following the restored saved cohort.
        g_membershipSaved = entries;
        std::sort(g_membershipSaved.begin(), g_membershipSaved.end(), std::less<void*>{});
        for (void* unit : g_membershipOrder)
            if (!std::binary_search(g_membershipSaved.begin(), g_membershipSaved.end(), unit,
                                    std::less<void*>{})) entries.push_back(unit);
    };
    for (uint32_t category = 0; category < categories; ++category) {
        auto& entries = g_primaryScratch[category];
        entries.clear();
        g_membershipLive.clear();
        for (void* unit = Read<void*>(unitGuard.manager, 0x50 + category * 0x30); unit;) {
            void* next = Read<void*>(unit, 0x18);
            if (!omitted(unit)) g_membershipLive.push_back(unit);
            else { Write(unit, 0x18, (void*)nullptr); Write(unit, 0x20, (void*)nullptr); }
            unit = next;
        }
        reconcile(manifest.primary[category], entries);
    }
    if (affectedGroups) {
        const uint32_t groups = Read<uint32_t>(unitGuard.manager, 0x1840);
        if (groups > 64 || (groups < 64 && (affectedGroups >> groups))) std::abort();
        // Native allocation preparation precedes commit, outside the recorder lock.
        for (uint32_t group = 0; group < groups; ++group) {
            if (!(affectedGroups & (uint64_t(1) << group))) continue;
            void* descriptor = static_cast<uint8_t*>(unitGuard.manager) + 0xC40 + group * 0x30;
            if (Read<uint8_t>(descriptor, 0x20)) std::abort(); // Native scheduler groups are non-owning.
            const uint32_t size = Read<uint32_t>(descriptor, 0x18);
            void* data = Read<void*>(descriptor, 0x28);
            auto& entries = g_groupScratch[group];
            g_membershipLive.clear();
            for (uint32_t j = 0; j < size; ++j) {
                void* unit = Read<void*>(data, size_t(j) * 8);
                if (!omitted(unit)) g_membershipLive.push_back(unit);
            }
            reconcile(manifest.groups[group], entries);
            if (entries.size() > Read<uint32_t>(descriptor, 0x1C)) {
                if (entries.size() > UINT32_MAX - 31) std::abort();
                using ExtendArray = void(__fastcall*)(void*, uint32_t);
                ReleaseSRWLockExclusive(&g_lock);
                ((ExtendArray)_addr(0x140011660))(static_cast<uint8_t*>(descriptor) + 0x10,
                    (uint32_t(entries.size()) + 31) & ~31u);
                AcquireSRWLockExclusive(&g_lock);
            }
        }
    }
    // No native callback, allocator or destructor is invoked in the commit phase.
    for (size_t i = 0; i < count; ++i) {
        const auto& saved = manifest.effects[i];
        if (saved.graph >= 0) {
            auto& graph = g_generations[saved.graph];
            if (identities[i]->resourceReference != graph.reference ||
                (graph.reference < 0 && Read<void*>(saved.object, 0x108) != graph.resource)) {
                if (identities[i]->resourceReference >= 0) TransferOwnerResource(*identities[i]);
                else releasedResources[i] = Read<void*>(saved.object, 0x108);
                if (graph.reference >= 0) {
                    auto& reference = g_resourceReferences[graph.reference];
                    if (reference.onOwner || reference.resource != graph.resource) std::abort();
                    reference.onOwner = true;
                }
                identities[i]->resourceReference = graph.reference;
                Push("graph_resource_transfer", identities[i], saved.object, graph.reference, 1, 0, 0, false);
            }
            if (identities[i]->graph != saved.graph) {
                if (identities[i]->graph >= 0) DetachGraph(*identities[i]);
                g_generations[saved.graph].attached = true;
                identities[i]->graph = saved.graph;
            }
            // The same payload commit runs for linked and quarantined identities.
            for (size_t j = 0; j < saved.spanCount; ++j) {
                const auto& span = manifest.spans[saved.spanBegin + j];
                std::memcpy(span.address, manifest.payload.data() + span.offset, span.size);
            }
            std::memcpy(static_cast<uint8_t*>(saved.object) + 0x40, saved.root + 0x40, 0x280);
        }
        std::memcpy(static_cast<uint8_t*>(saved.object) + 8, saved.control, sizeof(saved.control));
        Write(saved.object, 0x28, saved.deltaTime);
        Write(saved.object, 0x30, saved.groups);
        if (wasHeld[i]) {
            --g_quarantined; identities[i]->occupancy = Occupancy::Live;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& saved = manifest.effects[i];
        Push("retiring_state_restored", identities[i], saved.object, uint32_t(frame), wasHeld[i],
              saved.category, Read<uint32_t>(saved.object, 0x10) & 7, true);
        if (saved.graph >= 0) {
            size_t bytes = 0;
            for (size_t j = 0; j < saved.spanCount; ++j) {
                const auto& span = manifest.spans[saved.spanBegin + j];
                bytes += span.size;
            }
            Push("effect_payload_restored", identities[i], saved.object,
                 uint32_t(frame), bytes, wasHeld[i], 1, false);
        }
    }
    for (uint32_t category = 0; category < categories; ++category) {
        const auto& entries = g_primaryScratch[category];
        void* line = static_cast<uint8_t*>(unitGuard.manager) + 0x50 + category * 0x30;
        Write(line, 0, entries.empty() ? nullptr : entries.front());
        Write(line, 8, entries.empty() ? nullptr : entries.back());
        for (size_t i = 0; i < entries.size(); ++i) {
            Write(entries[i], 0x20, i ? entries[i - 1] : nullptr);
            Write(entries[i], 0x18, i + 1 < entries.size() ? entries[i + 1] : nullptr);
        }
    }
    for (uint32_t group = 0; group < 64; ++group) {
        if (!(affectedGroups & (uint64_t(1) << group))) continue;
        void* descriptor = static_cast<uint8_t*>(unitGuard.manager) + 0xC40 + group * 0x30;
        const auto& entries = g_groupScratch[group];
        if (!entries.empty()) std::memcpy(Read<void*>(descriptor, 0x28), entries.data(), entries.size() * sizeof(void*));
        Write(descriptor, 0x18, uint32_t(entries.size()));
    }
    Push("retiring_restore_applied", nullptr, nullptr, uint32_t(frame), count, held, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < count; ++i)
        if (releasedResources[i]) releaseResourceRef(releasedResources[i]);
}

void LifetimeTrace_ClearSnapshots()
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    AcquireSRWLockExclusive(&g_lock);
    g_retentionOpen = false;
    for (const auto* saved : g_liveSnapshots) DropShotGenerations(saved, UINT32_MAX);
    std::fill(std::begin(g_liveSnapshots), std::end(g_liveSnapshots), nullptr);
    for (auto& identity : g_registry) identity.snapshots = identity.dependencies = 0;
    for (auto& graph : g_generations) graph.snapshots = 0;
    Push("snapshot_manifests_clear", nullptr, nullptr, 0, 0, 0, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
    CollectGraphGenerations();
    CollectShotGenerations();
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
    const bool complete = !g_lifetimeEnabled.load(std::memory_order_relaxed) &&
        g_quarantined == 0 && g_count == 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (!complete) g_finishRequested.store(true, std::memory_order_release);
    return complete;
}

void LifetimeTrace_WritePending(FILE* file)
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (!file) return;
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        std::fprintf(file, "lifetime pending: trace lock busy\n");
        return;
    }
    Event events[64];
    const size_t pending = g_count;
    const size_t count = std::min(pending, std::size(events));
    for (size_t i = 0; i < count; ++i)
        events[i] = g_events[(g_read + pending - count + i) % EventCapacity];
    ReleaseSRWLockExclusive(&g_lock);
    std::fprintf(file, "lifetime pending: total=%zu shown=%zu\n", pending, count);
    const auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    for (size_t i = 0; i < count; ++i) {
        const Event& e = events[i];
        std::fprintf(file, "LIFE_PENDING seq=%llu frame=%d resim=%u event=%s id=%llu kind=%s "
                     "obj=0x%llX birthvt=0x%llX flags=0x%X caller=0x%llX snapshots=0x%X "
                     "a=0x%llX b=0x%llX c=0x%llX d=0x%llX\n",
                     u(e.sequence), int32_t(e.context >> 1), unsigned(e.context & 1), e.name, u(e.id),
                     e.id ? (e.kind == Kind::Shot ? "shot" : (e.kind == Kind::Effect ? "effect" : "unit")) : "none",
                     u(e.object), u(e.birthVtable), e.flags, u(e.caller), e.snapshots,
                     u(e.a), u(e.b), u(e.c), u(e.d));
    }
}

void LifetimeTrace_Flush()
{
    if constexpr (!LIFETIME_TRACE_ENABLED) return;
    if (g_finishRequested.exchange(false, std::memory_order_acq_rel)) {
        LifetimeTrace_ClearSnapshots();
        LifetimeTrace_Marker("capture_finish", int32_t(g_context.load(std::memory_order_relaxed) >> 1));
        g_recording.store(false, std::memory_order_relaxed);
        g_lifetimeEnabled.store(false, std::memory_order_relaxed);
    }
    // Trace exhaustion must not revoke snapshot ownership. Only slot expiry or
    // explicit history/session close can release a generation's leases.
    if constexpr (EMPTY_EFFECT_QUARANTINE_ENABLED) {
        CollectGraphGenerations();
        CollectShotGenerations();
        CollectQuarantine();
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
                e.name, e.id, e.id ? (e.kind == Kind::Shot ? "shot" : (e.kind == Kind::Effect ? "effect" : "unit")) : "none",
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
        {0x1405F0980, (void*)&BaseEffectResource, (void**)&oBaseEffectResource},
        {0x1405E6890, (void*)&ReleaseGraph, (void**)&oReleaseGraph},
        {0x1405E27C0, (void*)&CreateGraph, (void**)&oCreateGraph},
        {0x1405E4DB0, (void*)&RestartGraph, (void**)&oRestartGraph},
        {0x140259990, (void*)&MainReset, (void**)&oMainReset},
        {0x140568670, (void*)&GenerationModel, (void**)&oGenerationModel},
        {0x1405291E0, (void*)&GenerationBlend, (void**)&oGenerationBlend},
        {0x140561CE0, (void*)&GenerationBaseModel, (void**)&oGenerationBaseModel},
        {0x14050D5A0, (void*)&GenerationResourceRelease, (void**)&oGenerationResourceRelease},
        {0x140011660, (void*)&GenerationExtendArray, (void**)&oGenerationExtendArray},
        {0x1405A4420, (void*)&GenerationAddChild, (void**)&oGenerationAddChild},
    };
    bool complete = true;
    if constexpr (EMPTY_EFFECT_QUARANTINE_ENABLED) {
        const struct { uintptr_t address; void* detour; void** original; } retentionHooks[] = {
            {0x140139E80, (void*)&DeleteEffect, (void**)&oDeleteEffect},
            {0x140202DE0, (void*)&ActionFinal, (void**)&oActionFinal},
            {0x140257D80, (void*)&MainDtor, (void**)&oMainDtor},
            {0x14025A070, (void*)&ManagerDtor, (void**)&oManagerDtor},
            {0x140151420, (void*)&UnlinkShot, (void**)&oUnlinkShot},
            {0x140151490, (void*)&ReleaseShotId, (void**)&oReleaseShotId},
            {0x14051BBB0, (void*)&UnitReset, (void**)&oUnitReset},
            {0x14051A6B0, (void*)&AddUnit, (void**)&oAddUnit},
            {0x1400604E0, (void*)&EffectOwner, (void**)&oEffectOwner},
            {0x1401091C0, (void*)&ConstructConstraint, (void**)&oConstructConstraint},
            {0x140109240, (void*)&DestroyConstraint, (void**)&oDestroyConstraint},
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
                                  uintptr_t(0x140257D80), uintptr_t(0x14025A070),
                                  uintptr_t(0x140151420), uintptr_t(0x140151490), uintptr_t(0x14051BBB0),
                                  uintptr_t(0x14051A6B0), uintptr_t(0x1400604E0),
                                  uintptr_t(0x1401091C0), uintptr_t(0x140109240)})
            complete &= GameplayHooks_QueueEnable((void*)_addr(address)) == MH_OK;
    }
    for (const auto& hook : hooks)
        complete &= GameplayHooks_QueueEnable((void*)_addr(hook.address)) == MH_OK;
    releaseResourceRef = (void (__fastcall*)(void*))_addr(0x14050D5A0);
    collectModelGeneration = (void (__fastcall*)(void*))_addr(0x1401FF6B0);
    logicallyRetireJoints = (Deleting)_addr(0x140564900);
    oRemoveGroups = (void (__fastcall*)(void*, void*))_addr(0x14051BAF0);
    g_lifetimeEnabled.store(complete, std::memory_order_relaxed);
    g_recording.store(complete, std::memory_order_relaxed);
    HookLog("LifetimeTrace: queued=%d image_base=0x%llX limit=%llu ring=%zu registry=%zu empty_effect_quarantine=%d quarantine_limit=%zu effect_restore=%d ordinary_cpu_graphs=%d simulation_snapshots=%d shot_model_retention=%d future_shot_withdrawal=%d; partial graph coverage\n",
            complete, uint64_t(_addr(0x140000000)), CaptureLimit, EventCapacity, RegistrySize,
            EMPTY_EFFECT_QUARANTINE_ENABLED, QuarantineLimit, EFFECT_RESTORE_ENABLED,
            ORDINARY_CPU_EFFECT_GRAPHS_ENABLED, EFFECT_SIMULATION_SNAPSHOTS_ENABLED,
            EMPTY_EFFECT_QUARANTINE_ENABLED, EMPTY_EFFECT_QUARANTINE_ENABLED && EFFECT_RESTORE_ENABLED);
}
