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
#define __fastcall
using SRWLOCK = std::mutex;
#define SRWLOCK_INIT {}
thread_local bool locked = false;
thread_local bool unitLocked = false;
thread_local unsigned unitLockDepth = 0;
void* unitManager = nullptr;
void* shotManager = nullptr;
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
    if (address == 0x140D47F90) return uintptr_t(shotHeads);
    if (address == 0x140D4D960) return uintptr_t(shotDti);
    if (address == 0x14050D100) return uintptr_t(&ResourceAdd);
    if (address == 0x14050D5A0) return uintptr_t(&ResourceRelease);
    if (address == 0x140011660) return uintptr_t(&ExtendGroup);
    return address;
}
'''

CHECKS = r'''
void reset() {
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
    for (auto& manifest : g_manifests) {
        manifest.valid = false; manifest.count = 0;
        manifest.effectCount = 0;
        for (auto& entries : manifest.primary) entries.clear();
        for (auto& entries : manifest.groups) entries.clear();
    }
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
    assert(creates == 24 && queues == 24 && g_recording);
    reset(); failCreate = 3; g_recording = false;
    LifetimeTrace_Install();
    assert(creates == 24 && queues == 0 && !g_recording);
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
    def compile_run(self, enabled, checks, quarantine=True):
        header = (ROOT / "UMVC3Hook/code/lifetime_trace.h").read_text()
        header = header.replace("#pragma once", "").replace(
            "LIFETIME_TRACE_ENABLED = true", f"LIFETIME_TRACE_ENABLED = {str(enabled).lower()}")
        header = header.replace("EMPTY_EFFECT_QUARANTINE_ENABLED = true",
                                f"EMPTY_EFFECT_QUARANTINE_ENABLED = {str(quarantine).lower()}")
        source = (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith('#include "'))
        rollback_header = (ROOT / "UMVC3Hook/code/rollback.h").read_text().replace("#pragma once", "")
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="lifetime-trace-") as tmp:
            cpp, exe = Path(tmp) / "test.cpp", Path(tmp) / "test"
            cpp.write_text(MOCKS + rollback_header + header + source + checks)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-pthread",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie", "-g",
                            str(cpp), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

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
    assert(!creates && !queues && !logs && !g_sequence && !g_recording);
}
''')

    def test_snapshot_observations(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
int main() {
    reset();
    Birth(object, Kind::Effect);
    LifetimeTrace_SaveSnapshot(0);
    assert(g_manifests[0].valid && g_manifests[0].count == 1);
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
    assert(last().c == 0 && last().d == 0 && g_manifests[0].count == 1);
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
    assert(g_manifests[ROLLBACK_WINDOW - 1].valid);
    LifetimeTrace_ClearSnapshots();
    assert(Find(object, false)->snapshots == 0);
    for (const auto& manifest : g_manifests) assert(!manifest.valid && manifest.count == 0);

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
    assert(g_manifests[2].count == RegistrySize);
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
    assert(!g_manifests[3].valid);
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
    assert(DeleteEffect(p, 1) == p && deletes == 0 && bodies == 0);
    assert(g_quarantined == 1 && Find(p, false)->occupancy == Occupancy::Quarantined);
    assert(Find(p, false)->id == id);
    assert(Read<uintptr_t>(p, 0) == _addr(0x140A8E180));
    assert(DeleteEffect(p, 1) == p && deletes == 0 && g_quarantined == 1);
    LifetimeTrace_CheckSnapshot(0);
    assert(!std::strcmp(last().name, "quarantine_restore_pending") && last().b == 1);
    // Retired objects are not admitted into new manifests or reactivated by checks.
    LifetimeTrace_SaveSnapshot(2);
    assert(g_manifests[2].count == 0 && Find(p, false)->snapshots == 3);
    LifetimeTrace_DropSnapshot(0);
    CollectQuarantine();
    assert(deletes == 0 && Find(p, false)->snapshots == 2);
    LifetimeTrace_DropSnapshot(1);
    CollectQuarantine();
    assert(deletes == 1 && bodies == 1 && g_quarantined == 0 && !Find(p, false));
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
    assert(creates == 15 && queues == 15);
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
    LifetimeTrace_SaveSnapshot(2); assert(g_manifests[2].count == 0);
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
    assert(g_manifests[0].effectCount == 4);
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
    assert(g_manifests[2].effectCount == 1);
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
    assert(g_manifests[4].effectCount == 1);
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
    assert(g_manifests[5].effectCount == 1);
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
    assert(g_manifests[0].count == 2 && creates == 1 && applies == 1);
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
    assert(g_manifests[0].effectCount == 2);
    assert(g_manifests[0].effects[0].simulation && g_manifests[0].payloadBytes > 0);
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
    assert(!g_manifests[1].valid);

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
        assert(g_manifests[8].effectCount == 1);
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

    def test_integrations(self):
        tracer = (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text()
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
        self.assertGreater(save.index("LifetimeTrace_SaveSnapshot(frame)"), save.index("slot.remoteInput ="))
        self.assertIn("LifetimeTrace_ClearSnapshots();", snapshot[snapshot.index("void Rollback_FreeSnapshots("):])
        restore = (ROOT / "UMVC3Hook/code/rollback_restore.cpp").read_text()
        restore = restore[restore.index("void Rollback_RestoreSnapshot("):]
        self.assertGreater(restore.index("LifetimeTrace_CheckSnapshot(frame)"), restore.index("return;"))
        self.assertGreater(restore.index("LifetimeTrace_RestoreEffects(frame)"),
                            restore.index("restoreCharacterPostState();"))


if __name__ == "__main__":
    unittest.main()
