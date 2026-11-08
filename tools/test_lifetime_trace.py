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
void* unitManager = nullptr;
using LPCRITICAL_SECTION = void*;
void EnterCriticalSection(LPCRITICAL_SECTION) { assert(!locked && !unitLocked); unitLocked = true; }
void LeaveCriticalSection(LPCRITICAL_SECTION) { assert(!locked && unitLocked); unitLocked = false; }
void AcquireSRWLockExclusive(SRWLOCK* lock) { assert(!locked); lock->lock(); locked = true; }
void ReleaseSRWLockExclusive(SRWLOCK* lock) { assert(locked); locked = false; lock->unlock(); }
uint32_t GetCurrentThreadId() { return 7; }
uint64_t GetTickCount64() { return 100; }
int logs = 0, creates = 0, queues = 0, failCreate = -1, failQueue = -1;
void HookLog(const char*, ...) { assert(!locked); ++logs; }
constexpr int MH_OK = 0;
int GameplayHooks_Create(void*, void*, void**) { return creates++ == failCreate ? 9 : MH_OK; }
int GameplayHooks_QueueEnable(void*) { return queues++ == failQueue ? 9 : MH_OK; }
uintptr_t _addr(uintptr_t address) { return address == 0x140E17698 ? uintptr_t(&unitManager) : address; }
'''

CHECKS = r'''
void reset() {
    for (auto& entry : g_registry) entry = {};
    for (auto& manifest : g_manifests) {
        manifest.valid = false; manifest.count = 0;
        manifest.retiringValid = false; manifest.retiringCount = 0;
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
    g_finishRequested = false;
    g_quarantined = 0;
    g_reportedQuarantined = 0;
    g_retentionOpen = false;
    creates = queues = logs = 0;
    failCreate = failQueue = -1;
}
Event& last() { return g_events[(g_read + g_count - 1) % EventCapacity]; }
alignas(16) uint8_t object[0x2200]{};
int originalCalls = 0;
void checkOriginal(void* p) { assert(!locked && p == object); ++originalCalls; }

int main() {
    reset();
    LifetimeTrace_Install();
    assert(creates == 17 && queues == 17 && g_recording);
    reset(); failCreate = 3; g_recording = false;
    LifetimeTrace_Install();
    assert(creates == 17 && queues == 0 && !g_recording);
    reset(); failQueue = 3;
    LifetimeTrace_Install();
    assert(!g_recording);
    reset();

    uintptr_t vtable = 0x12345678;
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
    LifetimeTrace_RestoreRetiringEffects(123);
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
    ShotDtor(allocation); // Native teardown must still execute despite snapshot observations.
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
    LifetimeTrace_Flush(); // Capture stop cannot strand a retained allocation.
    assert(deletes == before + 1 && g_quarantined == 0 && !Find(p, false));
    assert(!g_retentionOpen);

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
    assert(creates == 13 && queues == 13);
    oDeleteEffect = [](void* p, uint32_t flags) -> void* {
        assert(p == (void*)123 && flags == 7 && !locked);
        return (void*)456;
    };
    assert(DeleteEffect((void*)123, 7) == (void*)456);
    assert(g_quarantined == 0);
}
''', quarantine=False)

    def test_retiring_effect_restore(self):
        self.compile_run(True, CHECKS[:CHECKS.index("int main() {")] + r'''
alignas(16) uint8_t manager[0xC40]{};
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
    assert(g_manifests[0].retiringValid && g_manifests[0].retiringCount == 2);
    detachAndDelete(b); detachAndDelete(c);
    assert(g_quarantined == 2 && deleted == 0 && Read<void*>(a, 0x18) == d);
    Write(b, 8, uint32_t(99)); Write(b, 0x28, 3.0f);
    LifetimeTrace_RestoreRetiringEffects(0);
    assert(!std::strcmp(last().name, "retiring_restore_applied") && last().b == 2 && last().c == 2);
    assert(g_quarantined == 0 && deleted == 0 && !unitLocked);
    assert(Read<void*>(a, 0x18) == b && Read<void*>(b, 0x20) == a);
    assert(Read<void*>(b, 0x18) == c && Read<void*>(c, 0x20) == b);
    assert(Read<void*>(c, 0x18) == d && Read<void*>(d, 0x20) == c);
    assert(Read<uint32_t>(b, 8) == 42 && Read<uint32_t>(c, 0xC) == 7);
    assert(Read<float>(b, 0x28) == 0.5f && (Read<uint32_t>(b, 0x10) & 7) == 3);
    assert((Read<uint32_t>(c, 0x10) & 7) == 4 && Read<void*>(line(), 0) == a);
    LifetimeTrace_RestoreRetiringEffects(0); // Already linked: no duplicate membership.
    assert(last().b == 2 && last().c == 0 && g_quarantined == 0);
    detachAndDelete(b); detachAndDelete(c); // Same identities can retire again.
    assert(g_quarantined == 2 && deleted == 0);
    void* extra = makeUnit(2);
    chain({a, extra, d});
    LifetimeTrace_RestoreRetiringEffects(0);
    assert(!std::strcmp(last().name, "retiring_restore_skip") && last().b == 4);
    assert(g_quarantined == 2 && Read<void*>(a, 0x18) == extra && RetainableDetachedEffect(b));
    chain({a, d}); DeleteEffect(extra, 1);
    Mutation(c, 2); // A valid first target must not be written before second-target failure.
    LifetimeTrace_RestoreRetiringEffects(0);
    assert(last().b == 3 && g_quarantined == 2 && RetainableDetachedEffect(b));
    LifetimeTrace_ClearSnapshots();
    assert(g_quarantined == 0 && deleted == 3);
    chain({}); DeleteEffect(a, 1); DeleteEffect(d, 1);

    // Singleton lane: restore head and tail, including retained resource identity.
    void* p = makeUnit(4);
    alignas(16) uint8_t resource[0x60]{};
    Write(p, 0x108, (void*)resource);
    chain({p}); LifetimeTrace_SaveSnapshot(1); detachAndDelete(p);
    LifetimeTrace_RestoreRetiringEffects(1);
    assert(last().c == 1 && Read<void*>(line(), 0) == p && Read<void*>(line(), 8) == p);
    assert(Read<void*>(p, 0x18) == nullptr && Read<void*>(p, 0x20) == nullptr);
    detachAndDelete(p);
    Write(p, 0x108, (void*)nullptr);
    LifetimeTrace_RestoreRetiringEffects(1);
    assert(last().b == 3 && RetainableDetachedEffect(p) && g_quarantined == 1);
    Write(p, 0x108, (void*)resource);
    LifetimeTrace_ClearSnapshots();

    // Unknown anchors cannot be trusted by address alone, even when currently readable.
    a = makeUnit(2, false); p = makeUnit(3); chain({a, p});
    LifetimeTrace_SaveSnapshot(2);
    assert(!g_manifests[2].retiringValid);
    detachAndDelete(p);
    LifetimeTrace_RestoreRetiringEffects(2);
    assert(last().b == 1 && g_quarantined == 1);
    LifetimeTrace_ClearSnapshots(); chain({}); std::free(a);

    // An anchor at the same address with a new generation must reject reattachment.
    a = makeUnit(2); p = makeUnit(4); chain({a, p});
    LifetimeTrace_SaveSnapshot(3); detachAndDelete(p);
    Destroyed("anchor_replaced", a, Find(a, false)->id); Birth(a, Kind::Effect);
    LifetimeTrace_RestoreRetiringEffects(3);
    assert(last().b == 4 && Read<void*>(line(), 8) == a && RetainableDetachedEffect(p));
    LifetimeTrace_ClearSnapshots(); chain({}); DeleteEffect(a, 1);

    // Active-at-save empty effects remain outside this retiring-only feature.
    p = makeUnit(2); chain({p}); LifetimeTrace_SaveSnapshot(4);
    assert(g_manifests[4].retiringValid && g_manifests[4].retiringCount == 0);
    detachAndDelete(p);
    LifetimeTrace_RestoreRetiringEffects(4);
    assert(g_quarantined == 1 && RetainableDetachedEffect(p));
    LifetimeTrace_ClearSnapshots();
    assert(g_quarantined == 0 && !unitLocked && !locked);

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
    Write(storage, 0x100, p); Write(pool, 0x20, uint32_t(0x12345678));
    assert(RetiringEffectGraph(p));
    Write(node, 0x48, (void*)1); assert(!RetiringEffectGraph(p)); Write(node, 0x48, (void*)nullptr);
    Write(parameter, 0x18, uint8_t(0x10)); assert(!RetiringEffectGraph(p)); Write(parameter, 0x18, uint8_t(0));
    Write(node, 0x20, uint32_t(0x40)); assert(!RetiringEffectGraph(p)); Write(node, 0x20, uint32_t(0));
    Write(node, 0x12C, uint32_t(0x400)); assert(!RetiringEffectGraph(p)); Write(node, 0x12C, uint32_t(0));
    Write(pool, 0x60, (void*)1); assert(!RetiringEffectGraph(p)); Write(pool, 0x60, (void*)nullptr);
    Write(pool, 8, pool); assert(!RetiringEffectGraph(p)); Write(pool, 8, (void*)nullptr);
    Write(node, 0x18, (void*)node); assert(!RetiringEffectGraph(p)); Write(node, 0x18, (void*)nullptr);
    Write(node, 0x34, uint32_t(0x40001)); assert(!RetiringEffectGraph(p)); Write(node, 0x34, uint32_t(0x120));
    chain({p}); LifetimeTrace_SaveSnapshot(5);
    assert(g_manifests[5].retiringCount == 1);
    int before = deleted;
    detachAndDelete(p);
    assert(g_quarantined == 1 && deleted == before && last().c == 1);
    LifetimeTrace_RestoreRetiringEffects(5);
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

    def test_integrations(self):
        tracer = (ROOT / "UMVC3Hook/code/lifetime_trace.cpp").read_text()
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
        self.assertGreater(restore.index("LifetimeTrace_RestoreRetiringEffects(frame)"),
                           restore.index("restoreCharacterPostState();"))


if __name__ == "__main__":
    unittest.main()
