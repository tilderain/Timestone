// Standalone payload experiment. Production sources are read/linked, never edited.
#include "rollback_character_restore.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#define NOINLINE __attribute__((noinline))
using Kernel = void (*)(uint8_t*, const uint8_t*, size_t);
struct Span { size_t offset, size; };
struct Case {
    const char* name;
    size_t full, packed, count, originalBytes, uniqueBytes;
    int zero;
    Kernel current, fixed, pack, unpack;
    std::vector<Span> spans;
};
#include "subsystem.generated.h"

static void barrier() { std::atomic_signal_fence(std::memory_order_seq_cst); }
static void require(bool condition) {
    if (!condition) throw std::runtime_error("correctness check failed");
}
NOINLINE static void fullCapture(uint8_t* dst, const uint8_t* src, size_t n) {
    std::memcpy(dst, src, n);
}
static void fill(std::vector<uint8_t>& bytes, uint32_t seed) {
    for (auto& byte : bytes) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        byte = uint8_t(seed);
    }
}

static void verify(const Case& c) {
    std::vector<uint8_t> source(c.full), live(c.full), packed(c.packed + 32, 0xA5);
    fill(source, 0x13579); fill(live, 0x24681);
    // Single-byte source mutations exercise every copied AND excluded position.
    for (size_t mutation = 0; mutation <= c.full; ++mutation) {
        if (mutation < c.full) source[mutation] ^= 0xFF;
        auto oracle = live, fixed = live, restored = live, expected = live;
        c.current(oracle.data(), source.data(), c.full);
        c.fixed(fixed.data(), source.data(), c.full);
        c.pack(packed.data(), source.data(), c.full);
        for (size_t i = c.packed; i < packed.size(); ++i) require(packed[i] == 0xA5);
        size_t cursor = 0;
        for (const auto& s : c.spans) {
            require(std::memcmp(packed.data() + cursor, source.data() + s.offset, s.size) == 0);
            std::memcpy(expected.data() + s.offset, source.data() + s.offset, s.size);
            cursor += s.size;
        }
        if (c.packed > cursor) { // Team's active-reference identity is retained, not repaired here.
            require(c.packed - cursor == 8);
            require(std::memcmp(packed.data() + cursor, source.data() + 0x48, 8) == 0);
        }
        if (c.zero >= 0) expected[size_t(c.zero)] = 0;
        // Poison the entire original image: unpack must only use its packed input.
        auto saved = source;
        std::fill(source.begin(), source.end(), 0xCC);
        c.unpack(restored.data(), packed.data(), c.packed);
        require(oracle == expected && fixed == expected && restored == expected);
        source = saved;
        if (mutation < c.full) source[mutation] ^= 0xFF;
    }
    // Mutate every destination byte, including excluded ownership fields.
    for (size_t i = 0; i < c.full; ++i) {
        auto a = live, b = live;
        a[i] ^= 0xFF; b[i] ^= 0xFF;
        c.pack(packed.data(), source.data(), c.full);
        c.current(a.data(), source.data(), c.full);
        c.unpack(b.data(), packed.data(), c.packed);
        require(a == b);
    }
    // Current partial-range semantics are preserved by fixed's explicit fallback.
    // Packed format accepts exact full records only; reject before any write.
    for (size_t n = 0; n < c.full; ++n) {
        auto a = live, b = live;
        std::vector<uint8_t> shortSource(source.begin(), source.begin() + n);
        c.current(a.data(), shortSource.data(), n);
        c.fixed(b.data(), shortSource.data(), n);
        require(a == b);
        auto before = packed;
        c.pack(packed.data(), shortSource.data(), n);
        require(packed == before);
    }
    for (size_t n = 0; n < c.packed; ++n) {
        auto a = live;
        std::vector<uint8_t> shortSource(packed.begin(), packed.begin() + n);
        c.unpack(a.data(), shortSource.data(), n);
        require(a == live);
    }
    // Reuse every ring position with changing bytes, then restore nonmonotonically.
    std::vector<uint8_t> ring(c.packed * 16), fullRing(c.full * 16);
    for (size_t frame = 0; frame < 64; ++frame) {
        fill(source, uint32_t(frame + 123));
        size_t slot = frame & 15;
        c.pack(ring.data() + slot*c.packed, source.data(), c.full);
        std::memcpy(fullRing.data() + slot*c.full, source.data(), c.full);
        if (frame < 15) continue;
        for (size_t step = 0; step < 16; ++step) {
            size_t rewind = (slot + step*7) & 15;
            auto a = live, b = live;
            c.current(a.data(), fullRing.data() + rewind*c.full, c.full);
            c.unpack(b.data(), ring.data() + rewind*c.packed, c.packed);
            require(a == b);
        }
    }
    std::fprintf(stderr, "%s: full destinations, exclusions, source/destination mutations, packed roundtrip, source poisoning, all truncations, changing ring reuse PASS\n", c.name);
}

struct Buffers {
    std::vector<uint8_t> source, snapshot, destination;
};

NOINLINE static double measure(const Case& c, Buffers& b, size_t slots,
                                int variant, int phase, size_t iterations) {
    Kernel capture = variant == 2 ? c.pack : fullCapture;
    Kernel restore = variant == 2 ? c.unpack : variant == 1 ? c.fixed : c.current;
    const size_t representation = variant == 2 ? c.packed : c.full;
    auto start = std::chrono::steady_clock::now();
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        size_t slot = iteration & (slots - 1);
        for (size_t object = 0; object < c.count; ++object) {
            // Live objects are shared across the snapshot ring, as in production.
            uint8_t* snapshot = b.snapshot.data() + (slot * c.count + object) * representation;
            uint8_t* destination = b.destination.data() + object * c.full;
            const uint8_t* source = b.source.data() + object * c.full;
            if (phase != 1) capture(snapshot, source, c.full);
            barrier();
            if (phase != 0) restore(destination, snapshot, representation);
            barrier();
        }
    }
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - start).count() /
        double(iterations * c.count);
}

int main(int argc, char** argv) {
    try {
        int trials = argc > 1 ? std::atoi(argv[1]) : 9;
        size_t iterations = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20000;
        require(trials >= 7 && iterations > 0);
        for (const auto& c : cases) verify(c);
        std::puts("case,workload,phase,variant,count,median_ns_op,median_ms_op,median_ms_count,capture_bytes_op,restore_copy_bytes_op,forced_store_bytes_op,trials_ns_op");
        const char* phases[] = {"capture", "restore", "combined"};
        const char* variants[] = {"current", "fixed", "packed"};
        for (const auto& c : cases) for (size_t slots : {size_t(1), size_t(16)}) {
            Buffers buffers[3];
            for (int v = 0; v < 3; ++v) {
                auto& b = buffers[v];
                size_t rep = v == 2 ? c.packed : c.full;
                b.source.resize(c.full * c.count);
                b.destination.resize(c.full * c.count);
                b.snapshot.resize(rep * c.count * slots);
                fill(b.source, 0xA123); fill(b.destination, 0xB234);
                for (size_t s = 0; s < slots; ++s) for (size_t o = 0; o < c.count; ++o)
                    (v == 2 ? c.pack : fullCapture)(b.snapshot.data() + (s*c.count+o)*rep,
                                                   b.source.data() + o*c.full, c.full);
            }
            for (int phase = 0; phase < 3; ++phase) {
                std::vector<double> samples[3];
                for (auto& s : samples) s.reserve(trials);
                for (int v = 0; v < 3; ++v) measure(c, buffers[v], slots, v, phase, 1000);
                for (int t = 0; t < trials; ++t) for (int order = 0; order < 3; ++order) {
                    int v = t % 2 ? 2 - order : order;
                    samples[v].push_back(measure(c, buffers[v], slots, v, phase, iterations));
                }
                for (int v = 0; v < 3; ++v) {
                    auto sorted = samples[v];
                    std::sort(sorted.begin(), sorted.end());
                    double median = (sorted[(trials-1)/2] + sorted[trials/2]) / 2;
                    std::printf("%s,%s,%s,%s,%zu,%.6f,%.9f,%.9f,%zu,%zu,%d,\"",
                        c.name, slots == 1 ? "hot" : "ring16", phases[phase], variants[v], c.count,
                        median, median / 1e6, median * c.count / 1e6,
                        phase == 1 ? 0 : v == 2 ? c.packed : c.full,
                        phase == 0 ? 0 : v == 0 ? c.originalBytes : c.uniqueBytes,
                        phase != 0 && c.zero >= 0 ? 1 : 0);
                    for (double s : samples[v]) std::printf("%.6f;", s);
                    std::puts("\"");
                }
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
