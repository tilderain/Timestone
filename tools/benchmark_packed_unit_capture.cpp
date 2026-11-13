// Host microbenchmark: full-body capture (current) vs packed capture of only the
// bytes restore needs, for shots, shot-driven models and effect roots.
//
// Two steps, because the best packed kernel has compile-time offsets/sizes
// (like the current restore tables) and those come from the derived byte set:
//   g++ -std=c++20 -O3 -DNDEBUG -I UMVC3Hook/code tools/benchmark_packed_unit_capture.cpp -o $TMPDIR/bpuc0
//   $TMPDIR/bpuc0 --emit > $TMPDIR/packed_plan.inc
//   g++ -std=c++20 -O3 -DNDEBUG -I UMVC3Hook/code -DPLAN_INC='"'$TMPDIR/packed_plan.inc'"' \
//       tools/benchmark_packed_unit_capture.cpp -o $TMPDIR/bpuc
//   taskset -c 2 $TMPDIR/bpuc [iterations]
//
// Current kernels are the body-byte parts of RollbackUnit_RestoreShots
// (storage-stage counts, RestoreShotCpuFields, RestoreShotTail with no holes,
// +123D), RestoreDrivenModel (same-storage path) and the effect root commit,
// copied from rollback_unit_shot_*.inl / rollback_unit.cpp. Rollback_RestoreActorFields
// is the real header. The packed byte set is derived by running the current
// kernel against two complementary saved images (write set), plus fields other
// stages only READ from the saved image (read set: flags, group mask, ID/team,
// unit references, storage identity). Read-only bytes are stored, never written.
// Shot tail [+2140, extent) is captured and restored whole in every variant (its
// holes are decided at restore time). No native memory, no timed allocation.
#include "rollback_character_restore.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

template<class T> static T Read(const void* o, size_t at) { T v; std::memcpy(&v, (const uint8_t*)o + at, sizeof v); return v; }
template<class T> static void Write(void* o, size_t at, T v) { std::memcpy((uint8_t*)o + at, &v, sizeof v); }
static inline void Clobber() { asm volatile("" ::: "memory"); }

// ---------------- Current kernels ----------------
constexpr size_t ShotCommon = 0x2140;
static void RestoreShotCpuFields(uint8_t* live, const uint8_t* saved)
{
    Rollback_RestoreActorFields(live, saved, ActorShot);
    static constexpr struct { size_t offset, size; } ranges[] = {
        {0x1480, 0x0C}, {0x1490, 0x0C}, {0x14A0, 0x20}, {0x1DD0, 0x60}, {0x1E30, 0x120},
        {0x1F58, 0x10}, {0x1F70, 0x70}, {0x1FE4, 0x1C}, {0x2000, 0x38}, {0x2038, 0x20},
        {0x2078, 0x40}, {0x20C0, 0x0C}, {0x20D0, 0x04}, {0x2118, 0x1B},
    };
    for (const auto& r : ranges) std::memcpy(live + r.offset, saved + r.offset, r.size);
    Write(live, 0x149C, (Read<uint32_t>(saved, 0x149C) & ~0x40u) | (Read<uint32_t>(live, 0x149C) & 0x40u));
    Write(live, 0x1FE0, (Read<uint32_t>(saved, 0x1FE0) & ~0x10000000u) | (Read<uint32_t>(live, 0x1FE0) & 0x10000000u));
    for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18)
        std::memcpy(live + record + 8, saved + record + 8, 0x10);
}
__attribute__((noinline)) static void CurrentShotRestore(uint8_t* live, const uint8_t* saved, size_t extent)
{
    for (size_t f : {size_t(0x530), size_t(0x550), size_t(0x554), size_t(0x558), size_t(0x1238)})
        Write(live, f, Read<uint32_t>(saved, f));
    Write(live, 0x540, Read<uintptr_t>(saved, 0x540));
    RestoreShotCpuFields(live, saved);
    std::memcpy(live + ShotCommon, saved + ShotCommon, extent - ShotCommon);
    Write(live, 0x123D, Read<uint8_t>(saved, 0x123D));
}

// Same full-image restore, tables expanded at compile time into fixed-size copies
// (MinGW GCC 10 leaves the table loops above as one memcpy call per row).
template<const auto& Rows, uint8_t Owner> __attribute__((always_inline)) inline void CopyRows(uint8_t* live, const uint8_t* saved)
{
    [&]<size_t... I>(std::index_sequence<I...>) {
        (((Rows[I].owners & Owner) ? (void)std::memcpy(live + Rows[I].offset, saved + Rows[I].offset, Rows[I].size) : (void)0), ...);
    }(std::make_index_sequence<std::size(Rows)>{});
}
struct PlainRange { size_t offset, size; };
inline constexpr PlainRange ShotRanges[] = {
    {0x1480, 0x0C}, {0x1490, 0x0C}, {0x14A0, 0x20}, {0x1DD0, 0x60}, {0x1E30, 0x120},
    {0x1F58, 0x10}, {0x1F70, 0x70}, {0x1FE4, 0x1C}, {0x2000, 0x38}, {0x2038, 0x20},
    {0x2078, 0x40}, {0x20C0, 0x0C}, {0x20D0, 0x04}, {0x2118, 0x1B},
};
__attribute__((always_inline)) inline void UnrolledActorFields(uint8_t* live, const uint8_t* saved)
{
    CopyRows<ActorFieldRows, ActorShot>(live, saved);
    for (size_t motion = 0x560; motion < 0x1160; motion += 0x180)
        CopyRows<ActorMotionRows, ActorShot>(live + motion, saved + motion);
}
__attribute__((noinline)) static void UnrolledShotRestore(uint8_t* live, const uint8_t* saved, size_t extent)
{
    for (size_t f : {size_t(0x530), size_t(0x550), size_t(0x554), size_t(0x558), size_t(0x1238)})
        Write(live, f, Read<uint32_t>(saved, f));
    Write(live, 0x540, Read<uintptr_t>(saved, 0x540));
    UnrolledActorFields(live, saved);
    [&]<size_t... I>(std::index_sequence<I...>) {
        ((std::memcpy(live + ShotRanges[I].offset, saved + ShotRanges[I].offset, ShotRanges[I].size)), ...);
    }(std::make_index_sequence<std::size(ShotRanges)>{});
    Write(live, 0x149C, (Read<uint32_t>(saved, 0x149C) & ~0x40u) | (Read<uint32_t>(live, 0x149C) & 0x40u));
    Write(live, 0x1FE0, (Read<uint32_t>(saved, 0x1FE0) & ~0x10000000u) | (Read<uint32_t>(live, 0x1FE0) & 0x10000000u));
    for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18)
        std::memcpy(live + record + 8, saved + record + 8, 0x10);
    std::memcpy(live + ShotCommon, saved + ShotCommon, extent - ShotCommon);
    Write(live, 0x123D, Read<uint8_t>(saved, 0x123D));
}
static bool SameModelStorage(const uint8_t* live, const uint8_t* saved);
__attribute__((noinline)) static void UnrolledModelRestore(uint8_t* live, const uint8_t* saved, size_t)
{
    if (SameModelStorage(live, saved)) UnrolledActorFields(live, saved);
    std::memcpy(live + 0x1CC0, saved + 0x1CC0, 0x40);
}

constexpr size_t ModelExtent = 0x1D00, ModelLifeAt = 0x1CC0, ModelLifeEnd = 0x1D00;
static bool SameModelStorage(const uint8_t* live, const uint8_t* saved)
{
    bool same = true;
    for (size_t f : {size_t(0x530), size_t(0x550), size_t(0x554), size_t(0x558), size_t(0x1238)})
        same &= Read<uint32_t>(live, f) == Read<uint32_t>(saved, f);
    for (size_t f : {size_t(0x538), size_t(0x540), size_t(0x548), size_t(0x11B0)})
        same &= Read<uintptr_t>(live, f) == Read<uintptr_t>(saved, f);
    for (size_t m = 0; m < 8; ++m)
        for (size_t f : {size_t(0x6C0), size_t(0x6D8)})
            same &= Read<uintptr_t>(live, f + m * 0x180) == Read<uintptr_t>(saved, f + m * 0x180);
    return same;
}
__attribute__((noinline)) static void CurrentModelRestore(uint8_t* live, const uint8_t* saved, size_t)
{
    if (SameModelStorage(live, saved)) Rollback_RestoreActorFields(live, saved, ActorShot);
    std::memcpy(live + ModelLifeAt, saved + ModelLifeAt, ModelLifeEnd - ModelLifeAt);
}
constexpr size_t EffectExtent = 0x2C0;
__attribute__((noinline)) static void CurrentEffectRestore(uint8_t* live, const uint8_t* saved, size_t)
{
    std::memcpy(live + 0x40, saved + 0x40, 0x280);
}
__attribute__((noinline)) static void FullCapture(uint8_t* dst, const uint8_t* src, size_t extent) { std::memcpy(dst, src, extent); }
__attribute__((noinline)) static void FullRestore(uint8_t* live, const uint8_t* saved, size_t extent) { std::memcpy(live, saved, extent); }
// Production effect root: constant-size copies (GCC emits rep movsq for both, MinGW build included).
__attribute__((noinline)) static void EffectCaptureAsBuilt(uint8_t* dst, const uint8_t* src, size_t) { std::memcpy(dst, src, EffectExtent); }
// The same 0x280 commit through a library memcpy call (runtime size), not rep movsq.
__attribute__((noinline)) static void EffectRestoreLibcall(uint8_t* live, const uint8_t* saved, size_t extent)
{
    std::memcpy(live + 0x40, saved + 0x40, extent - 0x40);
}
// Candidate: fixed-size copy as unaligned SSE2 moves, independent of the C library.
#include <emmintrin.h>
template<size_t Bytes> __attribute__((always_inline)) inline void CopyFixed(void* destination, const void* source)
{
    static_assert(Bytes % 16 == 0);
    auto* to = static_cast<uint8_t*>(destination);
    const auto* from = static_cast<const uint8_t*>(source);
    for (size_t at = 0; at < Bytes; at += 16)
        _mm_storeu_si128((__m128i*)(to + at), _mm_loadu_si128((const __m128i*)(from + at)));
}
__attribute__((noinline)) static void EffectCaptureSse(uint8_t* dst, const uint8_t* src, size_t) { CopyFixed<EffectExtent>(dst, src); }
__attribute__((noinline)) static void EffectRestoreSse(uint8_t* live, const uint8_t* saved, size_t) { CopyFixed<0x280>(live + 0x40, saved + 0x40); }
template<size_t Bytes> __attribute__((always_inline)) inline void CopyFixed32(void* destination, const void* source)
{
    static_assert(Bytes % 32 == 0);
    auto* to = static_cast<uint8_t*>(destination);
    const auto* from = static_cast<const uint8_t*>(source);
    for (size_t at = 0; at < Bytes; at += 32) { // both loads before either store
        const __m128i a = _mm_loadu_si128((const __m128i*)(from + at)), b = _mm_loadu_si128((const __m128i*)(from + at + 16));
        _mm_storeu_si128((__m128i*)(to + at), a); _mm_storeu_si128((__m128i*)(to + at + 16), b);
    }
}
__attribute__((noinline)) static void EffectCaptureSse32(uint8_t* dst, const uint8_t* src, size_t) { CopyFixed32<EffectExtent>(dst, src); }
__attribute__((noinline)) static void EffectRestoreSse32(uint8_t* live, const uint8_t* saved, size_t) { CopyFixed32<0x280>(live + 0x40, saved + 0x40); }
static size_t g_packedEffectBytes = 0;
__attribute__((noinline)) static void EffectPackedCaptureLibcall(uint8_t* dst, const uint8_t* src, size_t) { std::memcpy(dst, src + 0x40, g_packedEffectBytes); }
__attribute__((noinline)) static void EffectPackedRestoreLibcall(uint8_t* live, const uint8_t* packed, size_t) { std::memcpy(live + 0x40, packed, g_packedEffectBytes); }

// ---------------- Plan derivation ----------------
struct Range { uint32_t offset, size, at; }; // at: offset inside the packed buffer
static std::vector<std::pair<uint32_t, uint32_t>> Runs(const std::vector<uint8_t>& mask)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (size_t i = 0; i < mask.size();) {
        if (!mask[i]) { ++i; continue; }
        size_t j = i; while (j < mask.size() && mask[j]) ++j;
        out.push_back({uint32_t(i), uint32_t(j - i)}); i = j;
    }
    return out;
}
struct Plan {
    std::vector<Range> gather;  // capture spans (exact needed bytes, or merged across small gaps)
    std::vector<Range> scatter; // restore writes (write set only), packed offsets into gather layout
    size_t bytes = 0;
};
static Plan MakePlan(const std::vector<uint8_t>& write, const std::vector<uint8_t>& read, size_t gap)
{
    std::vector<uint8_t> need(write.size());
    for (size_t i = 0; i < need.size(); ++i) need[i] = write[i] | read[i];
    Plan p;
    for (auto [o, s] : Runs(need)) {
        if (!p.gather.empty() && o - (p.gather.back().offset + p.gather.back().size) <= gap)
            p.gather.back().size = o + s - p.gather.back().offset;
        else p.gather.push_back({o, s, 0});
    }
    for (auto& g : p.gather) { g.at = uint32_t(p.bytes); p.bytes += g.size; }
    size_t k = 0;
    for (auto [o, s] : Runs(write)) {
        while (o >= p.gather[k].offset + p.gather[k].size) ++k;
        p.scatter.push_back({o, s, p.gather[k].at + (o - p.gather[k].offset)});
    }
    return p;
}
static uint32_t PackedAt(const Plan& p, size_t field)
{
    for (const auto& g : p.gather) if (field >= g.offset && field < g.offset + g.size) return uint32_t(g.at + field - g.offset);
    std::abort();
}
template<class F> static std::vector<uint8_t> WriteMask(size_t extent, F restore)
{
    std::mt19937_64 rng(1234);
    std::vector<uint8_t> live(extent), a(extent), b(extent), mask(extent);
    for (auto& v : live) v = uint8_t(rng());
    for (size_t i = 0; i < extent; ++i) { a[i] = uint8_t(rng()); b[i] = uint8_t(~a[i]); }
    auto outA = live, outB = live;
    restore(outA.data(), a.data(), extent);
    restore(outB.data(), b.data(), extent);
    for (size_t i = 0; i < extent; ++i) mask[i] = outA[i] != outB[i];
    return mask;
}
static void Mark(std::vector<uint8_t>& m, size_t at, size_t n) { for (size_t i = at; i < at + n; ++i) m[i] = 1; }

static void CopyStorageWords(uint8_t* dst, const uint8_t* src)
{
    for (size_t f : {0x530, 0x538, 0x540, 0x548, 0x550, 0x554, 0x558, 0x11B0, 0x1238}) std::memcpy(dst + f, src + f, 8);
    for (size_t m = 0; m < 8; ++m) for (size_t f : {0x6C0, 0x6D8}) std::memcpy(dst + f + m * 0x180, src + f + m * 0x180, 8);
}

struct Plans { Plan shotExact, shotMerged, modelExact, modelMerged, effect; };
static Plans DerivePlans(size_t gap)
{
    Plans out;
    {   // Shot common prefix only; tail is whole.
        auto w = WriteMask(ShotCommon, [](uint8_t* l, const uint8_t* s, size_t) {
            for (size_t f : {size_t(0x530), size_t(0x550), size_t(0x554), size_t(0x558), size_t(0x1238)}) Write(l, f, Read<uint32_t>(s, f));
            Write(l, 0x540, Read<uintptr_t>(s, 0x540));
            RestoreShotCpuFields(l, s);
            Write(l, 0x123D, Read<uint8_t>(s, 0x123D));
        });
        std::vector<uint8_t> r(ShotCommon);
        Mark(r, 0x10, 4); Mark(r, 0x30, 8); Mark(r, 0x1F50, 8); // registration reads (shot_world)
        for (size_t f : {0x40, 0x1308, 0x13D8, 0x1480, 0x1E30, 0x1E38, 0x1E40}) Mark(r, f, 8); // unit refs
        Mark(r, 0x1F70, 0x70); Mark(r, 0x2000, 0x38);
        for (size_t rec = 0x14C0; rec < 0x1DC0; rec += 0x18) Mark(r, rec + 8, 8);
        out.shotExact = MakePlan(w, r, 0); out.shotMerged = MakePlan(w, r, gap);
    }
    {   // Model, same-storage path: storage words in saved forced equal to live.
        auto w = WriteMask(ModelExtent, [](uint8_t* l, const uint8_t* s, size_t e) {
            std::vector<uint8_t> t(s, s + e); CopyStorageWords(t.data(), l); CurrentModelRestore(l, t.data(), e);
        });
        std::vector<uint8_t> r(ModelExtent);
        for (size_t f : {0x530, 0x550, 0x554, 0x558, 0x1238}) Mark(r, f, 4);
        for (size_t f : {0x538, 0x540, 0x548, 0x11B0, 0x40, 0x1308, 0x13D8}) Mark(r, f, 8);
        for (size_t m = 0; m < 8; ++m) { Mark(r, 0x6C0 + m * 0x180, 8); Mark(r, 0x6D8 + m * 0x180, 8); }
        out.modelExact = MakePlan(w, r, 0); out.modelMerged = MakePlan(w, r, gap);
    }
    out.effect = MakePlan(WriteMask(EffectExtent, CurrentEffectRestore), std::vector<uint8_t>(EffectExtent), 0);
    return out;
}

static void Emit(const char* name, const Plan& p, size_t extra)
{
    std::printf("inline constexpr size_t %s_bytes = %zu;\n", name, p.bytes + extra);
    std::printf("inline constexpr PRange %s_gather[] = {", name);
    for (const auto& g : p.gather) std::printf("{%#x,%#x,%#x},", g.offset, g.size, g.at);
    std::printf("};\ninline constexpr PRange %s_scatter[] = {", name);
    for (const auto& s : p.scatter) std::printf("{%#x,%#x,%#x},", s.offset, s.size, s.at);
    std::printf("};\n");
}

// ---------------- Fixed packed kernels (generated tables) ----------------
struct PRange { uint32_t offset, size, at; };
template<const auto& R, bool ToPacked> __attribute__((always_inline)) inline void Copy(uint8_t* packed, uint8_t* object)
{
    [&]<size_t... I>(std::index_sequence<I...>) {
        if constexpr (ToPacked) ((std::memcpy(packed + R[I].at, object + R[I].offset, R[I].size)), ...);
        else ((std::memcpy(object + R[I].offset, packed + R[I].at, R[I].size)), ...);
    }(std::make_index_sequence<std::size(R)>{});
}
#ifdef PLAN_INC
#include PLAN_INC
template<const auto& G, size_t Bytes> __attribute__((noinline)) void FixedShotCapture(uint8_t* packed, const uint8_t* src, size_t extent)
{
    Copy<G, true>(packed, const_cast<uint8_t*>(src));
    std::memcpy(packed + Bytes, src + ShotCommon, extent - ShotCommon);
}
template<const auto& S, size_t Bytes, const auto& G> __attribute__((noinline)) void FixedShotRestore(uint8_t* live, const uint8_t* packed, size_t extent)
{
    constexpr auto at = [](size_t field) { for (const auto& g : G) if (field >= g.offset && field < g.offset + g.size) return size_t(g.at + (field - g.offset)); return size_t(0); };
    const uint32_t a = Read<uint32_t>(live, 0x149C) & 0x40u, b = Read<uint32_t>(live, 0x1FE0) & 0x10000000u;
    Copy<S, false>(const_cast<uint8_t*>(packed), live);
    Write(live, 0x149C, (Read<uint32_t>(packed, at(0x149C)) & ~0x40u) | a);
    Write(live, 0x1FE0, (Read<uint32_t>(packed, at(0x1FE0)) & ~0x10000000u) | b);
    std::memcpy(live + ShotCommon, packed + Bytes, extent - ShotCommon);
}
template<const auto& G> __attribute__((noinline)) void FixedCapture(uint8_t* packed, const uint8_t* src, size_t)
{
    Copy<G, true>(packed, const_cast<uint8_t*>(src));
}
template<const auto& S, const auto& G> __attribute__((noinline)) void FixedModelRestore(uint8_t* live, const uint8_t* packed, size_t)
{
    constexpr auto at = [](size_t field) { for (const auto& g : G) if (field >= g.offset && field < g.offset + g.size) return size_t(g.at + (field - g.offset)); return size_t(0); };
    bool same = true;
    [&]<size_t... I>(std::index_sequence<I...>) {
        constexpr size_t narrow[] = {0x530, 0x550, 0x554, 0x558, 0x1238};
        ((same &= Read<uint32_t>(live, narrow[I]) == Read<uint32_t>(packed, at(narrow[I]))), ...);
    }(std::make_index_sequence<5>{});
    [&]<size_t... I>(std::index_sequence<I...>) {
        constexpr size_t wide[] = {0x538, 0x540, 0x548, 0x11B0,
            0x6C0, 0x6D8, 0x840, 0x858, 0x9C0, 0x9D8, 0xB40, 0xB58, 0xCC0, 0xCD8, 0xE40, 0xE58, 0xFC0, 0xFD8, 0x1140, 0x1158};
        ((same &= Read<uint64_t>(live, wide[I]) == Read<uint64_t>(packed, at(wide[I]))), ...);
    }(std::make_index_sequence<20>{});
    if (same) Copy<S, false>(const_cast<uint8_t*>(packed), live);
    else std::memcpy(live + ModelLifeAt, packed + at(ModelLifeAt), ModelLifeEnd - ModelLifeAt);
}
template<const auto& S> __attribute__((noinline)) void FixedRestore(uint8_t* live, const uint8_t* packed, size_t)
{
    Copy<S, false>(const_cast<uint8_t*>(packed), live);
}
#endif

// ---------------- Harness ----------------
using Kernel = void (*)(uint8_t*, const uint8_t*, size_t);
struct Variant { const char* name; Kernel capture, restore; size_t bytes; }; // bytes: per object stored, at extent
using Clock = std::chrono::steady_clock;
constexpr size_t Slots = 16, Trials = 9;

struct Result { double capture, restore, both; };
static size_t g_pad = 0; // shifts every snapshot buffer, to expose alignment/aliasing effects
static Result Run(const Variant& v, size_t extent, size_t count, size_t iterations, bool ring)
{
    std::mt19937_64 rng(77);
    std::vector<std::vector<uint8_t>> live(count, std::vector<uint8_t>(extent));
    for (auto& o : live) for (auto& b : o) b = uint8_t(rng());
    const size_t slots = ring ? Slots : 1;
    std::vector<std::vector<uint8_t>> store(slots * count, std::vector<uint8_t>(v.bytes + 64));
    for (size_t s = 0; s < store.size(); ++s) v.capture(store[s].data() + g_pad, live[s % count].data(), extent);
    auto time = [&](auto body) {
        std::vector<double> t;
        for (size_t trial = 0; trial < Trials; ++trial) {
            const auto t0 = Clock::now();
            for (size_t i = 0; i < iterations; ++i) {
                const size_t slot = (i % slots) * count;
                for (size_t o = 0; o < count; ++o) body(store[slot + o].data() + g_pad, live[o].data());
                Clobber();
            }
            t.push_back(std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / double(iterations));
        }
        std::sort(t.begin(), t.end()); return t[t.size() / 2];
    };
    Result r;
    r.capture = time([&](uint8_t* s, uint8_t* l) { v.capture(s, l, extent); });
    r.restore = time([&](uint8_t* s, uint8_t* l) { v.restore(l, s, extent); });
    r.both = time([&](uint8_t* s, uint8_t* l) { v.capture(s, l, extent); Clobber(); v.restore(l, s, extent); });
    return r;
}

// Each variant restore must match the current kernel from the same live/saved pair.
static bool Verify(const char* name, const Variant& v, Kernel current, size_t extent, bool storageMatch)
{
    std::mt19937_64 rng(99);
    for (int round = 0; round < 64; ++round) {
        std::vector<uint8_t> live(extent), saved(extent), packed(v.bytes + 64);
        for (auto& b : live) b = uint8_t(rng());
        for (auto& b : saved) b = uint8_t(rng());
        if (storageMatch && (round & 1)) CopyStorageWords(saved.data(), live.data());
        auto a = live, b = live;
        current(a.data(), saved.data(), extent);
        v.capture(packed.data(), saved.data(), extent);
        std::fill(saved.begin(), saved.end(), 0xCD);
        v.restore(b.data(), packed.data(), extent);
        if (a != b) { std::printf("VERIFY FAIL %s / %s round %d\n", name, v.name, round); return false; }
    }
    return true;
}

int main(int argc, char** argv)
{
    const size_t gap = 32;
    const Plans plans = DerivePlans(gap);
    if (argc > 1 && !std::strcmp(argv[1], "--emit")) {
        Emit("shotExact", plans.shotExact, 0); Emit("shotMerged", plans.shotMerged, 0);
        Emit("modelExact", plans.modelExact, 0); Emit("modelMerged", plans.modelMerged, 0);
        Emit("effect", plans.effect, 0);
        return 0;
    }
#ifndef PLAN_INC
    std::fprintf(stderr, "build with -DPLAN_INC (see header comment)\n");
    return 2;
#else
    const size_t iterations = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 20000;
    g_pad = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 0; // 0..63
    const char* filter = argc > 3 ? argv[3] : "";
    auto line = [](const char* n, const Plan& p) {
        size_t w = 0; for (auto& s : p.scatter) w += s.size;
        std::printf("  %-12s stored %5zu B in %3zu gather spans; restore writes %5zu B in %3zu ranges\n",
            n, p.bytes, p.gather.size(), w, p.scatter.size());
    };
    std::printf("Derived layouts (shot: common prefix +0..+2140 only; tail stored whole)\n");
    line("shotExact", plans.shotExact); line("shotMerged", plans.shotMerged);
    line("modelExact", plans.modelExact); line("modelMerged", plans.modelMerged); line("effect", plans.effect);

    struct Case { const char* name; size_t extent, count; Kernel current; std::vector<Variant> variants; bool storage; };
    std::vector<Case> cases;
    for (size_t extent : {size_t(0x2140), size_t(0x2230), size_t(0x2300)}) {
        static char names[3][32]; static int n = 0;
        std::snprintf(names[n], 32, "shot %#zx x8", extent);
        const size_t tail = extent - ShotCommon;
        cases.push_back({names[n++], extent, 8, CurrentShotRestore, {
            {"current(full+subset)", FullCapture, CurrentShotRestore, extent},
            {"full + unrolled", FullCapture, UnrolledShotRestore, extent},
            {"packed exact", FixedShotCapture<shotExact_gather, shotExact_bytes>, FixedShotRestore<shotExact_scatter, shotExact_bytes, shotExact_gather>, shotExact_bytes + tail},
            {"packed merged32", FixedShotCapture<shotMerged_gather, shotMerged_bytes>, FixedShotRestore<shotMerged_scatter, shotMerged_bytes, shotMerged_gather>, shotMerged_bytes + tail},
            {"full memcpy both", FullCapture, FullRestore, extent},
        }, false});
    }
    cases.push_back({"model 0x1d00 x4", ModelExtent, 4, CurrentModelRestore, {
        {"current(full+subset)", FullCapture, CurrentModelRestore, ModelExtent},
        {"full + unrolled", FullCapture, UnrolledModelRestore, ModelExtent},
        {"packed exact", FixedCapture<modelExact_gather>, FixedModelRestore<modelExact_scatter, modelExact_gather>, modelExact_bytes},
        {"packed merged32", FixedCapture<modelMerged_gather>, FixedModelRestore<modelMerged_scatter, modelMerged_gather>, modelMerged_bytes},
        {"full memcpy both", FullCapture, FullRestore, ModelExtent},
    }, true});
    cases.push_back({"effect root x64", EffectExtent, 64, CurrentEffectRestore, {
        {"current as built", EffectCaptureAsBuilt, CurrentEffectRestore, EffectExtent},
        {"current libcall", FullCapture, EffectRestoreLibcall, EffectExtent},
        {"sse2 chunks", EffectCaptureSse, EffectRestoreSse, EffectExtent},
        {"sse2 2x16 chunks", EffectCaptureSse32, EffectRestoreSse32, EffectExtent},
        {"packed exact", FixedCapture<effect_gather>, FixedRestore<effect_scatter>, effect_bytes},
        {"packed libcall", EffectPackedCaptureLibcall, EffectPackedRestoreLibcall, effect_bytes},
        {"full memcpy both", FullCapture, FullRestore, EffectExtent},
    }, false});

    g_packedEffectBytes = effect_bytes;
    bool ok = true;
    for (const auto& c : cases) for (const auto& v : c.variants)
        if (std::strcmp(v.name, "full memcpy both")) ok &= Verify(c.name, v, c.current, c.extent, c.storage);
    std::printf("correctness (packed restore == current restore, saved image poisoned): %s\n", ok ? "PASS" : "FAIL");

    for (bool ring : {false, true}) {
        std::printf("\n%s, %zu iterations, median of %zu; ns per frame for all objects in the case\n",
            ring ? "ring16 (16 snapshot slots)" : "hot (1 slot)", iterations, Trials);
        std::printf("%-17s %-21s %8s %9s %9s %9s\n", "case", "variant", "bytes/obj", "capture", "restore", "cap+rest");
        for (const auto& c : cases) for (const auto& v : c.variants) {
            if (!std::strstr(c.name, filter)) continue;
            const Result r = Run(v, c.extent, c.count, iterations, ring);
            std::printf("%-17s %-21s %8zu %9.1f %9.1f %9.1f\n", c.name, v.name, v.bytes, r.capture, r.restore, r.both);
        }
    }
    return ok ? 0 : 1;
#endif
}
