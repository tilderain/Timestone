// Benchmarks extracted production common-character code against an idealized,
// fixed-policy selective plan. See benchmark_character_restore.md for scope.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "current_character_restore.generated.h"

using Clock = std::chrono::steady_clock;
inline void barrier() { asm volatile("" ::: "memory"); }

struct Buffer {
    uint8_t* data = nullptr;
    size_t size;
    explicit Buffer(size_t bytes) : size(bytes) {
        if (posix_memalign(reinterpret_cast<void**>(&data), 64, bytes) != 0)
            throw std::bad_alloc();
        std::memset(data, 0, bytes);
    }
    ~Buffer() { std::free(data); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
};

void put64(uint8_t* p, size_t offset, uint64_t value) {
    std::memcpy(p + offset, &value, sizeof(value));
}

struct Shape {
    const char* name;
    size_t pointers, floatPairs;
};

// All synthetic scalar bytes have their high bit set, keeping ordinary qwords
// outside IsLikelyPointer's address interval. Explicit fields below exercise
// pointer-shaped values and scalar false positives reproducibly.
void fill(uint8_t* live, uint8_t* saved, size_t bytes, const Shape& shape,
          uint32_t seed, const uint64_t* roster) {
    std::mt19937 rng(seed);
    for (size_t i = 0; i < bytes; ++i) {
        live[i] = static_cast<uint8_t>(0x80 | (rng() & 0x7f));
        saved[i] = live[i] ^ 0x40;
    }
    std::vector<size_t> positions;
    for (size_t i = 8; i + 8 <= bytes; i += 8) positions.push_back(i);
    std::mt19937 locations(9137); // Layout stays fixed as scalar values vary.
    std::shuffle(positions.begin(), positions.end(), locations);
    for (size_t i = 0; i < shape.pointers + shape.floatPairs; ++i) {
        if (i < shape.pointers) {
            // Valid opaque addresses; no pointee access is performed by the kernel.
            put64(live, positions[i], roster[(i + seed) % 6]);
            put64(saved, positions[i], roster[(i + seed + 1) % 6]);
        } else {
            put64(live, positions[i], 0x000000003f800000ull);
            put64(saved, positions[i], 0x0000000040000000ull);
        }
    }
    // Stable field types and owner identities. Initialize synthesized owner
    // fields as native setup would, so repeated restores remain stationary.
    for (size_t offset : {size_t(0x1888), size_t(0x2878), size_t(0x2898),
                          size_t(0x2a90), size_t(0x2ab0), size_t(0x2b78)}) {
        put64(live, offset, reinterpret_cast<uint64_t>(live));
        put64(saved, offset, reinterpret_cast<uint64_t>(live));
    }
    for (size_t n = 0; n < 6; ++n) {
        size_t offset = 0x2c50 + n * 0x30;
        put64(live, offset + 8, reinterpret_cast<uint64_t>(live));
        put64(saved, offset + 8, reinterpret_cast<uint64_t>(live));
        uint32_t type = static_cast<uint32_t>(n + 1);
        std::memcpy(live + offset + 0x10, &type, 4);
        std::memcpy(saved + offset + 0x10, &type, 4);
    }
    for (size_t offset = 0x1408; offset <= 0x1438; offset += 8) {
        put64(live, offset, roster[0]);
        put64(saved, offset, roster[0]);
    }
    // Exercise saved null and valid roster relationships, including late repair.
    std::vector<size_t> refs = {0x2b88, 0x6718, 0x6720, 0x6728, 0x6730};
    for (size_t base : {size_t(0x3bf0), size_t(0x3d60)}) {
        refs.push_back(base + 0xa8);
        put64(live, base + 0x100, reinterpret_cast<uint64_t>(live) + base + 0xb0);
        put64(saved, base + 0x100, reinterpret_cast<uint64_t>(live) + base + 0xb0);
        put64(live, base + 0x118, reinterpret_cast<uint64_t>(live) + 0x3bf0);
        put64(saved, base + 0x118, reinterpret_cast<uint64_t>(live) + 0x3bf0);
    }
    for (size_t n = 0; n < 5; ++n) refs.push_back(0x6498 + n * 0x80);
    for (size_t table : {size_t(0x18c8), size_t(0x1bc8), size_t(0x1ec8)})
        for (size_t n = 0; n < 32; ++n) refs.push_back(table + n * 0x18 + 8);
    for (size_t n = 0; n < refs.size(); ++n) {
        put64(live, refs[n], roster[(n + seed) % 6]);
        // Null/non-null changes are within each field's established policy.
        put64(saved, refs[n], (seed % 3 == 0) ? 0 : roster[(n + seed + 1) % 6]);
    }
    // No embedded CPU object; the real kernel still executes its search loop.
}

struct Plan {
    std::vector<SnapshotRange> ranges;
    std::vector<SnapshotRange> lateRanges;
    size_t bytes = 0;
};

__attribute__((noinline)) void selectiveRestore(uint8_t* live, const uint8_t* saved,
                                               const Plan& plan) {
    for (const auto& r : plan.ranges)
        std::memcpy(live + r.offset, saved + r.offset, r.size);
    barrier();
    for (const auto& r : plan.lateRanges)
        std::memcpy(live + r.offset, saved + r.offset, r.size);
}

// Derive byte provenance from many reference executions for THIS fixed fixture
// family. This is an oracle-assisted performance experiment, NOT discovery of a
// production-safe schema. No generation/training work occurs in measured loops.
Plan buildPlan(uint8_t* live, uint8_t* saved, size_t bytes, const Shape& shape,
               const uint64_t* roster, bool late = false) {
    std::vector<bool> canKeep(bytes, true), canCopy(bytes, true);
    std::vector<uint8_t> before(bytes);
    for (uint32_t seed = 1; seed <= 48; ++seed) {
        // Vary all address bytes, not just nearby allocation low bits. Otherwise
        // an ambiguous high byte can be incorrectly omitted from a copied field.
        std::array<uint64_t, 6> trainingRoster;
        trainingRoster[0] = reinterpret_cast<uint64_t>(live);
        std::mt19937 addresses(seed * 1777);
        for (size_t n = 1; n < trainingRoster.size(); ++n)
            trainingRoster[n] = 0x10000 | (((uint64_t(addresses()) << 32) | addresses()) &
                                          0x00007ffffffffff8ull);
        fill(live, saved, bytes, shape, seed, trainingRoster.data());
        std::memcpy(before.data(), live, bytes);
        if (late) currentLate(live, saved, bytes, trainingRoster.data());
        else currentEarly(live, saved, bytes, trainingRoster.data());
        for (size_t i = 0; i < bytes; ++i) {
            canKeep[i] = canKeep[i] && live[i] == before[i];
            canCopy[i] = canCopy[i] && live[i] == saved[i];
            if (!canKeep[i] && !canCopy[i])
                throw std::runtime_error("fixture needs a conditional fixup at " + std::to_string(i));
        }
    }
    Plan plan;
    size_t start = 0;
    while (start < bytes) {
        if (canKeep[start]) { ++start; continue; }
        size_t end = start + 1;
        while (end < bytes && !canKeep[end]) ++end;
        plan.ranges.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(end - start)});
        plan.bytes += end - start;
        start = end;
    }
    if (!late) {
        const Plan latePlan = buildPlan(live, saved, bytes, shape, roster, true);
        plan.lateRanges = latePlan.ranges;
        plan.bytes += latePlan.bytes;
    }
    return plan;
}

void verify(uint8_t* live, uint8_t* saved, size_t bytes, const Shape& shape,
            const uint64_t* roster, const Plan& plan) {
    std::vector<uint8_t> before(bytes), expected(bytes);
    for (uint32_t seed = 1001; seed <= 1032; ++seed) {
        fill(live, saved, bytes, shape, seed, roster);
        for (int repeat = 0; repeat < 3; ++repeat) {
            std::memcpy(before.data(), live, bytes);
            currentRestore(live, saved, bytes, roster);
            std::memcpy(expected.data(), live, bytes);
            std::memcpy(live, before.data(), bytes);
            selectiveRestore(live, saved, plan);
            if (std::memcmp(expected.data(), live, bytes) != 0) {
                size_t at = 0;
                while (expected[at] == live[at]) ++at;
                throw std::runtime_error("selective mismatch at " + std::to_string(at) +
                                         " seed " + std::to_string(seed));
            }
        }
    }
}

void demonstratePolicyDrift(uint8_t* live, uint8_t* saved, size_t bytes,
                            const Shape& shape, const uint64_t* roster, const Plan& plan) {
    // A scalar-to-pointer transition outside this fixture's fixed policy must
    // NOT silently be claimed safe by the inferred plan.
    fill(live, saved, bytes, shape, 2019, roster);
    put64(live, 0x3100, roster[0]);
    std::vector<uint8_t> before(live, live + bytes), expected(bytes);
    currentRestore(live, saved, bytes, roster);
    std::memcpy(expected.data(), live, bytes);
    std::memcpy(live, before.data(), bytes);
    selectiveRestore(live, saved, plan);
    if (std::memcmp(live, expected.data(), bytes) == 0)
        throw std::runtime_error("policy-drift negative test did not trigger");
}

struct Fixture {
    size_t bytes, stride, count, history;
    Buffer live, snapshots, captured;
    std::array<uint64_t, 6> roster;
    std::vector<std::array<uint64_t, 6>> rosters;
    Fixture(size_t size, size_t characters, size_t slots)
        : bytes(size), stride((size + 63) & ~size_t(63)), count(characters), history(slots),
          live(stride * std::max(size_t(6), count)), snapshots(stride * count * history),
          captured(stride * count) {
        for (size_t n = 0; n < 6; ++n) roster[n] = reinterpret_cast<uint64_t>(live.data + n * stride);
        rosters.assign(count, roster);
        for (size_t n = 0; n < count; ++n)
            rosters[n][n % 6] = reinterpret_cast<uint64_t>(live.data + n * stride);
    }
    void initialize(const Shape& shape) {
        for (size_t n = 0; n < count; ++n) {
            for (size_t slot = 0; slot < history; ++slot)
                fill(live.data + n * stride, snapshots.data + (slot * count + n) * stride,
                     bytes, shape, static_cast<uint32_t>(5000 + n * history + slot), rosters[n].data());
        }
    }
};

double measure(Fixture& f, const Plan& plan, bool selective, bool capture, double ms) {
    size_t object = 0, history = 0, operations = 0;
    auto start = Clock::now(), end = start;
    do {
        for (int batch = 0; batch < 32; ++batch) {
            auto* live = f.live.data + object * f.stride;
            const auto* saved = f.snapshots.data + (history * f.count + object) * f.stride;
            barrier();
            if (capture) std::memcpy(f.captured.data + object * f.stride, live, f.bytes);
            if (selective) selectiveRestore(live, saved, plan);
            else currentRestore(live, saved, f.bytes, f.rosters[object].data());
            barrier();
            if (++object == f.count) { object = 0; if (++history == f.history) history = 0; }
            ++operations;
        }
        end = Clock::now();
    } while (operations < f.count * f.history ||
             std::chrono::duration<double, std::milli>(end - start).count() < ms);
    return std::chrono::duration<double, std::nano>(end - start).count() / operations;
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: benchmark samples ms stream-mib");
        int samples = std::stoi(argv[1]);
        double ms = std::stod(argv[2]);
        size_t streamMiB = std::stoull(argv[3]);
        if (samples < 1 || ms <= 0 || streamMiB == 0) throw std::runtime_error("invalid arguments");
        const Shape shapes[] = {{"sparse32", 32, 0}, {"pointers200", 200, 0},
                                {"false_positive256", 32, 256}, {"cap_stress600", 600, 0}};
        std::cout << "bytes,shape,working_set,allocated_mib,ranges,selected_bytes,phase,current_ns,selective_ns,speedup,current_min_ns,current_max_ns,selective_min_ns,selective_max_ns\n";
        for (size_t bytes : {size_t(0x6a60), size_t(30000)}) {
            for (const Shape& shape : shapes) {
                for (const std::string workload : {"hot", "match_ring", "streaming"}) {
                    size_t count = workload == "hot" ? 1 : workload == "match_ring" ? 6 :
                        std::max(size_t(6), streamMiB * 1048576 / ((bytes + 63) & ~size_t(63)));
                    Fixture f(bytes, count, workload == "match_ring" ? 12 : 2);
                    Plan plan = buildPlan(f.live.data, f.snapshots.data, bytes, shape, f.roster.data());
                    if (std::string(shape.name) == "sparse32" && workload == "hot")
                        demonstratePolicyDrift(f.live.data, f.snapshots.data, bytes, shape,
                                              f.roster.data(), plan);
                    // Verify at multiple actual addresses and independent scalar/input values.
                    for (size_t n = 0; n < std::min(count, size_t(6)); ++n)
                        verify(f.live.data + n * f.stride, f.snapshots.data, bytes, shape,
                               f.roster.data(), plan);
                    f.initialize(shape);
                    // Validate every history transition before using this plan for timing.
                    std::vector<uint8_t> before(bytes), expected(bytes);
                    for (size_t slot = 0; slot < f.history; ++slot) {
                        for (size_t n = 0; n < f.count; ++n) {
                            auto* live = f.live.data + n * f.stride;
                            const auto* saved = f.snapshots.data + (slot * f.count + n) * f.stride;
                            std::memcpy(before.data(), live, bytes);
                            currentRestore(live, saved, bytes, f.rosters[n].data());
                            std::memcpy(expected.data(), live, bytes);
                            std::memcpy(live, before.data(), bytes);
                            selectiveRestore(live, saved, plan);
                            if (std::memcmp(live, expected.data(), bytes)) {
                                size_t at = 0;
                                while (live[at] == expected[at]) ++at;
                                throw std::runtime_error("history-transition equivalence failed: " +
                                    std::string(shape.name) + " " + workload + " byte " +
                                    std::to_string(at) + " slot " + std::to_string(slot) +
                                    " object " + std::to_string(n));
                            }
                        }
                    }
                    for (bool capture : {false, true}) {
                        measure(f, plan, false, capture, 5);
                        measure(f, plan, true, capture, 5);
                        std::vector<double> a, b;
                        for (int sample = 0; sample < samples; ++sample) {
                            if (sample % 2 == 0) {
                                a.push_back(measure(f, plan, false, capture, ms));
                                b.push_back(measure(f, plan, true, capture, ms));
                            } else {
                                b.push_back(measure(f, plan, true, capture, ms));
                                a.push_back(measure(f, plan, false, capture, ms));
                            }
                        }
                        std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
                        const double am = a[a.size() / 2], bm = b[b.size() / 2];
                        std::printf("%zu,%s,%s,%.3f,%zu,%zu,%s,%.2f,%.2f,%.3f,%.2f,%.2f,%.2f,%.2f\n",
                            bytes, shape.name, workload.c_str(),
                            (f.live.size + f.snapshots.size + f.captured.size) / 1048576.0,
                            plan.ranges.size() + plan.lateRanges.size(), plan.bytes,
                            capture ? "capture_restore" : "restore",
                            am, bm, am / bm, a.front(), a.back(), b.front(), b.back());
                        std::fflush(stdout);
                    }
                }
            }
        }
        std::cerr << "PASS: independent fixture values, address changes, repeated restores and history transitions.\n"
                     "PASS: out-of-policy pointer transition demonstrates that plans are fixture-specific, not production schemas.\n";
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
