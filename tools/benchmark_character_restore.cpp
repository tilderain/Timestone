// Benchmarks frozen legacy common-character code against an idealized,
// fixed-policy selective plan. See benchmark_character_restore.md for scope.
// current* names and CSV columns refer to that frozen oracle, not new production.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
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

using CopyFunction = void (*)(uint8_t*, const uint8_t*, size_t);

template<size_t N> void fixedCopy(uint8_t* dst, const uint8_t* src, size_t) {
    std::memcpy(dst, src, N);
}

void runtimeCopy(uint8_t* dst, const uint8_t* src, size_t size) {
    std::memcpy(dst, src, size);
}

// Keep the switch and precomputed dispatch experiments on the same size set.
#define COPY_SIZES(X) X(1) X(2) X(4) X(8) X(16) X(24) X(32) X(48) X(64) X(96) X(128)

CopyFunction copyFunction(size_t size) {
    switch (size) {
#define COPY_CASE(N) case N: return fixedCopy<N>;
        COPY_SIZES(COPY_CASE)
#undef COPY_CASE
        default: return runtimeCopy;
    }
}

struct CopyOperation {
    SnapshotRange range;
    CopyFunction copy;
};

struct Plan {
    std::vector<SnapshotRange> ranges;
    std::vector<SnapshotRange> lateRanges;
    std::vector<CopyOperation> operations, lateOperations;
    size_t bytes = 0;
};

enum class Kernel { Current, Runtime, SizeSwitch, Precomputed, InlineSmall, Inline64, Inline128, Count };
constexpr size_t kernelCount = static_cast<size_t>(Kernel::Count);
const char* kernelNames[] = {"current", "runtime_memcpy", "size_switch", "precomputed", "inline_small", "inline64", "inline128"};

template<size_t Limit>
inline void smallCopy(uint8_t* dst, const uint8_t* src, size_t size) {
    // Overlapping head/tail stores stay entirely inside the selected span.
    if (size >= Limit) {
        std::memcpy(dst, src, size);
    } else if (size >= 64) {
        std::memcpy(dst, src, 64);
        std::memcpy(dst + size - 64, src + size - 64, 64);
    } else if (size >= 32) {
        std::memcpy(dst, src, 32);
        std::memcpy(dst + size - 32, src + size - 32, 32);
    } else if (size >= 16) {
        std::memcpy(dst, src, 16);
        std::memcpy(dst + size - 16, src + size - 16, 16);
    } else if (size >= 8) {
        std::memcpy(dst, src, 8);
        std::memcpy(dst + size - 8, src + size - 8, 8);
    } else if (size >= 4) {
        std::memcpy(dst, src, 4);
        std::memcpy(dst + size - 4, src + size - 4, 4);
    } else if (size >= 2) {
        std::memcpy(dst, src, 2);
        std::memcpy(dst + size - 2, src + size - 2, 2);
    } else if (size) {
        *dst = *src;
    }
}

template<Kernel K> inline void copyRanges(uint8_t* live, const uint8_t* saved,
                                        const std::vector<SnapshotRange>& ranges,
                                        const std::vector<CopyOperation>& operations) {
    if constexpr (K == Kernel::Precomputed) {
        for (const auto& op : operations)
            op.copy(live + op.range.offset, saved + op.range.offset, op.range.size);
    } else {
        for (const auto& r : ranges) {
            auto* dst = live + r.offset;
            const auto* src = saved + r.offset;
            if constexpr (K == Kernel::InlineSmall) smallCopy<32>(dst, src, r.size);
            else if constexpr (K == Kernel::Inline64) smallCopy<64>(dst, src, r.size);
            else if constexpr (K == Kernel::Inline128) smallCopy<128>(dst, src, r.size);
            else if constexpr (K == Kernel::SizeSwitch) {
                switch (r.size) {
#define COPY_CASE(N) case N: std::memcpy(dst, src, N); break;
                    COPY_SIZES(COPY_CASE)
#undef COPY_CASE
                    default: std::memcpy(dst, src, r.size); break;
                }
            } else std::memcpy(dst, src, r.size);
        }
    }
}
#undef COPY_SIZES

template<Kernel K = Kernel::Runtime>
__attribute__((noinline)) void selectiveRestore(uint8_t* live, const uint8_t* saved,
                                               const Plan& plan) {
    copyRanges<K>(live, saved, plan.ranges, plan.operations);
    barrier();
    copyRanges<K>(live, saved, plan.lateRanges, plan.lateOperations);
}

using RestoreFunction = void (*)(uint8_t*, const uint8_t*, const Plan&);
const RestoreFunction selectiveKernels[] = {
    selectiveRestore<Kernel::Runtime>, selectiveRestore<Kernel::SizeSwitch>,
    selectiveRestore<Kernel::Precomputed>, selectiveRestore<Kernel::InlineSmall>,
    selectiveRestore<Kernel::Inline64>, selectiveRestore<Kernel::Inline128>
};

void verifyCopyKernels() {
    std::array<uint8_t, 512> live, saved, expected;
    saved.fill(0xa7);
    for (uint32_t size = 0; size <= 257; ++size) {
        for (uint32_t offset = 1; offset <= 16; ++offset) {
            Plan plan;
            plan.ranges.push_back({offset, size});
            plan.operations.push_back({plan.ranges.front(), copyFunction(size)});
            expected.fill(0x39);
            std::memcpy(expected.data() + offset, saved.data() + offset, size);
            for (auto restore : selectiveKernels) {
                live.fill(0x39);
                restore(live.data(), saved.data(), plan);
                if (live != expected) throw std::runtime_error("copy kernel span/guard mismatch");
            }
        }
    }
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
    for (const auto& r : plan.ranges) plan.operations.push_back({r, copyFunction(r.size)});
    for (const auto& r : plan.lateRanges) plan.lateOperations.push_back({r, copyFunction(r.size)});
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
            for (auto restore : selectiveKernels) {
                std::memcpy(live, before.data(), bytes);
                restore(live, saved, plan);
                if (std::memcmp(expected.data(), live, bytes) != 0) {
                    size_t at = 0;
                    while (expected[at] == live[at]) ++at;
                    throw std::runtime_error("selective mismatch at " + std::to_string(at) +
                                             " seed " + std::to_string(seed));
                }
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

constexpr Shape experimentShapes[] = {{"sparse32", 32, 0}, {"pointers200", 200, 0},
                                      {"false_positive256", 32, 256}, {"cap_stress600", 600, 0}};

void dumpPlans() {
    std::cout << "[";
    bool first = true;
    for (size_t bytes : {size_t(0x6a60), size_t(30000)}) {
        for (const auto& shape : experimentShapes) {
            Fixture f(bytes, 6, 1);
            const auto plan = buildPlan(f.live.data, f.snapshots.data, bytes, shape, f.roster.data());
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"bytes\":" << bytes << ",\"shape\":\"" << shape.name << "\"";
            for (bool late : {false, true}) {
                std::cout << (late ? ",\"late\":[" : ",\"early\":[");
                bool firstRange = true;
                for (const auto& r : late ? plan.lateRanges : plan.ranges) {
                    if (!firstRange) std::cout << ",";
                    firstRange = false;
                    std::cout << "[" << r.offset << "," << r.size << "]";
                }
                std::cout << "]";
            }
            std::cout << "}";
        }
    }
    std::cout << "]\n";
}

#ifdef FIXED_LAYOUT_EXPERIMENTS
using PhaseFunction = void (*)(uint8_t*, const uint8_t*);
struct FixedLayout {
    size_t bytes;
    const char* shape;
    PhaseFunction early, late;
    std::vector<SnapshotRange> earlyRanges, lateRanges;
};
#include "fixed_character_restore.generated.h"

bool contains(const std::string& filter, const std::string& value) {
    return filter == "all" || ("," + filter + ",").find("," + value + ",") != std::string::npos;
}

std::vector<SnapshotRange> fragment(const std::vector<SnapshotRange>& ranges) {
    std::vector<SnapshotRange> result;
    for (const auto& r : ranges) {
        for (uint32_t at = 0; at < r.size;) {
            uint32_t length = std::min(uint32_t(32), r.size - at);
            result.push_back({r.offset + at, length});
            if (length > 8) result.push_back({r.offset + at + length - 8, 8});
            at += length;
        }
    }
    return result;
}

std::vector<SnapshotRange> coalesce(std::vector<SnapshotRange> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
        return a.offset < b.offset;
    });
    std::vector<SnapshotRange> result;
    for (const auto& r : ranges) {
        if (!r.size) continue;
        const uint64_t end = uint64_t(r.offset) + r.size;
        if (end > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("invalid coalescing range");
        if (!result.empty() && r.offset <= uint64_t(result.back().offset) + result.back().size) {
            auto& last = result.back();
            last.size = static_cast<uint32_t>(std::max(end, uint64_t(last.offset) + last.size) - last.offset);
        } else result.push_back(r);
    }
    return result;
}

bool validRange(const SnapshotRange& r, size_t dstSize, size_t srcSize) {
    return r.offset <= dstSize && r.size <= dstSize - r.offset &&
           r.offset <= srcSize && r.size <= srcSize - r.offset;
}

// Insufficient buffers skip invalid spans, without forming out-of-bounds pointers.
// The layout fast path falls back to exactly these semantics for short buffers.
template<bool Once>
__attribute__((noinline)) void checkedPhase(uint8_t* dst, const uint8_t* src,
        size_t dstSize, size_t srcSize, const std::vector<SnapshotRange>& ranges, size_t extent) {
    if constexpr (Once) {
        if (dstSize >= extent && srcSize >= extent) {
            copyRanges<Kernel::Inline64>(dst, src, ranges, {});
            return;
        }
    }
    for (const auto& r : ranges)
        if (validRange(r, dstSize, srcSize)) smallCopy<64>(dst + r.offset, src + r.offset, r.size);
}

__attribute__((noinline)) void tablePhase(uint8_t* dst, const uint8_t* src,
                                        const std::vector<SnapshotRange>& ranges) {
    copyRanges<Kernel::Inline64>(dst, src, ranges, {});
}

struct ExperimentPlan {
    size_t id;
    Plan plan;
    std::array<std::vector<SnapshotRange>, 2> fragmented, merged;
    std::array<size_t, 2> extent{};
    const FixedLayout* fixed;
};

// Generated functions are unchecked internals, just like the unchecked table.
// A size-aware entry point keeps the same skip-invalid-span policy on short input.
void fixedCheckedPhase(uint8_t* dst, const uint8_t* src, size_t dstSize, size_t srcSize,
                       const ExperimentPlan& p, bool late) {
    if (dstSize >= p.extent[late] && srcSize >= p.extent[late])
        (late ? p.fixed->late : p.fixed->early)(dst, src);
    else checkedPhase<false>(dst, src, dstSize, srcSize,
                            late ? p.plan.lateRanges : p.plan.ranges, p.extent[late]);
}

enum class Experiment { Table, Fixed, Fragmented, Coalesced, BoundsRange, BoundsLayout, Lookup, Count };
const char* experimentNames[] = {"table_inline64", "fixed_cached", "fragmented_inline64", "coalesced_inline64",
                                "bounds_range", "bounds_layout", "fixed_lookup"};

void experimentPhase(Experiment mode, uint8_t* dst, const uint8_t* src,
                     const ExperimentPlan& p, bool late, size_t dstSize, size_t srcSize) {
    const auto& ranges = late ? p.plan.lateRanges : p.plan.ranges;
    switch (mode) {
        case Experiment::Fixed: (late ? p.fixed->late : p.fixed->early)(dst, src); break;
        case Experiment::Lookup: throw std::runtime_error("lookup must be resolved once per character");
        case Experiment::Fragmented: tablePhase(dst, src, p.fragmented[late]); break;
        case Experiment::Coalesced: tablePhase(dst, src, p.merged[late]); break;
        case Experiment::BoundsRange: checkedPhase<false>(dst, src, dstSize, srcSize, ranges, p.extent[late]); break;
        case Experiment::BoundsLayout: checkedPhase<true>(dst, src, dstSize, srcSize, ranges, p.extent[late]); break;
        default: tablePhase(dst, src, ranges); break;
    }
}

void validateExperiment(ExperimentPlan& p, Fixture& f, const Shape& shape) {
    if (p.fixed->bytes != f.bytes || std::string(p.fixed->shape) != shape.name)
        throw std::runtime_error("generated layout identity mismatch");
    for (bool late : {false, true}) {
        const auto& ranges = late ? p.plan.lateRanges : p.plan.ranges;
        const auto& generated = late ? p.fixed->lateRanges : p.fixed->earlyRanges;
        p.fragmented[late] = fragment(ranges);
        p.merged[late] = coalesce(p.fragmented[late]);
        auto mask = [&](const auto& rs) {
            std::vector<bool> selected(f.bytes, false);
            for (const auto& r : rs) {
                if (!validRange(r, f.bytes, f.bytes)) throw std::runtime_error("invalid fixture range");
                for (size_t i = r.offset; i < size_t(r.offset) + r.size; ++i) selected[i] = true;
            }
            return selected;
        };
        if (mask(ranges) != mask(p.fragmented[late]) || mask(ranges) != mask(p.merged[late]) ||
            mask(ranges) != mask(generated) || ranges.size() != p.merged[late].size())
            throw std::runtime_error("phase mask/generation mismatch");
        for (const auto& r : ranges) p.extent[late] = std::max(p.extent[late], size_t(r.offset) + r.size);
    }
    // Full-buffer comparisons include all excluded bytes and 64-byte outer guards.
    // Use actual fixture addresses for owner-pointer setup, then snapshot inputs.
    std::vector<uint8_t> before(f.bytes + 128, 0x39), saved(f.bytes + 128, 0xa7), expected, actual;
    for (uint32_t seed = 1001; seed <= 1032; ++seed) {
        fill(f.live.data, f.snapshots.data, f.bytes, shape, seed, f.roster.data());
        std::memcpy(before.data() + 64, f.live.data, f.bytes);
        std::memcpy(saved.data() + 64, f.snapshots.data, f.bytes);
        // The legacy oracle must run at fill's owner address, not the guarded copy.
        for (bool late : {false, true}) {
            std::memcpy(f.live.data, before.data() + 64, f.bytes);
            if (late) currentLate(f.live.data, saved.data() + 64, f.bytes, f.roster.data());
            else currentEarly(f.live.data, saved.data() + 64, f.bytes, f.roster.data());
            expected = before;
            std::memcpy(expected.data() + 64, f.live.data, f.bytes);
            for (size_t k = 0; k < size_t(Experiment::Count); ++k) {
                actual = before;
                if (Experiment(k) == Experiment::Lookup) {
                    const auto* resolved = resolveFixedLayout(p.id);
                    if (resolved != p.fixed) throw std::runtime_error("dispatch mismatch");
                    (late ? resolved->late : resolved->early)(actual.data() + 64, saved.data() + 64);
                } else experimentPhase(Experiment(k), actual.data() + 64, saved.data() + 64,
                                       p, late, f.bytes, f.bytes);
                if (actual != expected) throw std::runtime_error("experiment per-phase/guard mismatch");
            }
            before = expected; // Late phase sees early result, with no phase removal.
        }
    }
    for (bool late : {false, true}) {
        auto ranges = late ? p.plan.lateRanges : p.plan.ranges;
        for (size_t dstSize : {size_t(0), size_t(7), f.bytes / 2, f.bytes}) {
            for (size_t srcSize : {size_t(0), size_t(11), f.bytes / 2, f.bytes}) {
                expected = before;
                for (const auto& r : ranges)
                    if (validRange(r, dstSize, srcSize))
                        std::memcpy(expected.data() + 64 + r.offset, saved.data() + 64 + r.offset, r.size);
                actual = before;
                fixedCheckedPhase(actual.data() + 64, saved.data() + 64, dstSize, srcSize, p, late);
                if (actual != expected) throw std::runtime_error("generated short-buffer mismatch");
            }
        }
        // Explicit invalid/overflow-sized entries test the checked fallback.
        ranges.push_back({uint32_t(f.bytes - 1), 8});
        ranges.push_back({std::numeric_limits<uint32_t>::max(), 16});
        for (size_t dstSize : {size_t(0), size_t(7), f.bytes / 2, f.bytes}) {
            for (size_t srcSize : {size_t(0), size_t(11), f.bytes / 2, f.bytes}) {
                expected = before;
                for (const auto& r : ranges)
                    if (validRange(r, dstSize, srcSize))
                        std::memcpy(expected.data() + 64 + r.offset, saved.data() + 64 + r.offset, r.size);
                for (bool once : {false, true}) {
                    actual = before;
                    // Prepared layout extent uses wide arithmetic, including invalid ranges.
                    size_t extent = 0;
                    for (const auto& r : ranges) extent = std::max(extent, size_t(r.offset) + r.size);
                    if (once) checkedPhase<true>(actual.data() + 64, saved.data() + 64, dstSize, srcSize, ranges, extent);
                    else checkedPhase<false>(actual.data() + 64, saved.data() + 64, dstSize, srcSize, ranges, extent);
                    if (actual != expected) throw std::runtime_error("short-buffer/invalid-range mismatch");
                }
            }
        }
    }
}

template<Experiment Mode>
double measureExperiment(Fixture& f, const std::vector<ExperimentPlan*>& objects, double ms) {
    size_t object = 0, history = 0, operations = 0;
    auto start = Clock::now(), end = start;
    do {
        for (int batch = 0; batch < 32; ++batch) {
            auto* live = f.live.data + object * f.stride;
            const auto* saved = f.snapshots.data + (history * f.count + object) * f.stride;
            const auto& p = *objects[object];
            barrier();
            if constexpr (Mode == Experiment::Fixed || Mode == Experiment::Lookup) {
                const auto* selected = Mode == Experiment::Lookup ? resolveFixedLayout(p.id) : p.fixed;
                selected->early(live, saved);
                barrier();
                selected->late(live, saved);
            } else {
                experimentPhase(Mode, live, saved, p, false, f.bytes, f.bytes);
                barrier();
                experimentPhase(Mode, live, saved, p, true, f.bytes, f.bytes);
            }
            barrier();
            if (++object == f.count) { object = 0; if (++history == f.history) history = 0; }
            ++operations;
        }
        end = Clock::now();
    } while (operations < f.count * f.history ||
             std::chrono::duration<double, std::milli>(end - start).count() < ms);
    return std::chrono::duration<double, std::nano>(end - start).count() / operations;
}

void runExperiments(int samples, double ms, size_t streamMiB, const std::string& shapes,
                    const std::string& workloads, const std::string& sizes) {
    using Measure = double (*)(Fixture&, const std::vector<ExperimentPlan*>&, double);
    const Measure measures[] = {measureExperiment<Experiment::Table>, measureExperiment<Experiment::Fixed>,
        measureExperiment<Experiment::Fragmented>, measureExperiment<Experiment::Coalesced>,
        measureExperiment<Experiment::BoundsRange>, measureExperiment<Experiment::BoundsLayout>,
        measureExperiment<Experiment::Lookup>};
    constexpr size_t modes = size_t(Experiment::Count);
    std::cout << "bytes,shape,working_set,allocated_mib,mean_ranges,mean_fragmented_ranges,kernel,median_ns,min_ns,max_ns,table_speedup\n";
    for (size_t bytes : {size_t(0x6a60), size_t(30000)}) {
        if (!contains(sizes, std::to_string(bytes))) continue;
        std::vector<ExperimentPlan> plans;
        std::vector<const Shape*> selectedShapes;
        for (size_t s = 0; s < std::size(experimentShapes); ++s) {
            const auto& shape = experimentShapes[s];
            if (!contains(shapes, shape.name)) continue;
            Fixture f(bytes, 6, 1);
            const size_t id = (bytes == 30000 ? 4 : 0) + s;
            ExperimentPlan p{id, buildPlan(f.live.data, f.snapshots.data, bytes, shape, f.roster.data()),
                             {}, {}, {}, resolveFixedLayout(id)};
            validateExperiment(p, f, shape);
            plans.push_back(std::move(p));
            selectedShapes.push_back(&shape);
        }
        if (plans.empty()) throw std::runtime_error("no selected layouts");
        for (size_t s = 0; s < plans.size(); ++s) {
            for (const std::string workload : {"hot", "match_ring", "streaming", "mixed_ring", "mixed_streaming"}) {
                if (!contains(workloads, workload)) continue;
                const bool mixed = workload.find("mixed") == 0;
                if (mixed && s != 0) continue;
                const bool streaming = workload.find("streaming") != std::string::npos;
                size_t count = streaming ? std::max(size_t(6), streamMiB * 1048576 / ((bytes + 63) & ~size_t(63))) :
                               workload == "hot" ? 1 : 6;
                Fixture f(bytes, count, streaming || workload == "hot" ? 2 : 12);
                std::vector<ExperimentPlan*> objects;
                for (size_t n = 0; n < count; ++n) {
                    const size_t layout = mixed ? n % plans.size() : s;
                    objects.push_back(&plans[layout]);
                    for (size_t slot = 0; slot < f.history; ++slot)
                        fill(f.live.data + n * f.stride, f.snapshots.data + (slot * count + n) * f.stride,
                             bytes, *selectedShapes[layout], uint32_t(5000 + n * f.history + slot), f.rosters[n].data());
                }
                // Verify all actual object/history transitions and each phase before timing.
                std::vector<uint8_t> before(bytes), expected(bytes);
                for (size_t slot = 0; slot < f.history; ++slot) {
                    for (size_t n = 0; n < count; ++n) {
                        auto* live = f.live.data + n * f.stride;
                        const auto* saved = f.snapshots.data + (slot * count + n) * f.stride;
                        for (bool late : {false, true}) {
                            std::memcpy(before.data(), live, bytes);
                            if (late) currentLate(live, saved, bytes, f.rosters[n].data());
                            else currentEarly(live, saved, bytes, f.rosters[n].data());
                            std::memcpy(expected.data(), live, bytes);
                            for (size_t k = 0; k < modes; ++k) {
                                std::memcpy(live, before.data(), bytes);
                                const auto mode = Experiment(k);
                                if (mode == Experiment::Lookup) {
                                    const auto* resolved = resolveFixedLayout(objects[n]->id);
                                    (late ? resolved->late : resolved->early)(live, saved);
                                } else experimentPhase(mode, live, saved, *objects[n], late, bytes, bytes);
                                if (std::memcmp(live, expected.data(), bytes))
                                    throw std::runtime_error("experiment history/phase mismatch");
                            }
                        }
                    }
                }
                for (auto measure : measures) measure(f, objects, 3);
                std::array<std::vector<double>, modes> timings;
                for (int sample = 0; sample < samples; ++sample) {
                    for (size_t step = 0; step < modes; ++step) {
                        size_t k = (sample + (sample % 2 ? modes - step : step)) % modes;
                        timings[k].push_back(measures[k](f, objects, ms));
                    }
                }
                for (auto& values : timings) std::sort(values.begin(), values.end());
                double rangeCount = 0, fragmentCount = 0;
                for (const auto* p : objects) {
                    rangeCount += p->plan.ranges.size() + p->plan.lateRanges.size();
                    fragmentCount += p->fragmented[0].size() + p->fragmented[1].size();
                }
                for (size_t k = 0; k < modes; ++k) {
                    const auto& t = timings[k];
                    std::printf("%zu,%s,%s,%.3f,%.2f,%.2f,%s,%.2f,%.2f,%.2f,%.3f\n", bytes,
                        mixed ? "mixed" : selectedShapes[s]->name, workload.c_str(),
                        (f.live.size + f.snapshots.size + f.captured.size) / 1048576.0,
                        rangeCount / objects.size(), fragmentCount / objects.size(), experimentNames[k],
                        t[t.size() / 2], t.front(), t.back(), timings[0][samples / 2] / t[t.size() / 2]);
                }
                std::fflush(stdout);
            }
        }
    }
    std::cerr << "PASS: generated/table phase equality, exact fragmented/coalesced masks, excluded bytes/guards, "
                 "short buffers/invalid ranges, equivalent dispatch, and every timed history transition.\n";
}
#endif

template<Kernel K>
double measure(Fixture& f, const Plan& plan, bool capture, double ms) {
    size_t object = 0, history = 0, operations = 0;
    auto start = Clock::now(), end = start;
    do {
        for (int batch = 0; batch < 32; ++batch) {
            auto* live = f.live.data + object * f.stride;
            const auto* saved = f.snapshots.data + (history * f.count + object) * f.stride;
            barrier();
            if (capture) std::memcpy(f.captured.data + object * f.stride, live, f.bytes);
            if constexpr (K == Kernel::Current)
                currentRestore(live, saved, f.bytes, f.rosters[object].data());
            else selectiveRestore<K>(live, saved, plan);
            barrier();
            if (++object == f.count) { object = 0; if (++history == f.history) history = 0; }
            ++operations;
        }
        end = Clock::now();
    } while (operations < f.count * f.history ||
             std::chrono::duration<double, std::milli>(end - start).count() < ms);
    return std::chrono::duration<double, std::nano>(end - start).count() / operations;
}

using MeasureFunction = double (*)(Fixture&, const Plan&, bool, double);
const MeasureFunction measurements[] = {
    measure<Kernel::Current>, measure<Kernel::Runtime>, measure<Kernel::SizeSwitch>,
    measure<Kernel::Precomputed>, measure<Kernel::InlineSmall>,
    measure<Kernel::Inline64>, measure<Kernel::Inline128>
};

int main(int argc, char** argv) {
    try {
        std::cerr << "Legacy frozen-oracle harness: not the explicit production restore path.\n";
        if (argc == 2 && std::string(argv[1]) == "--dump-plans") { dumpPlans(); return 0; }
#ifdef FIXED_LAYOUT_EXPERIMENTS
        if (argc == 8 && std::string(argv[1]) == "--experiments") {
            const int samples = std::stoi(argv[2]);
            const double ms = std::stod(argv[3]);
            const size_t streamMiB = std::stoull(argv[4]);
            if (samples < 1 || ms <= 0 || !streamMiB) throw std::runtime_error("invalid arguments");
            verifyCopyKernels();
            runExperiments(samples, ms, streamMiB, argv[5], argv[6], argv[7]);
            return 0;
        }
#endif
        if (argc != 4) throw std::runtime_error("usage: benchmark samples ms stream-mib");
        int samples = std::stoi(argv[1]);
        double ms = std::stod(argv[2]);
        size_t streamMiB = std::stoull(argv[3]);
        if (samples < 1 || ms <= 0 || streamMiB == 0) throw std::runtime_error("invalid arguments");
        verifyCopyKernels();
        const Shape shapes[] = {{"sparse32", 32, 0}, {"pointers200", 200, 0},
                                {"false_positive256", 32, 256}, {"cap_stress600", 600, 0}};
        std::cout << "bytes,shape,working_set,allocated_mib,ranges,selected_bytes,phase,kernel,current_ns,selective_ns,speedup,current_min_ns,current_max_ns,selective_min_ns,selective_max_ns\n";
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
                            for (auto restore : selectiveKernels) {
                                std::memcpy(live, before.data(), bytes);
                                restore(live, saved, plan);
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
                    }
                    for (bool capture : {false, true}) {
                        for (auto measureKernel : measurements) measureKernel(f, plan, capture, 5);
                        std::array<std::vector<double>, kernelCount> timings;
                        for (int sample = 0; sample < samples; ++sample) {
                            // Rotate first position and reverse direction on alternate samples.
                            for (size_t step = 0; step < kernelCount; ++step) {
                                const size_t k = (sample + (sample % 2 ? kernelCount - step : step)) % kernelCount;
                                timings[k].push_back(measurements[k](f, plan, capture, ms));
                            }
                        }
                        for (auto& values : timings) std::sort(values.begin(), values.end());
                        const auto& a = timings[0];
                        for (size_t k = 1; k < kernelCount; ++k) {
                            const auto& b = timings[k];
                            const double am = a[a.size() / 2], bm = b[b.size() / 2];
                            std::printf("%zu,%s,%s,%.3f,%zu,%zu,%s,%s,%.2f,%.2f,%.3f,%.2f,%.2f,%.2f,%.2f\n",
                                bytes, shape.name, workload.c_str(),
                                (f.live.size + f.snapshots.size + f.captured.size) / 1048576.0,
                                plan.ranges.size() + plan.lateRanges.size(), plan.bytes,
                                capture ? "capture_restore" : "restore", kernelNames[k],
                                am, bm, am / bm, a.front(), a.back(), b.front(), b.back());
                        }
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
