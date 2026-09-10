#include "../UMVC3Hook/code/callback_particle_snapshot.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdio>

using namespace CallbackParticles;

namespace {
constexpr size_t Stride = 0xD0;
constexpr size_t Capacity = 3;
constexpr size_t PoolBytes = Stride * Capacity + 0x80;

struct Graph {
    std::array<uint8_t, 0x250> effect{};
    std::array<uint8_t, ManagerSize> manager{};
    std::array<uint8_t, GeneratorSize> generator{};
    std::array<uint8_t, PoolBytes> pool{};
    std::array<uint8_t, 0x20> subBuffer{};

    Graph() {
        Write<uint32_t>(effect.data(), 0x130, 0x1000000);
        Write<uint16_t>(effect.data(), 0x204, 1);
        Write<uint16_t>(effect.data(), 0x208, 1);
        Write(effect.data(), 0x218, (uint64_t)generator.data());
        Write(effect.data(), 0x220, (uint64_t)manager.data());
        Write(manager.data(), 0x100, (uint64_t)effect.data());
        Write<uint64_t>(manager.data(), 0x108, 0x8000);
        Write<uint32_t>(manager.data(), 0x128, subBuffer.size());
        Write(manager.data(), 0x130, (uint64_t)subBuffer.data());
        Write(manager.data(), 0x138, (uint64_t)subBuffer.data());
        Write<uint64_t>(generator.data(), 0, 0x1000);
        Write(generator.data(), 0x10, (uint64_t)effect.data());
        Write<uint32_t>(generator.data(), 0x20, 7);
        Write(generator.data(), 0x28, (uint64_t)manager.data());
        Write<uint32_t>(generator.data(), 0x34, pool.size());
        Write(generator.data(), 0x38, (uint64_t)pool.data());
        Write<uint16_t>(generator.data(), 0x110, Capacity);
        Write<uint16_t>(generator.data(), 0x114, Stride);
        Write<uint16_t>(generator.data(), 0x116, Stride);
        FreeAll();
    }

    void FreeAll() {
        Write<uint64_t>(generator.data(), 0xE0, 0);
        Write<uint64_t>(generator.data(), 0xE8, 0);
        Write(generator.data(), 0xF0, (uint64_t)pool.data());
        Write(generator.data(), 0xF8, (uint64_t)(pool.data() + 2 * Stride));
        for (size_t i = 0; i < Capacity; ++i) {
            auto* p = pool.data() + i * Stride;
            Write<uint16_t>(p, 0x10, i);
            Write<uint64_t>(p, 0, 0xC0000000); // native free prev is not authoritative
            Write(p, 8, i == 2 ? uint64_t(0) : (uint64_t)(p + Stride));
        }
    }

    void MakeDraining() {
        Write(generator.data(), 0xE0, (uint64_t)pool.data());
        Write(generator.data(), 0xE8, (uint64_t)(pool.data() + Stride));
        Write(generator.data(), 0xF0, (uint64_t)(pool.data() + 2 * Stride));
        Write<uint64_t>(pool.data(), 0, 0);
        Write(pool.data() + Stride, 0, (uint64_t)pool.data());
        Write<uint64_t>(pool.data() + Stride, 8, 0);
        Write<uint8_t>(generator.data(), 0x77, 5);
        Write<uint32_t>(generator.data(), 0x20, 0x80000047);
        Write<uint16_t>(generator.data(), 0x112, 2);
        Write<uint16_t>(generator.data(), 0x122, 9);
        Write<uint16_t>(generator.data(), 0x206, 1);
        Write<uint32_t>(generator.data(), 0xB8, 47);
        Write<uint32_t>(pool.data(), 0x1C, 46);
        Write<uint32_t>(pool.data() + Stride, 0x1C, 45);
        Write<float>(pool.data(), Stride * Capacity + 0x10, 0.9375f);
        Write<float>(subBuffer.data(), 0, 42.0f);
    }
};

bool Accessible(const void* pointer, size_t size) { return pointer && size <= MaxBytes; }
bool Supported(const uint8_t* node) { return Read<uint64_t>(node, 0) == 0x1000; }

void RestoreDrainingParticlesAndLifetime() {
    Graph old, fresh;
    old.MakeDraining();
    Write<uint64_t>(fresh.generator.data(), 0x210, 0x12345678); // factory-owned, never copied
    std::vector<uint8_t> snapshot;
    assert(Capture(old.effect.data(), snapshot, Accessible, Supported));
    assert(Restore(fresh.effect.data(), snapshot, Accessible));
    assert(Read<uint64_t>(fresh.generator.data(), 0xE0) == (uint64_t)fresh.pool.data());
    assert(Read<uint64_t>(fresh.pool.data() + Stride, 0) == (uint64_t)fresh.pool.data());
    assert(Read<uint8_t>(fresh.generator.data(), 0x77) == 5);
    assert(Read<uint32_t>(fresh.generator.data(), 0x20) == 0x80000007); // finished, no stolen sound handle
    assert(Read<uint16_t>(fresh.generator.data(), 0x112) == 2);
    assert(Read<uint16_t>(fresh.generator.data(), 0x122) == 9);
    assert(Read<uint16_t>(fresh.generator.data(), 0x206) == 1);
    assert(Read<uint32_t>(fresh.generator.data(), 0xB8) == 47);
    assert(Read<uint32_t>(fresh.pool.data(), 0x1C) == 46);
    assert(Read<float>(fresh.pool.data(), Stride * Capacity + 0x10) == 0.9375f);
    assert(Read<float>(fresh.subBuffer.data(), 0) == 42.0f);
    assert(Read<uint64_t>(fresh.generator.data(), 0x210) == 0x12345678);
    assert(Read<uint64_t>(fresh.manager.data(), 0x130) == (uint64_t)fresh.subBuffer.data());
    assert(Read<uint64_t>(fresh.manager.data(), 0x100) == (uint64_t)fresh.effect.data());
    assert(ValidPool(fresh.generator.data(), fresh.pool.data()));

    // Repeated restore preserves remaining progress rather than respawning the
    // burst or resetting its particles to age zero on each rollback.
    assert(Restore(fresh.effect.data(), snapshot, Accessible));
    assert(Read<uint32_t>(fresh.pool.data(), 0x1C) == 46);
}

void PreserveLastPassCompletionGuard() {
    Graph old, fresh;
    old.MakeDraining();
    old.FreeAll();
    Write<uint16_t>(old.generator.data(), 0x112, 1); // last particle recycled this pass
    std::vector<uint8_t> snapshot;
    assert(Capture(old.effect.data(), snapshot, Accessible, Supported));
    assert(Restore(fresh.effect.data(), snapshot, Accessible));
    assert(Read<uint64_t>(fresh.generator.data(), 0xE0) == 0);
    assert(Read<uint16_t>(fresh.generator.data(), 0x112) == 1);
    assert(Read<uint32_t>(fresh.generator.data(), 0x20) & 0x80000000);
}

void RejectWithoutPartialWrites() {
    Graph old, fresh;
    old.MakeDraining();
    std::vector<uint8_t> snapshot;
    assert(Capture(old.effect.data(), snapshot, Accessible, Supported));
    Write<uint16_t>(fresh.generator.data(), 0x116, Stride + 16);
    auto beforeManager = fresh.manager;
    auto beforeGenerator = fresh.generator;
    auto beforePool = fresh.pool;
    assert(!Restore(fresh.effect.data(), snapshot, Accessible));
    assert(fresh.manager == beforeManager && fresh.generator == beforeGenerator && fresh.pool == beforePool);

    // Node-like matrix bytes must never be traversed as a particle list.
    Write<uint64_t>(old.generator.data(), 0, 0x2000);
    Write<uint64_t>(old.generator.data(), 0xE0, 0x3F800000);
    assert(!Capture(old.effect.data(), snapshot, Accessible, Supported));
    assert(snapshot.empty());
}

void RejectOwnershipCorruptionAndBudgets() {
    Graph graph;
    graph.MakeDraining();
    std::vector<uint8_t> snapshot;
    assert(!Capture(graph.effect.data(), snapshot, Accessible, Supported, 16));
    Write<uint64_t>(graph.pool.data(), 0x60, 0x5678);
    assert(!Capture(graph.effect.data(), snapshot, Accessible, Supported));
    Write<uint64_t>(graph.pool.data(), 0x60, 0);
    Write(graph.pool.data() + Stride, 8, (uint64_t)graph.pool.data());
    assert(!Capture(graph.effect.data(), snapshot, Accessible, Supported));
    graph.FreeAll();
    Write<uint32_t>(graph.generator.data(), 0xC4, 0x30);
    assert(!Capture(graph.effect.data(), snapshot, Accessible, Supported));
}

void RestorePolylineHistory() {
    Graph old, fresh;
    constexpr size_t historyStride = 0x150; // base 0xf0 + two slots * 0x30
    std::vector<uint8_t> oldPool(Capacity * historyStride + 0x80);
    std::vector<uint8_t> freshPool(oldPool.size());
    auto attach = [&](Graph& graph, std::vector<uint8_t>& pool) {
        Write<uint32_t>(graph.generator.data(), 0x34, pool.size());
        Write(graph.generator.data(), 0x38, (uint64_t)pool.data());
        Write<uint16_t>(graph.generator.data(), 0x114, 0xF0);
        Write<uint16_t>(graph.generator.data(), 0x116, historyStride);
        Write(graph.generator.data(), 0xE0, (uint64_t)pool.data());
        Write(graph.generator.data(), 0xE8, (uint64_t)pool.data());
        Write(graph.generator.data(), 0xF0, (uint64_t)(pool.data() + historyStride));
        Write(graph.generator.data(), 0xF8, (uint64_t)(pool.data() + 2 * historyStride));
        for (size_t i = 0; i < Capacity; ++i) {
            auto* p = pool.data() + i * historyStride;
            Write<uint16_t>(p, 0x10, i);
            Write<uint8_t>(p, 0x9D, 2);
            Write(p, 8, i == 1 ? (uint64_t)(p + historyStride) : uint64_t(0));
        }
    };
    attach(old, oldPool);
    attach(fresh, freshPool);
    Write<float>(oldPool.data(), 0x130, 123.0f);
    Write<float>(oldPool.data(), 0x140, 122.0f);
    std::vector<uint8_t> snapshot;
    assert(Capture(old.effect.data(), snapshot, Accessible, Supported));
    assert(Restore(fresh.effect.data(), snapshot, Accessible));
    assert(Read<float>(freshPool.data(), 0x130) == 123.0f);
    assert(Read<float>(freshPool.data(), 0x140) == 122.0f);
    Write<uint8_t>(oldPool.data(), 0x9D, 0);
    assert(!Capture(old.effect.data(), snapshot, Accessible, Supported));
}

void Benchmark(size_t capacity) {
    Graph old, fresh;
    std::vector<uint8_t> oldPool(capacity * Stride + 0x80);
    std::vector<uint8_t> freshPool(oldPool.size());
    auto initialize = [=](Graph& graph, std::vector<uint8_t>& pool) {
        Write<uint16_t>(graph.generator.data(), 0x110, capacity);
        Write<uint32_t>(graph.generator.data(), 0x34, pool.size());
        Write(graph.generator.data(), 0x38, (uint64_t)pool.data());
        Write<uint64_t>(graph.generator.data(), 0xE0, 0);
        Write<uint64_t>(graph.generator.data(), 0xE8, 0);
        Write(graph.generator.data(), 0xF0, (uint64_t)pool.data());
        Write(graph.generator.data(), 0xF8, (uint64_t)(pool.data() + (capacity - 1) * Stride));
        for (size_t i = 0; i < capacity; ++i) {
            auto* p = pool.data() + i * Stride;
            Write<uint16_t>(p, 0x10, i);
            Write(p, 8, i + 1 == capacity ? uint64_t(0) : (uint64_t)(p + Stride));
        }
    };
    initialize(old, oldPool);
    initialize(fresh, freshPool);
    std::vector<uint8_t> snapshot;
    constexpr size_t iterations = 20000;
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        // Match current shot capture, which resets the containing snapshot.
        snapshot = std::vector<uint8_t>{};
        if (!Capture(old.effect.data(), snapshot, Accessible, Supported) ||
            !Restore(fresh.effect.data(), snapshot, Accessible)) std::abort();
    }
    double micros = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count() / iterations;
    std::printf("%zu entries, %zu snapshot bytes: %.3f us / capture+restore (synthetic)\n",
                capacity, snapshot.size(), micros);
}
} // namespace

int main(int argc, char**) {
    RestoreDrainingParticlesAndLifetime();
    PreserveLastPassCompletionGuard();
    RejectWithoutPartialWrites();
    RejectOwnershipCorruptionAndBudgets();
    RestorePolylineHistory();
    std::puts("callback particle snapshot tests passed");
    if (argc > 1) {
        Benchmark(16);
        Benchmark(256);
    }
}
