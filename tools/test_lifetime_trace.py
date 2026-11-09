"""Compile the production lifetime tracer against native-call and Windows mocks."""

from pathlib import Path
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
uint32_t GetCurrentThreadId() { return 7; }
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
constexpr int MH_OK = 0;
int GameplayHooks_Create(void* target, void* detour, void** original) {
    assert(!locked);
    if (creates++ == failCreate) return 9;
    if (original) *original = target;
    mockHooks.emplace_back(target, detour);
    return MH_OK;
}
int GameplayHooks_QueueEnable(void*) { return queues++ == failQueue ? 9 : MH_OK; }
bool GameplayHooks_Apply() { assert(!locked); ++applies; return !failApply; }
uintptr_t _addr(uintptr_t address) {
    if (address == 0x140E17698) return uintptr_t(&unitManager);
    if (address == 0x140D47F40) return uintptr_t(&shotManager);
    if (address == 0x140E18250) return uintptr_t(&collisionManager);
    if (address == 0x140D50F88) return uintptr_t(&gameConfig);
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
    g_shotGenerations.clear(); g_shotDomains.clear(); g_attackOwners.clear(); g_keyframeExtents.clear(); g_nextShotGeneration = 0;
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
    std::memset(shotHeads, 0, sizeof(shotHeads)); std::memset(shotDti, 0, sizeof(shotDti));
    unitLockDepth = 0; unitLocked = false;
    creates = queues = logs = 0;
    applies = 0; failApply = false; mockHooks.clear();
    failCreate = failQueue = -1;
}
Event& last() { return g_events[(g_read + g_count - 1) % EventCapacity]; }
alignas(16) uint8_t object[0x2200]{};
int originalCalls = 0;
void checkOriginal(void* p) { assert(!locked && p == object); ++originalCalls; }

int main() {
    reset();
    LifetimeTrace_Install();
    assert(creates == 32 && queues == 32 && g_recording);
    reset(); failCreate = 3; g_recording = false;
    LifetimeTrace_Install();
    assert(creates == 32 && queues == 0 && !g_recording);
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


class LifetimeTraceTests(unittest.TestCase):
    def compile_run(self, enabled, checks, quarantine=True, expected_returncode=0):
        header = (ROOT / "UMVC3Hook/code/lifetime_trace.h").read_text()
        header = header.replace("#pragma once", "").replace(
            "LIFETIME_TRACE_ENABLED = true", f"LIFETIME_TRACE_ENABLED = {str(enabled).lower()}")
        header = header.replace("EMPTY_EFFECT_QUARANTINE_ENABLED = true",
                                f"EMPTY_EFFECT_QUARANTINE_ENABLED = {str(quarantine).lower()}")
        source = (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text()
        source = source.replace('#include "lifetime_shot_generations.inl"',
            (ROOT / "UMVC3Hook/code/lifetime_shot_generations.inl").read_text())
        source = source.replace('#include "lifetime_shot_world.inl"',
            (ROOT / "UMVC3Hook/code/lifetime_shot_world.inl").read_text())
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
std::shared_ptr<LifetimeSnapshot> testSnapshots[ROLLBACK_WINDOW];
SnapshotManifest& TestManifest(size_t slot) {
    if (!testSnapshots[slot]) testSnapshots[slot] = std::make_shared<LifetimeSnapshot>();
    return *testSnapshots[slot];
}
void SaveTestSnapshot(int32_t frame) {
    LifetimeTrace_SaveSnapshot(frame, testSnapshots[(frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW]);
}
void SaveTestSnapshot(int32_t frame, std::shared_ptr<LifetimeSnapshot>& storage) {
    LifetimeTrace_SaveSnapshot(frame, storage);
}
'''
        checks = checks.replace("LifetimeTrace_SaveSnapshot(", "SaveTestSnapshot(")
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="lifetime-trace-") as tmp:
            cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
            cpp.write_text(MOCKS + rollback_header + character_header + header + source + bridge + checks)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-pthread",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-g",
                            str(cpp), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], check=False)
            self.assertEqual(result.returncode, expected_returncode)

    def test_trace_and_native_passthrough(self):
        self.compile_run(True, CHECKS)

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
    assert(TestManifest(0).valid && TestManifest(0).count == 1);
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
    assert(last().c == 0 && last().d == 0 && TestManifest(0).count == 1);
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
    LifetimeTrace_SaveSnapshot(2); // Bounded manifest, no dereference even for invalid addresses.
    assert(TestManifest(2).count == RegistrySize);
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
    assert(TestManifest(2).count == 0 && Find(p, false)->snapshots == 3);
    LifetimeTrace_DropSnapshot(0);
    CollectQuarantine();
    assert(deletes == 0 && Find(p, false)->snapshots == 2);
    LifetimeTrace_DropSnapshot(1);
    CollectQuarantine();
    assert(deletes == 1 && bodies == 1 && g_quarantined == 0 && !Find(p, false));
    assert(diagnosticHistory->valid && diagnosticHistory->count == 1);
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
}
''')

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
alignas(16) uint8_t units[0x1860]{}, shots[0x440]{};
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
int main() {
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
        Write(next, 0x10, node); Write(head, 8, node);
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
    LifetimeTrace_SaveSnapshot(2); assert(TestManifest(2).count == 0);
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
''')

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
int main() {
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
    assert(TestManifest(0).effectCount == 4);
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
    assert(TestManifest(2).effectCount == 1);
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
    assert(TestManifest(4).effectCount == 1);
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
    assert(TestManifest(5).effectCount == 1);
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
''')

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
    assert(TestManifest(0).count == 2 && creates == 1 && applies == 1);
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
int main() {
    reset(); unitManager = manager; Write(manager, 0xC38, uint32_t(32));
    Graph a, b;
    chain({a.owner, b.owner});
    LifetimeTrace_SaveSnapshot(0);
    assert(TestManifest(0).effectCount == 2);
    assert(TestManifest(0).effects[0].simulation && TestManifest(0).payloadBytes > 0);
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
        assert(TestManifest(8).effectCount == 1);
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
''')

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
        assert(saved->frame == frame && saved->effectCount == 1);
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
    DestroyConstraint(static_cast<uint8_t*>(p) + 0x2140);
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
    Write(constraint, 0x60, uintptr_t(0x1234)); ConstructConstraint(constraint);
    auto* line = units + 0x50 + 13*0x30;
    void* next = Read<void*>(line, 0);
    Write(shot, 0x18, next); if (next) Write(next, 0x20, shot);
    Write(line, 0, shot); if (!next) Write(line, 8, shot);
    auto* sentinel = shotHeads[0] + 8;
    auto* node = static_cast<uint8_t*>(shot) + 0x1450;
    void* first = Read<void*>(sentinel, 8);
    Write(node, 8, first); Write(node, 0x10, static_cast<void*>(sentinel));
    Write(first, 0x10, static_cast<void*>(node)); Write(sentinel, 8, static_cast<void*>(node));
    identity->shotLinked = true;
    return shot;
}
int main() {
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
    oConstructConstraint = [](void* p) -> void* { assert(!locked); return p; };
    oDestroyConstraint = [](void*) { assert(!locked); };
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
''')

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
    manifest.effectCount = 1;
    manifest.effects.resize(1);
    auto& effect = manifest.effects[0]; effect = {};
    effect.object = (void*)0xDEAD; effect.id = 7;
    effect.spanBegin = 0; effect.spanCount = 1;
    Write(effect.root, 0, uint64_t(0x1234)); effect.root[0x40] = 42;
    manifest.spans.push_back({(void*)0xBEEF, 4, 0});
    manifest.payload.assign({'s', 'a', 'v', 'e'});
    manifest.count = 1;
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

    def test_integrations(self):
        tracer = (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text()
        generations = (ROOT / "UMVC3Hook/code/lifetime_shot_generations.inl").read_text()
        self.assertNotIn("OwnedAllocatorFree", generations)
        self.assertNotIn("GameplayHooks_Create", generations)  # No leaf-delete or allocator-free detours.
        self.assertNotIn("RegisterDeleting(", generations)
        self.assertIn("StageShotGenerationStorage", generations)
        restore = (ROOT / "UMVC3Hook/code/rollback_restore.cpp").read_text()
        self.assertLess(restore.index("LifetimeTrace_RetireFutureShots(frame)"),
                        restore.index("LifetimeTrace_RestoreEffects(frame)"))
        self.assertNotIn("StageShotGenerationStorage", restore)
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
        self.assertGreater(save.index("LifetimeTrace_SaveSnapshot(frame, slot.lifetime)"), save.index("slot.remoteInput ="))
        self.assertLess(save.index("LifetimeTrace_SaveSnapshot(frame, slot.lifetime)"), save.index("SnapDump_CaptureSnapshot(slot)"))
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


if __name__ == "__main__":
    unittest.main()
