"""Compile the production lifetime tracer against native-call and Windows mocks."""

from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

MOCKS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>
#include <span>
#include <csignal>
#include <type_traits>
#define __fastcall
using SRWLOCK = std::mutex;
#define SRWLOCK_INIT {}
thread_local bool locked = false;
thread_local bool unitLocked = false;
thread_local unsigned unitLockDepth = 0;
void* unitManager = nullptr;
void* shotManager = nullptr;
void* collisionManager = nullptr;
void* gameConfig = nullptr;
void* renderManager = nullptr;
void* actionManager = nullptr;
int renderWaits = 0;
void MockRenderSync(void* render) {
    assert(!locked && !unitLocked);
    auto* bytes = static_cast<uint8_t*>(render);
    assert(bytes[0x38] && bytes[0x3E]); ++renderWaits; bytes[0x38] = 0;
}
void MockRecycleCollision(void*);
void MockClearCollisionGroups(void*);
void MockResetCollisionGroup(void*);
bool MockUnlinkConstraint(void*);
alignas(16) uint8_t shotHeads[10][0x28]{}, shotDti[0x40]{};
using LPCRITICAL_SECTION = void*;
void EnterCriticalSection(LPCRITICAL_SECTION) { assert(!locked); ++unitLockDepth; unitLocked = true; }
void LeaveCriticalSection(LPCRITICAL_SECTION) {
    assert(!locked && unitLockDepth); unitLocked = --unitLockDepth != 0;
}
void AcquireSRWLockExclusive(SRWLOCK* lock) { assert(!locked); lock->lock(); locked = true; }
void ReleaseSRWLockExclusive(SRWLOCK* lock) { assert(locked); locked = false; lock->unlock(); }
bool TryAcquireSRWLockExclusive(SRWLOCK* lock) {
    if (locked || !lock->try_lock()) return false;
    locked = true; return true;
}
// The process's first thread is 7, as before; any other test thread is 8.
const std::thread::id g_mainThreadId = std::this_thread::get_id();
uint32_t GetCurrentThreadId() { return std::this_thread::get_id() == g_mainThreadId ? 7 : 8; }
uint32_t GetCurrentProcessId() { return 123; }
uint64_t GetTickCount64() { return 100; }
uint64_t testDti = 0;
uint64_t GetObjectDti(uint64_t) { assert(!locked); return testDti; }
uint32_t DecodeDtiSize(uint64_t dti) {
    if (!dti) return 0;
    uint32_t encoded; std::memcpy(&encoded, (void*)(dti + 0x30), 4);
    return (encoded & 0x7FFFFF) << 2;
}
int logs = 0, creates = 0, queues = 0, failCreate = -1, failQueue = -1;
int applies = 0;
bool failApply = false;
std::vector<std::pair<void*, void*>> mockHooks;
std::vector<void*> mockQueuedTargets;
int resourceAdds = 0, resourceReleases = 0;
int groupExpansions = 0;
void ExtendGroup(void* array, uint32_t capacity) {
    assert(!locked && unitLocked);
    auto* bytes = static_cast<uint8_t*>(array);
    uint32_t count; void* old;
    std::memcpy(&count, bytes + 8, 4); std::memcpy(&old, bytes + 0x18, 8);
    assert(capacity >= count);
    void* data = std::calloc(capacity, sizeof(void*));
    if (count) std::memcpy(data, old, size_t(count) * sizeof(void*));
    std::free(old);
    std::memcpy(bytes + 0x18, &data, 8); std::memcpy(bytes + 0xC, &capacity, 4);
    ++groupExpansions;
}
void ResourceAdd(void* p) {
    assert(!locked);
    uint32_t refs; std::memcpy(&refs, static_cast<uint8_t*>(p) + 0x4C, 4);
    assert(refs); ++refs; ++resourceAdds;
    std::memcpy(static_cast<uint8_t*>(p) + 0x4C, &refs, 4);
}
void ResourceRelease(void* p) {
    assert(!locked);
    uint32_t refs; std::memcpy(&refs, static_cast<uint8_t*>(p) + 0x4C, 4);
    assert(refs); --refs; ++resourceReleases;
    std::memcpy(static_cast<uint8_t*>(p) + 0x4C, &refs, 4);
}
int ownedFrees = 0;
void MockOwnedFree(void*, void* p) { assert(!locked); ++ownedFrees; std::free(p); }
void* MockOwnedAllocate(void*, size_t size, uint32_t) { assert(!locked); return std::malloc(size); }
void MockCollectModel(void*);
void* MockOwnedAllocator(void*) {
    static uintptr_t vt[13]{};
    static uintptr_t allocator[1]{};
    vt[8] = uintptr_t(&MockOwnedAllocate); vt[12] = uintptr_t(&MockOwnedFree); allocator[0] = uintptr_t(vt);
    return allocator;
}
void* MockJointDelete(void* p, uint32_t flags) {
    assert(!locked && (flags == 2 || flags == 3));
    auto* base = static_cast<uint8_t*>(p) - 0x10;
    uint32_t count; std::memcpy(&count, base, 4);
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t vtable = 0xDEAD, owner = 0;
        std::memcpy(static_cast<uint8_t*>(p) + i*0xC0, &vtable, 8);
        std::memcpy(static_cast<uint8_t*>(p) + i*0xC0 + 0xA8, &owner, 8);
    }
    if (flags & 1) std::free(base);
    return p;
}
uintptr_t mockJointVtable[] = {uintptr_t(&MockJointDelete)};
void HookLog(const char*, ...) { assert(!locked); ++logs; }
int fatalLogAttempts = 0;
void HookLogTryWriteFatal(const char*) { ++fatalLogAttempts; }
constexpr int MH_OK = 0;
int GameplayHooks_Create(void* target, void* detour, void** original) {
    assert(!locked);
    if (creates++ == failCreate) return 9;
    if (original) *original = target;
    mockHooks.emplace_back(target, detour);
    return MH_OK;
}
int GameplayHooks_QueueEnable(void* target) {
    const int index = queues++;
    mockQueuedTargets.push_back(target);
    bool created = false;
    for (const auto& hook : mockHooks) created |= hook.first == target;
    if (!created) return 4; // MH_ERROR_NOT_CREATED, matching real MinHook.
    return index == failQueue ? 9 : MH_OK;
}
bool GameplayHooks_Apply() { assert(!locked); ++applies; return !failApply; }
uintptr_t _addr(uintptr_t address) {
    if (address == 0x140E17698) return uintptr_t(&unitManager);
    if (address == 0x140D47F40) return uintptr_t(&shotManager);
    if (address == 0x140E18250) return uintptr_t(&collisionManager);
    if (address == 0x140D50F88) return uintptr_t(&gameConfig);
    if (address == 0x140E179A8) return uintptr_t(&renderManager);
    if (address == 0x140D47E68) return uintptr_t(&actionManager);
    if (address == 0x140541030) return uintptr_t(&MockRenderSync);
    if (address == 0x1405B7F20) return uintptr_t(&MockRecycleCollision);
    if (address == 0x1405B5B60) return uintptr_t(&MockClearCollisionGroups);
    if (address == 0x1405A45C0) return uintptr_t(&MockResetCollisionGroup);
    if (address == 0x1405EC190) return uintptr_t(&MockUnlinkConstraint);
    if (address == 0x140D47F90) return uintptr_t(shotHeads);
    if (address == 0x140D4D960) return uintptr_t(shotDti);
    if (address == 0x14050D100) return uintptr_t(&ResourceAdd);
    if (address == 0x14050D5A0) return uintptr_t(&ResourceRelease);
    if (address == 0x140011660) return uintptr_t(&ExtendGroup);
    if (address == 0x14045D4F0) return uintptr_t(&MockOwnedAllocator);
    return address;
}
'''

CHECKS = r'''
void reset() {
    g_constructedConstraints.clear();
    for (auto& bucket : g_constraintBuckets) bucket = 0;
    g_attackOwnerCount = 0; g_armedResourceTransfers = 0;
    g_nextConstraintConstruction = 0;
    g_nextSnapshotCapture = 0;
    g_clearedSlots = 0;
    g_shotGenerations.clear(); g_ownerGenerations.clear(); g_shotDomains.clear(); g_attackOwners.clear(); g_keyframeExtents.clear(); g_nextShotGeneration = 0;
    g_collectingShotGeneration = false; g_transferredShotResource = nullptr; ownedFrees = 0;
    collectModelGeneration = MockCollectModel;
    logicallyRetireJoints = MockJointDelete;
    for (auto& graph : g_generations) graph = {};
    for (auto& reference : g_resourceReferences) reference = {};
    g_nextGeneration = 0;
    g_generatorLayoutCount = 1;
    g_generatorLayouts[0] = {_addr(0x140BC4CB0), 0x240, true};
    testDti = 0;
    g_generationHighWater = 0;
    releaseResourceRef = ResourceRelease;
    resourceAdds = resourceReleases = 0;
    groupExpansions = 0;
    for (auto& entry : g_registry) entry = {};
    std::fill(std::begin(g_activeSlots), std::end(g_activeSlots), uint64_t(0));
    for (auto& storage : testSnapshots) storage.reset();
    std::fill(std::begin(g_liveSnapshots), std::end(g_liveSnapshots), nullptr);
    unitManager = nullptr;
    g_read = g_count = 0;
    g_nextId = g_sequence = g_epoch = 0;
    g_dropped = g_registryMisses = g_untracked = g_reusedLive = 0;
    g_unknownSamples = 0;
    g_graphSamples = 0;
    g_caller = 0;
    g_reportedDropped = g_reportedMisses = g_reportedUntracked = g_reportedReused = 0;
    g_reportedStopped = false;
    g_recording = true;
    g_lifetimeEnabled = true;
    g_finishRequested = false;
    g_quarantined = 0;
    g_reportedQuarantined = 0;
    g_retentionOpen = false;
    g_unitDeleteHookCount = 0;
    for (auto& hook : g_unitDeleteHooks) hook = {};
    g_collectingShot = g_shotBase = nullptr;
    shotManager = nullptr;
    collisionManager = nullptr;
    gameConfig = nullptr;
    renderManager = nullptr; renderWaits = 0; actionManager = nullptr;
    std::memset(shotHeads, 0, sizeof(shotHeads)); std::memset(shotDti, 0, sizeof(shotDti));
    unitLockDepth = 0; unitLocked = false;
    creates = queues = logs = fatalLogAttempts = 0;
    applies = 0; failApply = false; mockHooks.clear(); mockQueuedTargets.clear();
    failCreate = failQueue = -1;
}
Event& last() { return g_events[(g_read + g_count - 1) % EventCapacity]; }
alignas(16) uint8_t object[0x2200]{};
int originalCalls = 0;
void checkOriginal(void* p) { assert(!locked && p == object); ++originalCalls; }

int main() {
    reset();
    LifetimeTrace_Install();
    assert(creates == 34 && queues == 34 && g_recording);
    assert(g_lifetimeEnabled && mockHooks.size() == mockQueuedTargets.size());
    for (const auto& hook : mockHooks)
        assert(std::count(mockQueuedTargets.begin(), mockQueuedTargets.end(), hook.first) == 1);
    assert(std::count(mockQueuedTargets.begin(), mockQueuedTargets.end(), (void*)_addr(0x1405EC000)) == 1);
    assert(std::count(mockQueuedTargets.begin(), mockQueuedTargets.end(), (void*)_addr(0x1409BFE90)) == 1);
    reset(); failCreate = 3; g_recording = false;
    LifetimeTrace_Install();
    assert(creates == 34 && queues == 0 && !g_recording);
    reset(); failQueue = 3;
    LifetimeTrace_Install();
    assert(!g_recording);
    reset();

    uintptr_t shotVtable[] = {uintptr_t(+[](void* p, uint32_t) -> void* { return p; })};
    uintptr_t vtable = uintptr_t(shotVtable);
    std::memcpy(object, &vtable, sizeof(vtable));
    LifetimeTrace_SetFrame(-5, true);
    oCreateShot = [](void* resource) -> void* {
        assert(!locked && resource == (void*)99); ++originalCalls; return object;
    };
    assert(CreateShot((void*)99) == object && originalCalls == 1);
    assert(last().caller != 0 && g_caller == 0 && !last().untracked);
    assert(last().id == 1 && last().birthVtable == vtable);
    assert(int32_t(last().context >> 1) == -5 && (last().context & 1));
    uint64_t id = last().id;
    oPublishShot = [](void* manager, uint32_t team, void* shot) {
        assert(!locked && manager == (void*)88 && team == 9 && shot == object); ++originalCalls;
    };
    PublishShot((void*)88, 9, object);
    assert(last().id == id && last().a == 9 && g_nextId == 1);

    oKill = [](void* p) { checkOriginal(p); p = static_cast<uint8_t*>(p) + 0x10; *(uint32_t*)p = 3; };
    Kill(object);
    assert(last().flags == 3 && !std::strcmp(last().name, "kill_end"));
    oRemoveUnit = [](void* manager, void* p) -> bool {
        assert(manager == (void*)88); checkOriginal(p); return false;
    };
    assert(!RemoveUnit((void*)88, object) && last().a == 0);
    oRemoveUnit = [](void*, void* p) -> bool { checkOriginal(p); return true; };
    assert(RemoveUnit(nullptr, object) && last().a == 1);

    oShotResource = [](void* p, void* r) { checkOriginal(p); assert(r == (void*)55); };
    ShotResource(object, (void*)55);
    assert(last().a == 55);
    oReleaseCollision = [](void* p, uint32_t slot, uint8_t full) {
        checkOriginal(p); assert(slot == UINT32_MAX && full == 1);
    };
    ReleaseCollision(object, UINT32_MAX, 1);
    assert(last().a == UINT32_MAX && last().b == 1);

    oShotDtor = [](void* p) {
        checkOriginal(p);
        const uintptr_t outerCaller = g_caller;
        assert(outerCaller);
        // Nested native teardown must still resolve this identity, without holding the lock.
        ReleaseCollision(p, UINT32_MAX, 1);
        assert(g_caller == outerCaller);
    };
    ShotDtor(object);
    assert(!Find(object, false) && last().id == id && last().vtable == 0);
    Birth(object, Kind::Effect);
    assert(last().id > id && last().kind == Kind::Effect && g_reusedLive == 0);
    const uint64_t effectId = last().id;
    Destroyed("old_completion", object, id);
    assert(Find(object, false)->id == effectId); // stale completion cannot erase new incarnation
    oEffectResource = [](void* p, void* r) { checkOriginal(p); assert(r == (void*)66); };
    EffectResource(object, (void*)66);
    oReleaseGraph = [](void* p) { checkOriginal(p); };
    ReleaseGraph(object);
    oCreateGraph = [](void* p) -> bool { checkOriginal(p); return false; };
    assert(!CreateGraph(object) && last().a == 0);
    alignas(16) uint8_t resource[0x60]{};
    void* resourcePointer = resource;
    const uint32_t resourceState = 0x801, effectStatus = 0x1234;
    std::memcpy(resource + 0x54, &resourceState, 4);
    std::memcpy(object + 0x108, &resourcePointer, sizeof(resourcePointer));
    std::memcpy(object + 0x130, &effectStatus, 4);
    const size_t beginIndex = (g_read + g_count) % EventCapacity;
    oCreateGraph = [](void* p) -> bool { checkOriginal(p); return true; };
    assert(CreateGraph(object) && last().a == 1);
    assert(g_events[beginIndex].a == uintptr_t(resource));
    assert(g_events[beginIndex].b == resourceState && g_events[beginIndex].c == effectStatus);
    oEffectDtor = [](void* p) { checkOriginal(p); ReleaseGraph(p); };
    EffectDtor(object);
    assert(!Find(object, false));
    oConstructEffect = [](void* p) -> void* {
        assert(!locked && p == object);
        const uintptr_t constructedVtable = 0x140A8E180;
        std::memcpy(p, &constructedVtable, sizeof(constructedVtable));
        return p;
    };
    const uint64_t beforeDirect = g_nextId;
    assert(ConstructEffect(object) == object && last().kind == Kind::Effect);
    assert(last().birthVtable == 0x140A8E180 && g_nextId == beforeDirect + 1);
    EffectDtor(object);
    // The unhooked factory delegates to the constructor: one identity, not two.
    auto factory = []() -> void* { return ConstructEffect(object); };
    const uint64_t beforeFactory = g_nextId;
    assert(factory() == object && g_nextId == beforeFactory + 1 && g_reusedLive == 0);
    EffectDtor(object);
    std::memcpy(object, &vtable, sizeof(vtable));

    // ASan checks that trace completion never dereferences freed object storage.
    auto* dying = std::calloc(1, 0x2200);
    Birth(dying, Kind::Shot);
    oShotDtor = [](void* p) { assert(!locked); std::free(p); };
    ShotDtor(dying);
    assert(last().vtable == 0 && !std::strcmp(last().name, "shot_base_end"));
    LifetimeTrace_Marker("snapshot_request", 123);
    assert(last().id == 0 && last().a == 123);
    oMainReset = [](void* p) { checkOriginal(p); };
    MainReset(object);
    assert(g_epoch == 1 && !std::strcmp(last().name, "main_reset_end"));

    reset();
    // Unknown typed boundaries retain evidence, not a fabricated allocation identity.
    alignas(16) uint8_t uiEffect[32]{};
    const uintptr_t uiVtable = _addr(0x140B213D0);
    std::memcpy(uiEffect, &uiVtable, sizeof(uiVtable));
    assert(Record("effect_resource_begin", uiEffect) == 0);
    assert(g_count == 0 && g_untracked == 0 && g_unknownSamples == 0);
    {
        CallerScope outer(0x140123456);
        {
            CallerScope inner(0x140ABCDEF);
            assert(Record("unknown_graph", object, 11, 22, 33, 44) == 0);
            assert(last().caller == 0x140ABCDEF && last().untracked && last().id == 0);
            assert(last().vtable == vtable && last().a == 11 && last().d == 44);
        }
        assert(g_caller == 0x140123456);
    }
    assert(g_caller == 0 && !Find(object, false) && g_nextId == 0);
    for (size_t i = 1; i < UnknownSampleLimit + 10; ++i) Record("unknown_graph", object);
    assert(g_untracked == UnknownSampleLimit + 10 && g_unknownSamples == UnknownSampleLimit);
    assert(g_count == UnknownSampleLimit);
    // Shared cUnit paths still neither sample nor dereference an unknown object.
    Record("shared", (void*)1, 0, 0, 0, 0, false);
    assert(g_count == UnknownSampleLimit && g_untracked == UnknownSampleLimit + 10);

    reset();
    // Fixed probe overflow cannot silently evict a live identity.
    for (size_t i = 0; i < ProbeLimit; ++i) {
        void* p = (void*)(0x1000 + i * RegistrySize * 16);
        Identity* entry = Find(p, true);
        assert(entry);
        *entry = {p, i + 1, 0, Kind::Shot, Occupancy::Live};
    }
    assert(!Find((void*)(0x1000 + ProbeLimit * RegistrySize * 16), true));
    assert(Find((void*)0x1000, false)->id == 1);

    reset();
    for (size_t i = 0; i < EventCapacity + 3; ++i) LifetimeTrace_Marker("fill", int(i));
    assert(g_count == EventCapacity && g_dropped == 3);
    LifetimeTrace_Flush();
    assert(g_count == EventCapacity - 256 && logs == 257);
    g_sequence = CaptureLimit - 1;
    LifetimeTrace_Marker("last", 0);
    assert(!g_recording && g_sequence == CaptureLimit);
    LifetimeTrace_Marker("ignored", 0);
    assert(g_sequence == CaptureLimit);
    const int before = originalCalls;
    Kill(object); // Original still runs after capture stops.
    assert(originalCalls == before + 1);

    reset();
    auto worker = [] {
        alignas(16) uint8_t local[32]{};
        for (int i = 0; i < 100; ++i) {
            Birth(local, Kind::Effect);
            uint64_t generation = Record("worker", local);
            assert(generation);
            Destroyed("worker_end", local, generation);
        }
    };
    std::thread a(worker), b(worker);
    a.join(); b.join();
    assert(g_nextId == 200 && g_sequence == 600 && g_count == 600);
    assert(!g_dropped && !g_registryMisses && !g_reusedLive);
}
'''


SHOT_PAYLOAD_FIXTURE = r'''
#include <string>
alignas(16) uint8_t units[0x1860]{}, shots[0x3450]{};
uintptr_t rootVT[] = {0};
struct PayloadFixture { uint8_t* shot; void* other; void* root; uint64_t id; };
// A shot with a borrowed unit, an attack root and an inline constructed constraint.
PayloadFixture makePayloadFixture() {
    reset(); unitManager = units; shotManager = shots;
    Write(shots, 0x438, uint32_t(5));
    auto* shot = static_cast<uint8_t*>(std::calloc(1, 0x2200));
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    void* other = std::calloc(1, 0x40);
    Birth(other, Kind::Unit);
    Write(shot, 0x2000, other);
    void* root = std::calloc(1, 0xE0); Write(root, 0, uintptr_t(rootVT));
    Write(shot, 0x2058, root);
    void* constraint = shot + 0x1000;
    RegisterConstraint(constraint);
    Write(constraint, 0x48, constraint);
    Birth(shot, Kind::Shot);
    auto* identity = Find(shot, false);
    identity->extent = 0x2200; identity->dti = 0x1234;
    return {shot, other, root, identity->id};
}
void releasePayloadFixture(const PayloadFixture& fixture) {
    LifetimeTrace_ClearSnapshots();
    reset(); // Drops C++ metadata only; no native callback reaches the storage below.
    std::free(fixture.shot); std::free(fixture.other); std::free(fixture.root);
}
'''

class LifetimeTraceTests(unittest.TestCase):
    def compile_run(self, enabled, checks, quarantine=True, expected_returncode=0, arguments=None,
                    sanitizers="address,undefined"):
        header = (ROOT / "UMVC3Hook/code/lifetime_trace.h").read_text()
        header = header.replace("#pragma once", "").replace(
            "LIFETIME_TRACE_ENABLED = true", f"LIFETIME_TRACE_ENABLED = {str(enabled).lower()}")
        header = header.replace("EMPTY_EFFECT_QUARANTINE_ENABLED = true",
                                f"EMPTY_EFFECT_QUARANTINE_ENABLED = {str(quarantine).lower()}")
        source = (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text()
        original_source = source
        for name in ("lifetime_failure.inl", "lifetime_shot_generations.inl", "lifetime_shot_cpu.inl",
                     "lifetime_shot_constraints.inl", "lifetime_shot_world.inl", "lifetime_shot_restore.inl",
                     "lifetime_shot_models.inl"):
            include = f'#include "{name}"'
            next_line = original_source[:original_source.index(include)].count("\n") + 2
            contents = (ROOT / "UMVC3Hook/code" / name).read_text()
            source = source.replace(include, f'#line 1 "UMVC3Hook/code/{name}"\n{contents}\n'
                                    f'#line {next_line} "UMVC3Hook/code/lifetime_trace.cpp"')
        source = "\n".join(line for line in source.splitlines() if not line.startswith('#include "'))
        rollback_header = (ROOT / "UMVC3Hook/code/rollback.h").read_text().replace("#pragma once", "")
        character_header = (ROOT / "UMVC3Hook/code/rollback_character_restore.h").read_text().replace("#pragma once", "")
        bridge = r'''
std::span<const uint32_t> Rollback_CharacterShotTailOffsets(uint64_t) { return {}; }
int collisionRecycles = 0, collisionGroupClears = 0, constraintUnlinks = 0;
void MockRecycleCollision(void* collider) {
    assert(!locked && unitLocked); ++collisionRecycles;
    Write(collider, 0x28, uint32_t(0));
    VisitCollisionLists(collider, [](void* list) {
        for (size_t bank = 0; bank < 41; ++bank) {
            Write(list, 0x20 + bank*0x20, uint32_t(0));
            Write(list, 0x540 + bank*0x20, uint32_t(0));
        }
    });
}
void MockClearCollisionGroups(void* list) {
    assert(!locked && unitLocked); ++collisionGroupClears;
    Write(list, 0xF80, uint32_t(0));
}
void MockResetCollisionGroup(void* group) {
    assert(!locked && unitLocked); Write(group, 0x20, uint32_t(0));
}
bool MockUnlinkConstraint(void* object) {
    assert(!locked && unitLocked); ++constraintUnlinks;
    Write(object, 0x50, uintptr_t(0)); Write(object, 0x58, uintptr_t(0));
    Write(object, 0x60, uintptr_t(0)); Write(object, 0x70, UINT32_MAX);
    return true;
}
void MockCollectModel(void* owner) {
    assert(!locked && unitLocked);
    for (size_t field : {size_t(0x100), size_t(0x108), size_t(0x110), size_t(0x170),
                         size_t(0x1250), size_t(0x1258), size_t(0x1408)})
        assert(!Read<void*>(owner, field));
    if (void* joint = Read<void*>(owner, 0x538)) MockJointDelete(joint, 3);
    for (size_t field : {size_t(0x548), size_t(0x11B0), size_t(0x6D8)}) {
        if (Read<void*>(owner, field)) { ++ownedFrees; std::free(Read<void*>(owner, field)); }
    }
    for (size_t i = 0; i < 8; ++i) if (void* work = Read<void*>(owner, 0x6C0 + i * 0x180)) {
        ++ownedFrees; std::free(work);
    }
    Write(owner, 0, uintptr_t(0xDEAD)); // Native base destruction resets vtables.
}
// For checks that write registry entries directly instead of through Birth.
void SyncActiveSlotsForTest() {
    std::fill(std::begin(g_activeSlots), std::end(g_activeSlots), uint64_t(0));
    for (const auto& entry : g_registry) if (ActiveOccupancy(entry.occupancy)) ActivateIdentity(entry);
}
std::shared_ptr<LifetimeSnapshot> testSnapshots[ROLLBACK_WINDOW];
SnapshotManifest& TestManifest(size_t slot) {
    if (!testSnapshots[slot]) testSnapshots[slot] = std::make_shared<LifetimeSnapshot>();
    return *testSnapshots[slot];
}
// Staging/dump checks need full capture. Retention tests also run with the
// production default (payload omitted) through the "omit" native argument.
LifetimeShotPayload g_testShotPayload = LifetimeShotPayload::Capture;
void UseTestPayloadArgument(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "omit")) g_testShotPayload = LifetimeShotPayload::Omit;
}
void SaveTestSnapshot(int32_t frame, std::shared_ptr<LifetimeSnapshot>& storage,
                      LifetimeShotPayload payload = g_testShotPayload) {
    LifetimeTrace_SaveSnapshot(frame, storage, payload);
}
void SaveTestSnapshot(int32_t frame, LifetimeShotPayload payload = g_testShotPayload) {
    SaveTestSnapshot(frame, testSnapshots[(frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW], payload);
}
'''
        checks = checks.replace("LifetimeTrace_SaveSnapshot(", "SaveTestSnapshot(")
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="lifetime-trace-") as tmp:
            cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
            cpp.write_text(MOCKS + rollback_header + character_header + header + source + bridge + checks)
            # ThreadSanitizer needs a position-independent executable.
            pie = [] if "thread" in sanitizers else ["-no-pie"]
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-pthread", "-DLIFETIME_VALIDATE_INDEXES",
                            f"-fsanitize={sanitizers}", "-fno-omit-frame-pointer", *pie, "-g",
                            str(cpp), "-o", str(exe)], check=True)
            self.last_results = []
            for args in arguments if arguments is not None else [()]:
                with self.subTest(native_arguments=args):
                    # TSan rejects, and this GCC's ASan intermittently faults at
                    # startup under, high-entropy ASLR mappings on current kernels.
                    launcher = ["setarch", platform.machine(), "-R"]
                    result = subprocess.run([*launcher, str(exe), *args], check=False, capture_output=True, text=True)
                    self.last_results.append(result)
                    self.assertEqual(result.returncode, expected_returncode, result.stdout + result.stderr)
            return result

    def test_trace_and_native_passthrough(self):
        self.compile_run(True, CHECKS)

    def test_failure_logs_ownership_values_without_relocking_recorder(self):
        result = self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
void reject(int) { std::_Exit(locked && fatalLogAttempts == 1 ? 17 : 18); }
int main() {
    reset(); uint8_t shot[0x2140]{};
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    Birth(shot, Kind::Shot); auto* owner = Find(shot, false);
    EnsureShotGeneration(*owner, ShotDomain::Pose);
    Write(shot, 0x538, uintptr_t(0x1234));
    LifetimeTrace_SetFrame(281, true); g_caller = 0x140056789;
    LifetimeFailureScope operation("test", shot, owner->id, 280);
    std::signal(SIGABRT, reject); AcquireSRWLockExclusive(&g_lock);
    RetireModelDomain(*owner, ShotDomain::Pose, true);
}
''', expected_returncode=17)
        self.assertIn("=== LIFETIME INVARIANT FAILURE ===", result.stderr)
        self.assertRegex(result.stderr, r"source=.*lifetime_shot_generations.inl:\d+")
        self.assertIn("RetireModelDomain", result.stderr)
        self.assertIn("model ownership changed outside a retained mutation boundary", result.stderr)
        self.assertIn("pid=123 thread=7", result.stderr)
        self.assertIn("frame=281 resim=1 target_frame=280 phase=shot.domain.retire", result.stderr)
        self.assertIn("identity=1 native_caller=0x140056789", result.stderr)
        for value in ("field=0x538", "expected=0x0", "actual=0x1234", "generation.id=0x1"):
            self.assertIn(value, result.stderr)

    def test_all_lifetime_abort_sites_emit_diagnostics(self):
        for name in ("lifetime_trace.cpp", "lifetime_shot_cpu.inl", "lifetime_shot_generations.inl",
                     "lifetime_shot_world.inl", "lifetime_shot_constraints.inl", "lifetime_shot_restore.inl",
                     "lifetime_shot_models.inl"):
            with self.subTest(source=name):
                source = (ROOT / "UMVC3Hook/code" / name).read_text()
                self.assertNotIn("std::abort()", source)
                self.assertNotIn('LIFE_FAIL("unconditional lifetime invariant failure")', source)
                self.assertNotIn('"reject_when:', source)  # Predicates are stringified, never pasted.
                self.assertTrue("LIFE_FAIL(" in source or "LIFE_REQUIRE(" in source)

    def test_disabled(self):
        self.compile_run(False, r'''
int main() {
    LifetimeTrace_Install();
    LifetimeTrace_SetFrame(123, true);
    LifetimeTrace_Marker("ignored", 123);
    LifetimeTrace_Flush();
    LifetimeTrace_SaveSnapshot(123);
    LifetimeTrace_CheckSnapshot(123);
    LifetimeTrace_DropSnapshot(123);
    LifetimeTrace_ClearSnapshots();
    LifetimeTrace_RestoreEffects(123);
    LifetimeTrace_RetireFutureShots(123);
    assert(!creates && !queues && !logs && !g_sequence && !g_recording);
}
''')

    def test_snapshot_observations(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset();
    Birth(object, Kind::Effect);
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).valid && TestManifest(0).count() == 1);
    assert(Find(object, false)->snapshots == 1);
    LifetimeTrace_SaveSnapshot(1);
    assert(Find(object, false)->snapshots == 3);
    Mutation(object, 2);
    assert(!std::strcmp(last().name, "snapshot_hazard"));
    assert(last().a == 2 && last().b == 3 && last().revision == 0);
    LifetimeTrace_CheckSnapshot(0);
    assert(!std::strcmp(last().name, "snapshot_manifest_check"));
    assert(last().b == 1 && last().c == 0 && last().d == 1);
    // Same-frame rewritten predictions replace their observation, not another slot's.
    LifetimeTrace_SaveSnapshot(0);
    LifetimeTrace_CheckSnapshot(0);
    assert(last().c == 0 && last().d == 0 && TestManifest(0).count() == 1);
    LifetimeTrace_CheckSnapshot(1);
    assert(last().c == 0 && last().d == 1);
    LifetimeTrace_DropSnapshot(1);
    assert(Find(object, false)->snapshots == 1);
    LifetimeTrace_CheckSnapshot(1);
    assert(!std::strcmp(last().name, "snapshot_manifest_absent"));
    const uint64_t oldId = Find(object, false)->id;
    Destroyed("gone", object, oldId);
    Birth(object, Kind::Effect);
    assert(Find(object, false)->id != oldId && Find(object, false)->snapshots == 0);
    LifetimeTrace_CheckSnapshot(0);
    assert(last().b == 1 && last().c == 1 && last().d == 0);
    const Event& detail = g_events[(g_read + g_count - 2) % EventCapacity];
    assert(detail.id == oldId && detail.b == Find(object, false)->id && detail.vtable == 0);
    LifetimeTrace_SaveSnapshot(ROLLBACK_WINDOW);
    LifetimeTrace_CheckSnapshot(0);
    assert(!std::strcmp(last().name, "snapshot_manifest_absent"));
    LifetimeTrace_CheckSnapshot(ROLLBACK_WINDOW);
    assert(last().c == 0 && last().d == 0);
    LifetimeTrace_SaveSnapshot(-1);
    assert(TestManifest(ROLLBACK_WINDOW - 1).valid);
    LifetimeTrace_ClearSnapshots();
    assert(Find(object, false)->snapshots == 0);
    for (const auto* manifest : g_liveSnapshots) assert(!manifest);
    assert(TestManifest(0).valid); // Clearing leases does not erase diagnostic bytes.

    reset();
    auto* allocation = std::calloc(1, 0x2200);
    Birth(allocation, Kind::Shot);
    LifetimeTrace_SaveSnapshot(5);
    oShotDtor = [](void* p) { assert(!locked); std::free(p); };
    ShotDtor(allocation); // A direct base call is not the intact most-derived deletion boundary.
    LifetimeTrace_CheckSnapshot(5); // ASan: never reads historical/freed storage.
    assert(last().b == 1 && last().c == 1 && last().d == 0);

    reset();
    for (size_t i = 0; i < RegistrySize; ++i)
        g_registry[i] = {(void*)(i + 1), i + 1, 0, Kind::Shot, Occupancy::Live};
    SyncActiveSlotsForTest();
    LifetimeTrace_SaveSnapshot(2); // Bounded manifest, no dereference even for invalid addresses.
    assert(TestManifest(2).count() == RegistrySize);
    LifetimeTrace_ClearSnapshots();

    reset();
    alignas(16) uint8_t objects[20][32]{};
    for (auto& p : objects) Birth(p, Kind::Effect);
    LifetimeTrace_SaveSnapshot(3);
    for (auto& p : objects) Destroyed("gone", p, Find(p, false)->id);
    const size_t before = g_count;
    LifetimeTrace_CheckSnapshot(3);
    assert(last().b == 20 && last().c == 20 && last().d == 0);
    assert(g_count == before + 17); // 16 detail samples plus full aggregate.
    g_recording = false;
    LifetimeTrace_ClearSnapshots();
    assert(!g_liveSnapshots[3]);
}
''')

    def test_empty_effect_quarantine(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int deletes = 0, bodies = 0;
void* makeEffect() {
    void* p = std::calloc(1, 0x2C0);
    const uintptr_t vt = _addr(0x140A8E180);
    const uint32_t flags = 0x3F8;
    std::memcpy(p, &vt, sizeof(vt));
    std::memcpy(static_cast<uint8_t*>(p) + 0x10, &flags, sizeof(flags));
    Birth(p, Kind::Effect);
    return p;
}
int main() {
    reset();
    oEffectDtor = [](void*) { assert(!locked); ++bodies; };
    oDeleteEffect = [](void* p, uint32_t flags) -> void* {
        assert(!locked); ++deletes;
        EffectDtor(p);
        if (flags & 1) std::free(p);
        return p;
    };
    void* p = makeEffect();
    assert(RetainableDetachedEffect(p));
    const uint64_t id = Find(p, false)->id;
    LifetimeTrace_SaveSnapshot(0);
    LifetimeTrace_SaveSnapshot(1);
    auto diagnosticHistory = testSnapshots[0];
    assert(DeleteEffect(p, 1) == p && deletes == 0 && bodies == 0);
    assert(g_quarantined == 1 && Find(p, false)->occupancy == Occupancy::Quarantined);
    assert(Find(p, false)->id == id);
    assert(Read<uintptr_t>(p, 0) == _addr(0x140A8E180));
    assert(DeleteEffect(p, 1) == p && deletes == 0 && g_quarantined == 1);
    LifetimeTrace_CheckSnapshot(0);
    assert(!std::strcmp(last().name, "quarantine_restore_pending") && last().b == 1);
    // Retired objects are not admitted into new manifests or reactivated by checks.
    LifetimeTrace_SaveSnapshot(2);
    assert(TestManifest(2).count() == 0 && Find(p, false)->snapshots == 3);
    LifetimeTrace_DropSnapshot(0);
    CollectQuarantine();
    assert(deletes == 0 && Find(p, false)->snapshots == 2);
    LifetimeTrace_DropSnapshot(1);
    CollectQuarantine();
    assert(deletes == 1 && bodies == 1 && g_quarantined == 0 && !Find(p, false));
    assert(diagnosticHistory->valid && diagnosticHistory->count() == 1);
    CollectQuarantine();
    assert(deletes == 1);

    // Native passthrough for nonempty/linked/grouped/derived/ready objects.
    alignas(16) uint8_t nativeNode[0xE0]{};
    for (size_t offset : {size_t(0x18), size_t(0x20), size_t(0x30), size_t(0x204),
                          size_t(0x218), size_t(0x220), size_t(0x228)}) {
        p = makeEffect();
        uint64_t value = offset == 0x218 ? uintptr_t(nativeNode) : 1;
        std::memcpy(static_cast<uint8_t*>(p) + offset, &value, sizeof(value));
        LifetimeTrace_SaveSnapshot(0);
        int before = deletes;
        DeleteEffect(p, 1);
        assert(deletes == before + 1 && g_quarantined == 0);
    }
    for (int mode = 0; mode < 3; ++mode) {
        p = makeEffect();
        if (mode == 0) { uint32_t active = 2; std::memcpy(static_cast<uint8_t*>(p)+0x10, &active, 4); }
        if (mode == 1) { uintptr_t derived = 0x140A8D330; std::memcpy(p, &derived, 8); }
        if (mode == 2) { uint32_t ready = 0x01000000; std::memcpy(static_cast<uint8_t*>(p)+0x130, &ready, 4); }
        LifetimeTrace_SaveSnapshot(0);
        int before = deletes;
        DeleteEffect(p, 1);
        assert(deletes == before + 1 && g_quarantined == 0);
    }
    p = makeEffect();
    LifetimeTrace_SaveSnapshot(0);
    int before = deletes;
    DeleteEffect(p, 0); // Non-deleting in-place invocation remains native.
    assert(deletes == before + 1 && !Find(p, false));
    std::free(p);

    p = makeEffect();
    LifetimeTrace_SaveSnapshot(0);
    DeleteEffect(p, 1);
    before = deletes;
    g_recording = false;
    LifetimeTrace_Flush(); // Trace exhaustion cannot revoke snapshot ownership.
    assert(deletes == before && g_quarantined == 1 && Find(p, false));
    LifetimeTrace_DropSnapshot(0);
    LifetimeTrace_Flush();
    assert(deletes == before + 1 && g_quarantined == 0 && !Find(p, false));

    reset();
    std::vector<void*> pool;
    for (size_t i = 0; i <= QuarantineLimit; ++i) pool.push_back(makeEffect());
    LifetimeTrace_SaveSnapshot(0);
    before = deletes;
    for (void* effect : pool) DeleteEffect(effect, 1);
    assert(g_quarantined == QuarantineLimit && deletes == before + 1);
    oActionFinal = [](void*) { assert(!locked && g_quarantined == 0 && !g_retentionOpen); };
    ActionFinal(nullptr);
    assert(g_quarantined == 0 && deletes == before + int(pool.size()));
    for (void* effect : pool) assert(!Find(effect, false));

    p = makeEffect();
    LifetimeTrace_SaveSnapshot(0); // A later saved world reopens the experiment.
    assert(g_retentionOpen);
    DeleteEffect(p, 1);
    oMainDtor = [](void*) { assert(!locked && g_quarantined == 0); };
    MainDtor(nullptr);
    p = makeEffect();
    LifetimeTrace_SaveSnapshot(0);
    DeleteEffect(p, 1);
    oManagerDtor = [](void*) { assert(!locked && g_quarantined == 0); };
    ManagerDtor(nullptr);

    // A multi-shot hyper holds far more effects than any fixed per-frame bound:
    // capture and restore are sized by the frame, not by a retention limit.
    reset(); alignas(16) static uint8_t units[0x1860]{}; unitManager = units;
    Write(units, 0xC38, uint32_t(1));
    std::vector<void*> burst;
    for (int i = 0; i < 300; ++i) burst.push_back(makeEffect());
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).effects.size() == burst.size());
    LifetimeTrace_RestoreEffects(0);
    assert(!g_quarantined && TestManifest(0).effects.size() == burst.size());
    LifetimeTrace_ClearSnapshots();
    before = deletes;
    for (void* effect : burst) DeleteEffect(effect, 1);
    assert(deletes == before + int(burst.size()));
}
''')

    def test_attack_space_rewinds_with_its_effects(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r"""
alignas(16) uint8_t units[0x1860]{}, action[0x250]{}, child[0x40]{};
int disposals = 0;
uint8_t* makeSpace() {
    auto* space = static_cast<uint8_t*>(std::calloc(1, 0x1D40));
    Write(space, 0, uintptr_t(_addr(0x140A8B400)) + 0x270);       // uExAtkSpaceLv1.
    Write(space, 0x10, uint32_t(2 | (1 << 3)));
    Birth(space, Kind::Unit);
    Find(space, false)->deleting = +[](void* object, uint32_t) -> void* { ++disposals; std::free(object); return object; };
    return space;
}
void line(std::initializer_list<void*> entries) {
    void* descriptor = units + 0x50 + 0x30; void* previous = nullptr;
    Write(descriptor, 0, entries.size() ? *entries.begin() : nullptr);
    for (void* object : entries) {
        Write(object, 0x20, previous); Write(object, 0x18, (void*)nullptr);
        if (previous) Write(previous, 0x18, object);
        previous = object;
    }
    Write(descriptor, 8, previous);
}
int main() {
    reset(); unitManager = units; actionManager = action; Write(units, 0xC38, uint32_t(2));
    assert(!IsAttackSpace(child));
    auto* space = makeSpace();
    Write(space, 0x1D0C, uint32_t(0));                             // mLocalStep: super still running.
    Write(space, 0x1D18, static_cast<void*>(child));               // One live child effect.
    Write(space, 0x1CF8, uint32_t(0x40000));                       // Effect-model lifecycle word.
    Write(action, 0x148, static_cast<void*>(space)); line({space});
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).attackSpaces.size() == 1 && Find(space, false)->dependencies == 1);

    // The super ended: children were told to finish and the space waits on them.
    Write(space, 0x1D0C, uint32_t(1)); Write(space, 0x1CF8, uint32_t(0x80000));
    LifetimeTrace_RestoreEffects(0);
    assert(Read<uint32_t>(space, 0x1D0C) == 0 && Read<void*>(space, 0x1D18) == child);
    assert(Read<uint32_t>(space, 0x1CF8) == 0x40000 && Read<void*>(action, 0x148) == space);

    // It finished, killed its children and was deleted: held, unlinked, forgotten by sAction.
    Write(space, 0x1D0C, uint32_t(3)); Write(space, 0x1D18, (void*)nullptr);
    Write(space, 0x10, uint32_t(0x3F8)); line({}); Write(space, 0x18, (void*)nullptr);
    Find(space, false)->occupancy = Occupancy::Quarantined; ++g_quarantined;
    Write(action, 0x148, (void*)nullptr);
    // The abandoned branch then started another super with its own space.
    auto* later = makeSpace(); Write(action, 0x148, static_cast<void*>(later)); line({later});
    LifetimeTrace_RestoreEffects(0);
    assert(Find(space, false)->occupancy == Occupancy::Live && Read<uint32_t>(space, 0x1D0C) == 0);
    assert(Read<void*>(space, 0x1D18) == child && (Read<uint32_t>(space, 0x10) & 0x3FF) == (2 | (1 << 3)));
    assert(Read<void*>(units, 0x80) == space && Read<void*>(units, 0x88) == space && !Read<void*>(space, 0x18));
    assert(Read<void*>(action, 0x148) == space);
    assert(Find(later, false)->occupancy == Occupancy::Quarantined && g_quarantined == 1);
    assert((Read<uint32_t>(later, 0x10) & 0x3FF) == 0x3F8 && !disposals);
    // Repeating the restore changes nothing; the later space is disposed once unleased.
    LifetimeTrace_RestoreEffects(0);
    assert(g_quarantined == 1 && Find(space, false)->occupancy == Occupancy::Live);
    LifetimeTrace_ClearSnapshots();
    assert(disposals == 1 && !g_quarantined && Find(space, false)->occupancy == Occupancy::Live);
    std::free(space);
}
""")

    def test_shot_driven_model_rewinds_with_its_shot(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r"""
alignas(16) uint8_t units[0x1860]{}, modelDti[0x40]{};
int deletes = 0;
void* destroy(void* object, uint32_t flags) { assert(!locked && flags == 1); ++deletes; return object; }
uintptr_t modelVtable[] = {uintptr_t(&destroy)};
uint8_t* makeModel() {
    auto* model = static_cast<uint8_t*>(std::calloc(1, 0x1D00));
    Write(model, 0, uintptr_t(modelVtable));
    Write(model, 0x10, uint32_t(2 | (1 << 3)));
    return model;
}
void line(std::initializer_list<void*> entries) {
    void* descriptor = units + 0x50 + 0x30; void* previous = nullptr;
    Write(descriptor, 0, entries.size() ? *entries.begin() : nullptr);
    for (void* object : entries) {
        Write(object, 0x20, previous); Write(object, 0x18, (void*)nullptr);
        if (previous) Write(previous, 0x18, object);
        previous = object;
    }
    Write(descriptor, 8, previous);
}
int main() {
    reset(); unitManager = units; Write(units, 0xC38, uint32_t(2));
    Write(modelDti, 0x20, uint64_t(_addr(0x140D46ED0))); Write(modelDti, 0x30, uint32_t(0x1D00 >> 2));
    testDti = uintptr_t(modelDti);
    uint8_t shot[0x2140]{};
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    Birth(shot, Kind::Shot); auto* owner = Find(shot, false);
    owner->extent = sizeof(shot); owner->dti = 0x1234;

    // The cart's model: animating, with its own joint storage.
    auto* model = makeModel();
    Birth(model, Kind::Unit);
    Find(model, false)->deleting = destroy; Find(model, false)->dti = uintptr_t(modelDti);
    Find(model, false)->extent = 0x1D00;
    alignas(16) uint8_t joints[0x10 + 2 * 0xC0]{};
    Write(joints, 0, uint32_t(2));
    Write(model, 0x538, static_cast<void*>(joints + 0x10)); Write(model, 0x530, uint32_t(2));
    Write(joints, 0x10 + 0xC0, 1.5f);
    Write(model, 0x1328, 20.0f);                                   // Animation time.
    Write(model, 0x1CD4, 30.0f); Write(model, 0x1CF8, uint32_t(0x40000)); // Remaining life, step flags.
    Write(shot, 0x1F78, static_cast<void*>(model)); line({model});
    assert(IsDrivenModel(*Find(model, false)) && !IsDrivenModel(*owner));
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).models.size() == 1 && TestManifest(0).models[0].object == model);
    assert(TestManifest(0).models[0].spans.size() == 2 && Find(model, false)->dependencies == 1);

    // The replayed frames advanced it on top of where it already was.
    Write(model, 0x1328, 26.0f); Write(model, 0x1CD4, 24.0f); Write(joints, 0x10 + 0xC0, 9.0f);
    LifetimeTrace_RetireFutureShots(0); LifetimeTrace_RestoreEffects(0);
    assert(Read<float>(model, 0x1328) == 20.0f && Read<float>(model, 0x1CD4) == 30.0f);
    assert(Read<float>(joints, 0x10 + 0xC0) == 1.5f && Find(model, false)->occupancy == Occupancy::Live);

    // Its animation ended and it retired: held, unlinked, and forgotten by the
    // cart. The abandoned branch's cart then made a model of its own.
    Write(model, 0x1328, 66.0f); Write(model, 0x1CF8, uint32_t(0x80000));
    Write(model, 0x10, uint32_t(0x3F8)); line({}); Write(model, 0x18, (void*)nullptr);
    Find(model, false)->occupancy = Occupancy::Quarantined; ++g_quarantined;
    auto* later = makeModel(); Write(shot, 0x1F78, static_cast<void*>(later)); line({later});
    assert(!Find(later, false));
    LifetimeTrace_RetireFutureShots(0);
    assert(Find(later, false) && Find(later, false)->abandonedModel && !Find(model, false)->abandonedModel);
    LifetimeTrace_RestoreEffects(0);
    assert(Find(model, false)->occupancy == Occupancy::Live && Read<float>(model, 0x1328) == 20.0f);
    assert(Read<uint32_t>(model, 0x1CF8) == 0x40000 && (Read<uint32_t>(model, 0x10) & 0x3FF) == (2 | (1 << 3)));
    assert(Read<void*>(units, 0x80) == model && Read<void*>(units, 0x88) == model && !Read<void*>(model, 0x18));
    assert(Find(later, false)->occupancy == Occupancy::Quarantined && g_quarantined == 1 && !deletes);
    assert((Read<uint32_t>(later, 0x10) & 0x3FF) == 0x3F8 && !Find(later, false)->abandonedModel);

    // A model set up again has other buffers: its animation stays live, the
    // lifecycle still rewinds, and a link to a unit the snapshot never held is dropped.
    alignas(16) uint8_t other[0x10 + 2 * 0xC0]{};
    Write(other, 0, uint32_t(2));
    Write(model, 0x538, static_cast<void*>(other + 0x10));
    Write(model, 0x1328, 40.0f); Write(model, 0x1CF8, uint32_t(0x80000)); Write(joints, 0x10 + 0xC0, 3.0f);
    Write(shot, 0x1F78, static_cast<void*>(model));
    LifetimeTrace_RetireFutureShots(0); LifetimeTrace_RestoreEffects(0);
    assert(Read<float>(model, 0x1328) == 40.0f && Read<float>(joints, 0x10 + 0xC0) == 3.0f);
    assert(Read<uint32_t>(model, 0x1CF8) == 0x40000 && Read<void*>(model, 0x538) == other + 0x10);

    LifetimeTrace_ClearSnapshots();
    assert(deletes == 1 && !g_quarantined && Find(model, false)->occupancy == Occupancy::Live);
    ForgetCurrentShotGenerations(owner->id, shot);
    std::free(model); std::free(later);
}
""")

    def test_quarantine_disabled(self):
        self.compile_run(True, r'''
int main() {
    LifetimeTrace_Install();
    assert(creates == 21 && queues == 21);
    oDeleteEffect = [](void* p, uint32_t flags) -> void* {
        assert(p == (void*)123 && flags == 7 && !locked);
        return (void*)456;
    };
    assert(DeleteEffect((void*)123, 7) == (void*)456);
    assert(g_quarantined == 0);
}
''', quarantine=False)

    def test_shot_model_delayed_destruction(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t units[0x1860]{}, shots[0x3450]{};
auto& sentinel = shotHeads[0];
bool palettes[64]{};
unsigned nextPalette = 0;
int derivedBodies = 0, modelBodies = 0, unlinks = 0, idReleases = 0;
int primaryRemovals = 0, groupRemovals = 0;
void* nestedParent = nullptr;
void* nestedChild = nullptr;

void* dispatch(void* p, uint32_t flags) {
    void* target = Read<void*>(Read<void*>(p, 0), 0);
    for (const auto& hook : mockHooks)
        if (hook.first == target) return ((Deleting)hook.second)(p, flags);
    return ((Deleting)target)(p, flags);
}
void nativeBase(void* p) {
    assert(!locked && g_shotBase == p);
    UnlinkShot(shots, Read<uint32_t>(p, 0x1F54), p);
    ReleaseShotId(shots, Read<uint32_t>(p, 0x1F50));
    // Model ownership is untouched until this physical teardown, not merely
    // kept allocated after its vector destructor has damaged joint state.
    auto* joints = Read<uint8_t*>(p, 0x538);
    assert(Read<void*>(joints, 0xA8) == p && Read<uint32_t>(joints - 0x10, 0) == 1);
    assert(Read<uint32_t>(joints, 0x20) == 0xCAFEBABE);
    std::free(joints - 0x10);
    for (size_t offset : {size_t(0x548), size_t(0x6C0), size_t(0x11B0), size_t(0x110)})
        std::free(Read<void*>(p, offset));
    const uint32_t palette = Read<uint32_t>(p, 0x116C);
    assert(palettes[palette]); palettes[palette] = false;
    void* resource = Read<void*>(p, 0x100);
    ResourceRelease(resource);
    assert(Read<uint32_t>(resource, 0x4C) == 0);
    std::free(resource);
    ++modelBodies;
}
void* nativeDerived(void* p, uint32_t flags) {
    assert(!locked);
    ++derivedBodies;
    if (p == nestedParent) {
        assert(g_collectingShot == p);
        dispatch(nestedChild, 1); // Must not inherit the parent's ID suppression.
        nestedParent = nestedChild = nullptr;
    }
    Write(p, 0, uintptr_t(0x12345678)); // Most-derived teardown starts here.
    std::free(Read<void*>(p, 0x2140));
    ShotDtor(p);
    if (flags & 1) std::free(p);
    return p;
}
void* otherDerived(void* p, uint32_t flags) { return nativeDerived(p, flags); }
uintptr_t vtableA[] = {uintptr_t(&nativeDerived)};
uintptr_t vtableAlias[] = {uintptr_t(&nativeDerived)};
uintptr_t vtableB[] = {uintptr_t(&otherDerived)};

void* makeShot(uint32_t id, uintptr_t* vt = vtableA) {
    auto* p = (uint8_t*)std::calloc(1, 0x2180);
    Write(p, 0, uintptr_t(vt)); Write(p, 0x10, uint32_t(0x3F8));
    Write(p, 0x1F50, id); Write(p, 0x1F54, uint32_t(0));
    auto* allocation = (uint8_t*)std::calloc(1, 0x10 + 0xC0);
    Write(allocation, 0, uint32_t(1)); Write(allocation + 0x10, 0xA8, (void*)p);
    Write(allocation + 0x10, 0x20, uint32_t(0xCAFEBABE));
    Write(p, 0x538, allocation + 0x10);
    Write(p, 0x548, std::calloc(1, 1)); Write(p, 0x6C0, std::calloc(1, 0x68));
    Write(p, 0x11B0, std::calloc(1, 0x40)); Write(p, 0x110, std::calloc(1, 0x60));
    Write(p, 0x2140, std::calloc(1, 0x90));
    void* resource = std::calloc(1, 0xC0); Write(resource, 0x4C, uint32_t(1));
    Write(p, 0x100, resource);
    assert(nextPalette < 64); palettes[nextPalette] = true;
    Write(p, 0x116C, nextPalette++);
    assert(!Read<uint32_t>(shots, 0x38 + 4 * id));
    Write(shots, 0x38 + 4 * id, uint32_t(1));
    assert(CreateShot(p) == p);
    PublishShot(shots, 0, p);
    return p;
}
void assertIntact(void* p) {
    assert(Read<uintptr_t>(p, 0) == uintptr_t(vtableA));
    assert(Read<uint32_t>(Read<void*>(p, 0x538), 0x20) == 0xCAFEBABE);
    assert(Read<void*>(Read<void*>(p, 0x538), 0xA8) == p);
    assert(Read<uint32_t>(Read<void*>(p, 0x100), 0x4C) == 1);
    assert(palettes[Read<uint32_t>(p, 0x116C)]);
}
int main(int argc, char** argv) {
    UseTestPayloadArgument(argc, argv);
    reset(); unitManager = units; shotManager = shots;
    Write(units, 0xC38, uint32_t(32));
    void* head = sentinel + 8;
    Write(head, 8, head); Write(head, 0x10, head);
    oCreateShot = [](void* p) -> void* { return p; };
    oPublishShot = [](void*, uint32_t, void* p) {
        assert(!locked);
        void* head = sentinel + 8; void* next = Read<void*>(head, 8);
        void* node = (uint8_t*)p + 0x1450;
        Write(node, 8, next); Write(node, 0x10, head);
        if (next != head) Write(next, 0x10, node); // Native empty-list insert leaves head prev stale.
        Write(head, 8, node);
    };
    oUnlinkShot = [](void* manager, uint32_t team, void* p) {
        assert(!locked && manager == shots && team == 0); ++unlinks;
        void* node = (uint8_t*)p + 0x1450;
        void* next = Read<void*>(node, 8); void* previous = Read<void*>(node, 0x10);
        if (next && previous) { Write(next, 0x10, previous); Write(previous, 8, next); }
        // Like native, deliberately leave stale own links to expose double unlink.
    };
    oReleaseShotId = [](void* manager, uint32_t id) {
        assert(!locked && manager == shots && id < 256); ++idReleases;
        Write(manager, 0x38 + 4 * id, uint32_t(0));
    };
    oRemoveGroups = [](void* manager, void* p) {
        assert(!locked && unitLocked && manager == units); ++groupRemovals;
        Write(p, 0x30, uint64_t(0));
    };
    oRemoveUnit = [](void* manager, void* p) -> bool {
        assert(!locked && unitLocked && manager == units); ++primaryRemovals;
        Write(manager, 0x50 + 12 * 0x30, (void*)nullptr);
        Write(manager, 0x58 + 12 * 0x30, (void*)nullptr);
        Write(p, 0x18, (void*)nullptr); Write(p, 0x20, (void*)nullptr);
        Write(p, 0x10, uint32_t(0x3F8)); return true;
    };
    oShotDtor = nativeBase;

    void* p = makeShot(7);
    assert(creates == 1 && queues == 1 && applies == 1);
    assert(Find(p, false)->deleting == nativeDerived && Find(p, false)->shotLinked);
    LifetimeTrace_SaveSnapshot(0); LifetimeTrace_SaveSnapshot(1);
    dispatch(p, 1);
    assert(g_quarantined == 1 && !derivedBodies && !modelBodies);
    assert(last().kind == Kind::Shot && !std::strcmp(last().name, "quarantine_hold"));
    assert(unlinks == 1 && idReleases == 1 && !Read<uint32_t>(shots, 0x38 + 4 * 7));
    assert(Read<void*>(head, 8) == head && Read<void*>(head, 0x10) == head);
    assertIntact(p);
    dispatch(p, 1); assert(unlinks == 1 && idReleases == 1 && g_quarantined == 1);
    LifetimeTrace_SaveSnapshot(2); assert(TestManifest(2).count() == 0);
    g_recording = false; LifetimeTrace_Flush(); assertIntact(p);
    LifetimeTrace_DropSnapshot(0); CollectQuarantine(); assertIntact(p);

    // The concrete deleting target is shared by another vtable and hooked once.
    // Both the native ID and list slot are reused before old model GC.
    void* q = makeShot(7, vtableAlias);
    assert(creates == 1 && applies == 1);
    const uint32_t qPalette = Read<uint32_t>(q, 0x116C);
    LifetimeTrace_DropSnapshot(1); CollectQuarantine();
    assert(!g_quarantined && derivedBodies == 1 && modelBodies == 1);
    assert(Read<uint32_t>(shots, 0x38 + 4 * 7) == 1 && unlinks == 1 && idReleases == 1);
    assert(Read<void*>(head, 8) == (uint8_t*)q + 0x1450 && palettes[qPalette]);
    dispatch(q, 1); // Unleased objects keep native immediate destruction.
    assert(derivedBodies == 2 && modelBodies == 2 && unlinks == 2 && idReleases == 2);

    // A direct delete, not just native/headless scheduler teardown, detaches
    // primary/group membership before retaining the whole derived model owner.
    p = makeShot(8); Write(p, 0x10, uint32_t(2 | (12 << 3)));
    Write(p, 0x30, uint64_t(2));
    Write(units, 0x50 + 12 * 0x30, p); Write(units, 0x58 + 12 * 0x30, p);
    LifetimeTrace_SaveSnapshot(3); dispatch(p, 1);
    assert(primaryRemovals == 1 && groupRemovals == 1 && g_quarantined == 1);
    assert(Read<uint32_t>(p, 0x10) == 0x3F8 && !Read<uint64_t>(p, 0x30));
    assert(!Read<void*>(units, 0x50 + 12 * 0x30)); assertIntact(p);
    LifetimeTrace_ClearSnapshots();
    assert(!g_quarantined && derivedBodies == 3 && modelBodies == 3);

    // Nested unleased shot destruction must still release its own ID/list/model.
    p = makeShot(9); LifetimeTrace_SaveSnapshot(4);
    q = makeShot(10, vtableB);
    assert(creates == 2 && applies == 2);
    nestedParent = p; nestedChild = q;
    dispatch(p, 1);
    const int beforeIds = idReleases, beforeUnlinks = unlinks;
    oUnitReset = [](void*) { assert(!locked && !g_quarantined); };
    UnitReset(units);
    assert(derivedBodies == 5 && modelBodies == 5);
    assert(idReleases == beforeIds + 1 && unlinks == beforeUnlinks + 1);
    assert(!Read<uint32_t>(shots, 0x38 + 4 * 9) && !Read<uint32_t>(shots, 0x38 + 4 * 10));
    assert(!g_collectingShot && !g_shotBase && !unitLocked);

    // A non-deleting invocation with no leases remains native and preserves the
    // caller's ownership of the outer allocation, not of its destroyed model.
    p = makeShot(11); dispatch(p, 0); std::free(p);
    assert(derivedBodies == 6 && modelBodies == 6 && resourceAdds == 0 && resourceReleases == 6);
    for (bool occupied : palettes) assert(!occupied);

    // An effect can discover a shot created outside the traced factory. Its
    // real native team-list membership must be recovered, not guessed from
    // stale own links or left falsely unlinked in the adopted sidecar.
    p = makeShot(12);
    Destroyed("fixture_missing_factory", p, Find(p, false)->id);
    alignas(16) uint8_t concreteDti[0x40]{};
    Write(concreteDti, 0x20, uint64_t(_addr(0x140D4D960)));
    Write(shotDti, 0x20, uint64_t(_addr(0x140E173D0)));
    testDti = uintptr_t(concreteDti);
    void* effect = std::calloc(1, 0x2C0);
    Write(effect, 0, uintptr_t(_addr(0x140A8E180)));
    Write(effect, 0x10, uint32_t(2 | (13 << 3))); Write(effect, 0x280, p);
    Write(units, 0x50 + 13 * 0x30, effect); Write(units, 0x58 + 13 * 0x30, effect);
    Birth(effect, Kind::Effect); LifetimeTrace_SaveSnapshot(5);
    assert(Find(p, false)->kind == Kind::Shot && Find(p, false)->shotLinked);
    const int beforeAdoptedUnlink = unlinks;
    dispatch(p, 1);
    assert(unlinks == beforeAdoptedUnlink + 1 && Read<void*>(head, 8) == head);
    Write(effect, 0x280, (void*)nullptr); // Native weak-owner pruning.
    LifetimeTrace_ClearSnapshots();
    assert(!g_quarantined && derivedBodies == 7 && modelBodies == 7);
    assert(resourceAdds == 0 && resourceReleases == 7);
    Destroyed("fixture_dispose", effect, Find(effect, false)->id); std::free(effect);
}
''', arguments=[(), ("omit",)])

    def test_retiring_effect_restore(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t manager[0x1860]{};
constexpr uint32_t category = 13;
void* line() { return manager + 0x50 + 0x30 * category; }
int deleted = 0;
void* makeUnit(uint32_t state, bool tracked = true) {
    void* p = std::calloc(1, 0x2C0);
    Write(p, 0, uintptr_t(_addr(0x140A8E180)));
    Write(p, 0x10, uint32_t(state | (category << 3)));
    if (tracked) Birth(p, Kind::Effect);
    return p;
}
void chain(std::initializer_list<void*> nodes) {
    void* previous = nullptr;
    Write(line(), 0, (void*)nullptr);
    for (void* p : nodes) {
        Write(p, 0x20, previous); Write(p, 0x18, (void*)nullptr);
        if (previous) Write(previous, 0x18, p); else Write(line(), 0, p);
        previous = p;
    }
    Write(line(), 8, previous);
}
void detachAndDelete(void* p) {
    void* previous = Read<void*>(p, 0x20);
    void* next = Read<void*>(p, 0x18);
    if (previous) Write(previous, 0x18, next); else Write(line(), 0, next);
    if (next) Write(next, 0x20, previous); else Write(line(), 8, previous);
    Write(p, 0x18, (void*)nullptr); Write(p, 0x20, (void*)nullptr);
    Write(p, 0x10, uint32_t(0x3F8));
    DeleteEffect(p, 1);
}
int main(int argc, char** argv) {
    UseTestPayloadArgument(argc, argv);
    reset();
    unitManager = manager;
    Write(manager, 0xC38, uint32_t(32));
    oEffectDtor = [](void*) { assert(!locked); };
    oDeleteEffect = [](void* p, uint32_t flags) -> void* {
        assert(!locked && flags == 1); ++deleted;
        EffectDtor(p); std::free(p); return p;
    };
    void* a = makeUnit(2); void* b = makeUnit(3);
    void* c = makeUnit(4); void* d = makeUnit(2);
    Write(b, 8, uint32_t(42)); Write(c, 0xC, uint32_t(7)); Write(b, 0x28, 0.5f);
    chain({a, b, c, d});
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).effectCount() == 4);
    detachAndDelete(b); detachAndDelete(c);
    assert(g_quarantined == 2 && deleted == 0 && Read<void*>(a, 0x18) == d);
    Write(b, 8, uint32_t(99)); Write(b, 0x28, 3.0f);
    LifetimeTrace_RestoreEffects(0);
    assert(!std::strcmp(last().name, "retiring_restore_applied") && last().b == 4 && last().c == 2);
    assert(g_quarantined == 0 && deleted == 0 && !unitLocked);
    assert(Read<void*>(a, 0x18) == b && Read<void*>(b, 0x20) == a);
    assert(Read<void*>(b, 0x18) == c && Read<void*>(c, 0x20) == b);
    assert(Read<void*>(c, 0x18) == d && Read<void*>(d, 0x20) == c);
    assert(Read<uint32_t>(b, 8) == 42 && Read<uint32_t>(c, 0xC) == 7);
    assert(Read<float>(b, 0x28) == 0.5f && (Read<uint32_t>(b, 0x10) & 7) == 3);
    assert((Read<uint32_t>(c, 0x10) & 7) == 4 && Read<void*>(line(), 0) == a);
    LifetimeTrace_RestoreEffects(0); // Already linked: no duplicate membership.
    assert(last().b == 4 && last().c == 0 && g_quarantined == 0);
    detachAndDelete(b); detachAndDelete(c); // Same identities can retire again.
    assert(g_quarantined == 2 && deleted == 0);
    void* extra = makeUnit(2);
    chain({a, extra, d});
    LifetimeTrace_RestoreEffects(0);
    assert(!std::strcmp(last().name, "retiring_restore_applied"));
    assert(g_quarantined == 1 && Read<void*>(a, 0x18) == b && RetainableDetachedEffect(extra));
    assert(Read<void*>(b, 0x18) == c && Read<void*>(c, 0x18) == d);
    Mutation(c, 2); // Revisions are expected state changes, not restore rejection.
    chain({d, c, b, a});
    LifetimeTrace_RestoreEffects(0);
    assert(g_quarantined == 1 && Read<void*>(line(), 0) == a && Read<void*>(line(), 8) == d);
    assert(Read<void*>(a, 0x18) == b && Read<void*>(b, 0x18) == c);
    LifetimeTrace_ClearSnapshots();
    assert(g_quarantined == 0 && deleted == 1);
    chain({}); DeleteEffect(a, 1); DeleteEffect(b, 1); DeleteEffect(c, 1); DeleteEffect(d, 1);

    // Singleton lane: restore head and tail, including retained resource identity.
    void* p = makeUnit(4);
    alignas(16) uint8_t resource[0x60]{};
    Write(p, 0x108, (void*)resource);
    chain({p}); LifetimeTrace_SaveSnapshot(1); detachAndDelete(p);
    LifetimeTrace_RestoreEffects(1);
    assert(last().c == 1 && Read<void*>(line(), 0) == p && Read<void*>(line(), 8) == p);
    assert(Read<void*>(p, 0x18) == nullptr && Read<void*>(p, 0x20) == nullptr);
    detachAndDelete(p);
    Write(p, 0x120, uint32_t(999));
    LifetimeTrace_RestoreEffects(1);
    assert(g_quarantined == 0 && Read<uint32_t>(p, 0x120) == 0);
    assert(Read<void*>(p, 0x108) == resource);
    LifetimeTrace_ClearSnapshots();
    chain({}); DeleteEffect(p, 1);

    // Unknown anchors cannot be trusted by address alone, even when currently readable.
    a = makeUnit(2, false); p = makeUnit(3); chain({a, p});
    LifetimeTrace_SaveSnapshot(2);
    assert(TestManifest(2).effectCount() == 1);
    chain({p}); std::free(a);
    LifetimeTrace_RestoreEffects(2);
    assert(!std::strcmp(last().name, "retiring_restore_applied"));
    assert(Read<void*>(p, 0x20) == nullptr && Read<void*>(line(), 0) == p);
    detachAndDelete(p);
    LifetimeTrace_RestoreEffects(2);
    assert(!std::strcmp(last().name, "retiring_restore_applied") && g_quarantined == 0);
    assert(Read<void*>(line(), 0) == p && Read<void*>(p, 0x20) == nullptr);
    LifetimeTrace_ClearSnapshots(); chain({}); DeleteEffect(p, 1);

    // A newly registered foreign unit cannot displace the saved retained cohort.
    a = makeUnit(2, false); p = makeUnit(4); chain({a, p});
    LifetimeTrace_SaveSnapshot(3); detachAndDelete(p);
    void* foreign = makeUnit(2, false);
    chain({foreign, a});
    LifetimeTrace_RestoreEffects(3);
    assert(g_quarantined == 0 && Read<void*>(a, 0x18) == p && Read<void*>(p, 0x18) == foreign);
    assert(Read<void*>(foreign, 0x20) == p && Read<void*>(line(), 8) == foreign);
    LifetimeTrace_ClearSnapshots(); chain({}); DeleteEffect(p, 1); std::free(a); std::free(foreign);

    // Active-at-save empty owners restore through the same retained graph path.
    p = makeUnit(2); chain({p}); LifetimeTrace_SaveSnapshot(4);
    assert(TestManifest(4).effectCount() == 1);
    detachAndDelete(p);
    LifetimeTrace_RestoreEffects(4);
    assert(g_quarantined == 0 && (Read<uint32_t>(p, 0x10) & 7) == 2);
    LifetimeTrace_ClearSnapshots();
    chain({}); DeleteEffect(p, 1);
    assert(g_quarantined == 0 && !unitLocked && !locked);

    // Empty target worlds must discard future-only owners, including owners
    // leased by a newer frame. Restoring that newer frame must resurrect them.
    Write(manager, 0x1840, uint32_t(2));
    void* futureGroup = manager + 0xC40 + 0x30;
    void* futureGroupData[1]{};
    Write(futureGroup, 0x28, (void*)futureGroupData);
    Write(futureGroup, 0x1C, uint32_t(1));
    chain({}); LifetimeTrace_SaveSnapshot(5);
    p = makeUnit(2); chain({p});
    futureGroupData[0] = p; Write(futureGroup, 0x18, uint32_t(1));
    Write(p, 0x30, uint64_t(2));
    Write(p, 0x120, uint32_t(73)); LifetimeTrace_SaveSnapshot(6);
    Write(p, 0x120, uint32_t(99));
    void* otherLine = manager + 0x50 + 0x30 * (category + 1);
    chain({}); Write(otherLine, 0, p); Write(otherLine, 8, p);
    Write(p, 0x10, uint32_t(2 | ((category + 1) << 3)));
    int beforeDiscard = deleted;
    LifetimeTrace_RestoreEffects(5);
    assert(deleted == beforeDiscard && g_quarantined == 1);
    assert(Read<void*>(line(), 0) == nullptr && Read<void*>(otherLine, 0) == nullptr);
    assert(Read<uint32_t>(futureGroup, 0x18) == 0 && Read<uint64_t>(p, 0x30) == 0);
    LifetimeTrace_RestoreEffects(6);
    assert(g_quarantined == 0 && Read<void*>(line(), 0) == p);
    assert(Read<void*>(otherLine, 0) == nullptr && Read<uint32_t>(p, 0x120) == 73);
    assert(((Read<uint32_t>(p, 0x10) >> 3) & 0x7F) == category);
    assert(Read<uint32_t>(futureGroup, 0x18) == 1 && futureGroupData[0] == p);
    assert(Read<uint64_t>(p, 0x30) == 2);
    LifetimeTrace_RestoreEffects(5);
    LifetimeTrace_ClearSnapshots();
    assert(deleted == beforeDiscard + 1 && g_quarantined == 0);

    // Unregistered callback owners have state but no primary-list membership.
    p = makeUnit(1); Write(p, 0x10, uint32_t(1 | (0x7F << 3)));
    Write(p, 0x120, uint32_t(17)); LifetimeTrace_SaveSnapshot(7);
    Write(p, 0x120, uint32_t(88)); Write(p, 0x10, uint32_t(0x3F8));
    DeleteEffect(p, 1); LifetimeTrace_RestoreEffects(7);
    assert(g_quarantined == 0 && Read<uint32_t>(p, 0x120) == 17);
    assert(Read<uint32_t>(p, 0x10) == (1 | (0x7F << 3)) && Read<void*>(line(), 0) == nullptr);
    LifetimeTrace_ClearSnapshots(); DeleteEffect(p, 1);

    // Retain a real populated CPU billboard layout, not just an empty owner.
    p = makeUnit(3);
    alignas(16) uint8_t node[0x240]{}, storage[0x270]{}, parameter[0x20]{};
    void* pool = storage + 0x150;
    Write(p, 0x204, uint16_t(1)); Write(p, 0x208, uint16_t(1));
    Write(p, 0x218, (void*)node); Write(p, 0x220, (void*)storage);
    Write(p, 0x130, uint32_t(0x01000000));
    Write(node, 0, uintptr_t(_addr(0x140BC4CB0)));
    Write(node, 0x10, p); Write(node, 0x28, (void*)storage);
    Write(node, 0x34, uint32_t(0x120)); Write(node, 0x38, pool);
    Write(node, 0x58, (void*)parameter); Write(node, 0xD4, int32_t(-1));
    Write(node, 0x110, uint16_t(1)); Write(node, 0xE0, pool);
    Write(node, 0x116, uint16_t(0xD0));
    Write(storage, 0x100, p); Write(pool, 0x20, uint32_t(0x12345678));
    assert(OwnedEffectGraph(p));
    Write(node, 0x48, (void*)1); assert(!OwnedEffectGraph(p)); Write(node, 0x48, (void*)nullptr);
    Write(parameter, 0x18, uint8_t(0x10)); assert(OwnedEffectGraph(p)); Write(parameter, 0x18, uint8_t(0));
    Write(node, 0x20, uint32_t(0x40)); assert(!OwnedEffectGraph(p)); Write(node, 0x20, uint32_t(0));
    Write(node, 0x12C, uint32_t(0x400)); assert(!OwnedEffectGraph(p)); Write(node, 0x12C, uint32_t(0));
    Write(pool, 0x60, (void*)1); assert(!OwnedEffectGraph(p)); Write(pool, 0x60, (void*)nullptr);
    Write(pool, 8, pool); assert(!OwnedEffectGraph(p)); Write(pool, 8, (void*)nullptr);
    Write(node, 0x18, (void*)node); assert(!OwnedEffectGraph(p)); Write(node, 0x18, (void*)nullptr);
    Write(node, 0x34, uint32_t(0x40001)); assert(!OwnedEffectGraph(p)); Write(node, 0x34, uint32_t(0x120));
    chain({p}); LifetimeTrace_SaveSnapshot(5);
    assert(TestManifest(5).effectCount() == 1);
    int before = deleted;
    detachAndDelete(p);
    assert(g_quarantined == 1 && deleted == before && last().c == 1);
    LifetimeTrace_RestoreEffects(5);
    assert(!std::strcmp(last().name, "retiring_restore_applied") && last().c == 1);
    assert(g_quarantined == 0 && Read<void*>(p, 0x218) == node);
    assert(Read<uint32_t>(pool, 0x20) == 0x12345678 && Read<void*>(line(), 0) == p);
    detachAndDelete(p);
    assert(!LifetimeTrace_FinishCapture() && g_quarantined == 1 && deleted == before);
    LifetimeTrace_Flush();
    while (!LifetimeTrace_FinishCapture()) LifetimeTrace_Flush();
    assert(deleted == before + 1 && g_quarantined == 0);
    assert(!g_recording && !g_count);
}
''', arguments=[(), ("omit",)])

    def test_borrowed_effect_owner_lifetime(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t manager[0x1860]{}, dti[0x40]{};
int ownersDestroyed = 0;
bool effectDisposed = false;
void* mustFollowEffect = nullptr;
void* nativeOwnerDelete(void* p, uint32_t flags) {
    assert(!locked && flags == 1);
    if (p == mustFollowEffect) assert(effectDisposed);
    ++ownersDestroyed;
    // Reproduce the invalid-vtable shape in the supplied 0x14004F0BC crash.
    Write(p, 0, uintptr_t(0x1728)); Write(p, 0x10, uint32_t(2));
    std::free(p);
    return p;
}
uintptr_t ownerVtable[] = {uintptr_t(&nativeOwnerDelete)};
void* makeOwner() {
    void* p = std::calloc(1, 0x40); // Deliberately not shot-sized.
    Write(p, 0, uintptr_t(ownerVtable)); Write(p, 0x10, uint32_t(2 | (0x7F << 3)));
    return p; // No shot/effect factory birth: adoption must come from the edge.
}
void deleteOwner(void* p) {
    void* target = Read<void*>(Read<void*>(p, 0), 0);
    for (const auto& hook : mockHooks) if (hook.first == target) {
        ((Deleting)hook.second)(p, 1); return;
    }
    assert(false);
}
bool nativeOwnerCheck(void* effect) {
    void* owner = Read<void*>(effect, 0x280);
    if (!owner || ((Read<uint32_t>(owner, 0x10) & 7) != 1 &&
                   (Read<uint32_t>(owner, 0x10) & 7) != 2)) return false;
    assert(Read<uintptr_t>(owner, 0) == uintptr_t(ownerVtable));
    return true;
}
int main() {
    reset(); unitManager = manager;
    Write(manager, 0xC38, uint32_t(32));
    Write(dti, 0x20, uint64_t(_addr(0x140E173D0))); testDti = uintptr_t(dti);
    void* effect = std::calloc(1, 0x2C0);
    Write(effect, 0, uintptr_t(_addr(0x140A8E180)));
    Write(effect, 0x10, uint32_t(2 | (13 << 3)));
    Write(manager, 0x50 + 13 * 0x30, effect); Write(manager, 0x58 + 13 * 0x30, effect);
    Birth(effect, Kind::Effect);
    void* first = makeOwner();
    for (size_t offset : {size_t(0x40), size_t(0x138), size_t(0x278), size_t(0x280), size_t(0x288)})
        Write(effect, offset, first);
    assert(nativeOwnerCheck(effect));
    LifetimeTrace_SaveSnapshot(0); LifetimeTrace_SaveSnapshot(1);
    Identity* owner = Find(first, false);
    assert(owner && owner->kind == Kind::Unit && owner->dependencies == 3 && owner->snapshots == 3);
    assert(TestManifest(0).count() == 2 && creates == 1 && applies == 1);
    deleteOwner(first);
    assert(ownersDestroyed == 0 && g_quarantined == 1 && !nativeOwnerCheck(effect));
    assert(Read<uintptr_t>(first, 0) == uintptr_t(ownerVtable));

    // Bind a new native owner; the old frame must still own the old pointer.
    void* second = makeOwner();
    for (size_t offset : {size_t(0x40), size_t(0x138), size_t(0x278), size_t(0x280), size_t(0x288)})
        Write(effect, offset, second);
    LifetimeTrace_SaveSnapshot(2);
    assert(creates == 1 && applies == 1 && nativeOwnerCheck(effect));
    assert(Find(second, false)->dependencies == 4);
    LifetimeTrace_RestoreEffects(0);
    assert(Read<void*>(effect, 0x280) == first && !nativeOwnerCheck(effect));
    assert(ownersDestroyed == 0 && Read<uintptr_t>(first, 0) == uintptr_t(ownerVtable));
    LifetimeTrace_RestoreEffects(2);
    assert(Read<void*>(effect, 0x280) == second && nativeOwnerCheck(effect));
    LifetimeTrace_DropSnapshot(0); CollectQuarantine();
    assert(!ownersDestroyed && Find(first, false)->dependencies == 2);
    LifetimeTrace_DropSnapshot(1); CollectQuarantine();
    assert(ownersDestroyed == 1 && !Find(first, false) && nativeOwnerCheck(effect));

    // Generic unleased deletion must tombstone its identity as well: unlike a
    // shot it has no hooked base destructor to do that bookkeeping for us.
    mustFollowEffect = second;
    deleteOwner(second);
    oEffectDtor = [](void*) { assert(!locked); effectDisposed = true; };
    oDeleteEffect = [](void* p, uint32_t) -> void* { EffectDtor(p); std::free(p); return p; };
    Write(manager, 0x50 + 13 * 0x30, (void*)nullptr);
    Write(manager, 0x58 + 13 * 0x30, (void*)nullptr);
    Write(effect, 0x10, uint32_t(0x3F8)); DeleteEffect(effect, 1);
    assert(g_quarantined == 2);
    LifetimeTrace_ClearSnapshots();
    assert(effectDisposed && ownersDestroyed == 2 && !Find(second, false));
    void* third = makeOwner(); Birth(third, Kind::Unit); RegisterUnit(third);
    deleteOwner(third);
    assert(ownersDestroyed == 3 && !Find(third, false));
    assert(!g_quarantined && !resourceAdds && !resourceReleases);
    assert(last().name && !std::strcmp(last().name, "unit_dispose_end"));
}
''')

    def test_pending_crash_trace(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset();
    void* p = std::calloc(1, 0x40); Write(p, 0, uintptr_t(0x12345678));
    Birth(p, Kind::Unit);
    const uint64_t id = Find(p, false)->id;
    Record("owner_pending", p, 0x280, 0x1728);
    Destroyed("owner_destroyed", p, id); std::free(p);
    assert(last().id == id);
    const size_t count = g_count, read = g_read;
    FILE* file = std::tmpfile(); assert(file);
    LifetimeTrace_WritePending(file); // ASan: no native/historical pointer reads.
    assert(g_count == count && g_read == read);
    AcquireSRWLockExclusive(&g_lock);
    LifetimeTrace_WritePending(file); // A crash holding g_lock must not deadlock.
    ReleaseSRWLockExclusive(&g_lock);
    char text[8192]{};
    std::rewind(file); std::fread(text, 1, sizeof(text) - 1, file); std::fclose(file);
    assert(std::strstr(text, "LIFE_PENDING seq=") && std::strstr(text, "event=owner_pending"));
    assert(std::strstr(text, "kind=unit") && std::strstr(text, "b=0x1728"));
    assert(std::strstr(text, "trace lock busy") && g_count == count && g_read == read);
}
''')

    def test_shared_graph_transaction(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t manager[0x1860]{};
constexpr uint32_t category = 13;
void* line() { return manager + 0x50 + 0x30 * category; }
struct Graph {
    alignas(16) uint8_t owner[0x2C0]{}, node[0x250]{}, storage[0x290]{};
    alignas(16) uint8_t resource[0xC0]{}, generator[0x80]{}, particle[0x50]{}, move[8]{};
    alignas(16) uint8_t info[0x78]{}, jointParam[0x70]{}, jointExtra[0x20]{};
    void* pool() { return storage + 0x150; }
    Graph() {
        Write(owner, 0, uintptr_t(_addr(0x140A8E180)));
        Write(owner, 0x10, uint32_t(2 | (category << 3)));
        Write(owner, 0x108, (void*)resource); Write(owner, 0x130, uint32_t(0x01000000));
        Write(resource, 0x4C, uint32_t(1));
        Write(owner, 0x204, uint16_t(1)); Write(owner, 0x208, uint16_t(1));
        Write(owner, 0x218, (void*)node); Write(owner, 0x220, (void*)storage);
        Write(node, 0, uintptr_t(_addr(0x140BC4CB0))); Write(node, 0x10, (void*)owner);
        Write(node, 0x28, (void*)storage); Write(node, 0x34, uint32_t(0x120));
        Write(node, 0x38, pool()); Write(node, 0x40, (void*)info);
        Write(node, 0x50, (void*)generator); Write(node, 0x58, (void*)particle);
        Write(node, 0x68, (void*)move); Write(node, 0x70, uint32_t(0x1000));
        Write(node, 0xD4, int32_t(-1)); Write(node, 0xE0, pool());
        Write(node, 0xE8, pool()); Write(node, 0x110, uint16_t(1));
        Write(node, 0x114, uint16_t(0xD0)); Write(node, 0x116, uint16_t(0xD0));
        Write(node, 0x1D0, (void*)(particle + 0x48)); Write(node, 0x1E8, (void*)(particle + 0x20));
        Write(storage, 0x100, (void*)owner); Write(storage, 0x108, (void*)jointParam);
        Write(storage, 0x128, uint32_t(sizeof(jointExtra)));
        Write(storage, 0x130, (void*)jointExtra); Write(storage, 0x138, (void*)jointExtra);
        Birth(owner, Kind::Effect);
    }
    void tick() {
        Write(owner, 0x120, Read<uint32_t>(owner, 0x120) + 1);
        Write(node, 0x78, Read<uint32_t>(node, 0x78) * 1664525u + 1013904223u);
        Write(pool(), 0x20, Read<uint32_t>(pool(), 0x20) + Read<uint32_t>(node, 0x78));
        ++jointExtra[0];
    }
};
Graph* replacement = nullptr;
std::vector<void*> disposedGraphs;
void NativeGraphRelease(void* owner) {
    assert(!locked);
    void* node = Read<void*>(owner, 0x218);
    if (node) {
        for (void* disposed : disposedGraphs) assert(disposed != node);
        disposedGraphs.push_back(node);
    }
    Write(owner, 0x218, (void*)nullptr); Write(owner, 0x220, (void*)nullptr);
    Write(owner, 0x228, (void*)nullptr); Write(owner, 0x204, uint64_t(0));
    Write(owner, 0x130, Read<uint32_t>(owner, 0x130) & ~0x01000000u);
}
void chain(std::initializer_list<void*> objects) {
    void* previous = nullptr;
    Write(line(), 0, (void*)nullptr);
    for (void* p : objects) {
        Write(p, 0x20, previous); Write(p, 0x18, (void*)nullptr);
        if (previous) Write(previous, 0x18, p); else Write(line(), 0, p);
        previous = p;
    }
    Write(line(), 8, previous);
}
int main(int argc, char** argv) {
    UseTestPayloadArgument(argc, argv);
    reset(); unitManager = manager; Write(manager, 0xC38, uint32_t(32));
    Graph a, b;
    chain({a.owner, b.owner});
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).effectCount() == 2);
    assert(TestManifest(0).effects[0].simulation && TestManifest(0).payloadBytes() > 0);
    a.tick(); b.tick();
    const uint32_t expected = Read<uint32_t>(a.pool(), 0x20);
    LifetimeTrace_RestoreEffects(0);
    assert(!std::strcmp(last().name, "retiring_restore_applied"));
    assert(Read<uint32_t>(a.owner, 0x120) == 0 && Read<uint32_t>(a.pool(), 0x20) == 0);
    assert(a.jointExtra[0] == 0 && Read<void*>(a.owner, 0x18) == b.owner);
    a.tick(); assert(Read<uint32_t>(a.pool(), 0x20) == expected);

    // The exact same saved graph now restores through the quarantine branch.
    chain({b.owner}); Write(a.owner, 0x18, (void*)nullptr); Write(a.owner, 0x20, (void*)nullptr);
    Write(a.owner, 0x10, uint32_t(0x3F8));
    int destroyed = 0;
    oEffectDtor = [](void*) { assert(!locked); };
    oDeleteEffect = [](void* p, uint32_t) -> void* { assert(!locked); EffectDtor(p); return p; };
    DeleteEffect(a.owner, 1); assert(g_quarantined == 1);
    LifetimeTrace_RestoreEffects(0);
    assert(!std::strcmp(last().name, "retiring_restore_applied") && last().c == 1);
    assert(g_quarantined == 0 && (Read<uint32_t>(a.owner, 0x10) & 7) == 2);
    assert(Read<void*>(line(), 0) == a.owner && Read<void*>(a.owner, 0x18) == b.owner);
    assert(Read<uint32_t>(a.pool(), 0x20) == 0 && a.jointExtra[0] == 0);
    a.tick(); assert(Read<uint32_t>(a.pool(), 0x20) == expected);

    // Arbitrary mutable state and restart do not invalidate owned allocations.
    Write(b.node, 0x78, uint32_t(0xDEADBEEF));
    oRestartGraph = [](void*) { assert(!locked); };
    RestartGraph(b.owner); LifetimeTrace_RestoreEffects(0);
    assert(!std::strcmp(last().name, "retiring_restore_applied"));
    assert(Read<uint32_t>(a.pool(), 0x20) == 0 && Read<uint32_t>(b.node, 0x78) == 0);
    LifetimeTrace_SaveSnapshot(1);
    LifetimeTrace_DropSnapshot(1);
    assert(!g_liveSnapshots[1]);

    // Restore actual ordered group membership, including native buffer growth.
    Write(manager, 0x1840, uint32_t(2));
    void* group = manager + 0xC40 + 0x30;
    void* groupData = std::calloc(1, sizeof(void*));
    Write(groupData, 0, (void*)a.owner);
    Write(group, 0x18, uint32_t(1)); Write(group, 0x1C, uint32_t(1)); Write(group, 0x28, groupData);
    Write(a.owner, 0x30, uint64_t(2)); LifetimeTrace_SaveSnapshot(2);
    a.tick(); LifetimeTrace_RestoreEffects(2);
    assert(!std::strcmp(last().name, "retiring_restore_applied") && Read<uint64_t>(a.owner, 0x30) == 2);
    // Native retirement removed A from the group; a new unrelated unit joined.
    alignas(16) uint8_t foreign[0x40]{};
    Write(groupData, 0, (void*)foreign);
    chain({b.owner}); Write(a.owner, 0x18, (void*)nullptr); Write(a.owner, 0x20, (void*)nullptr);
    Write(a.owner, 0x30, uint64_t(0)); Write(a.owner, 0x10, uint32_t(0x3F8));
    DeleteEffect(a.owner, 1); LifetimeTrace_RestoreEffects(2);
    assert(!std::strcmp(last().name, "retiring_restore_applied") && g_quarantined == 0);
    assert(Read<uint64_t>(a.owner, 0x30) == 2 && Read<uint32_t>(group, 0x18) == 2);
    groupData = Read<void*>(group, 0x28);
    assert(Read<void*>(groupData, 0) == a.owner && Read<void*>(groupData, 8) == foreign);
    assert(groupExpansions == 1);
    LifetimeTrace_ClearSnapshots(); assert(g_quarantined == 0);
    std::free(groupData); Write(group, 0x28, (void*)nullptr); Write(group, 0x18, uint32_t(0));

    // Resource replacement keeps both graphs alive at their original addresses.
    reset(); unitManager = manager; Write(manager, 0xC38, uint32_t(32));
    Graph c, d;
    Destroyed("fixture_storage", d.owner, Find(d.owner, false)->id);
    replacement = &d;
    chain({c.owner});
    oReleaseGraph = NativeGraphRelease;
    oBaseEffectResource = [](void* owner, void* resource) {
        assert(!locked);
        if (void* old = Read<void*>(owner, 0x108)) releaseResourceRef(old);
        Write(owner, 0x108, resource);
        if (resource) ResourceAdd(resource);
    };
    oEffectResource = [](void* owner, void* resource) {
        assert(!locked && replacement && resource == replacement->resource);
        ReleaseGraph(owner);
        BaseEffectResource(owner, resource);
        std::memcpy(static_cast<uint8_t*>(owner) + 0x40, replacement->owner + 0x40, 0x280);
        Write(replacement->node, 0x10, owner); Write(replacement->storage, 0x100, owner);
    };
    LifetimeTrace_SaveSnapshot(2); LifetimeTrace_SaveSnapshot(3);
    assert(Read<uint32_t>(c.resource, 0x4C) == 1 && resourceAdds == 0 && resourceReleases == 0);
    const int oldGraph = Find(c.owner, false)->graph;
    EffectResource(c.owner, d.resource);
    assert(disposedGraphs.empty() && Read<uint32_t>(c.resource, 0x4C) == 1);
    assert(!g_generations[oldGraph].attached && g_generations[oldGraph].snapshots == 12);
    Write(c.owner, 0x120, uint32_t(9)); Write(d.pool(), 0x20, uint32_t(999));
    LifetimeTrace_SaveSnapshot(4);
    const int newGraph = Find(c.owner, false)->graph;
    assert(newGraph != oldGraph && Read<uint32_t>(d.resource, 0x4C) == 2);
    LifetimeTrace_RestoreEffects(3);
    assert(Find(c.owner, false)->graph == oldGraph && Read<void*>(c.owner, 0x218) == c.node);
    assert(Read<uint32_t>(c.owner, 0x120) == 0 && Read<uint32_t>(c.pool(), 0x20) == 0);
    assert(Read<uint32_t>(c.resource, 0x4C) == 1 && Read<uint32_t>(d.resource, 0x4C) == 2);
    LifetimeTrace_RestoreEffects(4);
    assert(Find(c.owner, false)->graph == newGraph && Read<void*>(c.owner, 0x218) == d.node);
    assert(Read<uint32_t>(c.owner, 0x120) == 9 && Read<uint32_t>(d.pool(), 0x20) == 999);
    assert(disposedGraphs.empty());

    // Neither logging shutdown nor expiry of just one lease may destroy A.
    g_recording = false; LifetimeTrace_Flush();
    assert(disposedGraphs.empty() && g_lifetimeEnabled);
    LifetimeTrace_DropSnapshot(3); LifetimeTrace_Flush();
    assert(disposedGraphs.empty());
    LifetimeTrace_DropSnapshot(2); LifetimeTrace_Flush();
    assert(disposedGraphs.size() == 1 && disposedGraphs[0] == c.node);
    assert(Read<void*>(c.owner, 0x218) == d.node && Read<uint32_t>(d.pool(), 0x20) == 999);
    assert(Read<uint32_t>(c.resource, 0x4C) == 0);
    LifetimeTrace_DropSnapshot(4); LifetimeTrace_Flush();
    assert(disposedGraphs.size() == 1); // Attached B still belongs to the native owner.
    oEffectDtor = [](void* owner) { ReleaseGraph(owner); releaseResourceRef(Read<void*>(owner, 0x108)); };
    chain({}); Write(c.owner, 0x10, uint32_t(0x3F8));
    DeleteEffect(c.owner, 1);
    assert(disposedGraphs.size() == 2 && disposedGraphs[1] == d.node);
    assert(Read<uint32_t>(d.resource, 0x4C) == 1 && g_quarantined == 0);
    assert(resourceAdds == 1 && resourceReleases == 2); // Native new binding, then final disposals only.

    // Rebuilding a graph without rebinding shares the one original reference.
    reset(); disposedGraphs.clear(); unitManager = manager;
    Graph e, f, g;
    Destroyed("fixture_storage", f.owner, Find(f.owner, false)->id);
    Destroyed("fixture_storage", g.owner, Find(g.owner, false)->id);
    chain({e.owner}); LifetimeTrace_SaveSnapshot(5);
    ReleaseGraph(e.owner);
    std::memcpy(e.owner + 0x40, f.owner + 0x40, 0x280);
    Write(e.owner, 0x108, (void*)e.resource);
    Write(f.node, 0x10, (void*)e.owner); Write(f.storage, 0x100, (void*)e.owner);
    LifetimeTrace_SaveSnapshot(6);
    const int reference = Find(e.owner, false)->resourceReference;
    assert(g_resourceReferences[reference].generations == 2);
    assert(Read<uint32_t>(e.resource, 0x4C) == 1 && resourceAdds == 0);
    replacement = &g; EffectResource(e.owner, g.resource);
    LifetimeTrace_SaveSnapshot(7);
    assert(!g_resourceReferences[reference].onOwner && resourceAdds == 1);
    LifetimeTrace_DropSnapshot(5); LifetimeTrace_Flush();
    assert(disposedGraphs.size() == 1 && Read<uint32_t>(e.resource, 0x4C) == 1);
    LifetimeTrace_DropSnapshot(6); LifetimeTrace_Flush();
    assert(disposedGraphs.size() == 2 && Read<uint32_t>(e.resource, 0x4C) == 0);
    LifetimeTrace_ClearSnapshots(); chain({}); Write(e.owner, 0x10, uint32_t(0x3F8));
    DeleteEffect(e.owner, 1);
    assert(disposedGraphs.size() == 3 && Read<uint32_t>(g.resource, 0x4C) == 1);
    assert(resourceAdds == 1 && resourceReleases == 2);

    // Capture raw state without a movement-mode or character-dependency policy.
    reset(); disposedGraphs.clear(); unitManager = manager;
    Graph attached;
    Write(attached.node, 0x70, uint32_t(0xA21200));
    Write(attached.node, 0x34, uint32_t(0x140));
    Write(attached.node, 0x118, uint16_t(0x10));
    Write(attached.node, 0x11A, uint16_t(0x60));
    alignas(16) uint8_t character[0x40]{};
    for (size_t field : {size_t(0x40), size_t(0x138), size_t(0x278), size_t(0x280), size_t(0x288)})
        Write(attached.owner, field, (void*)character);
    chain({attached.owner});
    for (uint32_t mode = 0; mode < 12; ++mode) {
        const uint32_t types = (mode << 20) | 0x20000 | ((mode % 9) << 12) | 0x200;
        Write(attached.node, 0x70, types);
        LifetimeTrace_SaveSnapshot(8);
        assert(TestManifest(8).effectCount() == 1);
        attached.tick(); Write(attached.owner, 0x280, (void*)nullptr);
        Write(attached.node, 0x70, uint32_t(0));
        LifetimeTrace_RestoreEffects(8);
        assert(!std::strcmp(last().name, "retiring_restore_applied"));
        assert(Read<uint32_t>(attached.node, 0x70) == types);
        assert(Read<uint32_t>(attached.pool(), 0x20) == 0);
        assert(Read<void*>(attached.owner, 0x280) == character);
    }
    chain({}); Write(attached.owner, 0x10, uint32_t(0x3F8));
    DeleteEffect(attached.owner, 1); assert(g_quarantined == 1);
    LifetimeTrace_RestoreEffects(8);
    assert(g_quarantined == 0 && (Read<uint32_t>(attached.owner, 0x10) & 7) == 2);
    assert(Read<void*>(attached.owner, 0x40) == character);
    LifetimeTrace_ClearSnapshots(); chain({}); Write(attached.owner, 0x10, uint32_t(0x3F8));
    DeleteEffect(attached.owner, 1);
    assert(disposedGraphs.size() == 1 && Read<uint32_t>(attached.resource, 0x4C) == 0);

    // Native ancestry and declared size replace concrete-generator whitelists.
    reset(); disposedGraphs.clear(); unitManager = manager;
    Graph generic;
    alignas(16) uint8_t dti[0x40]{};
    Write(dti, 0x20, uint64_t(_addr(0x140E1E120)));
    Write(dti, 0x30, uint32_t(0x250 >> 2));
    testDti = uintptr_t(dti);
    Write(generic.node, 0, uintptr_t(0x123000));
    Write(generic.node, 0x116, uint16_t(0x100));
    Write(generic.node, 0x240, uint64_t(0xCAFEBABE));
    RegisterGeneratorLayouts(generic.owner);
    assert(OwnedEffectGraph(generic.owner));
    chain({generic.owner}); LifetimeTrace_SaveSnapshot(10);
    Write(generic.node, 0x240, uint64_t(0));
    LifetimeTrace_RestoreEffects(10);
    assert(Read<uint64_t>(generic.node, 0x240) == 0xCAFEBABE);
    LifetimeTrace_ClearSnapshots(); chain({}); Write(generic.owner, 0x10, uint32_t(0x3F8));
    DeleteEffect(generic.owner, 1);
    assert(disposedGraphs.size() == 1);
    (void)destroyed;
}
''', arguments=[(), ("omit",)])

    def test_shared_snapshot_history_outlives_rollback_window(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset();
    alignas(16) uint8_t manager[0x2000]{};
    auto* owner = static_cast<uint8_t*>(std::calloc(1, 0x2C0));
    unitManager = manager;
    Write(manager, 0xC38, uint32_t(1)); Write(manager, 0x50, owner);
    Write(owner, 0, uintptr_t(_addr(0x140A8E180))); Write(owner, 0x10, uint32_t(2));
    Birth(owner, Kind::Effect);
    std::shared_ptr<LifetimeSnapshot> history[255];
    const LifetimeSnapshot* firstBank = nullptr;
    for (int frame = 0; frame < 300; ++frame) {
        LifetimeTrace_DropSnapshot(frame);
        auto& slot = testSnapshots[frame % ROLLBACK_WINDOW];
        slot = history[frame % 255]; // Same bank handoff as SnapDump_AcquireSnapshotStorage.
        Write(owner, 0x124, uint32_t(frame));
        LifetimeTrace_SaveSnapshot(frame, slot);
        history[frame % 255] = slot;
        assert(history[frame % 255].get() == g_liveSnapshots[frame % ROLLBACK_WINDOW]);
        if (frame == 0) firstBank = slot.get();
        if (frame == 255) assert(slot.get() == firstBank); // Reuses history-owned capacity.
    }
    // The live rollback lease index has only the last sixteen frames, but all
    // 255 diagnostic captures retain their own roots, graph bytes, and identities.
    for (const auto* saved : g_liveSnapshots) assert(saved && saved->frame >= 284);
    for (int frame = 45; frame < 300; ++frame) {
        const auto& saved = history[frame % 255];
        assert(saved->frame == frame && saved->effectCount() == 1);
        assert(Read<uint32_t>(saved->effects[0].root, 0x124) == uint32_t(frame));
    }
    LifetimeTrace_ClearSnapshots();
    for (const auto* saved : g_liveSnapshots) assert(!saved);
    Write(manager, 0x50, static_cast<void*>(nullptr));
    Destroyed("gone", owner, Find(owner, false)->id);
    std::free(owner);
    size_t roots = 0;
    auto counts = LifetimeTrace_VisitSnapshot(45, history[45].get(), false,
        [](void* context, const char* name, const void*, uint64_t, uint64_t,
           const void* bytes, size_t) {
            if (!std::strstr(name, ".root")) return;
            ++*static_cast<size_t*>(context);
            assert(Read<uint32_t>(bytes, 0x124) == 45);
        }, &roots);
    assert(counts.effects == 1 && roots == 1); // ASan: no native memory access after free.
}
''')

    def test_coarse_model_and_attack_generations(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t manager[0x2000]{};
void* resource(uint32_t joints) {
    auto* result = std::calloc(1, 0x200);
    Write(result, 0x4C, uint32_t(1)); Write(result, 0x78, joints);
    return result;
}
void pose(void* owner, uint32_t count) {
    auto* joints = static_cast<uint8_t*>(std::calloc(1, 0x10 + count*0xC0));
    Write(joints, 0, count); Write(joints + 0x10, 0, uintptr_t(mockJointVtable));
    Write(joints + 0x10, 0xA8, owner);
    Write(owner, 0x538, joints + 0x10);
    Write(owner, 0x548, std::calloc(1, count));
    Write(owner, 0x11B0, std::calloc(1, count*0x40));
    Write(owner, 0x530, count);
}
void work(void* owner, uint32_t count) {
    const auto joints = Read<uint32_t>(owner, 0x530);
    for (uint32_t i = 0; i < count; ++i) {
        auto* buffer = std::calloc(joints, 0x68);
        Write(buffer, 0, uint32_t(0xAB00 + count));
        Write(owner, 0x6C0 + i*0x180, buffer);
    }
}
void keys(void* owner, uint32_t count) {
    const size_t length = Read<uint32_t>(owner, 0x1238)*0xC;
    auto* slab = static_cast<uint8_t*>(std::calloc(count, length));
    for (uint32_t i = 0; i < count; ++i) Write(owner, 0x6D8 + i*0x180, slab + i*length);
}
void releaseWork(void* owner) {
    for (size_t i = 0; i < 8; ++i) {
        auto* buffer = Read<void*>(owner, 0x6C0 + i*0x180);
        if (buffer) { ++ownedFrees; std::free(buffer); }
        Write(owner, 0x6C0 + i*0x180, uintptr_t(0));
    }
}
void nativeBlend(void* owner, uint32_t count) {
    assert(!locked && unitLocked);
    if (!Read<void*>(owner, 0x100) || !Read<uint32_t>(owner, 0x530)) { Write(owner, 0x554, count); return; }
    releaseWork(owner);
    if (auto* slab = Read<void*>(owner, 0x6D8)) { ++ownedFrees; std::free(slab); }
    for (size_t i = 0; i < 8; ++i) Write(owner, 0x6D8 + i*0x180, uintptr_t(0));
    Write(owner, 0x554, count); work(owner, count); keys(owner, count);
}
void nativeBase(void* owner, void* incoming) {
    assert(!locked && unitLocked);
    if (Read<void*>(owner, 0x100) == incoming) return;
    if (auto* old = Read<void*>(owner, 0x100)) GenerationResourceRelease(old);
    Write(owner, 0x100, incoming);
    if (incoming) ResourceAdd(incoming);
}
void nativeModel(void* owner, void* incoming) {
    assert(!locked && unitLocked);
    if (Read<void*>(owner, 0x100) == incoming) return;
    if (Read<uint32_t>(owner, 0x1238) && !Read<void*>(owner, 0x6D8)) keys(owner, Read<uint32_t>(owner, 0x554));
    const auto count = incoming ? Read<uint32_t>(incoming, 0x78) : 0;
    const bool replace = !incoming || count != Read<uint32_t>(owner, 0x530);
    if (replace) {
        if (auto* joints = Read<void*>(owner, 0x538)) MockJointDelete(joints, 3);
        for (size_t offset : {size_t(0x548), size_t(0x11B0)}) {
            if (auto* data = Read<void*>(owner, offset)) { ++ownedFrees; std::free(data); }
            Write(owner, offset, uintptr_t(0));
        }
        Write(owner, 0x538, uintptr_t(0)); Write(owner, 0x530, uint32_t(0)); releaseWork(owner);
    }
    GenerationBaseModel(owner, incoming); // Includes the null-input native reset branch.
    if (incoming && replace) { pose(owner, count); work(owner, Read<uint32_t>(owner, 0x554)); }
}
void* owner(void* modelResource = nullptr) {
    auto* result = std::calloc(1, 0x2200);
    Write(result, 0, uintptr_t(0xCAFE)); Write(result, 0x10, uint32_t(0x3F8));
    Write(result, 0x116C, UINT32_MAX); Write(result, 0x100, modelResource);
    Write(result, 0x554, uint32_t(1)); Write(result, 0x1238, uint32_t(2));
    Birth(result, Kind::Shot);
    auto* identity = Find(result, false); identity->dti = 0x12345; identity->extent = 0x2200;
    return result;
}
void disposeModel(void* object) {
    auto id = Find(object, false)->id;
    auto* model = Read<void*>(object, 0x100);
    ForgetCurrentShotGenerations(id, object);
    Write(object, 0x100, uintptr_t(0)); Write(object, 0x1250, uintptr_t(0));
    { UnitGuard guard; MockCollectModel(object); }
    if (model) ResourceRelease(model);
    Destroyed("gone", object, id); std::free(object);
}
int wrapperDeletes = 0, primitiveDeletes = 0, rootDeletes = 0;
void* deleteWrapper(void* p, uint32_t flags) {
    assert(!locked && flags == 1); ++wrapperDeletes; std::free(p); return p;
}
void* deletePrimitive(void* p, uint32_t flags) {
    assert(!locked && flags == 1); ++primitiveDeletes; std::free(p); return p;
}
void* deleteRoot(void* p, uint32_t flags) {
    assert(!locked && flags == 1); ++rootDeletes;
    auto* array = Read<void*>(p, 0x30);
    for (uint32_t i = 0; i < Read<uint32_t>(p, 0x20); ++i)
        if (auto* entry = Read<void*>(array, i*8)) deleteWrapper(entry, 1);
    std::free(array); std::free(p); return p;
}
uintptr_t wrapperVT[] = {uintptr_t(&deleteWrapper)}, primitiveVT[] = {uintptr_t(&deletePrimitive)}, rootVT[] = {uintptr_t(&deleteRoot)};
void* wrapper(void* primitive) {
    auto* result = std::calloc(1, 0x60);
    Write(result, 0, uintptr_t(wrapperVT)); Write(result, 0x10, primitive); return result;
}
uint32_t nativeAddChild(void* root, void* primitive, uint8_t clone) {
    assert(!locked && unitLocked && !clone);
    auto* entry = wrapper(primitive);
    const auto count = Read<uint32_t>(root, 0x20);
    for (uint32_t i = 0; i < count; ++i) if (!Read<void*>(Read<void*>(root, 0x30), i*8)) {
        Write(Read<void*>(root, 0x30), i*8, entry); return i;
    }
    if (count == Read<uint32_t>(root, 0x24))
        GenerationExtendArray(static_cast<uint8_t*>(root) + 0x18, count + 32);
    Write(Read<void*>(root, 0x30), count*8, entry); Write(root, 0x20, count + 1);
    return count;
}
int main() {
    reset(); unitManager = manager;
    oGenerationBlend = nativeBlend; oGenerationModel = nativeModel;
    oGenerationBaseModel = nativeBase; oGenerationResourceRelease = ResourceRelease;
    auto* model = resource(2); auto* shot = owner(model);
    pose(shot, 2); work(shot, 1); keys(shot, 1);
    Write(shot, 0x1250, uintptr_t(0xBAD)); // Live ownership must be masked, never collected.
    auto* originalPose = Read<void*>(shot, 0x538);
    auto* originalWork = Read<void*>(shot, 0x6C0);
    auto* originalKeys = Read<void*>(shot, 0x6D8);
    LifetimeTrace_SaveSnapshot(0);
    GenerationBlend(shot, 2);
    assert(Read<void*>(shot, 0x538) == originalPose && Read<void*>(shot, 0x6C0) != originalWork);
    assert(Read<uint32_t>(originalWork, 0) == 0xAB01 && ownedFrees == 0);
    auto* replacementWork = Read<void*>(shot, 0x6C0);
    LifetimeTrace_SaveSnapshot(1);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(0).shots[0], 1);
    assert(Read<void*>(shot, 0x6C0) == originalWork && Read<void*>(shot, 0x6D8) == originalKeys);
    assert(Read<uint32_t>(shot, 0x554) == 1 && Read<uint32_t>(originalWork, 0) == 0xAB01);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(1).shots[0], 2);
    assert(Read<void*>(shot, 0x6C0) == replacementWork && Read<uint32_t>(shot, 0x554) == 2);
    LifetimeTrace_DropSnapshot(0); CollectShotGenerations();
    assert(ownedFrees == 2 && Read<void*>(shot, 0x538) == originalPose);
    assert(Read<uintptr_t>(shot, 0) == 0xCAFE && Read<uintptr_t>(shot, 0x1250) == 0xBAD);
    LifetimeTrace_ClearSnapshots(); disposeModel(shot); std::free(model);

    reset(); unitManager = manager;
    oGenerationBlend = nativeBlend; oGenerationModel = nativeModel;
    oGenerationBaseModel = nativeBase; oGenerationResourceRelease = ResourceRelease;
    model = resource(2); auto* nextModel = resource(2); shot = owner(model);
    pose(shot, 2); work(shot, 1); keys(shot, 1);
    LifetimeTrace_SaveSnapshot(0); originalPose = Read<void*>(shot, 0x538);
    GenerationModel(shot, nextModel);
    assert(Read<void*>(shot, 0x538) == originalPose && Read<uint32_t>(model, 0x4C) == 1);
    assert(Read<uint32_t>(nextModel, 0x4C) == 2 && resourceAdds == 1);
    LifetimeTrace_SaveSnapshot(1);
    auto* thirdModel = resource(3);
    originalKeys = Read<void*>(shot, 0x6D8);
    GenerationModel(shot, thirdModel);
    assert(Read<uint32_t>(shot, 0x530) == 3 && Read<void*>(shot, 0x538) != originalPose);
    assert(Read<void*>(shot, 0x6D8) == originalKeys); // setModel does not replace the key slab.
    LifetimeTrace_SaveSnapshot(2);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(1).shots[0], 2);
    assert(Read<void*>(shot, 0x538) == originalPose && Read<void*>(shot, 0x100) == nextModel);
    assert(Read<void*>(originalPose, 0xA8) == shot && Read<uint32_t>(shot, 0x530) == 2);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(2).shots[0], 4);
    assert(Read<void*>(shot, 0x100) == thirdModel && Read<uint32_t>(shot, 0x530) == 3);
    GenerationModel(shot, nullptr);
    assert(!Read<void*>(shot, 0x100) && !Read<void*>(shot, 0x538));
    LifetimeTrace_DropSnapshot(0); CollectShotGenerations();
    assert(Read<uint32_t>(model, 0x4C) == 0 && Read<uint32_t>(nextModel, 0x4C) == 2);
    assert(!Read<void*>(originalPose, 0xA8)); // Logical retirement happened, vector storage is retained.
    LifetimeTrace_ClearSnapshots();
    assert(Read<uint32_t>(nextModel, 0x4C) == 1 && Read<uint32_t>(thirdModel, 0x4C) == 1 && resourceAdds == 2);
    disposeModel(shot); ResourceRelease(nextModel); std::free(nextModel); std::free(model);
    ResourceRelease(thirdModel); std::free(thirdModel);

    reset(); unitManager = manager;
    oGenerationBlend = nativeBlend; oGenerationModel = nativeModel;
    oGenerationBaseModel = nativeBase; oGenerationResourceRelease = ResourceRelease;
    model = resource(2); shot = owner();
    LifetimeTrace_SaveSnapshot(0); // Empty owned domains can later acquire native storage.
    GenerationModel(shot, model);
    assert(Read<void*>(shot, 0x538) && Read<void*>(shot, 0x6D8));
    LifetimeTrace_SaveSnapshot(1);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(0).shots[0], 1);
    assert(!Read<void*>(shot, 0x538) && !Read<void*>(shot, 0x6D8));
    StageShotGenerationStorage(*Find(shot, false), TestManifest(1).shots[0], 2);
    assert(Read<void*>(shot, 0x538) && Read<void*>(shot, 0x6D8));
    assert(Read<void*>(Read<void*>(shot, 0x538), 0xA8) == shot);
    LifetimeTrace_DropSnapshot(0); CollectShotGenerations();
    LifetimeTrace_ClearSnapshots(); disposeModel(shot);
    ResourceRelease(model); assert(Read<uint32_t>(model, 0x4C) == 0); std::free(model);

    reset(); unitManager = manager;
    oGenerationBlend = nativeBlend;
    shot = owner(); keys(shot, 1);
    LifetimeTrace_SaveSnapshot(0);
    originalKeys = Read<void*>(shot, 0x6D8);
    GenerationBlend(shot, 2); // Native early branch changes count, not allocation.
    assert(Read<void*>(shot, 0x6D8) == originalKeys);
    LifetimeTrace_SaveSnapshot(1);
    const auto& keySpans = TestManifest(1).shots[0].spans;
    bool keySpan = false;
    for (const auto& span : keySpans) if (!std::strcmp(span.name, "model.keyframes")) {
        assert(span.memory.size == 2*0xC); keySpan = true;
    }
    assert(keySpan); // ASan: never read the new count's larger, nonexistent slab.
    LifetimeTrace_ClearSnapshots(); disposeModel(shot);

    reset(); unitManager = manager;
    shot = owner();
    auto* root = std::calloc(1, 0xE0); Write(root, 0, uintptr_t(rootVT));
    auto* primitive = std::calloc(1, 0x70); Write(primitive, 0, uintptr_t(primitiveVT)); Write(primitive, 8, uint32_t(5));
    Write(primitive, 0x20, float(222));
    auto* populated = wrapper(primitive); auto* empty = wrapper(nullptr);
    auto* array = std::calloc(4, 8);
    Write(array, 0, populated); Write(array, 16, empty);
    Write(root, 0x20, uint32_t(3)); Write(root, 0x24, uint32_t(4)); Write(root, 0x30, array);
    Write(shot, 0x2058, root);
    LifetimeTrace_SaveSnapshot(0);
    ReleaseCollision(shot, 0, 0);
    assert(Read<uint32_t>(root, 0x20) == 3 && Read<uint32_t>(root, 0x24) == 4);
    auto* postimage = Read<void*>(root, 0x30);
    assert(!Read<void*>(postimage, 0) && !Read<void*>(postimage, 8) && Read<void*>(postimage, 16) == empty);
    assert(!wrapperDeletes && !primitiveDeletes && !rootDeletes);
    LifetimeTrace_SaveSnapshot(1);
    GenerationExtendArray(static_cast<uint8_t*>(root) + 0x18, 8);
    assert(Read<uint32_t>(root, 0x24) == 8 && Read<void*>(Read<void*>(root, 0x30), 16) == empty);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(0).shots[0], 1);
    assert(Read<void*>(shot, 0x2058) == root && Read<void*>(root, 0x30) == array);
    assert(Read<void*>(array, 0) == populated && Read<float>(primitive, 0x20) == 222);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(1).shots[0], 2);
    LifetimeTrace_DropSnapshot(0); CollectShotGenerations();
    assert(wrapperDeletes == 1 && primitiveDeletes == 1 && rootDeletes == 0);
    assert(Read<void*>(Read<void*>(root, 0x30), 16) == empty);
    ReleaseCollision(shot, 0, 1);
    assert(!Read<void*>(shot, 0x2058) && rootDeletes == 0);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(1).shots[0], 2);
    assert(Read<void*>(shot, 0x2058) == root && Read<void*>(Read<void*>(root, 0x30), 16) == empty);
    assert(rootDeletes == 0); // Full-release resurrection uses the retained root, not a factory.
    ReleaseCollision(shot, 0, 1);
    LifetimeTrace_ClearSnapshots();
    assert(wrapperDeletes == 2 && primitiveDeletes == 1 && rootDeletes == 1);
    disposeModel(shot);

    reset(); unitManager = manager;
    wrapperDeletes = primitiveDeletes = rootDeletes = 0;
    oGenerationAddChild = nativeAddChild;
    shot = owner(); root = std::calloc(1, 0xE0); Write(root, 0, uintptr_t(rootVT));
    Write(shot, 0x2058, root);
    LifetimeTrace_SaveSnapshot(0);
    primitive = std::calloc(1, 0x70);
    Write(primitive, 0, uintptr_t(primitiveVT)); Write(primitive, 8, uint32_t(5));
    assert(GenerationAddChild(root, primitive, 0) == 0);
    assert(Read<uint32_t>(root, 0x20) == 1 && Read<uint32_t>(root, 0x24) == 32);
    LifetimeTrace_SaveSnapshot(1);
    StageShotGenerationStorage(*Find(shot, false), TestManifest(0).shots[0], 1);
    assert(Read<uint32_t>(root, 0x20) == 0 && !Read<void*>(root, 0x30));
    LifetimeTrace_DropSnapshot(1); CollectShotGenerations();
    assert(wrapperDeletes == 1 && primitiveDeletes == 1 && rootDeletes == 0);
    ReleaseCollision(shot, 0, 1); LifetimeTrace_ClearSnapshots();
    assert(rootDeletes == 1);
    disposeModel(shot);
}
''')

    def test_future_shot_withdrawal_and_replay(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t units[0x1860]{}, shots[0x3450]{}, collision[0x73B0]{};
alignas(16) uint8_t config[0x4A8A0]{};
alignas(16) uint8_t lists[2][0x2550]{}, category[0x20]{}, group[0x50]{}, foreign[0x40]{};
int deletes = 0, releases = 0, unlinks = 0, kills = 0;
void* deleteRoot(void* p, uint32_t) { return p; }
uintptr_t rootVT[] = {uintptr_t(&deleteRoot)};
void* nativeDelete(void* p, uint32_t flags) {
    assert(!locked && unitLocked && flags == 1); ++deletes;
    ShotDtor(p);
    for (size_t field = 0x2058; field < 0x2078; field += 8) std::free(Read<void*>(p, field));
    ForgetConstraintObject(static_cast<uint8_t*>(p) + 0x2140);
    std::free(p); return p;
}
void* spawn() {
    void* shot = std::calloc(1, 0x2300);
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x10, uint32_t(2 | (13 << 3)));
    Write(shot, 0x116C, UINT32_MAX);
    const uint32_t id = Read<uint32_t>(shots, 0x438);
    assert(!Read<uint32_t>(shots, 0x38 + id*4));
    Write(shots, 0x38 + id*4, id); Write(shots, 0x438, id + 1);
    Write(shot, 0x1F50, id);
    for (size_t field = 0x2058; field < 0x2078; field += 8) {
        void* root = std::calloc(1, 0xE0); Write(root, 0, uintptr_t(rootVT));
        Write(root, 8, uint8_t(1)); Write(shot, field, root);
    }
    Birth(shot, Kind::Shot);
    auto* identity = Find(shot, false);
    identity->extent = 0x2300; identity->dti = 0x1234; identity->deleting = nativeDelete;
    void* constraint = static_cast<uint8_t*>(shot) + 0x2140;
    Write(constraint, 0x10, uint32_t(0x3FA));
    InitializeConstraint(constraint); Write(constraint, 0x60, uintptr_t(0x1234));
    auto* line = units + 0x50 + 13*0x30;
    void* next = Read<void*>(line, 0);
    Write(shot, 0x18, next); if (next) Write(next, 0x20, shot);
    Write(line, 0, shot); if (!next) Write(line, 8, shot);
    auto* sentinel = shotHeads[0] + 8;
    auto* node = static_cast<uint8_t*>(shot) + 0x1450;
    void* first = Read<void*>(sentinel, 8);
    Write(node, 8, first); Write(node, 0x10, static_cast<void*>(sentinel));
    if (first != sentinel) Write(first, 0x10, static_cast<void*>(node)); // Native: header prev stays stale.
    Write(sentinel, 8, static_cast<void*>(node));
    identity->shotLinked = true;
    return shot;
}
int main(int argc, char** argv) {
    UseTestPayloadArgument(argc, argv);
    reset(); unitManager = units; shotManager = shots; collisionManager = collision; gameConfig = config;
    Write(units, 0xC38, uint32_t(20));
    Write(units, 0x50 + 13*0x30, static_cast<void*>(foreign));
    Write(units, 0x58 + 13*0x30, static_cast<void*>(foreign));
    Write(foreign, 0x10, uint32_t(2 | (13 << 3)));
    auto* sentinel = shotHeads[0] + 8;
    Write(sentinel, 8, static_cast<void*>(sentinel)); Write(sentinel, 0x10, static_cast<void*>(sentinel));
    Write(shots, 0x438, uint32_t(11));
    void* passes[] = {lists[0], lists[1]}, *categories[] = {category}, *freeGroups[] = {group};
    Write(category, 8, uint32_t(2)); Write(category, 0x18, static_cast<void*>(passes));
    Write(collision, 0x7368 + 0x10, uint32_t(1));
    Write(collision, 0x7368 + 0x20, static_cast<void*>(categories));
    Write(collision, 0x7368 + 0x28, uint32_t(2));
    Write(lists[1], 0xFA0, uint32_t(1)); Write(lists[1], 0xFB0, static_cast<void*>(freeGroups));
    Write(group, 0x20, uint32_t(3)); // Includes an unused pass: one tick alone cannot clear it.
    oInitializeConstraint = [](void* p) { assert(!locked); Write(p, 0x48, p); };
    oConstraintObjectDtor = [](void*) { assert(!locked); };
    oKill = [](void*) { ++kills; };
    oRemoveUnit = [](void* manager, void* p) -> bool {
        assert(!locked && unitLocked && manager == units);
        auto* line = units + 0x50 + 13*0x30;
        void* next = Read<void*>(p, 0x18), *previous = Read<void*>(p, 0x20);
        if (previous) Write(previous, 0x18, next); else Write(line, 0, next);
        if (next) Write(next, 0x20, previous); else Write(line, 8, previous);
        Write(p, 0x18, uintptr_t(0)); Write(p, 0x20, uintptr_t(0)); return true;
    };
    oUnlinkShot = [](void* manager, uint32_t team, void* p) {
        assert(!locked && unitLocked && manager == shots && team == 0); ++unlinks;
        auto* node = static_cast<uint8_t*>(p) + 0x1450;
        void* next = Read<void*>(node, 8), *previous = Read<void*>(node, 0x10);
        Write(previous, 8, next); Write(next, 0x10, previous);
    };
    oReleaseShotId = [](void* manager, uint32_t id) {
        assert(!locked && unitLocked && manager == shots); ++releases;
        Write(shots, 0x38 + id*4, uint32_t(0));
    };
    oShotDtor = [](void* p) {
        UnlinkShot(shots, 0, p); ReleaseShotId(shots, Read<uint32_t>(p, 0x1F50));
    };
    LifetimeTrace_SaveSnapshot(775);
    Write(shots, 0x438, uint32_t(19)); Write(config, 0x4A89C, uint32_t(1));
    LifetimeTrace_RetireFutureShots(775); // Also rewind allocation state when a future shot already expired.
    assert(Read<uint32_t>(shots, 0x438) == 11 && !Read<uint32_t>(config, 0x4A89C));
    assert(!collisionRecycles && !unlinks && !releases);
    void* abandoned = spawn(); const uint64_t abandonedId = Find(abandoned, false)->id;
    Write(config, 0x4A89C, uint32_t(0x01010101));
    void* root = Read<void*>(abandoned, 0x2058);
    Write(shots, 0x448, abandoned); Write(shots, 0x450, uint64_t(13));
    Write(shots, 0x458, static_cast<void*>(foreign)); Write(shots, 0x460, uint64_t(9));
    Write(shots, 0x2448, uint32_t(2));
    LifetimeTrace_SaveSnapshot(776); // A newer snapshot leases the abandoned allocation.
    Write(lists[0], 0x20, uint32_t(1)); Write(lists[0], 0x540, uint32_t(1));
    Write(lists[1], 0xF80, uint32_t(1));
    LifetimeTrace_RetireFutureShots(775);
    assert(Find(abandoned, false)->occupancy == Occupancy::Quarantined && !deletes && !kills);
    assert((Read<uint32_t>(abandoned, 0x10)&0x3FF) == 0x3F8 && !Read<uint8_t>(root, 8));
    assert(!Read<void*>(static_cast<uint8_t*>(abandoned) + 0x2140, 0x60) && constraintUnlinks == 1);
    assert(Read<void*>(units, 0x50 + 13*0x30) == foreign && !Read<void*>(foreign, 0x20));
    assert(Read<void*>(sentinel, 8) == sentinel && Read<void*>(sentinel, 0x10) == sentinel);
    assert(!Read<uint32_t>(shots, 0x38 + 11*4) && Read<uint32_t>(shots, 0x438) == 11);
    assert(!Read<uint32_t>(config, 0x4A89C));
    assert(Read<uint32_t>(shots, 0x2448) == 1 && Read<void*>(shots, 0x448) == foreign);
    assert(Read<uint64_t>(shots, 0x450) == 9); // Keep the entire queue record, not just its pointer.
    assert(!Read<uint32_t>(collision, 0x7368 + 0x28) && collisionRecycles == 1);
    assert(!Read<uint32_t>(lists[0], 0x540) && !Read<uint32_t>(lists[1], 0xF80));
    assert(collisionGroupClears == 2 && !Read<uint32_t>(group, 0x20));
    LifetimeTrace_RetireFutureShots(775); // No duplicate logical retirement.
    assert(unlinks == 1 && releases == 1 && constraintUnlinks == 1);
    void* replayed = spawn();
    assert(Read<uint32_t>(replayed, 0x1F50) == 11 && replayed != abandoned);
    assert(Read<void*>(replayed, 0x18) == foreign);
    LifetimeTrace_SaveSnapshot(777);
    assert(TestManifest(777 % ROLLBACK_WINDOW).shots.size() == 1);
    assert(TestManifest(777 % ROLLBACK_WINDOW).shots[0].id == Find(replayed, false)->id);
    CollectQuarantine(); assert(!deletes); // Snapshot 776 still owns abandoned storage.
    auto* borrower = std::calloc(1, 0x2C0);
    Write(borrower, 0x280, abandoned); Birth(borrower, Kind::Effect);
    const uint64_t borrowerId = Find(borrower, false)->id;
    LifetimeTrace_DropSnapshot(776); CollectShotGenerations(); CollectQuarantine();
    assert(!deletes && Find(abandoned, false)->id == abandonedId); // A live unsupported effect is also an obligation.
    Destroyed("gone", borrower, borrowerId); std::free(borrower);
    Write(replayed, 0x2000, abandoned);
    CollectQuarantine(); assert(!deletes); // Another retained shot may still borrow its parent.
    Write(replayed, 0x2000, uintptr_t(0));
    auto* character = std::calloc(1, 0x68F0);
    Write(character, 0x6428, abandoned); Write(units, 0x50 + 8*0x30, character);
    testDti = _addr(0x140D44A30);
    CollectQuarantine(); assert(!deletes); // mpLastCreateShot is a native borrower too.
    Write(character, 0x6428, uintptr_t(0));
    CollectQuarantine(); assert(deletes == 1 && !Find(abandoned, false));
    Write(units, 0x50 + 8*0x30, uintptr_t(0)); std::free(character); testDti = 0;
    assert(unlinks == 1 && releases == 1 && Read<uint32_t>(shots, 0x38 + 11*4) == 11);
    LifetimeTrace_ClearSnapshots();
    { UnitGuard fence; nativeDelete(replayed, 1); }
    assert(deletes == 2 && releases == 2 && !g_quarantined && !kills);
}
''', arguments=[(), ("omit",)])

    def test_shot_cpu_kernel_byte_coverage_and_bounds(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset();
    constexpr size_t size = 0x2300;
    uint8_t live[size], saved[size], original[size], masks[size]{};
    for (size_t i = 0; i < size; ++i) {
        live[i] = uint8_t(i*13 + 0x80); saved[i] = uint8_t(i*37 + 0x11);
    }
    std::memcpy(original, live, size);
    const struct { size_t begin, end; } expected[] = {
        {0x08, 0x10}, {0x28, 0x2C}, {0x40, 0x100}, {0x55C, 0x560},
        {0x1180, 0x11B0}, {0x1240, 0x1250}, {0x1260, 0x129F},
        {0x1300, 0x1310}, {0x1310, 0x1344}, {0x1350, 0x1380},
        {0x1388, 0x13B8}, {0x13C0, 0x13C1}, {0x13C4, 0x13D4}, {0x13D8, 0x13E8},
        {0x1480, 0x148C}, {0x1490, 0x149C}, {0x14A0, 0x14C0},
        {0x1DD0, 0x1E30}, {0x1E30, 0x1F50}, {0x1F58, 0x1F68}, {0x1F70, 0x1FE0},
        {0x1FE4, 0x2000}, {0x2000, 0x2038}, {0x2038, 0x2058}, {0x2078, 0x20B8},
        {0x20C0, 0x20CC}, {0x20D0, 0x20D4}, {0x2118, 0x2133},
    };
    for (const auto& range : expected) std::memset(masks + range.begin, 0xFF, range.end - range.begin);
    for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18)
        std::memset(masks + record + 8, 0xFF, 0x10);
    for (size_t i = 0; i < 8; ++i) {
        const size_t base = 0x560 + i*0x180;
        std::memset(masks + base + 8, 0xFF, 6);
        std::memset(masks + base + 0x10, 0xFF, 0x64);
        std::memset(masks + base + 0x80, 0xFF, 0x60);
        for (size_t sampler : {size_t(0xE0), size_t(0x100)}) {
            std::memset(masks + base + sampler, 0xFF, 0xC);
            std::memset(masks + base + sampler + 0x10, 0xFF, 0xC);
        }
        std::memset(masks + base + 0x120, 0xFF, 0x40);
        std::memset(masks + base + 0x168, 0xFF, 4);
        std::memset(masks + base + 0x170, 0xFF, 8);
    }
    std::memset(masks + 0x149C, 0xFF, 4); masks[0x149C] = 0xBF;
    std::memset(masks + 0x1FE0, 0xFF, 4); masks[0x1FE3] = 0xEF;
    assert(RestoreShotCpuFields(live, size, saved, size) == ShotCpuRestoreStatus::ReviewedSubset);
    for (size_t i = 0; i < size; ++i)
        assert(live[i] == uint8_t((saved[i] & masks[i]) | (original[i] & ~masks[i])));
    // Includes borrowed pointer/history payloads and preserved topology/vtable
    // holes, audio cRefs, allocation bounds, presentation and derived suffix.
    std::memcpy(original, live, size);
    assert(RestoreShotCpuFields(nullptr, size, saved, size) == ShotCpuRestoreStatus::InvalidBuffer);
    assert(RestoreShotCpuFields(live, size, nullptr, size) == ShotCpuRestoreStatus::InvalidBuffer);
    assert(RestoreShotCpuFields(live, 0x213F, saved, size) == ShotCpuRestoreStatus::InvalidBuffer);
    assert(RestoreShotCpuFields(live, size, saved, 0x213F) == ShotCpuRestoreStatus::InvalidBuffer);
    assert(!std::memcmp(live, original, size));
    assert(RestoreShotCpuFields(live, size, live, size) == ShotCpuRestoreStatus::ReviewedSubset);
    assert(!std::memcmp(live, original, size) && !creates && !resourceAdds && !resourceReleases);
}
''')

    def test_restore_waits_only_for_outstanding_threaded_render(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset(); uint8_t render[0x108]{};
    LifetimeTrace_WaitForRenderWorker(); assert(!renderWaits);
    renderManager = render;
    for (int active : {0, 1}) for (int configured : {0, 1}) for (int latched : {0, 1}) {
        render[0x38] = uint8_t(active); render[0x3D] = uint8_t(configured); render[0x3E] = uint8_t(latched);
        const int before = renderWaits;
        LifetimeTrace_WaitForRenderWorker();
        assert(renderWaits == before + (active && latched));
        assert(render[0x38] == uint8_t(active && !latched));
        assert(render[0x3D] == configured && render[0x3E] == latched);
    }
    assert(!creates && !resourceAdds && !resourceReleases);
}
''')

    def test_shot_motion_bindings_and_external_work_rewind(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
void bindMotion(void* object, void* resource, uint32_t slot) {
    assert(!locked && unitLocked);
    if (void* old = Read<void*>(object, 0x11B8 + slot*8)) GenerationResourceRelease(old);
    Write(object, 0x11B8 + slot*8, resource); if (resource) ResourceAdd(resource);
}
int main() {
    reset(); alignas(16) uint8_t units[0x1860]{}; unitManager = units;
    uint8_t shot[0x2140]{};
    void* a = std::calloc(1, 0x400), *b = std::calloc(1, 0x400), *model = std::calloc(1, 0x100);
    Write(a, 0x4C, uint32_t(1)); Write(b, 0x4C, uint32_t(1)); Write(model, 0x4C, uint32_t(1));
    Write(model, 0x78, uint32_t(1));
    auto* jointHeader = static_cast<uint8_t*>(std::calloc(1, 0x10 + 0xC0));
    Write(jointHeader, 0, uint32_t(1)); Write(jointHeader + 0x10, 0, uintptr_t(mockJointVtable));
    auto* keys = static_cast<uint8_t*>(std::calloc(4, 0xC));
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    Write(shot, 0x100, model); Write(shot, 0x530, uint32_t(1)); Write(shot, 0x554, uint32_t(2));
    Write(shot, 0x538, jointHeader + 0x10); Write(shot, 0x1238, uint32_t(2));
    Write(shot, 0x11B8, a); oSetShotMotionList = bindMotion;
    for (size_t i = 0; i < 2; ++i) {
        const size_t base = 0x560 + i*0x180;
        void* work = std::calloc(1, 0x68); Write(work, 0x10, uint32_t(3));
        Write(shot, base + 0x160, work); Write(shot, base + 0x178, keys + i*2*0xC);
        Write(shot, base + 0x168, uint32_t(2)); Write(shot, base + 0x170, static_cast<uint8_t*>(a) + 0x280);
        for (size_t sampler : {size_t(0xE0), size_t(0x100)}) {
            Write(shot, base + sampler, static_cast<uint8_t*>(a) + 0x80);
            Write(shot, base + sampler + 8, 3.0f);
            Write(shot, base + sampler + 0x10, static_cast<uint8_t*>(a) + 0xC0);
            Write(shot, base + sampler + 0x18, uint32_t(2));
        }
        for (size_t page = 0; page < 4; ++page) {
            Write(shot, base + 0x120 + page*8, static_cast<uint8_t*>(a) + 0x140 + page*0x50);
            Write(shot, base + 0x140 + page*4, uint32_t(1u << page));
            Write(shot, base + 0x150 + page*4, uint32_t(0x10u << page));
        }
    }
    for (size_t key = 0; key < 4; ++key) {
        Write(keys, key*0xC, uint32_t(3)); Write(keys, key*0xC + 4, uint32_t(10));
        Write(keys, key*0xC + 8, uint32_t(9)); // Previous sampled value, not padding.
    }
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->extent = sizeof(shot); identity->dti = 0x1234;
    LifetimeTrace_SaveSnapshot(0); GenerationMotionList(shot, b, 0);
    assert(Read<uint32_t>(a, 0x4C) == 1);
    const int adds = resourceAdds, releases = resourceReleases;
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (size_t i = 0; i < 2; ++i) {
            const size_t base = 0x560 + i*0x180;
            std::memset(shot + base + 0xE0, 0xBB, 0x80);
            Write(shot, base + 0x168, uint32_t(0)); Write(shot, base + 0x170, (void*)nullptr);
            Write(Read<void*>(shot, base + 0x160), 0x10, uint32_t(11));
        }
        std::memset(keys, 0xDD, 4*0xC);
        StageShotGenerationStorage(*identity, TestManifest(0).shots[0], 1);
        { UnitGuard fence; StageShotCpuFields(*identity, TestManifest(0).shots[0], 1); }
        assert(Read<void*>(shot, 0x11B8) == a && resourceAdds == adds && resourceReleases == releases);
        for (size_t i = 0; i < 2; ++i) {
            const size_t base = 0x560 + i*0x180;
            assert(Read<void*>(shot, base + 0x178) == keys + i*2*0xC);
            assert(Read<uint32_t>(Read<void*>(shot, base + 0x160), 0x10) == 3);
            for (size_t sampler : {size_t(0xE0), size_t(0x100)}) {
                assert(Read<void*>(shot, base + sampler) == static_cast<uint8_t*>(a) + 0x80);
                assert(Read<float>(shot, base + sampler + 8) == 3);
                assert(Read<void*>(shot, base + sampler + 0x10) == static_cast<uint8_t*>(a) + 0xC0);
                assert(Read<uint32_t>(shot, base + sampler + 0xC) == 0xBBBBBBBB);
            }
            for (size_t page = 0; page < 4; ++page) {
                assert(Read<void*>(shot, base + 0x120 + page*8) == static_cast<uint8_t*>(a) + 0x140 + page*0x50);
                assert(Read<uint32_t>(shot, base + 0x140 + page*4) == (1u << page));
                assert(Read<uint32_t>(shot, base + 0x150 + page*4) == (0x10u << page));
            }
            assert(Read<uint32_t>(shot, base + 0x168) == 2);
            assert(Read<void*>(shot, base + 0x170) == static_cast<uint8_t*>(a) + 0x280);
        }
        for (size_t key = 0; key < 4; ++key)
            assert(Read<uint32_t>(keys, key*0xC) == 3 && Read<uint32_t>(keys, key*0xC + 4) == 10 &&
                   Read<uint32_t>(keys, key*0xC + 8) == 9);
    }
    const uint64_t id = identity->id;
    LifetimeTrace_ClearSnapshots(); ForgetCurrentShotGenerations(id, shot);
    ResourceRelease(a); ReleaseTerminalShotReferences(id); ResourceRelease(b); ResourceRelease(model);
    assert(!Read<uint32_t>(a, 0x4C) && !Read<uint32_t>(b, 0x4C));
    for (size_t i = 0; i < 2; ++i) std::free(Read<void*>(shot, 0x6C0 + i*0x180));
    MockJointDelete(jointHeader + 0x10, 3); std::free(keys); std::free(a); std::free(b); std::free(model);
}
''')

    def test_shot_dependency_adopts_untracked_unit(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int deletes = 0;
void* destroy(void* object, uint32_t flags) { assert(!locked && flags == 1); ++deletes; return object; }
int main() {
    reset(); alignas(16) uint8_t units[0x1860]{}, dti[0x40]{}; unitManager = units;
    uint8_t shot[0x2140]{}, target[0x40]{};
    uintptr_t vtable[] = {uintptr_t(&destroy)};
    Write(target, 0, uintptr_t(vtable)); Write(target, 0x10, uint32_t(0x3FA));
    Write(dti, 0x20, uint64_t(_addr(0x140E173D0))); testDti = uintptr_t(dti);
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    Write(shot, 0x13D8, static_cast<void*>(target));
    Birth(shot, Kind::Shot); auto* owner = Find(shot, false);
    owner->extent = sizeof(shot); owner->dti = 0x1234;
    LifetimeTrace_SaveSnapshot(0);
    auto* adopted = Find(target, false);
    assert(adopted && adopted->kind == Kind::Unit && adopted->snapshots == 1 && adopted->dependencies == 1);
    assert(TestManifest(0).count() == 2 && TestManifest(0).shots.size() == 1 && creates == 1);
    DeleteUnit(target, 1, destroy, 0);
    assert(!deletes && adopted->occupancy == Occupancy::Quarantined);
    Write(shot, 0x13D8, (void*)nullptr);
    { UnitGuard fence; StageShotCpuFields(*owner, TestManifest(0).shots[0], 1); }
    assert(Read<void*>(shot, 0x13D8) == target && !deletes);
    Write(shot, 0x13D8, (void*)nullptr);
    LifetimeTrace_DropSnapshot(0); CollectQuarantine();
    assert(deletes == 1 && !g_quarantined);
    ForgetCurrentShotGenerations(owner->id, shot);
}
''')

    def test_shot_borrowed_references_and_history_rewind(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset(); alignas(16) uint8_t units[0x1860]{}; unitManager = units;
    uint8_t shot[0x2140]{}, owner[0x40]{}, target[0x40]{}, callback[0x2C0]{};
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    Write(shot, 0x10, uint32_t(0x3FA));
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->dti = 0x1234; identity->extent = sizeof(shot);
    Birth(owner, Kind::Unit); Birth(target, Kind::Unit); Birth(callback, Kind::Effect);
    // Unsupported graph: the new borrowed dependency lease must still prevent
    // deleting dispatch, without claiming its simulation graph can be rewound.
    Write(callback, 0, uintptr_t(0xBAD)); Write(callback, 0x10, uint32_t(0x3FA));
    for (size_t field : {size_t(0x40), size_t(0x1308), size_t(0x1E38), size_t(0x1E40), size_t(0x2030)})
        Write(shot, field, static_cast<void*>(owner));
    Write(shot, 0x1480, static_cast<void*>(shot)); Write(shot, 0x1E30, static_cast<void*>(shot));
    Write(shot, 0x1F70, static_cast<void*>(shot));
    for (size_t field = 0x1F78; field < 0x1FE0; field += 8) Write(shot, field, static_cast<void*>(callback));
    Write(shot, 0x13D8, static_cast<void*>(callback));
    Write(shot, 0x2008, static_cast<void*>(target));
    for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18) {
        Write(shot, record, uint64_t(0xA000 + record));
        Write(shot, record + 8, static_cast<void*>(target));
        Write(shot, record + 0x10, uint32_t(record)); Write(shot, record + 0x14, 3.0f);
    }
    LifetimeTrace_SaveSnapshot(0); LifetimeTrace_SaveSnapshot(1);
    assert(Find(callback, false)->dependencies == 3 && Find(target, false)->dependencies == 3);
    assert(Find(owner, false)->dependencies == 3 && !identity->dependencies);
    assert(TestManifest(0).effects.empty());
    oDeleteEffect = +[](void* p, uint32_t) -> void* { assert(false); return p; };
    Write(callback, 0x10, uint32_t(0x3F8)); DeleteEffect(callback, 1);
    assert(Find(callback, false)->occupancy == Occupancy::Quarantined && g_quarantined == 1);
    const auto& saved = TestManifest(0).shots[0];
    for (int repeat = 0; repeat < 2; ++repeat) {
        VisitShotUnitReferences(shot, [&](size_t field, void*) { Write(shot, field, (void*)nullptr); });
        for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18) {
            Write(shot, record + 0x10, uint32_t(99)); Write(shot, record + 0x14, 11.0f);
        }
        { UnitGuard fence; StageShotCpuFields(*identity, saved, 1); }
        assert(Read<void*>(shot, 0x40) == owner && Read<void*>(shot, 0x1308) == owner);
        assert(Read<void*>(shot, 0x2030) == owner && Read<void*>(shot, 0x2008) == target);
        assert(Read<void*>(shot, 0x13D8) == callback && Read<void*>(shot, 0x1F70) == shot);
        for (size_t field = 0x1F78; field < 0x1FE0; field += 8) assert(Read<void*>(shot, field) == callback);
        for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18) {
            assert(Read<uint64_t>(shot, record) == 0xA000 + record);
            assert(Read<void*>(shot, record + 8) == target);
            assert(Read<uint32_t>(shot, record + 0x10) == record && Read<float>(shot, record + 0x14) == 3);
        }
        // No liveness filtering: the saved callback is restored exactly, but
        // publication must restore the callback's lifecycle before execution.
        assert((Read<uint32_t>(callback, 0x10) & 7) == 0);
        assert(HasLiveUnitReference(callback) && HasLiveUnitReference(target));
    }
    VisitShotUnitReferences(shot, [&](size_t field, void*) { Write(shot, field, (void*)nullptr); });
    LifetimeTrace_DropSnapshot(0); CollectQuarantine();
    assert(g_quarantined == 1 && Find(callback, false)->dependencies == 2);
    oDeleteEffect = +[](void* p, uint32_t) -> void* { assert(!locked); return p; };
    LifetimeTrace_DropSnapshot(1); CollectQuarantine();
    assert(!g_quarantined && !resourceAdds && !resourceReleases && !creates);
    ForgetCurrentShotGenerations(identity->id, shot);
}
''')

    def test_shot_cpu_existing_timer_rewind(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset(); alignas(16) uint8_t units[0x1860]{}; unitManager = units;
    auto* shot = static_cast<uint8_t*>(std::calloc(1, 0x21A0));
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x10, uint32_t(0x3FA));
    Write(shot, 0x116C, UINT32_MAX); Write(shot, 0x1F50, uint32_t(11));
    Write(shot, 0x1F58, 3.0f); Write(shot, 0x2078, 97.0f); Write(shot, 0x50, 6.0f);
    Write(shot, 0x14AC, 3.0f); Write(shot, 0x212C, uint32_t(0x80000001));
    void* model = std::calloc(1, 0x100); Write(model, 0x4C, uint32_t(1)); Write(model, 0x78, uint32_t(1));
    auto* jointHeader = static_cast<uint8_t*>(std::calloc(1, 0x10 + 0xC0));
    Write(jointHeader, 0, uint32_t(1)); void* joint = jointHeader + 0x10;
    Write(joint, 0, uintptr_t(mockJointVtable)); Write(joint, 0xA8, static_cast<void*>(shot)); Write(joint, 0x20, 6.0f);
    void* depth = std::calloc(1, 1), *interpolation = std::calloc(1, 0x40);
    Write(shot, 0x100, model); Write(shot, 0x530, uint32_t(1)); Write(shot, 0x538, joint);
    Write(shot, 0x548, depth); Write(shot, 0x11B0, interpolation);
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->extent = 0x21A0; identity->dti = 0x1234;
    LifetimeTrace_SaveSnapshot(280);
    const uint32_t bit = uint32_t(1) << (280 % ROLLBACK_WINDOW);
    const auto& saved = TestManifest(280 % ROLLBACK_WINDOW).shots[0];
    int attackSounds = 0, loopSounds = 0;
    const auto advance = [&] {
        Write(shot, 0x1F58, Read<float>(shot, 0x1F58) + 1);
        Write(shot, 0x14AC, Read<float>(shot, 0x14AC) + 1);
        Write(shot, 0x2078, Read<float>(shot, 0x2078) - 1);
        Write(shot, 0x50, Read<float>(shot, 0x50) + 2);
        Write(joint, 0x20, Read<float>(joint, 0x20) + 2);
        if (Read<float>(shot, 0x1F58) >= 8) Write(shot, 0x212C, uint32_t(0x80000003));
        if (!(Read<uint32_t>(shot, 0x149C) & 0x40)) {
            ++attackSounds; Write(shot, 0x149C, Read<uint32_t>(shot, 0x149C) | 0x40u);
        }
        if (!(Read<uint32_t>(shot, 0x1FE0) & 0x10000000)) {
            ++loopSounds; Write(shot, 0x1FE0, Read<uint32_t>(shot, 0x1FE0) | 0x10000000u);
        }
    };
    for (int i = 0; i < 8; ++i) advance();
    assert(Read<float>(shot, 0x1F58) == 11 && (Read<uint32_t>(shot, 0x212C) & 0xFF) == 3);
    Write(shot, 0x20E0, uint64_t(0xABCDEF)); // Live external sound-cookie identity.
    Write(shot, 0x2160, uint64_t(0x123456)); // Unreviewed derived callback remains live.
    for (int i = 0; i < 2; ++i) {
        { UnitGuard fence;
          StageShotGenerationStorage(*identity, saved, bit);
          assert(StageShotCpuFields(*identity, saved, bit) == ShotCpuRestoreStatus::ReviewedSubset); }
        assert(Read<float>(shot, 0x1F58) == 3 && Read<float>(shot, 0x14AC) == 3);
        assert(Read<float>(shot, 0x2078) == 97 && Read<float>(shot, 0x50) == 6);
        assert(Read<float>(joint, 0x20) == 6 && Read<void*>(joint, 0xA8) == shot);
        assert(Read<uint32_t>(shot, 0x212C) == 0x80000001);
        advance();
        assert(Read<float>(shot, 0x1F58) == 4 && Read<float>(shot, 0x14AC) == 4);
        assert(Read<float>(shot, 0x2078) == 96 && Read<float>(shot, 0x50) == 8);
        assert(Read<float>(joint, 0x20) == 8);
    }
    assert(attackSounds == 1 && loopSounds == 1);
    assert(Read<uint64_t>(shot, 0x20E0) == 0xABCDEF && Read<uint64_t>(shot, 0x2160) == 0x123456);
    identity->occupancy = Occupancy::Quarantined; ++g_quarantined;
    Write(shot, 0x10, uint32_t(0x3F8)); Write(shot, 0x1F50, uint32_t(99));
    { UnitGuard fence; StageShotGenerationStorage(*identity, saved, bit); StageShotCpuFields(*identity, saved, bit); }
    assert(Read<float>(shot, 0x1F58) == 3 && identity->occupancy == Occupancy::Quarantined && g_quarantined == 1);
    assert(Read<uint32_t>(shot, 0x10) == 0x3F8 && Read<uint32_t>(shot, 0x1F50) == 99);
    assert(Read<float>(saved.payload.data() + saved.spans[0].memory.offset, 0x1F58) == 3);
    // The stage did not publish a resurrection or restore reciprocal graphs.
    identity->occupancy = Occupancy::Live; --g_quarantined;
    const uint64_t id = identity->id;
    LifetimeTrace_ClearSnapshots(); ForgetCurrentShotGenerations(id, shot);
    MockJointDelete(joint, 3); std::free(depth); std::free(interpolation);
    ResourceRelease(model); std::free(model); std::free(shot);
}
''')

    def test_shot_cpu_stage_rejects_invalid_contract_before_writing(self):
        harness = CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t units[0x1860]{}, target[0x40]{};
void* shot;
void reject(int) { std::_Exit(Read<float>(shot, 0x1F58) == 11.0f && Read<float>(shot, 0x50) == 22.0f ? 17 : 18); }
int main(int argc, char** argv) {
    assert(argc == 2);
    reset(); unitManager = units; shot = std::calloc(1, 0x2140);
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX); Write(shot, 0x1F58, 3.0f);
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false); identity->extent = 0x2140; identity->dti = 0x1234;
    Birth(target, Kind::Unit); Write(shot, 0x2008, static_cast<void*>(target));
    LifetimeTrace_SaveSnapshot(0); auto& saved = TestManifest(0).shots[0];
    Write(shot, 0x1F58, 11.0f); Write(shot, 0x50, 22.0f); std::signal(SIGABRT, reject);
'''
        invalid_cases = (
            "identity->snapshots = 0;",
            "saved.spans[0].memory.offset = UINT32_MAX;",
            "saved.spans.push_back(saved.spans[0]);",
            "Find(target, false)->snapshots = 0;",
            "Destroyed(\"test_dispose\", target, Find(target, false)->id); Birth(target, Kind::Unit); Find(target, false)->snapshots = 1;",
            "Write(saved.payload.data() + saved.spans[0].memory.offset, 0x2008, uintptr_t(1));",
        )
        cases = "\n".join(f"case {i}: {{ {invalid} break; }}" for i, invalid in enumerate(invalid_cases))
        self.compile_run(True, harness + "\nswitch (std::atoi(argv[1])) {\n" + cases +
                         "\n}\nStageShotCpuFields(*identity, saved, 1);\n}\n",
                         expected_returncode=17, arguments=[[str(i)] for i in range(len(invalid_cases))])

    def test_shot_storage_leaves_materials_and_vjobs_live(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset(); alignas(16) uint8_t units[0x1860]{}; unitManager = units;
    auto* shot = static_cast<uint8_t*>(std::calloc(1, 0x2140));
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    // Poison presentation pointers: capture must not inspect their pointees or
    // reject a snapshot based on material/vjob configuration.
    for (size_t field : {size_t(0x108), size_t(0x110), size_t(0x170), size_t(0x198),
                         size_t(0x1250), size_t(0x1258), size_t(0x12A0)}) Write(shot, field, uintptr_t(1));
    for (size_t field = 0x1408; field < 0x1440; field += 8) Write(shot, field, uintptr_t(1));
    Write(shot, 0x118, UINT32_MAX); Write(shot, 0x1440, UINT32_MAX);
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->extent = 0x2140; identity->dti = 0x1234;
    LifetimeTrace_SaveSnapshot(0);
    const auto& saved = TestManifest(0).shots[0];
    assert(saved.spans.size() == 1 && !std::strcmp(saved.spans[0].name, "body"));
    assert(saved.payload.size() == 0x2140 && saved.generations.size() == ShotDomainCount);
    struct Range { size_t offset, size; } ranges[] = {
        {0x108, 0x408}, {0x1250, 0x10}, {0x12A0, 0x20}, {0x1408, 0x48}
    };
    for (const auto& range : ranges) std::memset(shot + range.offset, 0xA5, range.size);
    StageShotGenerationStorage(*identity, saved, 1);
    for (const auto& range : ranges)
        for (size_t i = 0; i < range.size; ++i) assert(shot[range.offset + i] == 0xA5);
    assert(!resourceAdds && !resourceReleases && !creates && !ownedFrees);
    const uint64_t id = identity->id;
    LifetimeTrace_ClearSnapshots(); ForgetCurrentShotGenerations(id, shot); std::free(shot);
}
''')

    def test_shot_command_and_motion_resource_borrowers(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t units[0x1860]{};
void* a; void* b; void* c;
int nativeDeletes = 0;
void nativeCommand(void* owner, void* incoming, uint32_t slot) {
    assert(!locked && unitLocked);
    const size_t field = 0x13E8 + slot*8;
    if (void* old = Read<void*>(owner, field)) GenerationResourceRelease(old);
    Write(owner, field, incoming);
    if (incoming) { Write(owner, 0x1360 + slot*8, incoming); ResourceAdd(incoming); }
}
void nativeMotion(void* owner, void* incoming, uint32_t slot) {
    assert(!locked && unitLocked);
    const size_t field = 0x11B8 + slot*8;
    if (void* old = Read<void*>(owner, field)) GenerationResourceRelease(old);
    Write(owner, field, incoming); if (incoming) ResourceAdd(incoming);
}
void* resource() {
    void* object = std::calloc(1, 0x100); Write(object, 0x4C, uint32_t(1)); return object;
}
void* dispose(void* owner, uint32_t flags) {
    assert(!locked && unitLocked && flags == 1); ++nativeDeletes;
    // A concrete destructor can still consume the old resource after every
    // owned slot and interpreter mirror has changed. Do not drain it early.
    assert(Read<uint32_t>(a, 0x4C) >= 1 && Read<uint32_t>(b, 0x4C) >= 1);
    for (size_t field = 0x11B8; field < 0x1238; field += 8)
        if (void* resource = Read<void*>(owner, field)) ResourceRelease(resource);
    for (size_t field = 0x13E8; field < 0x1408; field += 8)
        if (void* resource = Read<void*>(owner, field)) ResourceRelease(resource);
    const uint64_t id = Find(owner, false)->id;
    Destroyed("gone", owner, id); std::free(owner); return owner;
}
int main() {
    reset(); unitManager = units;
    oSetShotCommandResource = nativeCommand; oSetShotMotionList = nativeMotion;
    oGenerationResourceRelease = ResourceRelease;
    a = resource(); b = resource(); c = resource();
    void* shot = std::calloc(1, 0x2140); Write(shot, 0, uintptr_t(0xCAFE));
    Write(shot, 0x10, uint32_t(0x3FA)); Write(shot, 0x116C, UINT32_MAX);
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->extent = 0x2140; identity->dti = 0x1234; identity->deleting = dispose;
    Write(shot, 0x13E8, a); Write(shot, 0x1360, a); // Transfer a's initial native reference.
    for (size_t field = 0x1390; field < 0x13B8; field += 8)
        Write(shot, field, static_cast<uint8_t*>(a) + 0x80 + (field - 0x1390));
    Write(shot, 0x1388, uint32_t(2)); Write(shot, 0x138C, uint32_t(7));
    LifetimeTrace_SaveSnapshot(0);
    GenerationCommandResource(shot, b, 0); assert(Read<uint32_t>(a, 0x4C) == 1);
    // The B acquisition below has no intervening snapshot, but the live
    // interpreter may resolve it. Retention must not depend on capture frequency.
    GenerationCommandResource(shot, c, 0); assert(Read<uint32_t>(b, 0x4C) == 2);
    GenerationCommandResource(shot, nullptr, 0);
    assert(Read<void*>(shot, 0x1360) == c && !Read<void*>(shot, 0x13E8));
    for (size_t field = 0x1390; field < 0x13B8; field += 8) Write(shot, field, static_cast<void*>(c));
    Write(shot, 0x138C, uint32_t(19));
    LifetimeTrace_SaveSnapshot(1);
    StageShotGenerationStorage(*identity, TestManifest(0).shots[0], 1);
    { UnitGuard fence; StageShotCpuFields(*identity, TestManifest(0).shots[0], 1); }
    assert(Read<void*>(shot, 0x1360) == a && Read<uint32_t>(shot, 0x138C) == 7);
    for (size_t field = 0x1390; field < 0x13B8; field += 8)
        assert(Read<void*>(shot, field) == static_cast<uint8_t*>(a) + 0x80 + (field - 0x1390));
    assert(Read<void*>(shot, 0x13E8) == a && Read<uint32_t>(a, 0x4C) == 1);
    GenerationCommandResource(shot, a, 0); // Same pointer is two native ownership occurrences.
    assert(Read<uint32_t>(a, 0x4C) == 2);
    for (uint32_t slot = 0; slot < 16; ++slot) GenerationMotionList(shot, b, slot);
    assert(Read<uint32_t>(b, 0x4C) == 18); // Caller + retired command + sixteen motion refs.
    for (uint32_t slot = 0; slot < 16; ++slot) GenerationMotionList(shot, nullptr, slot);
    LifetimeTrace_ClearSnapshots();
    assert(Read<uint32_t>(a, 0x4C) == 2 && Read<uint32_t>(b, 0x4C) == 18 && Read<uint32_t>(c, 0x4C) == 2);
    assert(!HasRetiredShotGenerations(identity->id)); // Terminal-only pins must not deadlock owner GC.
    DeleteUnit(shot, 1, dispose, 0);
    assert(nativeDeletes == 1 && Read<uint32_t>(a, 0x4C) == 0);
    assert(Read<uint32_t>(b, 0x4C) == 1 && Read<uint32_t>(c, 0x4C) == 1);
    for (const auto& generation : g_shotGenerations) assert(!generation.id);
    ResourceRelease(b); ResourceRelease(c); std::free(a); std::free(b); std::free(c);
}
''')

    def test_constraint_initialization_and_address_only_teardown(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset(); uint8_t constraint[0x90]{}; int initializations = 0, destructions = 0;
    oInitializeConstraint = +[](void* object) { assert(!locked); Write(object, 0x48, object); };
    oConstraintObjectDtor = +[](void*) { assert(!locked); };
    InitializeConstraint(constraint); ++initializations;
    const uint64_t first = g_constructedConstraints.at(constraint);
    InitializeConstraint(constraint); ++initializations;
    assert(g_constructedConstraints.at(constraint) > first && Read<void*>(constraint, 0x48) == constraint);
    // Re-initialization renews the identity without counting the address twice.
    assert(ConstraintBucket(constraint) == 1);
    // With no shot there is no chain to discover: nothing is captured or walked.
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).constraints.empty() && TestManifest(0).constraintsComplete &&
        TestManifest(0).constraintWatermark == g_nextConstraintConstruction);
    LifetimeTrace_ClearSnapshots();
    // The hook is MtObject's base destructor. An object that was never a
    // constraint must pass without the recorder lock, even while it is held.
    static int unlockedTeardowns = 0;
    oConstraintObjectDtor = +[](void*) { ++unlockedTeardowns; };
    uint8_t unrelated[0x90]{};
    AcquireSRWLockExclusive(&g_lock);
    ForgetConstraintObject(unrelated); ForgetConstraintObject((void*)1);
    ReleaseSRWLockExclusive(&g_lock);
    assert(unlockedTeardowns == 2 && g_constructedConstraints.size() == 1);
    oConstraintObjectDtor = +[](void*) { assert(!locked); };
    // No DTI/vptr reads, even for unrelated objects or already-freed addresses.
    ForgetConstraintObject((void*)1); ++destructions;
    ForgetConstraintObject(constraint); ++destructions;
    assert(!ConstraintBucket(constraint));
    ForgetConstraintObject(constraint); ++destructions;
    assert(g_constructedConstraints.empty() && initializations == 2 && destructions == 3 && !creates);
    // Every registration is balanced: no bucket is left counting a dead address.
    uint8_t many[64][0x90]{};
    for (auto& object : many) InitializeConstraint(object);
    for (auto& object : many) ForgetConstraintObject(object);
    for (const auto& bucket : g_constraintBuckets) assert(!bucket);
}
''')

    def test_shot_constraint_graph_restore_and_preflight(self):
        harness = CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t units[0x1860]{};
void *a, *b, *n0, *n1, *n2, *n3;
bool executionFixture = false;
uint8_t beforeA[0x2400], beforeB[0x2400], beforeJA[2*0xC0], beforeJB[2*0xC0], beforeN1[0x90];
uint8_t beforeDA[2], beforeDB[2];
void* joint(void* owner, size_t i) { return static_cast<uint8_t*>(Read<void*>(owner, 0x538)) + i*0xC0; }
void initialize(void* object) {
    assert(!locked); Write(object, 0, uintptr_t(0xC0A5)); Write(object, 0x40, uintptr_t(0xFACE));
    Write(object, 0x48, object); Write(object, 0x10, uint32_t(0x3F8)); Write(object, 0x70, int32_t(-1));
}
void* make() {
    auto* owner = static_cast<uint8_t*>(std::calloc(1, 0x2400));
    auto* header = static_cast<uint8_t*>(std::calloc(1, 0x10 + 2*0xC0));
    Write(header, 0, uint32_t(2));
    for (size_t i = 0; i < 2; ++i) {
        Write(header + 0x10 + i*0xC0, 0, uintptr_t(mockJointVtable));
        Write(header + 0x10 + i*0xC0, 0xA8, static_cast<void*>(owner));
        Write(header + 0x10 + i*0xC0, 0xA4, uint32_t(0x01030009 + i));
        Write(header + 0x10 + i*0xC0, 0x14, uint32_t(7 + i));
    }
    auto* depths = static_cast<uint8_t*>(std::calloc(2, 1)); depths[0] = 1; depths[1] = 0;
    Write(owner, 0, uintptr_t(0xCAFE)); Write(owner, 0x116C, UINT32_MAX);
    Write(owner, 0x530, uint32_t(2)); Write(owner, 0x538, header + 0x10); Write(owner, 0x548, depths);
    Write(owner, 0x558, uint32_t(0x01000015)); Write(owner, 0x123D, uint8_t(0));
    Write(owner, 0x1F58, 3.0f);
    Birth(owner, Kind::Shot); auto* identity = Find(owner, false);
    identity->extent = 0x2400; identity->dti = 0x1234; return owner;
}
void chain(void* model, size_t index, std::initializer_list<void*> entries) {
    void* target = joint(model, index); void* previous = nullptr; int order = 2;
    Write(target, 8, entries.size() ? static_cast<uint8_t*>(*entries.begin()) + 0x40 : nullptr);
    for (void* node : entries) {
        Write(node, 0x50, previous); Write(node, 0x58, (void*)nullptr);
        if (previous) Write(previous, 0x58, node);
        Write(node, 0x60, target); Write(node, 0x70, int32_t(order)); order += 5;
        Write(node, 0x74, uint32_t(1000)); Write(node, 0x78, model);
        Write(node, 0x68, 0.25f); Write(node, 0x80, 0.02f); Write(node, 0x84, 0.5f);
        Write(node, 0x88, uint8_t(1)); Write(node, 0x89, uint8_t(1)); previous = node;
    }
}
void future() {
    Write(joint(a, 1), 8, (void*)nullptr);
    chain(b, 1, {n2, n0, n3, n1});
    for (void* owner : {a, b}) {
        Write(owner, 0x558, uint32_t(0xFFFFFFFF)); Write(owner, 0x123D, uint8_t(1)); Write(owner, 0x1F58, 11.0f);
        std::memset(Read<void*>(owner, 0x548), 0xEE, 2);
        for (size_t i = 0; i < 2; ++i) {
            Write(joint(owner, i), 0x14, uint32_t(99)); Write(joint(owner, i), 0xA4, uint32_t(0xDEADBEEF));
        }
    }
    for (void* node : {n0, n1, n2}) {
        Write(node, 0x68, 0.9f); Write(node, 0x74, uint32_t(500));
        std::memset(static_cast<uint8_t*>(node) + 0x8A, 0xEF, 6);
    }
}
void setup() {
    reset(); unitManager = units; oInitializeConstraint = initialize;
    oConstraintObjectDtor = +[](void*) { assert(!locked); };
    a = make(); b = make();
    n0 = static_cast<uint8_t*>(a) + 0x2140; n2 = static_cast<uint8_t*>(a) + 0x21D0;
    n3 = static_cast<uint8_t*>(a) + 0x2260;
    n1 = std::calloc(1, 0x90); // Foreign node is reached through the complete native chain.
    for (void* node : {n0, n1, n2}) InitializeConstraint(node);
    chain(a, 1, {n0, n1, n2});
    if (executionFixture) {
        Write(units, 0xC38, uint32_t(2)); Write(units, 0x1840, uint32_t(1));
        Write(a, 0x10, uint32_t(0x3FA)); Write(b, 0x10, uint32_t(0x3FA));
        void* previous = nullptr; int rno = 42;
        auto* group = static_cast<uint8_t*>(std::calloc(3, 8)); size_t i = 0;
        for (void* node : {n0, n1, n2}) {
            Write(node, 8, int32_t(rno++)); Write(node, 0x28, 1.5f);
            Write(node, 0x10, uint32_t(0x40A)); Write(node, 0x30, uint64_t(1));
            Write(node, 0x20, previous); if (previous) Write(previous, 0x18, node);
            Write(node, 0x18, (void*)nullptr); Write(group, (i++)*8, node); previous = node;
        }
        Write(units, 0x80, n0); Write(units, 0x88, n2);
        Write(units, 0xC58, uint32_t(3)); Write(units, 0xC5C, uint32_t(3)); Write(units, 0xC68, group);
    }
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).constraints.size() == 3);
    InitializeConstraint(n3); future();
}
void remember() {
    std::memcpy(beforeA, a, sizeof(beforeA)); std::memcpy(beforeB, b, sizeof(beforeB));
    std::memcpy(beforeJA, joint(a, 0), sizeof(beforeJA)); std::memcpy(beforeJB, joint(b, 0), sizeof(beforeJB));
    std::memcpy(beforeDA, Read<void*>(a, 0x548), 2); std::memcpy(beforeDB, Read<void*>(b, 0x548), 2);
    std::memcpy(beforeN1, n1, sizeof(beforeN1));
}
void swapPose() {
    UnitGuard fence;
    RetireModelDomain(*Find(a, false), ShotDomain::Pose, true);
    auto* header = static_cast<uint8_t*>(std::calloc(1, 0x10 + 3*0xC0));
    Write(header, 0, uint32_t(3)); Write(header + 0x10, 0, uintptr_t(mockJointVtable));
    Write(a, 0x538, header + 0x10); Write(a, 0x548, std::calloc(3, 1)); Write(a, 0x530, uint32_t(3));
    EnsureShotGeneration(*Find(a, false), ShotDomain::Pose);
    Write(joint(b, 1), 8, (void*)nullptr); chain(a, 2, {n2, n0, n3, n1});
}
void reject(int) {
    const bool unchanged = !std::memcmp(beforeA, a, sizeof(beforeA)) && !std::memcmp(beforeB, b, sizeof(beforeB)) &&
        !std::memcmp(beforeJA, joint(a, 0), sizeof(beforeJA)) && !std::memcmp(beforeJB, joint(b, 0), sizeof(beforeJB)) &&
        !std::memcmp(beforeDA, Read<void*>(a, 0x548), 2) && !std::memcmp(beforeDB, Read<void*>(b, 0x548), 2) &&
        !std::memcmp(beforeN1, n1, sizeof(beforeN1));
    std::_Exit(unchanged ? 17 : 18);
}
void clean() {
    LifetimeTrace_ClearSnapshots();
    for (void* node : {n0, n1, n2, n3}) ForgetConstraintObject(node);
    for (void* owner : {a, b}) {
        const uint64_t id = Find(owner, false)->id;
        ForgetCurrentShotGenerations(id, owner); { UnitGuard fence; MockCollectModel(owner); }
        Destroyed("gone", owner, id); std::free(owner);
    }
    std::free(n1);
}
'''
        self.compile_run(True, harness + r'''
int main() {
    setup(); const auto history = testSnapshots[0];
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto plan = PrepareShotConstraintGraph(TestManifest(0), 1);
        for (const auto& shot : TestManifest(0).shots) {
            auto* owner = Find(shot.spans[0].memory.address, false);
            StageShotGenerationStorage(*owner, shot, 1); { UnitGuard fence; StageShotCpuFields(*owner, shot, 1); }
        }
        StageShotConstraintGraph(plan);
        assert(Read<void*>(joint(a, 1), 8) == static_cast<uint8_t*>(n0) + 0x40 && !Read<void*>(joint(b, 1), 8));
        void* previous = nullptr; int order = 2;
        for (void* node : {n0, n1, n2}) {
            assert(Read<void*>(node, 0x48) == node && Read<void*>(node, 0x50) == previous);
            assert(Read<void*>(node, 0x60) == joint(a, 1) && Read<void*>(node, 0x78) == a);
            assert(Read<int32_t>(node, 0x70) == order && Read<uint32_t>(node, 0x74) == 1000);
            assert(Read<float>(node, 0x68) == 0.25f && Read<float>(node, 0x84) == 0.5f);
            assert(Read<uint8_t>(node, 0x8A) == 0xEF && Read<uint8_t>(node, 0x8F) == 0xEF);
            previous = node; order += 5;
        }
        assert(Read<void*>(n0, 0x58) == n1 && Read<void*>(n1, 0x58) == n2 && !Read<void*>(n2, 0x58));
        assert(!Read<void*>(n3, 0x50) && !Read<void*>(n3, 0x58) && !Read<void*>(n3, 0x60));
        assert(Read<int32_t>(n3, 0x70) == -1);
        for (void* owner : {a, b}) {
            assert(Read<uint32_t>(owner, 0x558) == 0x01000015 && !Read<uint8_t>(owner, 0x123D));
            assert(Read<uint8_t>(Read<void*>(owner, 0x548), 0) == 1 && !Read<uint8_t>(Read<void*>(owner, 0x548), 1));
            for (size_t i = 0; i < 2; ++i)
                assert(Read<uint32_t>(joint(owner, i), 0x14) == 7 + i &&
                       Read<uint32_t>(joint(owner, i), 0xA4) == 0x01030009 + i);
            Write(owner, 0x1F58, Read<float>(owner, 0x1F58) + 1); assert(Read<float>(owner, 0x1F58) == 4);
        }
        future();
    }
    assert(!creates && !constraintUnlinks && !resourceAdds && !resourceReleases);
    clean();
    size_t exported = 0;
    LifetimeTrace_VisitSnapshot(0, history.get(), false,
        [](void* context, const char* name, const void*, uint64_t, uint64_t, const void* bytes, size_t size) {
            if (!std::strstr(name, "lifetime.constraint.")) return;
            ++*static_cast<size_t*>(context); assert(size == 0x90 && Read<uint32_t>(bytes, 0x74) == 1000);
        }, &exported);
    assert(exported == 3); // Historical export after all native storage has been freed.
}
''')
        self.compile_run(True, harness + r'''
int main() {
    setup(); swapPose();
    void* displaced = joint(a, 2);
    auto plan = PrepareShotConstraintGraph(TestManifest(0), 1);
    for (const auto& shot : TestManifest(0).shots) {
        auto* owner = Find(shot.spans[0].memory.address, false); StageShotGenerationStorage(*owner, shot, 1);
    }
    StageShotConstraintGraph(plan);
    assert(!Read<void*>(displaced, 8) && Read<void*>(n0, 0x60) == joint(a, 1));
    assert(Read<void*>(joint(a, 1), 8) == static_cast<uint8_t*>(n0) + 0x40);
    CollectShotGenerations(); // ASan: collection of the displaced pose is now safe.
    clean();
}
''')
        self.compile_run(True, harness + r'''
int main() {
    setup();
    // Constraint model links can be the only edge retaining a detached owner.
    Find(b, false)->occupancy = Occupancy::Quarantined; ++g_quarantined;
    Write(b, 0x10, uint32_t(0x3F8)); LifetimeTrace_SaveSnapshot(1);
    assert(Find(b, false)->dependencies == 2 && TestManifest(1).shots.size() == 2);
    LifetimeTrace_DropSnapshot(0); CollectShotGenerations(); CollectQuarantine();
    assert(g_quarantined == 1 && Find(b, false)->occupancy == Occupancy::Quarantined);
    auto plan = PrepareShotConstraintGraph(TestManifest(1), 2); StageShotConstraintGraph(plan);
    assert(Read<void*>(n0, 0x78) == b && !creates && !resourceAdds && !resourceReleases);
    Find(b, false)->occupancy = Occupancy::Live; --g_quarantined;
    clean();
}
''')
        self.compile_run(True, harness + r'''
int main() {
    executionFixture = true; setup();
    Write(units, 0x50, n3); Write(units, 0x58, n2); Write(units, 0x80, n0); Write(units, 0x88, n0);
    Write(n3, 0x18, n2); Write(n3, 0x20, (void*)nullptr); Write(n3, 0x10, uint32_t(0x402)); Write(n3, 0x30, uint64_t(1));
    Write(n2, 0x18, (void*)nullptr); Write(n2, 0x20, n3); Write(n2, 0x10, uint32_t(0x402)); Write(n2, 0x30, uint64_t(0));
    Write(n0, 0x18, (void*)nullptr); Write(n0, 0x20, (void*)nullptr); Write(n0, 0x10, uint32_t(0x40B));
    Write(n1, 0x18, (void*)nullptr); Write(n1, 0x20, (void*)nullptr); Write(n1, 0x10, uint32_t(0x3F8));
    Write(n1, 0x30, uint64_t(0));
    void* group = Read<void*>(units, 0xC68); Write(group, 0, n3); Write(group, 8, n0); Write(units, 0xC58, uint32_t(2));
    for (void* node : {n0, n1, n2}) { Write(node, 8, int32_t(99)); Write(node, 0x28, 9.0f); }
    for (int repeat = 0; repeat < 2; ++repeat) {
        auto plan = PrepareShotConstraintGraph(TestManifest(0), 1);
        for (const auto& shot : TestManifest(0).shots) {
            auto* owner = Find(shot.spans[0].memory.address, false);
            StageShotGenerationStorage(*owner, shot, 1); { UnitGuard fence; StageShotCpuFields(*owner, shot, 1); }
        }
        StageShotConstraintGraph(plan); StageShotExecutionState(TestManifest(0), 1);
        assert(!Read<void*>(units, 0x50) && !Read<void*>(units, 0x58));
        assert(Read<void*>(units, 0x80) == n0 && Read<void*>(units, 0x88) == n2);
        assert(Read<void*>(n0, 0x18) == n1 && Read<void*>(n1, 0x18) == n2 && !Read<void*>(n2, 0x18));
        assert(!Read<void*>(n0, 0x20) && Read<void*>(n1, 0x20) == n0 && Read<void*>(n2, 0x20) == n1);
        int rno = 42;
        for (void* node : {n0, n1, n2}) {
            assert(Read<uint32_t>(node, 0x10) == 0x40A && Read<uint64_t>(node, 0x30) == 1);
            assert(Read<int32_t>(node, 8) == rno++ && Read<float>(node, 0x28) == 1.5f);
        }
        assert((Read<uint32_t>(n3, 0x10) & 0x3FF) == 0x3F8 && !Read<uint64_t>(n3, 0x30));
        assert(!Read<void*>(n3, 0x18) && !Read<void*>(n3, 0x20));
        assert(Read<uint32_t>(units, 0xC58) == 3 && Read<void*>(group, 0) == n0 && Read<void*>(group, 8) == n1 &&
               Read<void*>(group, 16) == n2 && !g_quarantined && !groupExpansions);
    }
    clean(); std::free(group);
}
''')
        invalid_cases = (
            "ForgetConstraintObject(n1);",
            "InitializeConstraint(n0);",
            "Write(joint(b, 1), 8, uintptr_t(1));",
            "Write(n1, 0x58, n2);",
            "TestManifest(0).constraints.push_back(TestManifest(0).constraints[0]);",
            "Write(TestManifest(0).constraints[0].image, 0x78, uintptr_t(1));",
            "TestManifest(0).constraintsComplete = false;",
            "auto* foreign = std::calloc(1, 0x90); InitializeConstraint(foreign); LifetimeTrace_SaveSnapshot(0); chain(b, 1, {n2, n0, n3, n1, foreign});",
            "auto* foreign = std::calloc(1, 0x90); InitializeConstraint(foreign); chain(b, 1, {n2, n0, n3, n1, foreign});",
            "auto plan = PrepareShotConstraintGraph(TestManifest(0), 1); InitializeConstraint(n0); remember(); StageShotConstraintGraph(plan);",
            "auto plan = PrepareShotConstraintGraph(TestManifest(0), 1); LifetimeTrace_SaveSnapshot(0); remember(); StageShotConstraintGraph(plan);",
            "swapPose(); auto plan = PrepareShotConstraintGraph(TestManifest(0), 1); for (const auto& shot : TestManifest(0).shots) StageShotGenerationStorage(*Find(shot.spans[0].memory.address, false), shot, 1); StageShotConstraintGraph(plan); CollectShotGenerations(); remember(); StageShotConstraintGraph(plan);",
        )
        cases = "\n".join(f"case {i}: {{ {invalid} break; }}" for i, invalid in enumerate(invalid_cases))
        self.compile_run(True, harness + "\nint main(int argc, char** argv) { assert(argc == 2); setup(); "
                         "std::signal(SIGABRT, reject); switch (std::atoi(argv[1])) {\n" + cases +
                         "\n}\nremember(); PrepareShotConstraintGraph(TestManifest(0), 1);\n}\n",
                         expected_returncode=17, arguments=[[str(i)] for i in range(len(invalid_cases))])

    def test_constraint_unknown_interface_is_not_dereferenced_or_partially_restored(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
uint8_t owner[0x2140]{}, header[0x10 + 0xC0]{}, depths[1]{};
void reject(int) { std::_Exit(Read<uintptr_t>(header + 0x10, 8) == 1 ? 17 : 18); }
int main() {
    reset(); uint8_t units[0x1860]{}; unitManager = units;
    Write(owner, 0, uintptr_t(0xCAFE)); Write(owner, 0x116C, UINT32_MAX);
    Write(header, 0, uint32_t(1)); Write(header + 0x10, 0, uintptr_t(mockJointVtable));
    Write(header + 0x10, 8, uintptr_t(1));
    Write(owner, 0x538, header + 0x10); Write(owner, 0x548, static_cast<void*>(depths));
    Write(owner, 0x530, uint32_t(1)); Birth(owner, Kind::Shot);
    Find(owner, false)->extent = sizeof(owner); Find(owner, false)->dti = 0x1234;
    LifetimeTrace_SaveSnapshot(0);
    assert(!TestManifest(0).constraintsComplete && TestManifest(0).constraints.empty());
    bool diagnostic = false;
    LifetimeTrace_VisitSnapshot(0, testSnapshots[0].get(), false,
        [](void* context, const char* name, const void*, uint64_t, uint64_t, const void* bytes, size_t) {
            if (std::strcmp(name, "diagnostic.lifetime.constraints")) return;
            *static_cast<bool*>(context) = true; assert(!Read<uint64_t>(bytes, 0));
        }, &diagnostic);
    assert(diagnostic);
    std::signal(SIGABRT, reject); PrepareShotConstraintGraph(TestManifest(0), 1);
}
''', expected_returncode=17)

    def test_shot_constraint_chain_exceeds_native_sort_limit(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset(); alignas(16) uint8_t units[0x1860]{}; unitManager = units;
    auto* owner = static_cast<uint8_t*>(std::calloc(1, 0x4000));
    auto* header = static_cast<uint8_t*>(std::calloc(1, 0x10 + 0xC0));
    Write(header, 0, uint32_t(1)); Write(header + 0x10, 0, uintptr_t(mockJointVtable));
    auto* joint = header + 0x10; auto* depths = static_cast<uint8_t*>(std::calloc(1, 1));
    Write(owner, 0, uintptr_t(0xCAFE)); Write(owner, 0x116C, UINT32_MAX);
    Write(owner, 0x538, joint); Write(owner, 0x548, depths); Write(owner, 0x530, uint32_t(1));
    Birth(owner, Kind::Shot); auto* identity = Find(owner, false); identity->extent = 0x4000; identity->dti = 0x1234;
    oInitializeConstraint = +[](void* node) {
        Write(node, 0, uintptr_t(0xA)); Write(node, 0x40, uintptr_t(0xB)); Write(node, 0x48, node);
    };
    oConstraintObjectDtor = +[](void*) { assert(!locked); };
    for (size_t i = 0; i < 40; ++i) {
        void* node = owner + 0x2140 + i*0x90; InitializeConstraint(node);
        Write(node, 0x50, i ? owner + 0x2140 + (i-1)*0x90 : nullptr);
        Write(node, 0x58, i+1 < 40 ? owner + 0x2140 + (i+1)*0x90 : nullptr);
        Write(node, 0x60, static_cast<void*>(joint)); Write(node, 0x78, static_cast<void*>(owner));
        Write(node, 0x70, int32_t(i*3)); Write(node, 0x74, uint32_t(1000));
    }
    Write(joint, 8, owner + 0x2140 + 0x40); LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).constraints.size() == 40 && TestManifest(0).constraintsComplete);
    // Reverse the current chain, preserving equal priorities.
    for (size_t i = 0; i < 40; ++i) {
        void* node = owner + 0x2140 + i*0x90;
        Write(node, 0x50, i+1 < 40 ? owner + 0x2140 + (i+1)*0x90 : nullptr);
        Write(node, 0x58, i ? owner + 0x2140 + (i-1)*0x90 : nullptr);
    }
    Write(joint, 8, owner + 0x2140 + 39*0x90 + 0x40);
    auto plan = PrepareShotConstraintGraph(TestManifest(0), 1); StageShotConstraintGraph(plan);
    void* node = static_cast<uint8_t*>(Read<void*>(joint, 8)) - 0x40;
    for (size_t i = 0; i < 40; ++i) {
        assert(node == owner + 0x2140 + i*0x90 && Read<int32_t>(node, 0x70) == int32_t(i*3));
        node = Read<void*>(node, 0x58);
    }
    assert(!node && !constraintUnlinks && !creates);
    LifetimeTrace_ClearSnapshots();
    for (size_t i = 0; i < 40; ++i) ForgetConstraintObject(owner + 0x2140 + i*0x90);
    ForgetCurrentShotGenerations(identity->id, owner); { UnitGuard fence; MockCollectModel(owner); }
    std::free(owner);
}
''')

    def test_shot_execution_restore_stage(self):
        harness = CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t units[0x1860]{}, oldForeign[0x40]{}, newForeign[0x40]{};
void *a, *b, *c, *detached;
void* make(uint32_t flags, uint64_t mask) {
    void* shot = std::calloc(1, 0x2140);
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x116C, UINT32_MAX);
    Write(shot, 0x10, flags | (0x7Fu << 3)); Write(shot, 0x30, mask);
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->extent = 0x2140; identity->dti = 0x1234; return shot;
}
void line(unsigned category, std::initializer_list<void*> entries) {
    void* descriptor = units + 0x50 + category*0x30;
    Write(descriptor, 0, entries.size() ? *entries.begin() : nullptr);
    void* previous = nullptr;
    for (void* object : entries) {
        Write(object, 0x20, previous); Write(object, 0x18, (void*)nullptr);
        if (previous) Write(previous, 0x18, object);
        Write(object, 0x10, (Read<uint32_t>(object, 0x10) & ~0x3F8u) | (category << 3));
        previous = object;
    }
    Write(descriptor, 8, previous);
}
void group(unsigned index, std::initializer_list<void*> entries) {
    void* descriptor = units + 0xC40 + index*0x30;
    if (auto* old = Read<void*>(descriptor, 0x28)) std::free(old);
    void* data = std::calloc(entries.size(), 8); size_t i = 0;
    for (void* object : entries) Write(data, (i++)*8, object);
    Write(descriptor, 0x18, uint32_t(entries.size())); Write(descriptor, 0x1C, uint32_t(entries.size()));
    Write(descriptor, 0x28, data);
}
void setup() {
    reset(); unitManager = units; Write(units, 0xC38, uint32_t(2)); Write(units, 0x1840, uint32_t(2));
    a = make(0x2402, 1); b = make(0x401, 1); c = make(3, 2); detached = make(1, 0);
    Write(oldForeign, 0x10, uint32_t(2)); Write(oldForeign, 0x30, uint64_t(1));
    Write(newForeign, 0x10, uint32_t(2)); Write(newForeign, 0x30, uint64_t(1));
    line(0, {oldForeign, a, b}); line(1, {c});
    group(0, {b, oldForeign, a}); group(1, {c});
    LifetimeTrace_SaveSnapshot(0);
    // Preserve native category descriptor controls, not invented counters/cursors.
    Write(units, 0x48, uint32_t(0xABCDEF)); Write(units, 0x60, 0.5f);
    line(0, {newForeign, c, oldForeign}); line(1, {a});
    Write(a, 0x10, uint32_t(4 | (1 << 3) | 0x800)); Write(a, 0x30, uint64_t(2));
    Write(b, 0x10, uint32_t(0x3F8 | 0x800)); Write(b, 0x30, uint64_t(0));
    Write(b, 0x18, (void*)nullptr); Write(b, 0x20, (void*)nullptr);
    Find(b, false)->occupancy = Occupancy::Quarantined; Find(b, false)->futureShot = true; ++g_quarantined;
    group(0, {oldForeign, newForeign}); group(1, {a, c});
}
void reject(int) {
    const bool untouched = Read<void*>(units, 0x50) == newForeign && Read<void*>(units, 0x80) == a &&
        Read<uint32_t>(a, 0x10) == (4 | (1 << 3) | 0x800) &&
        Find(b, false)->occupancy == Occupancy::Quarantined && g_quarantined == 1 && !groupExpansions;
    std::_Exit(untouched ? 17 : 18);
}
'''
        self.compile_run(True, harness + r'''
int main() {
    setup(); StageShotExecutionState(TestManifest(0), 1);
    assert(Read<void*>(units, 0x50) == oldForeign && Read<void*>(oldForeign, 0x18) == a);
    assert(Read<void*>(a, 0x18) == b && Read<void*>(b, 0x18) == newForeign);
    assert(Read<void*>(units, 0x58) == newForeign && !Read<void*>(newForeign, 0x18));
    assert(!Read<void*>(oldForeign, 0x20) && Read<void*>(b, 0x20) == a);
    assert(Read<void*>(units, 0x80) == c && Read<void*>(units, 0x88) == c);
    assert(!Read<void*>(c, 0x18) && !Read<void*>(c, 0x20));
    assert(Read<uint32_t>(a, 0x10) == (0x2402 | 0x800));
    assert(Read<uint32_t>(b, 0x10) == (0x401 | 0x800) && (Read<uint32_t>(c, 0x10) & 7) == 3);
    assert((Read<uint32_t>(detached, 0x10) & 0x3FF) == 0x3F9);
    assert(Read<uint64_t>(a, 0x30) == 1 && Read<uint64_t>(b, 0x30) == 1);
    void* group0 = Read<void*>(units, 0xC68);
    assert(Read<uint32_t>(units, 0xC58) == 4 && groupExpansions == 1);
    assert(Read<void*>(group0, 0) == b && Read<void*>(group0, 8) == oldForeign);
    assert(Read<void*>(group0, 16) == a && Read<void*>(group0, 24) == newForeign);
    assert(Read<uint32_t>(units, 0xC88) == 1 && Read<void*>(Read<void*>(units, 0xC98), 0) == c);
    assert(Read<uint32_t>(units, 0x48) == 0xABCDEF && Read<float>(units, 0x60) == 0.5f);
    assert(!g_quarantined && Find(b, false)->occupancy == Occupancy::Live && !Find(b, false)->futureShot);
    StageShotExecutionState(TestManifest(0), 1);
    assert(!g_quarantined && groupExpansions == 1 && !creates && !unitLocked);
    LifetimeTrace_ClearSnapshots();
    for (void* object : {a, b, c, detached}) {
        ForgetCurrentShotGenerations(Find(object, false)->id, object); std::free(object);
    }
    std::free(Read<void*>(units, 0xC68)); std::free(Read<void*>(units, 0xC98));
}
''')
        self.compile_run(True, harness + r'''
int main() {
    setup();
    // A later snapshot can retain a dead shot through a borrowed reference.
    // Rewinding earlier revives it; restoring the later slot must detach it again.
    Write(a, 0x2008, b); LifetimeTrace_SaveSnapshot(1);
    assert(Find(b, false)->dependencies == 2);
    StageShotExecutionState(TestManifest(0), 1);
    assert(!g_quarantined && Find(b, false)->occupancy == Occupancy::Live);
    StageShotExecutionState(TestManifest(1), 2);
    assert(g_quarantined == 1 && Find(b, false)->occupancy == Occupancy::Quarantined);
    assert((Read<uint32_t>(b, 0x10) & 0x3FF) == 0x3F8 && !Read<uint64_t>(b, 0x30));
    assert(!Read<void*>(b, 0x18) && !Read<void*>(b, 0x20));
    assert(Read<void*>(units, 0x50) == newForeign && Read<void*>(newForeign, 0x18) == c);
    assert(Read<void*>(c, 0x18) == oldForeign && Read<void*>(units, 0x80) == a);
    StageShotExecutionState(TestManifest(1), 2); assert(g_quarantined == 1);
    Find(b, false)->occupancy = Occupancy::Live; --g_quarantined;
    LifetimeTrace_ClearSnapshots();
    for (void* object : {a, b, c, detached}) {
        ForgetCurrentShotGenerations(Find(object, false)->id, object); std::free(object);
    }
    std::free(Read<void*>(units, 0xC68)); std::free(Read<void*>(units, 0xC98));
}
''')
        invalid_cases = (
            "Find(b, false)->snapshots = 0;",
            "TestManifest(0).primary[0].push_back(a);",
            "TestManifest(0).groups[0].clear();",
            "Write(units, 0xC60, uint8_t(1));",
            "make(2, 0);",
            "g_liveSnapshots[0] = nullptr;",
            "TestManifest(0).shots[0].spans.push_back(TestManifest(0).shots[0].spans[0]);",
        )
        cases = "\n".join(f"case {i}: {{ {invalid} break; }}" for i, invalid in enumerate(invalid_cases))
        self.compile_run(True, harness + "\nint main(int argc, char** argv) { assert(argc == 2); setup(); "
                         "std::signal(SIGABRT, reject); switch (std::atoi(argv[1])) {\n" + cases +
                         "\n}\nStageShotExecutionState(TestManifest(0), 1);\n}\n",
                         expected_returncode=17, arguments=[[str(i)] for i in range(len(invalid_cases))])

    def test_shot_manager_restore_stage(self):
        harness = CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t units[0x1860]{}, manager[0x3450]{}, config[0x4A8A0]{};
alignas(16) uint8_t oldForeign[0x40]{}, newForeign[0x40]{};
void *a, *b;
void* head(unsigned team) { return shotHeads[team] + 8; }
void* node(void* shot) { return static_cast<uint8_t*>(shot) + 0x1450; }
void link(unsigned team, std::initializer_list<void*> shots) {
    void* previous = head(team);
    for (void* shot : shots) {
        Write(previous, 8, node(shot)); Write(node(shot), 0x10, previous); previous = node(shot);
    }
    // Native insertAfter never updates the header's previous link: leave it stale.
    Write(previous, 8, head(team)); Write(head(team), 0x10, head(team));
}
void* make(unsigned id) {
    void* shot = std::calloc(1, 0x2140);
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x10, uint32_t(0x3FA));
    Write(shot, 0x116C, UINT32_MAX); Write(shot, 0x1F50, id);
    Write(shot, 0x1F58, 3.0f);
    Write(shot, 0x1450, uintptr_t(0xBEEF)); Write(shot, 0x1468, shot);
    Birth(shot, Kind::Shot);
    auto* identity = Find(shot, false); identity->extent = 0x2140; identity->dti = 0x1234;
    identity->shotLinked = true;
    Write(manager, 0x38 + id*4, id);
    return shot;
}
void setup() {
    reset(); unitManager = units; shotManager = manager; gameConfig = config;
    Write(units, 0xC38, uint32_t(20));
    for (unsigned team = 0; team < 10; ++team) link(team, {});
    a = make(1); b = make(2); link(0, {b, a});
    Write(manager, 0x438, uint32_t(3)); Write(manager, 0x43C, uint32_t(7));
    Write(manager, 0x440, uint32_t(8)); Write(manager, 0x444, 0.5f);
    Write(manager, 0x448, b); Write(manager, 0x450, uint64_t(0xABCDEF120000000D));
    Write(manager, 0x458, static_cast<void*>(oldForeign)); Write(manager, 0x460, uint64_t(8));
    Write(manager, 0x2448, uint32_t(2)); Write(config, 0x4A89C, uint32_t(0x01000100));
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).shotLists[0].size() == 2);
    assert(TestManifest(0).shotLists[0][0].id == Find(b, false)->id);
    assert(TestManifest(0).shotPending[0].id == Find(b, false)->id);
    assert(!TestManifest(0).shotPending[1].id);
    // A reflected live shot and a terminally retained shot take the same stage.
    link(0, {}); link(2, {a});
    Write(a, 0x1F50, uint32_t(99)); Write(a, 0x1F54, uint32_t(2));
    Find(b, false)->occupancy = Occupancy::Quarantined; ++g_quarantined;
    Find(b, false)->shotLinked = false; Find(b, false)->shotRegistrationReleased = true;
    Write(b, 0x1458, uintptr_t(0)); Write(b, 0x1460, uintptr_t(0));
    Write(b, 0x10, uint32_t(0x3F8)); Write(manager, 0x40, uint32_t(0));
    Write(a, 0x1F58, 19.0f); Write(b, 0x1F58, 19.0f);
    Write(manager, 0x438, uint32_t(91)); Write(manager, 0x43C, uint64_t(0));
    Write(manager, 0x444, 2.0f); Write(manager, 0x2450, uint64_t(0xCAFE));
    Write(config, 0x4A89C, uint32_t(0));
    Write(manager, 0x448, a); Write(manager, 0x450, uint64_t(13));
    Write(manager, 0x458, static_cast<void*>(newForeign)); Write(manager, 0x460, uint64_t(9));
    // Registration must not touch foreign execution lists or the shot bodies.
    Write(units, 0x50, static_cast<void*>(newForeign)); Write(units, 0x58, static_cast<void*>(newForeign));
}
void reject(int) {
    const bool untouched = Read<uint32_t>(manager, 0x438) == 91 &&
        Read<void*>(head(2), 8) == node(a) && !Read<uint32_t>(manager, 0x40) &&
        Find(b, false)->occupancy == Occupancy::Quarantined && g_quarantined == 1;
    std::_Exit(untouched ? 17 : 18);
}
'''
        scenarios = [
            (r'''
    StageShotManagerState(TestManifest(0), 1);
    assert(Read<void*>(head(0), 8) == node(b) && Read<void*>(node(b), 8) == node(a));
    assert(Read<void*>(node(a), 8) == head(0) && Read<void*>(head(0), 0x10) == node(a));
    assert(Read<void*>(node(b), 0x10) == head(0) && Read<void*>(node(a), 0x10) == node(b));
    assert(Read<void*>(head(2), 8) == head(2) && Read<void*>(head(2), 0x10) == head(2));
    assert(Read<uintptr_t>(a, 0x1450) == 0xBEEF && Read<void*>(a, 0x1468) == a);
    assert(Read<uint32_t>(manager, 0x3C) == 1 && Read<uint32_t>(manager, 0x40) == 2);
    assert(Read<uint32_t>(a, 0x1F50) == 1 && Read<uint32_t>(a, 0x1F54) == 0);
    assert(Read<uint32_t>(manager, 0x438) == 3 && Read<uint32_t>(manager, 0x43C) == 7);
    assert(Read<uint32_t>(manager, 0x440) == 8 && Read<float>(manager, 0x444) == 0.5f);
    assert(Read<uint64_t>(manager, 0x2450) == 0xCAFE); // Scratch is not ownership/registration state.
    assert(Read<uint32_t>(config, 0x4A89C) == 0x01000100);
    assert(Read<uint32_t>(manager, 0x2448) == 2 && Read<void*>(manager, 0x448) == b);
    assert(Read<uint64_t>(manager, 0x450) == 0xABCDEF120000000D);
    assert(Read<void*>(manager, 0x458) == newForeign && Read<uint64_t>(manager, 0x460) == 9);
    assert(Find(b, false)->shotLinked && !Find(b, false)->shotRegistrationReleased);
    assert(Find(b, false)->occupancy == Occupancy::Quarantined && g_quarantined == 1);
    assert(Read<float>(a, 0x1F58) == 19.0f && Read<float>(b, 0x1F58) == 19.0f);
    assert(Read<uint32_t>(b, 0x10) == 0x3F8 && Read<void*>(units, 0x50) == newForeign);
    StageShotManagerState(TestManifest(0), 1); // No list/count duplication.
    assert(Read<uint32_t>(manager, 0x2448) == 2 && g_quarantined == 1 && !unitLocked);
    std::free(a); std::free(b);
''', 0),
            (r'''
    Find(b, false)->snapshots = 0;
    StageShotManagerState(TestManifest(0), 1);
''', 17),
            (r'''
    // A nonempty list must be circular through its sentinel, not null-terminated.
    Write(node(a), 8, static_cast<void*>(nullptr));
    StageShotManagerState(TestManifest(0), 1);
''', 17),
            (r'''
    // Even a future shot absent from both list and queue must be retired first.
    make(3);
    StageShotManagerState(TestManifest(0), 1);
''', 17),
            (r'''
    // Both saved bodies claiming native ID 1 must fail before any list write.
    for (auto& saved : TestManifest(0).shots) {
        const auto& span = saved.spans[0];
        Write(saved.payload.data() + span.memory.offset, 0x1F50, uint32_t(1));
    }
    Write(b, 0x1F50, uint32_t(1));
    StageShotManagerState(TestManifest(0), 1);
''', 17),
            (r'''
    // A foreign pending record may not be dropped just to fit restored shots.
    Write(manager, 0x2448, uint32_t(512));
    for (size_t i = 0; i < 512; ++i) {
        Write(manager, 0x448 + i*0x10, static_cast<void*>(newForeign));
        Write(manager, 0x450 + i*0x10, uint64_t(9));
    }
    StageShotManagerState(TestManifest(0), 1);
''', 17),
        ]
        for scenario, expected in scenarios:
            with self.subTest(expected=expected, scenario=scenario):
                self.compile_run(True, harness + "\nint main() { setup(); std::signal(SIGABRT, reject);\n" +
                                 scenario + "\n}\n", expected_returncode=expected)

    def test_saved_shot_restore_transaction(self):
        harness = CHECKS[:CHECKS.index("int main() {")] + r"""
alignas(16) uint8_t units[0x1860]{}, manager[0x3450]{}, config[0x4A8A0]{}, effect[0x40]{}, other[0x40]{};
uint8_t *a, *b, *constraint, *later, *joint;
void* head(unsigned team) { return shotHeads[team] + 8; }
void* node(void* shot) { return static_cast<uint8_t*>(shot) + 0x1450; }
void link(std::initializer_list<void*> shots) {
    void* previous = head(0);
    for (void* shot : shots) {
        Write(previous, 8, node(shot)); Write(node(shot), 0x10, previous); previous = node(shot);
    }
    // Native insertAfter never updates the header's previous link: leave it stale.
    Write(previous, 8, head(0)); Write(head(0), 0x10, head(0));
}
void line(unsigned category, std::initializer_list<void*> entries) {
    void* descriptor = units + 0x50 + category*0x30;
    Write(descriptor, 0, entries.size() ? *entries.begin() : nullptr);
    void* previous = nullptr;
    for (void* object : entries) {
        Write(object, 0x20, previous); Write(object, 0x18, (void*)nullptr);
        if (previous) Write(previous, 0x18, object);
        Write(object, 0x10, (Read<uint32_t>(object, 0x10) & ~0x3F8u) | (category << 3));
        previous = object;
    }
    Write(descriptor, 8, previous);
}
uint8_t* make(unsigned id, size_t size, uintptr_t dti) {
    auto* shot = static_cast<uint8_t*>(std::calloc(1, size));
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x10, uint32_t(2)); Write(shot, 0x116C, UINT32_MAX);
    Write(shot, 0x1450, uintptr_t(0xBEEF)); Write(shot, 0x1468, static_cast<void*>(shot));
    Write(shot, 0x1F50, id); Write(shot, 0x1F58, 3.0f); Write(shot, 0x2078, 97.0f);
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->extent = uint32_t(size); identity->dti = dti; identity->shotLinked = true;
    Write(manager, 0x38 + id*4, id);
    return shot;
}
void advance(uint8_t* shot) {
    Write(shot, 0x1F58, Read<float>(shot, 0x1F58) + 1); Write(shot, 0x2078, Read<float>(shot, 0x2078) - 1);
    if (shot == a) {
        Write(a, 0x2140, Read<float>(a, 0x2140) + 100); Write(a, 0x2190, Read<float>(a, 0x2190) + 0.5f);
        Write(joint, 0x20, Read<float>(joint, 0x20) + 2);
    }
}
void setup() {
    reset(); unitManager = units; shotManager = manager; gameConfig = config;
    Write(units, 0xC38, uint32_t(2)); Write(units, 0x1840, uint32_t(0));
    oInitializeConstraint = +[](void* object) {
        Write(object, 0, uintptr_t(0xC0A5)); Write(object, 0x40, uintptr_t(0xFACE));
        Write(object, 0x48, object); Write(object, 0x10, uint32_t(2)); Write(object, 0x70, int32_t(-1));
    };
    oConstraintObjectDtor = +[](void*) {};
    Birth(effect, Kind::Unit); Birth(other, Kind::Unit);
    // uShotVulcanAxe: the class owns one resource reference at +0x2218.
    a = make(5, 0x2400, 0x140D48808); b = make(6, 0x2140, 0x1234);
    void* model = std::calloc(1, 0x100); Write(model, 0x4C, uint32_t(1)); Write(model, 0x78, uint32_t(1));
    auto* jointHeader = static_cast<uint8_t*>(std::calloc(1, 0x10 + 0xC0));
    Write(jointHeader, 0, uint32_t(1)); joint = jointHeader + 0x10;
    Write(joint, 0, uintptr_t(mockJointVtable)); Write(joint, 0xA8, static_cast<void*>(a)); Write(joint, 0x20, 6.0f);
    Write(a, 0x100, model); Write(a, 0x530, uint32_t(1)); Write(a, 0x538, joint);
    Write(a, 0x548, std::calloc(1, 1)); Write(a, 0x11B0, std::calloc(1, 0x40));
    Write(a, 0x2140, 150.0f); Write(a, 0x2190, 0.125f);                  // Laser-style tail scalars.
    Write(a, 0x2160, static_cast<void*>(effect));                          // Borrowed unit in the tail.
    Write(a, 0x2218, uintptr_t(0x51AB));                                   // Owned resource reference.
    constraint = a + 0x2240; InitializeConstraint(constraint);
    Write(constraint, 0x60, static_cast<void*>(joint)); Write(constraint, 0x70, int32_t(2));
    Write(constraint, 0x78, static_cast<void*>(a)); Write(constraint, 0x68, 0.25f);
    Write(constraint, 0xA0, 7.0f);                                         // Derived TSR payload past +0x90.
    Write(joint, 8, static_cast<void*>(constraint + 0x40));
    line(0, {a, b}); line(1, {constraint}); link({b, a});
    Write(manager, 0x438, uint32_t(7)); Write(config, 0x4A89C, uint32_t(1));
}
void future() {
    for (int i = 0; i < 8; ++i) { advance(a); advance(b); }
    Write(a, 0x2160, static_cast<void*>(other)); Write(a, 0x2218, uintptr_t(0x51AC));
    // The constraint detached and stopped executing; its payload moved on.
    Write(joint, 8, (void*)nullptr); Write(constraint, 0x60, (void*)nullptr); Write(constraint, 0x70, int32_t(-1));
    Write(constraint, 0x68, 0.9f); Write(constraint, 0xA0, 9.0f);
    Write(constraint, 0x10, uint32_t(0x3F8));
    // A second inline constraint was initialized after the snapshot and took the joint.
    later = a + 0x2300; InitializeConstraint(later);
    Write(later, 0x60, static_cast<void*>(joint)); Write(later, 0x70, int32_t(2));
    Write(joint, 8, static_cast<void*>(later + 0x40)); line(1, {later});
    // b died: detached, unregistered and held, exactly as the deleting hook leaves it.
    line(0, {a}); link({a}); Write(b, 0x10, uint32_t(0x3F8));
    Write(b, 0x18, (void*)nullptr); Write(b, 0x20, (void*)nullptr);
    Write(b, 0x1458, (void*)nullptr); Write(b, 0x1460, (void*)nullptr); Write(manager, 0x38 + 6*4, uint32_t(0));
    auto* identity = Find(b, false); identity->occupancy = Occupancy::Quarantined; ++g_quarantined;
    identity->shotLinked = false; identity->shotRegistrationReleased = true;
    Write(manager, 0x438, uint32_t(40));
}
void check() {
    assert(Read<float>(a, 0x1F58) == 3 && Read<float>(a, 0x2078) == 97 && Read<float>(b, 0x1F58) == 3);
    assert(Read<float>(a, 0x2140) == 150 && Read<float>(a, 0x2190) == 0.125f && Read<float>(joint, 0x20) == 6);
    assert(Read<void*>(a, 0x2160) == effect);
    assert(Read<uintptr_t>(a, 0x2218) == 0x51AC);                          // Ownership stays live.
    assert(Read<void*>(joint, 8) == constraint + 0x40 && Read<void*>(constraint, 0x60) == joint);
    assert(Read<int32_t>(constraint, 0x70) == 2 && Read<float>(constraint, 0x68) == 0.25f);
    assert(Read<float>(constraint, 0xA0) == 7 && Read<uintptr_t>(constraint, 0) == 0xC0A5);
    assert(Read<void*>(units, 0x50) == a && Read<void*>(a, 0x18) == b && Read<void*>(units, 0x58) == b);
    assert(Read<void*>(units, 0x80) == constraint && (Read<uint32_t>(constraint, 0x10) & 0x3FF) == (2 | (1 << 3)));
    // The later constraint keeps its constructed bytes but no longer executes or links anywhere.
    assert(Read<void*>(units, 0x88) == constraint && !Read<void*>(constraint, 0x18));
    assert(Read<uintptr_t>(later, 0) == 0xC0A5 && Read<void*>(later, 0x48) == later && !Read<void*>(later, 0x60));
    assert(Read<int32_t>(later, 0x70) == -1 && (Read<uint32_t>(later, 0x10) & 0x3FF) == 0x3F8);
    assert(Read<void*>(head(0), 8) == node(b) && Read<void*>(node(b), 8) == node(a) && Read<void*>(node(a), 8) == head(0));
    assert(Read<uint32_t>(manager, 0x38 + 6*4) == 6 && Read<uint32_t>(manager, 0x438) == 7);
    assert(Find(b, false)->occupancy == Occupancy::Live && Find(b, false)->shotLinked && !g_quarantined);
    assert((Read<uint32_t>(b, 0x10) & 0x3FF) == 2 && !unitLocked && !locked);
}
void release() {
    LifetimeTrace_ClearSnapshots();
    for (uint8_t* shot : {a, b}) ForgetCurrentShotGenerations(Find(shot, false)->id, shot);
    MockJointDelete(joint, 3); std::free(Read<void*>(a, 0x548)); std::free(Read<void*>(a, 0x11B0));
    ResourceRelease(Read<void*>(a, 0x100)); std::free(Read<void*>(a, 0x100)); std::free(a); std::free(b);
}
void reject(int) { std::_Exit(Read<float>(a, 0x1F58) == 11.0f && Read<float>(a, 0x2140) == 950.0f ? 17 : 18); }
"""
        self.compile_run(True, harness + r"""
int main() {
    // Tail copy: everything past the common prefix except the holes, in any order.
    std::vector<uint8_t> live(0x2200, 0xAA), saved(0x2200, 0x55);
    ShotTailHoles holes{{0x21F0, 0x40}, {0x2150, 0x10}, {0x2158, 0x10}};
    RestoreShotTail(live.data(), saved.data(), live.size(), holes);
    for (size_t i = 0; i < live.size(); ++i)
        assert(live[i] == (i < 0x2140 || (i >= 0x2150 && i < 0x2168) || i >= 0x21F0 ? 0xAA : 0x55));

    setup(); LifetimeTrace_SaveSnapshot(0);
    // Capture retains the unit the tail names, without a per-class field list.
    assert(Find(effect, false)->dependencies == 1 && !Find(other, false)->dependencies);
    future();
    assert(Read<float>(a, 0x1F58) == 11 && Read<float>(a, 0x2140) == 950);
    // A character's weak reference to b: cleared by the abandoned branch. Until b
    // is live again the saved address is not put back.
    assert(LifetimeTrace_ResolveUnitReference(0, b, nullptr) == nullptr);
    assert(LifetimeTrace_ResolveUnitReference(0, nullptr, b) == nullptr); // Held unit: drop the reference.
    for (int i = 0; i < 2; ++i) {
        LifetimeTrace_RestoreShots(0); check();
        advance(a);                                                        // One replayed tick, not nine.
        assert(Read<float>(a, 0x1F58) == 4 && Read<float>(a, 0x2140) == 250 && Read<float>(joint, 0x20) == 8);
    }
    assert(!creates && !resourceAdds && !resourceReleases && !constraintUnlinks);
    // Restored: the saved reference names the same live identity again.
    assert(LifetimeTrace_ResolveUnitReference(0, b, nullptr) == b);
    assert(LifetimeTrace_ResolveUnitReference(0, b, a) == b);
    // Not a unit of that snapshot (unknown address, later birth, other frame): keep the live value.
    assert(LifetimeTrace_ResolveUnitReference(0, (void*)0x1234, a) == a);
    assert(LifetimeTrace_ResolveUnitReference(0, nullptr, a) == a);
    assert(LifetimeTrace_ResolveUnitReference(1, b, nullptr) == nullptr);
    alignas(16) static uint8_t newer[0x40]{}; Birth(newer, Kind::Unit);
    assert(LifetimeTrace_ResolveUnitReference(0, newer, a) == a);
    // A reused address is a different identity, even though it is live.
    const uint64_t savedId = TestManifest(0).members[0].id;
    for (auto& member : TestManifest(0).members) if (member.object == b) member.id = savedId + 1000;
    assert(LifetimeTrace_ResolveUnitReference(0, b, a) == a);
    for (auto& member : TestManifest(0).members) if (member.object == b) member.id = Find(b, false)->id;
    // A frame with no saved manifest, or one that held no shots, is left alone.
    LifetimeTrace_RestoreShots(1);
    assert(Read<float>(a, 0x1F58) == 4);
    release();
}
""")
        # Unsupported or inconsistent cases stop the transaction before any shot byte moves.
        invalid_cases = (
            "LifetimeTrace_SaveSnapshot(0, LifetimeShotPayload::Omit); future();",
            "LifetimeTrace_SaveSnapshot(0); future(); Find(a, false)->snapshots = 0;",
            "LifetimeTrace_SaveSnapshot(0); future(); InitializeConstraint(constraint);",
            "LifetimeTrace_SaveSnapshot(0); future(); Write(constraint, 0x40, uintptr_t(0xBAD));",
            "LifetimeTrace_SaveSnapshot(0); future(); Find(a, false)->extent = 0x2500;",
        )
        cases = "\n".join(f"case {i}: {{ {invalid} break; }}" for i, invalid in enumerate(invalid_cases))
        self.compile_run(True, harness + "\nint main(int argc, char** argv) { assert(argc == 2); setup(); "
                         "std::signal(SIGABRT, reject); switch (std::atoi(argv[1])) {\n" + cases +
                         "\n}\nLifetimeTrace_RestoreShots(0);\n}\n",
                         expected_returncode=17, arguments=[[str(i)] for i in range(len(invalid_cases))])

    def test_future_shot_rejects_continuous_collision_registration(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset();
    alignas(16) uint8_t units[0x1860]{}, collision[0x73B0]{}, list[0x2550]{}, category[0x20]{}, record[0x48]{};
    unitManager = units; collisionManager = collision;
    void* root = (void*)0x1234;
    void* categories[] = {category}, *passes[] = {list}, *records[] = {record};
    Write(collision, 0x7368 + 0x10, uint32_t(1));
    Write(collision, 0x7368 + 0x20, static_cast<void*>(categories));
    Write(category, 8, uint32_t(1)); Write(category, 0x18, static_cast<void*>(passes));
    Write(list, 0x24A8 + 8, uint32_t(1)); Write(list, 0x24A8 + 0x18, static_cast<void*>(records));
    Write(record, 8, root);
    // A coverage failure must precede native recycling or any shot mutation.
    std::signal(SIGABRT, +[](int) { std::_Exit(collisionRecycles ? 18 : 17); });
    UnitGuard fence;
    DetachTransientShotCollisions({root});
    return 0;
}
''', expected_returncode=17)

    def test_historical_shot_and_model_cpu_capture(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset();
    alignas(16) uint8_t manager[0x2000]{};
    unitManager = manager;
    auto* shot = static_cast<uint8_t*>(std::calloc(1, 0x2200));
    auto* joints = static_cast<uint8_t*>(std::calloc(1, 0x10 + 2*0xC0));
    uint8_t depths[2]{1, 2}, work[2*0x68]{}, interpolation[2*0x40]{}, keys[2*0xC]{};
    Write(joints, 0, uint32_t(2));
    Write(joints + 0x10, 0, uintptr_t(mockJointVtable));
    Write(joints + 0x10, 0x20, uint32_t(0xCAFEBABE));
    Write(shot, 0x10, uint32_t(0x3F8));
    Write(shot, 0x50, float(112.5)); Write(shot, 0x530, uint32_t(2));
    Write(shot, 0x538, joints + 0x10); Write(shot, 0x548, depths);
    Write(shot, 0x554, uint32_t(1)); Write(shot, 0x6C0, work);
    Write(shot, 0x11B0, interpolation); Write(shot, 0x6D8, keys);
    Write(shot, 0x1238, uint32_t(2));
    Birth(shot, Kind::Shot);
    auto* identity = Find(shot, false);
    identity->extent = 0x2200; identity->dti = 0x12345; // Cached at completed construction.
    const uint64_t id = identity->id;
    LifetimeTrace_SaveSnapshot(1);
    auto history = testSnapshots[1];
    assert(history->shots.size() == 1 && history->shots[0].spans.size() == 6);
    const auto* backing = history->shots[0].payload.data();
    Write(shot, 0x50, float(999)); Write(joints + 0x10, 0x20, uint32_t(0));
    LifetimeTrace_DropSnapshot(1);
    testSnapshots[1].reset();
    LifetimeTrace_SaveSnapshot(17);
    assert(history.get() != testSnapshots[1].get());
    assert(history->shots[0].payload.data() == backing); // No second history copy.
    LifetimeTrace_ClearSnapshots();
    Destroyed("gone", shot, id);
    std::free(joints); std::free(shot);
    size_t seen = 0;
    const auto counts = LifetimeTrace_VisitSnapshot(1, history.get(), false,
        [](void* context, const char* name, const void*, uint64_t, uint64_t,
           const void* bytes, size_t size) {
            if (std::strstr(name, ".body")) {
                assert(size == 0x2200 && Read<float>(bytes, 0x50) == 112.5f);
                ++*static_cast<size_t*>(context);
            }
            if (std::strstr(name, ".model.joints")) {
                assert(size == 2*0xC0 && Read<uint32_t>(bytes, 0x20) == 0xCAFEBABE);
                ++*static_cast<size_t*>(context);
            }
        }, &seen);
    assert(counts.shots == 1 && seen == 2); // Never reads the freed native owner/buffers.
}
''')

    def test_dump_time_export_never_reads_historical_native_memory(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
#include <string>
struct DumpRegion { std::string name; std::vector<uint8_t> bytes; };
void capture(void* context, const char* name, const void*, uint64_t, uint64_t,
             const void* bytes, size_t size) {
    auto& regions = *static_cast<std::vector<DumpRegion>*>(context);
    const auto* data = static_cast<const uint8_t*>(bytes);
    regions.push_back({name, {data, data + size}});
}
int main() {
    reset();
    auto& manifest = TestManifest(3);
    manifest.valid = true; manifest.frame = 3;
    manifest.shotPayload = LifetimeShotPayload::Capture;
    manifest.effects.resize(1);
    auto& effect = manifest.effects[0]; effect = {};
    effect.object = (void*)0xDEAD; effect.id = 7;
    effect.spanBegin = 0; effect.spanCount = 1;
    Write(effect.root, 0, uint64_t(0x1234)); effect.root[0x40] = 42;
    manifest.spans.push_back({(void*)0xBEEF, 4, 0});
    manifest.payload.assign({'s', 'a', 'v', 'e'});
    manifest.members.push_back({(void*)0xBAD, 8, 0x555, Kind::Shot, Occupancy::Live});
    std::vector<DumpRegion> regions;
    auto counts = LifetimeTrace_VisitSnapshot(3, testSnapshots[3].get(), false, capture, &regions);
    assert(counts.effects == 1 && counts.shots == 1 && counts.shotModels == 0);
    assert(regions.size() == 4);
    assert(regions[1].name == "lifetime.effect.7.root" && regions[1].bytes[0x40] == 42);
    assert(regions[2].name == "lifetime.effect.7.graph.0");
    assert(!std::memcmp(regions[2].bytes.data(), "save", 4));
    assert(regions[3].name == "lifetime.shot.8.identity");
    regions.clear();
    counts = LifetimeTrace_VisitSnapshot(3 + ROLLBACK_WINDOW, testSnapshots[3].get(), false, capture, &regions);
    assert(!counts.effects && !counts.shots && regions.size() == 1);
    assert(Read<int64_t>(regions[0].bytes.data(), 8) == 0); // unavailable, not empty

    alignas(16) uint8_t body[0x2200]{}, dti[0x40]{}, joints[0x10 + 2*0xC0]{};
    uint8_t depths[2]{}, work[2*0x68]{}, interpolation[2*0x40]{}, keys[2*0xC]{};
    Write(dti, 0x30, uint32_t(sizeof(body) >> 2)); testDti = uintptr_t(dti);
    Write(joints, 0, uint32_t(2));
    Write(body, 0x538, joints + 0x10); Write(body, 0x548, depths);
    Write(body, 0x530, uint32_t(2)); Write(body, 0x554, uint32_t(1));
    Write(body, 0x6C0, work); Write(body, 0x6D8, keys);
    Write(body, 0x11B0, interpolation); Write(body, 0x1238, uint32_t(2));
    g_registry[0] = {body, 9, 0x777, Kind::Shot, Occupancy::Quarantined};
    SyncActiveSlotsForTest();
    LifetimeTrace_SetFrame(50, false);
    regions.clear();
    counts = LifetimeTrace_VisitSnapshot(3 + ROLLBACK_WINDOW, testSnapshots[3].get(), true, capture, &regions);
    assert(!counts.effects && !counts.shots && counts.shotModels == 1);
    assert(Read<int64_t>(regions[0].bytes.data(), 16) == 50);
    assert(regions.size() == 8);
    assert(regions[1].name == "observed.shot.9.identity");
    assert(regions[2].name == "observed.shot.9.body" && regions[2].bytes.size() == sizeof(body));
    assert(regions[3].name == "observed.shot.9.model.joints" && regions[3].bytes.size() == 2*0xC0);
    assert(regions[6].name == "observed.shot.9.model.motion.0.joints" && regions[6].bytes.size() == sizeof(work));
    assert(regions[7].name == "observed.shot.9.model.keyframes" && regions[7].bytes.size() == sizeof(keys));
}
''')

    def test_shot_payload_omit_and_capture_share_retention(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + SHOT_PAYLOAD_FIXTURE + r'''
void record(void* context, const char* name, const void*, uint64_t, uint64_t, const void* bytes, size_t size) {
    static_cast<std::vector<std::string>*>(context)->push_back(name);
    if (!std::strcmp(name, "diagnostic.lifetime.shot_payload_omitted"))
        assert(size == 8 && Read<uint64_t>(bytes, 0) == 1); // Shots whose payload is absent.
}
bool has(const std::vector<std::string>& names, const std::string& prefix) {
    return std::any_of(names.begin(), names.end(), [&](const std::string& name) { return !name.rfind(prefix, 0); });
}
int main() {
    for (const auto mode : {LifetimeShotPayload::Omit, LifetimeShotPayload::Capture}) {
        const bool capture = mode == LifetimeShotPayload::Capture;
        const auto fixture = makePayloadFixture();
        LifetimeTrace_SaveSnapshot(0, mode);
        const auto& manifest = TestManifest(0);
        assert(manifest.valid && manifest.shotPayload == mode);
        // Identity, dependency and generation leases plus withdrawal metadata are payload-independent.
        assert(Find(fixture.shot, false)->snapshots == 1);
        assert(Find(fixture.other, false)->snapshots == 1 && Find(fixture.other, false)->dependencies == 1);
        assert(manifest.shotManager == shots && manifest.shotCursor == 5 && manifest.birthWatermark == g_nextId);
        assert(manifest.shots.size() == 1 && manifest.shots[0].id == fixture.id);
        assert(manifest.shots[0].payloadState == mode);
        assert(manifest.shots[0].generations.size() == ShotDomainCount);
        for (const auto& ref : manifest.shots[0].generations)
            assert(g_shotGenerations[ref.index].id == ref.id && (g_shotGenerations[ref.index].snapshots & 1));
        assert(g_attackOwners.count(fixture.root)); // Attack cohort ownership bookkeeping.
        // Only the whole-shot bytes and constraint images follow the mode.
        const auto& spans = manifest.shots[0].spans;
        assert(spans.empty() == !capture && manifest.shots[0].payload.empty() == !capture);
        if (capture) {
            assert(!std::strcmp(spans[0].name, "body") && spans[0].memory.size == 0x2200);
            assert(std::any_of(spans.begin(), spans.end(),
                [](const ShotSpan& span) { return !std::strcmp(span.name, "collision.root"); }));
            assert(manifest.constraints.size() == 1 && manifest.constraintsComplete);
        } else {
            assert(manifest.constraints.empty() && !manifest.constraintsComplete);
        }
        std::vector<std::string> names;
        const auto counts = LifetimeTrace_VisitSnapshot(0, &manifest, false, record, &names);
        const std::string shot = "lifetime.shot." + std::to_string(fixture.id) + ".";
        assert(counts.shots == 1 && has(names, shot + "identity"));
        assert(has(names, "diagnostic.lifetime.shot_payload_omitted") == !capture);
        assert(has(names, shot + "body") == capture && has(names, shot + "collision.root") == capture);
        assert(has(names, "lifetime.constraint.") == capture);
        // Replacing the same frame reports the new mode, never stale bytes from the old one.
        LifetimeTrace_SaveSnapshot(0, capture ? LifetimeShotPayload::Omit : LifetimeShotPayload::Capture);
        assert(TestManifest(0).shotPayload != mode && TestManifest(0).shots[0].payloadState != mode);
        assert(TestManifest(0).shots[0].spans.empty() == capture);
        assert(Find(fixture.other, false)->dependencies == 1);
        releasePayloadFixture(fixture);
    }
}
''')

    def test_shot_stages_reject_omitted_payload(self):
        stages = [
            ("StageShotGenerationStorage(*owner, manifest.shots[0], 1);", "shot.storage.stage",
             "saved.payloadState == LifetimeShotPayload::Capture"),
            ("{ UnitGuard fence; StageShotCpuFields(*owner, manifest.shots[0], 1); }", "shot.cpu.stage",
             "saved.payloadState == LifetimeShotPayload::Capture"),
            ("StageShotManagerState(manifest, 1);", "shot.manager.stage",
             "saved.shotPayload == LifetimeShotPayload::Capture"),
            ("StageShotExecutionState(manifest, 1);", "shot.execution.stage",
             "saved.shotPayload == LifetimeShotPayload::Capture"),
            ("PrepareShotConstraintGraph(manifest, 1);", "shot.constraints.prepare",
             "saved.shotPayload == LifetimeShotPayload::Capture"),
        ]
        cases = "\n".join(f"    if (stage == {i}) {call}" for i, (call, _, _) in enumerate(stages))
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + SHOT_PAYLOAD_FIXTURE + r'''
void reject(int) { std::_Exit(17); }
int main(int, char** argv) {
    const auto fixture = makePayloadFixture();
    LifetimeTrace_SaveSnapshot(0, LifetimeShotPayload::Omit);
    const auto& manifest = TestManifest(0);
    auto* owner = Find(fixture.shot, false);
    const int stage = std::atoi(argv[1]);
    std::signal(SIGABRT, reject);
''' + cases + r'''
    return 0; // Reaching here means an omitted payload was accepted.
}
''', expected_returncode=17, arguments=[[str(i)] for i in range(len(stages))])
        for result, (_, phase, condition) in zip(self.last_results, stages):
            with self.subTest(phase=phase):
                self.assertIn("=== LIFETIME INVARIANT FAILURE ===", result.stderr)
                self.assertIn(f"phase={phase}", result.stderr)
                self.assertIn(f"reason=requirement failed: {condition}\n", result.stderr)

    def test_save_skips_clearing_only_after_drop(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + SHOT_PAYLOAD_FIXTURE + r'''
int main() {
    const auto fixture = makePayloadFixture();
    alignas(16) static uint8_t unit[0x40]{};
    Birth(unit, Kind::Unit);
    Identity* held = Find(unit, false);
    Identity* borrowed = Find(fixture.other, false);
    const auto setLive = [&](bool live) { held->occupancy = live ? Occupancy::Live : Occupancy::Quarantined; };
    const auto setBorrowed = [&](bool on) { Write(fixture.shot, 0x2000, on ? fixture.other : nullptr); };
    const auto leaseGraph = [] {
        g_generations[0] = {}; g_generations[0].id = 99; g_generations[0].snapshots = 1;
        g_generationHighWater = 1;
    };

    // Direct save, then same-frame replacement: no drop, so save clears in full.
    LifetimeTrace_SaveSnapshot(0);
    assert(held->snapshots == 1 && borrowed->dependencies == 1 && !(g_clearedSlots & 1));
    setLive(false); setBorrowed(false); leaseGraph();
    LifetimeTrace_SaveSnapshot(0);
    assert(!held->snapshots && !borrowed->dependencies && !g_generations[0].snapshots);
    // Ring-slot reuse by a later frame, still without a drop.
    setLive(true); setBorrowed(true); LifetimeTrace_SaveSnapshot(0);
    setLive(false); setBorrowed(false); leaseGraph();
    LifetimeTrace_SaveSnapshot(ROLLBACK_WINDOW);
    assert(TestManifest(0).frame == ROLLBACK_WINDOW);
    assert(!held->snapshots && !borrowed->dependencies && !g_generations[0].snapshots);

    // Production order: drop releases the slot's leases and marks it cleared.
    setLive(true); setBorrowed(true); LifetimeTrace_SaveSnapshot(ROLLBACK_WINDOW); leaseGraph();
    LifetimeTrace_DropSnapshot(2 * ROLLBACK_WINDOW);
    assert(!g_liveSnapshots[0] && (g_clearedSlots & 1));
    assert(!held->snapshots && !borrowed->dependencies && !g_generations[0].snapshots);
    LifetimeTrace_SaveSnapshot(2 * ROLLBACK_WINDOW);
    assert(held->snapshots == 1 && borrowed->dependencies == 1 && !(g_clearedSlots & 1));

    // Drop followed by an early return: no capture, yet the leases are already gone.
    LifetimeTrace_DropSnapshot(3 * ROLLBACK_WINDOW);
    assert(!held->snapshots && !borrowed->dependencies && (g_clearedSlots & 1));
    setLive(false); setBorrowed(false);
    LifetimeTrace_SaveSnapshot(1); // Another slot does not disturb slot 0's cleared state.
    assert((g_clearedSlots & 1) && !(g_clearedSlots & 2) && borrowed->snapshots == 2);
    LifetimeTrace_SaveSnapshot(4 * ROLLBACK_WINDOW); // Skips clearing, still exact.
    assert(!held->snapshots && borrowed->snapshots == 3 && !borrowed->dependencies);

    // Clearing every lease marks every slot cleared; the next save is still exact.
    g_generations[0] = {}; g_generationHighWater = 0; // The injected graph has no owner to collect.
    LifetimeTrace_ClearSnapshots();
    assert(g_clearedSlots == UINT32_MAX >> (32 - ROLLBACK_WINDOW));
    assert(!held->snapshots && !borrowed->snapshots && !borrowed->dependencies);
    setLive(true); setBorrowed(true);
    LifetimeTrace_SaveSnapshot(5);
    assert(held->snapshots == (1u << 5) && borrowed->dependencies == (1u << 5));
    assert(g_clearedSlots == ((UINT32_MAX >> (32 - ROLLBACK_WINDOW)) & ~(1u << 5)));
    releasePayloadFixture(fixture);
}
''')

    def test_attack_owner_lookup_from_other_threads_is_race_free(self):
        # Engine-wide grow/addChild callers can run on any thread. Unmanaged roots
        # must be rejected without racing main-thread cohort registration/erase.
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + SHOT_PAYLOAD_FIXTURE + r'''
int foreignGrows = 0, foreignAdds = 0;
int main() {
    const auto fixture = makePayloadFixture();
    oGenerationExtendArray = [](void*, uint32_t) { ++foreignGrows; };
    oGenerationAddChild = [](void*, void*, uint8_t) -> uint32_t { ++foreignAdds; return 0; };
    alignas(16) static uint8_t unmanaged[0x40]{}, foreignObject[0x90]{}, constraint[0x90]{};
    // The MtObject-teardown and resource-release detours are engine-wide too.
    static std::atomic<int> foreignTeardowns{0}, foreignReleases{0};
    oInitializeConstraint = +[](void*) {};
    oConstraintObjectDtor = +[](void*) { ++foreignTeardowns; };
    oGenerationResourceRelease = +[](void*) { ++foreignReleases; };
    std::atomic<bool> stop{false};
    std::thread caller([&] {
        for (int i = 0; i < 20000 && !stop; ++i) {
            GenerationExtendArray(unmanaged + 0x18, 8);
            GenerationAddChild(unmanaged, nullptr, 0);
            ForgetConstraintObject(foreignObject);
            GenerationResourceRelease(foreignObject);
        }
    });
    for (int frame = 0; frame < 200; ++frame) {
        LifetimeTrace_SaveSnapshot(0);                   // Registers the attack root.
        assert(g_attackOwners.size() == 1 && g_attackOwnerCount == 1);
        LifetimeTrace_DropSnapshot(0);
        ForgetCurrentShotGenerations(fixture.id, fixture.shot); // Erases it again.
        assert(!g_attackOwnerCount);
        InitializeConstraint(constraint); ForgetConstraintObject(constraint);
    }
    stop = true; caller.join();
    assert(foreignGrows && foreignAdds && foreignGrows == foreignAdds);
    assert(foreignTeardowns && foreignReleases && !g_constructedConstraints.count(constraint));
    releasePayloadFixture(fixture);
}
''', sanitizers="thread")

    def test_managed_attack_root_from_foreign_thread_fails_loudly(self):
        result = self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + SHOT_PAYLOAD_FIXTURE + r'''
void reject(int) { std::_Exit(17); }
int main() {
    const auto fixture = makePayloadFixture();
    LifetimeTrace_SaveSnapshot(0);
    oGenerationExtendArray = [](void*, uint32_t) { std::_Exit(18); };
    std::signal(SIGABRT, reject);
    std::thread([&] { GenerationExtendArray(static_cast<uint8_t*>(fixture.root) + 0x18, 8); }).join();
    return 0;
}
''', expected_returncode=17)
        self.assertIn("reason=managed attack root reached from a thread other than its owner", result.stderr)
        self.assertIn("attack.thread=0x7", result.stderr)
        self.assertIn("GetCurrentThreadId()=0x8", result.stderr)

    def test_active_registry_tracks_every_lifetime_transition(self):
        # Every other check also validates the mask at lifetime entry points; this
        # one drives each boundary crossing directly and checks it immediately.
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
bool active(void* object) {
    const Identity* entry = Find(object, false);
    if (!entry) return false;
    const size_t slot = size_t(entry - g_registry);
    return (g_activeSlots[slot / 64] >> (slot % 64)) & 1;
}
size_t activeCount() {
    size_t count = 0;
    for (const auto& entry : ActiveIdentities()) { (void)entry; ++count; }
    return count;
}
int collected = 0;
int main() {
    reset();
    oDeleteEffect = [](void*, uint32_t) -> void* { ++collected; return nullptr; };
    oMainReset = [](void*) {};
    alignas(16) static uint8_t a[0x40]{}, b[0x40]{}, effect[0x40]{};
    // Birth, then a second birth at a live address (native reuse without teardown).
    Birth(a, Kind::Unit); assert(active(a) && activeCount() == 1);
    const uint64_t first = Find(a, false)->id;
    Birth(a, Kind::Unit); assert(active(a) && activeCount() == 1 && Find(a, false)->id != first);
    { AcquireSRWLockExclusive(&g_lock); ValidateActiveSlots(); ReleaseSRWLockExclusive(&g_lock); }
    // Tombstone drops the slot and its lease bits; rebirth at the address reuses it.
    LifetimeTrace_SaveSnapshot(0);
    Identity* slot = Find(a, false); assert(slot->snapshots == 1);
    Destroyed("gone", a, slot->id);
    assert(!Find(a, false) && slot->occupancy == Occupancy::Tombstone && !slot->snapshots);
    assert(activeCount() == 0);
    Birth(a, Kind::Unit); assert(Find(a, false) == slot && active(a) && !slot->snapshots);
    // Probe chains: a tombstone between live entries keeps later entries reachable.
    for (size_t i = 0; i < 3; ++i) Birth(b + i * 0x10, Kind::Unit);
    Destroyed("gone", b + 0x10, Find(b + 0x10, false)->id);
    assert(active(b) && !active(b + 0x10) && active(b + 0x20) && activeCount() == 3);
    // Quarantine stays active; collection waits for the last lease, then tombstones.
    Birth(effect, Kind::Effect);
    LifetimeTrace_SaveSnapshot(1);
    Find(effect, false)->occupancy = Occupancy::Quarantined; ++g_quarantined;
    CollectQuarantine();
    assert(active(effect) && !collected); // Still leased by slot 1: no premature collection.
    LifetimeTrace_DropSnapshot(1);
    CollectQuarantine();
    assert(collected == 1 && !Find(effect, false) && activeCount() == 3);
    // Resurrection: quarantined back to live never leaves the active set.
    Birth(effect, Kind::Effect);
    Find(effect, false)->occupancy = Occupancy::Quarantined;
    assert(active(effect));
    Find(effect, false)->occupancy = Occupancy::Live;
    assert(active(effect) && activeCount() == 4);
    // Epoch reset clears every lease and keeps the mask exact.
    LifetimeTrace_SaveSnapshot(2);
    MainReset(nullptr);
    for (const auto& entry : ActiveIdentities()) assert(!entry.snapshots && !entry.dependencies);
    assert(activeCount() == 4);
    { AcquireSRWLockExclusive(&g_lock); ValidateActiveSlots(); ReleaseSRWLockExclusive(&g_lock); }
    // A full registry has no slot for a miss, and the mask is unchanged by it.
    reset();
    for (size_t i = 0; i < RegistrySize; ++i) g_registry[i] = {(void*)(0x1000 + i * 0x10), i + 1, 0, Kind::Unit, Occupancy::Live};
    SyncActiveSlotsForTest();
    alignas(16) static uint8_t extra[0x40]{};
    Birth(extra, Kind::Unit);
    assert(!Find(extra, false) && g_registryMisses == 1 && activeCount() == RegistrySize);
    { AcquireSRWLockExclusive(&g_lock); ValidateActiveSlots(); ReleaseSRWLockExclusive(&g_lock); }
}
''')

    def test_integrations(self):
        tracer = (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text()
        generations = (ROOT / "UMVC3Hook/code/lifetime_shot_generations.inl").read_text()
        self.assertNotIn("OwnedAllocatorFree", generations)
        self.assertNotIn("GameplayHooks_Create", generations)  # No leaf-delete or allocator-free detours.
        self.assertNotIn("RegisterDeleting(", generations)
        self.assertIn("StageShotGenerationStorage", generations)
        self.assertIn('#include "lifetime_shot_cpu.inl"', tracer)
        self.assertIn('#include "lifetime_shot_constraints.inl"', tracer)
        self.assertIn('{0x1405EC000, (void*)&InitializeConstraint', tracer)
        self.assertIn('{0x1409BFE90, (void*)&ForgetConstraintObject', tracer)
        self.assertNotIn('0x1401091C0', tracer)
        self.assertNotIn('0x140109240', tracer)
        restore = (ROOT / "UMVC3Hook/code/rollback_restore.cpp").read_text()
        self.assertLess(restore.index("LifetimeTrace_RetireFutureShots(frame)"),
                        restore.index("LifetimeTrace_RestoreEffects(frame)"))
        self.assertNotIn("StageShotGenerationStorage", restore)
        self.assertNotIn("StageShotManagerState", restore)
        self.assertNotIn("StageShotExecutionState", restore)
        self.assertNotIn("StageShotCpuFields", restore)
        self.assertNotIn("StageShotConstraintGraph", restore)
        self.assertLess(restore.index("LifetimeTrace_WaitForRenderWorker()"),
                        restore.index("LifetimeTrace_CheckSnapshot(frame)"))
        self.assertNotIn("HashBytes", tracer)
        self.assertNotIn("RootBindings", tracer)
        self.assertNotIn("DescribeSimulationGraph", tracer)
        self.assertNotIn("characterDependencies", tracer)
        self.assertNotIn("std::memcmp", tracer)
        self.assertNotIn("0x14050D100", tracer)  # Snapshot ownership never adds a resource reference.
        self.assertIn("{0x1405F0980, (void*)&BaseEffectResource", tracer)
        self.assertIn("{0x140139D30, (void*)&ConstructEffect", tracer)
        self.assertNotIn("0x14013AB60", tracer)
        hooks = (ROOT / "UMVC3Hook/code/rollback_hooks.cpp").read_text()
        self.assertIn("LifetimeTrace_Install();", hooks)
        self.assertIn("LifetimeTrace_Flush();", hooks)
        resim = (ROOT / "UMVC3Hook/code/rollback_resim.cpp").read_text()
        self.assertIn("LifetimeTrace_SetFrame(frame, true);", resim)
        self.assertIn("LifetimeTrace_SetFrame(endFrame, false);", resim)
        self.assertNotIn("MH_EnableHook", (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text())
        snapshot = (ROOT / "UMVC3Hook/code/rollback_snapshot.cpp").read_text()
        save = snapshot[snapshot.index("void Rollback_SaveSnapshot("):snapshot.index("void Rollback_UpdateRemoteInput(")]
        self.assertLess(save.index("LifetimeTrace_DropSnapshot(frame)"), save.index("Rollback_BuildRegions("))
        call = "LifetimeTrace_SaveSnapshot(frame, slot.lifetime, shotPayload)"
        self.assertGreater(save.index(call), save.index("slot.remoteInput ="))
        self.assertLess(save.index(call), save.index("SnapDump_CaptureSnapshot(slot)"))
        # Historical capture, not a later dump request, selects whole-shot payload.
        self.assertIn("const bool captureSnapDump = SnapDump_CaptureEnabled();", save)
        # Saved-shot restore reads the payload, so production captures it on every save.
        self.assertIn("SAVED_SHOT_RESTORE_ENABLED || captureSnapDump", save)
        self.assertIn("SAVED_SHOT_RESTORE_ENABLED ? LifetimeShotPayload::Capture : LifetimeShotPayload::Omit", save)
        self.assertLess(save.index("LifetimeTrace_DropSnapshot(frame)"), save.index("if (count == 0) return;"))
        self.assertIn("&sharedCapacity, &slot.lifetime", save)
        self.assertIn("std::shared_ptr<LifetimeSnapshot> lifetime;",
                      (ROOT / "UMVC3Hook/code/rollback_snapshot.h").read_text())
        dump = (ROOT / "UMVC3Hook/code/rollback_snapdump.cpp").read_text()
        per_frame = dump[dump.index("void SnapDump_CaptureSnapshot("):dump.index("void SnapDump_UpdateInputs(")]
        self.assertNotIn("LifetimeTrace_VisitSnapshot", per_frame)
        self.assertNotIn("LifetimeTrace_VisitSnapshot", save)
        self.assertIn("LifetimeTrace_VisitSnapshot", dump[dump.index("bool SnapDump_WriteHistory("):])
        self.assertIn("destination.lifetime = slot.lifetime;", per_frame)
        self.assertIn("frame->lifetime.get()", dump)
        self.assertNotIn("SnapshotManifest g_manifests[", tracer)
        self.assertIn("LifetimeTrace_ClearSnapshots();", snapshot[snapshot.index("void Rollback_FreeSnapshots("):])
        restore = (ROOT / "UMVC3Hook/code/rollback_restore.cpp").read_text()
        restore = restore[restore.index("void Rollback_RestoreSnapshot("):]
        self.assertGreater(restore.index("LifetimeTrace_CheckSnapshot(frame)"), restore.index("return;"))
        self.assertGreater(restore.index("LifetimeTrace_RestoreEffects(frame)"),
                            restore.index("restoreCharacterPostState();"))
        # Withdraw later shots, restore effects, then rewind the shots that remain.
        self.assertLess(restore.index("LifetimeTrace_RetireFutureShots(frame)"),
                        restore.index("LifetimeTrace_RestoreEffects(frame)"))
        self.assertLess(restore.index("LifetimeTrace_RestoreEffects(frame)"),
                        restore.index("LifetimeTrace_RestoreShots(frame)"))
        self.assertIn('#include "lifetime_shot_restore.inl"', tracer)
        # Character weak references are resolved by identity once shots are back.
        self.assertLess(restore.index("LifetimeTrace_RestoreShots(frame)"),
                        restore.index("LifetimeTrace_ResolveUnitReference(frame, savedUnit, liveUnit)"))
        for field in ("resolve(0x13D8)", "resolve(0x6428)", "Rollback_CharacterShotTailOffsets(dti)) resolve(field)",
                      "Rollback_CharacterEffectTailOffsets(dti)) resolve(field)"):
            self.assertIn(field, restore)
        character = (ROOT / "UMVC3Hook/code/rollback_character_restore.cpp").read_text()
        self.assertIn("static constexpr uint32_t oneSpark[] = {0x6908};", character)


if __name__ == "__main__":
    unittest.main()
