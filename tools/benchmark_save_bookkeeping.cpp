// Isolated save experiments. Native work is deliberately not assigned fake timings.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "concrete_snapshot.h"
#include "rollback_capture_set.h"
#define NOINLINE __attribute__((noinline))
#define ROLLBACK_SNAPDUMP_ENABLED 0
#include "save.generated.h"

static void require(bool b) { if (!b) { std::fputs("verification failed\n", stderr); std::abort(); } }
template<class T> static void put(void* p, size_t offset, T v) {
    std::memcpy((uint8_t*)p + offset, &v, sizeof(v));
}
static uint64_t sink = 0;
static void escape(const void* p) { asm volatile("" : : "g"(p) : "memory"); }

struct Fixture {
    std::array<uint64_t, 0xD00 / 8> mgr{}, unit{};
    std::array<std::array<uint64_t, 4>, 32> nodes{};
    std::vector<std::vector<uint64_t>> characters;
    std::vector<std::vector<uint64_t>> weapons;
    std::vector<std::vector<uint64_t>> weaponArrays;
    std::vector<void*> queries;
    Fixture(size_t roster, size_t helpers, size_t weaponCount, int pattern, bool cycle = false)
        : characters(roster + helpers, std::vector<uint64_t>(0x7100 / 8)),
          weapons((roster + helpers) * weaponCount, std::vector<uint64_t>(0x200 / 8)),
          weaponArrays(roster + helpers, std::vector<uint64_t>(weaponCount)) {
        manager = (uint64_t)mgr.data(); units = (uint64_t)unit.data();
        for (size_t i = 0; i < characters.size(); ++i) {
            auto p = characters[i].data();
            p[0] = 0x1234 + i;
            put(p, 0x180, float(i + 1));
            for (size_t j = 0; j < weaponCount; ++j) {
                auto w = weapons[i * weaponCount + j].data();
                w[0] = 0x9876 + j;
                weaponArrays[i][j] = (uint64_t)w;
            }
            put(p, 0x3F38, uint32_t(weaponCount));
            put(p, 0x3F40, (uint64_t)weaponArrays[i].data());
            auto parent = [&](size_t n) -> uint64_t {
                if (pattern == 0) return 0;
                if (pattern == 1) return weaponCount ? weaponArrays[i].back() : (uint64_t)p;
                switch (n % 4) {
                case 0: return 0;
                case 1: return (uint64_t)p;
                case 2: return weaponCount ? weaponArrays[i][n % weaponCount] : (uint64_t)p;
                default: return 0xDEADBEEF; // Unknown parents must remain omitted.
                }
            };
            size_t n = 0;
            for (size_t k = 0; k < 16; ++k) {
                put(p, 0x4210 + k * 0xB0 + 0x58, parent(n++));
                put(p, 0x4210 + k * 0xB0 + 0x60, parent(n++));
            }
            for (size_t base : {0x4E20, 0x5530, 0x5C40})
                for (size_t k = 0; k < 16; ++k) put(p, base + k * 0x70 + 0x38, parent(n++));
            put(p, 0x18, i + 1 < characters.size() ? (uint64_t)characters[i + 1].data() :
                (cycle && !characters.empty() ? (uint64_t)characters[0].data() : uint64_t(0)));
            queries.push_back(p);
        }
        for (size_t team = 0; team < 2; ++team) {
            const size_t first = team * (roster / 2), count = roster / 2;
            uint64_t node = manager + (team ? TEAM1_OFFSET : TEAM0_OFFSET) + TEAM_FIRST_NODE_OFFSET;
            for (size_t k = 0; k < count; ++k) {
                put((void*)node, NODE_CHR_OFFSET, (uint64_t)characters[first + k].data());
                const auto next = k + 1 < count ? (uint64_t)nodes[first + k].data() : uint64_t(0);
                put((void*)node, NODE_NEXT_OFFSET, next);
                node = next;
            }
        }
        put(unit.data(), SUNIT_CATEGORY_COUNT_OFFSET, uint32_t(9));
        put(unit.data(), SUNIT_CATEGORY_LISTS_OFFSET + SUNIT_CATEGORY_LIST_STRIDE * 8,
            characters.empty() ? uint64_t(0) : (uint64_t)characters[0].data());
        // Only large regions reach the production membership check. Small
        // weapon/joint regions short-circuit before querying the character set.
        for (int i = 0; i < 8; ++i) queries.push_back((void*)(uintptr_t)(0x8000 + i * 16));
    }
};

static void equal(const RollbackSnapshotSlot& a, const RollbackSnapshotSlot& b) {
    require(a.characterCollisionParentReferences.size() == b.characterCollisionParentReferences.size());
    for (size_t i = 0; i < a.characterCollisionParentReferences.size(); ++i) {
        auto& x = a.characterCollisionParentReferences[i]; auto& y = b.characterCollisionParentReferences[i];
        require(x.character == y.character && x.expectedCharacterVtable == y.expectedCharacterVtable &&
                x.offset == y.offset && x.weaponIndex == y.weaponIndex && x.kind == y.kind);
    }
    require(a.modelRenderStates.size() == b.modelRenderStates.size());
    for (size_t i = 0; i < a.modelRenderStates.size(); ++i) {
        auto& x = a.modelRenderStates[i]; auto& y = b.modelRenderStates[i];
        require(x.model == y.model && x.expectedVtable == y.expectedVtable &&
                x.objectFlags == y.objectFlags && x.drawLayerMask == y.drawLayerMask &&
                x.transparency == y.transparency && x.character == y.character);
    }
}
NOINLINE static size_t bookkeeping(RollbackSnapshotSlot& slot, const Fixture& f, int mode) {
    if (mode == 0) {
        baseline_collision(slot); baseline_render(slot);
        return baseline_membership(f.queries);
    }
    if (mode == 3) {
        hashless_collision(slot); baseline_render(slot);
        return hashless_membership(f.queries);
    }
    auto inventory = gather();
    if (mode == 1) candidate_collision(slot, inventory);
    else cached_collision(slot, inventory);
    candidate_render(slot, inventory);
    size_t n = 0; for (auto p : f.queries) n += inventory.all.count(p);
    return n;
}

static void equal(const ConcreteShotSnapshot::CallbackEffectState& a,
                  const ConcreteShotSnapshot::CallbackEffectState& b) {
    require(a.occupied == b.occupied && a.savedEffect == b.savedEffect && a.unitState == b.unitState &&
            a.emissionFinished == b.emissionFinished && a.hasState == b.hasState &&
            a.hasGeneratorEmissionState == b.hasGeneratorEmissionState && a.dti == b.dti && a.resource == b.resource);
    require(!std::memcmp(a.flags100, b.flags100, sizeof(a.flags100)) &&
            !std::memcmp(a.timing110, b.timing110, sizeof(a.timing110)) &&
            !std::memcmp(a.state1D0, b.state1D0, sizeof(a.state1D0)) &&
            !std::memcmp(a.state1E0, b.state1E0, sizeof(a.state1E0)) &&
            !std::memcmp(a.state1F8, b.state1F8, sizeof(a.state1F8)) &&
            !std::memcmp(a.state230, b.state230, sizeof(a.state230)));
    require(a.generatorEmissionStates.size() == b.generatorEmissionStates.size());
    for (size_t i = 0; i < a.generatorEmissionStates.size(); ++i)
        require(!std::memcmp(&a.generatorEmissionStates[i], &b.generatorEmissionStates[i],
                            sizeof(a.generatorEmissionStates[i])));
}
static void equal(const ConcreteShotSnapshot& a, const ConcreteShotSnapshot& b) {
    require(a.savedObject == b.savedObject && a.dti == b.dti && a.vtable == b.vtable && a.body == b.body);
    require(a.collisionPrimitives.size() == b.collisionPrimitives.size());
    // Fixture primitives are value initialized; compare meaningful fields, not padding.
    for (size_t i = 0; i < a.collisionPrimitives.size(); ++i) {
        require(a.collisionPrimitives[i].index == b.collisionPrimitives[i].index);
        require(a.collisionPrimitives[i].parent0ShotOffset == b.collisionPrimitives[i].parent0ShotOffset);
    }
    for (size_t i = 0; i < 13; ++i) equal(a.callbackEffects[i], b.callbackEffects[i]);
}

struct RecipeFixture {
    std::vector<uint64_t> shot = std::vector<uint64_t>(0x3000 / 8);
    std::vector<uint64_t> effect = std::vector<uint64_t>(0x300 / 8);
    RecipeFixture() {
        for (size_t i = 0; i < effect.size(); ++i) effect[i] = i * 119;
        effect[0] = kEffectDti; shot[0] = 0x3000;
        for (size_t i = 0; i < 13; ++i) put(shot.data(), kShotCallbackEffectsOffset + i * 8, (void*)effect.data());
    }
};

template<class F> static void measure(const std::string& name, int modes, size_t iterations, F fn) {
    std::vector<std::vector<double>> samples(modes);
    for (int m = 0; m < modes; ++m) for (size_t i = 0; i < 64; ++i) fn(m, i);
    for (int trial = 0; trial < 9; ++trial) {
        for (int j = 0; j < modes; ++j) {
            const int m = trial % 2 ? modes - 1 - j : j;
            auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < iterations; ++i) fn(m, i);
            double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / iterations;
            samples[m].push_back(us);
            std::printf("%s,%d,%d,%.6f\n", name.c_str(), m, trial, us);
        }
    }
    for (int m = 0; m < modes; ++m) {
        std::sort(samples[m].begin(), samples[m].end());
        std::fprintf(stderr, "%s mode=%d median_us=%.6f range_us=%.6f..%.6f\n",
                     name.c_str(), m, samples[m][4], samples[m].front(), samples[m].back());
    }
}

int main(int argc, char** argv) {
    const size_t iterations = argc > 1 ? std::stoul(argv[1]) : 20000;
    RollbackCaptureSet<uint64_t, 96> bounded;
    for (size_t i = 0; i < 96; ++i) require(bounded.insert(i).second);
    for (size_t i = 0; i < 96; ++i) require(!bounded.insert(i).second && bounded.count(i));
    bool overflowRejected = false;
    try { bounded.insert(96); } catch (const std::out_of_range&) { overflowRejected = true; }
    require(overflowRejected && !bounded.count(96));
    // Boundaries, duplicate units, cycles, null managers, unknown parents and invalid weapon counts.
    for (size_t roster : {0, 6, 32}) for (int pattern = 0; pattern < 3; ++pattern) {
        Fixture f(roster, 64, 4, pattern, true);
        for (int edge = 0; edge < 4; ++edge) {
            manager = edge == 1 ? 0 : (uint64_t)f.mgr.data();
            units = edge == 2 ? 0 : (uint64_t)f.unit.data();
            if (edge == 3) for (auto& c : f.characters) put(c.data(), 0x3F38, uint32_t(257));
            RollbackSnapshotSlot baseline;
            size_t expected = bookkeeping(baseline, f, 0);
            for (int m = 1; m < 4; ++m) {
                RollbackSnapshotSlot candidate;
                require(bookkeeping(candidate, f, m) == expected);
                equal(baseline, candidate);
            }
        }
    }
    RecipeFixture recipes;
    ConcreteShotSnapshot a, b;
    ConcreteShotSnapshot::CallbackEffectState x, y;
    for (size_t i = 0; i < 96; ++i) {
        put(recipes.effect.data(), 0x110, uint32_t(i));
        recipes.shot[0] = i % 3 ? 0x3000 : 1;
        recipes.effect[0] = i % 4 ? kEffectDti : 1;
        put(recipes.shot.data(), kShotCallbackEffectsOffset, i % 2 ? (void*)recipes.effect.data() : nullptr);
        void* shot = i % 5 ? recipes.shot.data() : nullptr;
        void* effect = i % 5 ? recipes.effect.data() : nullptr;
        require(baseline_shot(shot, a) == candidate_shot(shot, b)); equal(a, b);
        require(baseline_effect(effect, x) == candidate_effect(effect, y)); equal(x, y);
    }
    // Ring reuse with growth, shrinkage, empty saves and invalid inputs must not
    // expose old state or discard another ring slot's captured payload.
    std::array<std::vector<ConcreteShotSnapshot>, 16> before, after;
    for (size_t frame = 0; frame < 160; ++frame) {
        size_t count = (frame / 16) % 5;
        auto& old = before[frame % 16]; auto& next = after[frame % 16];
        recipes.shot[0] = frame % 7 ? 0x3000 : 1;
        put(recipes.shot.data(), 0x110, uint32_t(frame));
        old.clear(); next.resize(count);
        for (size_t j = 0; j < count; ++j) {
            ConcreteShotSnapshot saved;
            baseline_shot(recipes.shot.data(), saved); old.push_back(std::move(saved));
            candidate_shot(recipes.shot.data(), next[j]);
        }
        put(recipes.shot.data(), 0x110, uint32_t(0xDEAD));
        for (size_t slot = 0; slot < 16; ++slot) {
            require(before[slot].size() == after[slot].size());
            for (size_t j = 0; j < before[slot].size(); ++j) equal(before[slot][j], after[slot][j]);
        }
    }
    // Exercise the actual production entry-reuse/metadata assignment blocks,
    // including nonempty source snapshots and removal before reference lookup.
    RollbackSnapshotSlot reused;
    for (size_t frame = 0; frame < 96; ++frame) {
        const size_t count = (frame / 4) % 5;
        size_t shotCount = 0, effectCount = 0;
        recipes.shot[0] = frame % 3 ? 0x3000 : 1;
        recipes.effect[0] = frame % 4 ? kEffectDti : 1;
        for (size_t j = 0; j < count; ++j) {
            RollbackShotRecipe source{};
            source.spawnSequence = frame * 10 + count - j;
            source.ownerChr = (void*)(uintptr_t)(frame + 1);
            source.objectRef1.shotSpawnSequence = frame;
            source.hasPreSpawnRng = frame % 2;
            source.reconstructed = frame % 2;
            source.flight.unitState = frame;
            source.concrete.body.assign(17, 0xA5);
            auto& saved = reuse_shot(reused, source, shotCount);
            candidate_shot(recipes.shot.data(), saved.concrete);
            ConcreteShotSnapshot expected;
            baseline_shot(recipes.shot.data(), expected);
            equal(expected, saved.concrete);
            require(saved.spawnSequence == source.spawnSequence && saved.ownerChr == source.ownerChr &&
                    saved.objectRef1.shotSpawnSequence == frame && saved.flight.unitState == frame &&
                    saved.hasPreSpawnRng == source.hasPreSpawnRng && saved.reconstructed == source.reconstructed);
            require(source.concrete.body.size() == 17 && source.concrete.body[0] == 0xA5);

            RollbackEffectRecipe effect{};
            effect.spawnSequence = frame * 10 + j;
            effect.rocketShort = frame % 2;
            effect.shortParentNo = frame;
            effect.a8 = source.ownerChr;
            effect.state.generatorEmissionStates.resize(3);
            auto& savedEffect = reuse_effect(reused, effect, effectCount);
            candidate_effect(recipes.effect.data(), savedEffect.state);
            ConcreteShotSnapshot::CallbackEffectState expectedEffect;
            baseline_effect(recipes.effect.data(), expectedEffect);
            equal(expectedEffect, savedEffect.state);
            require(savedEffect.spawnSequence == effect.spawnSequence && savedEffect.rocketShort == effect.rocketShort &&
                    savedEffect.shortParentNo == frame && savedEffect.a8 == effect.a8);
            require(effect.state.generatorEmissionStates.size() == 3);
        }
        reused.shotRecipes.resize(shotCount);
        reused.effectRecipes.resize(effectCount);
        std::sort(reused.shotRecipes.begin(), reused.shotRecipes.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.spawnSequence < rhs.spawnSequence; });
        require(reused.shotRecipes.size() == count && reused.effectRecipes.size() == count);
        for (size_t j = 0; j < count; ++j) require(reused.shotRecipes[j].spawnSequence == frame * 10 + j + 1);
    }
    // No allocation replacement for a stable-sized capture, even after invalid input.
    recipes.shot[0] = 0x3000; recipes.effect[0] = kEffectDti;
    candidate_shot(recipes.shot.data(), b);
    auto* bodyStorage = b.body.data();
    auto* primitiveStorage = b.collisionPrimitives.data();
    candidate_shot(nullptr, b);
    candidate_shot(recipes.shot.data(), b);
    require(b.body.data() == bodyStorage && b.collisionPrimitives.data() == primitiveStorage);
    candidate_effect(recipes.effect.data(), y);
    auto* generatorStorage = y.generatorEmissionStates.data();
    candidate_effect(nullptr, y); candidate_effect(recipes.effect.data(), y);
    require(y.generatorEmissionStates.data() == generatorStorage);
    std::fputs("PASS: extracted-kernel equivalence, bounds, cycles, null/invalid transitions, repeated capture\n", stderr);
    std::puts("case,mode,trial,us_per_save");
    for (int workload = 0; workload < 3; ++workload) {
        Fixture f(6, workload == 2 ? 12 : 0, workload == 2 ? 16 : 2, workload);
        std::array<std::array<RollbackSnapshotSlot, 16>, 4> slots;
        measure(std::string("inventory_ring16_") + std::to_string(workload), 4, iterations,
            [&](int m, size_t i) {
                auto& slot = slots[m][i % 16];
                sink += bookkeeping(slot, f, m); escape(&slot);
            });
    }
    for (size_t count : {0, 1, 12}) {
        // Per-slot reusable entries, keeping only active entries. Shrinking to zero
        // deliberately releases entries, matching a resize-based production candidate.
        std::array<std::array<RollbackSnapshotSlot, 16>, 2> shots;
        std::array<std::array<RollbackSnapshotSlot, 16>, 2> effects;
        RollbackShotRecipe shotSource{};
        RollbackEffectRecipe effectSource{};
        RecipeFixture f;
        for (size_t callbacks : {0, 13}) {
        for (size_t j = 0; j < 13; ++j)
            put(f.shot.data(), kShotCallbackEffectsOffset + j * 8,
                j < callbacks ? (void*)f.effect.data() : nullptr);
        measure("shot_recipe_storage_ring16_" + std::to_string(count) + "_callbacks" + std::to_string(callbacks), 2, iterations,
            [&](int m, size_t i) {
                auto& slot = shots[m][i % 16];
                auto& v = slot.shotRecipes;
                put(f.shot.data(), 0x110, uint32_t(i));
                if (!m) { v.clear(); for (size_t j = 0; j < count; ++j) {
                    auto s = shotSource; baseline_shot(f.shot.data(), s.concrete); v.push_back(std::move(s));
                }} else {
                    size_t captured = 0;
                    while (captured < count) {
                        auto& s = reuse_shot(slot, shotSource, captured);
                        candidate_shot(f.shot.data(), s.concrete);
                    }
                    v.resize(captured);
                }
                escape(&v);
            });
        }
        measure("effect_recipe_storage_ring16_" + std::to_string(count), 2, iterations,
            [&](int m, size_t i) {
                auto& slot = effects[m][i % 16];
                auto& v = slot.effectRecipes;
                put(f.effect.data(), 0x110, uint32_t(i));
                if (!m) { v.clear(); for (size_t j = 0; j < count; ++j) {
                    auto s = effectSource; baseline_effect(f.effect.data(), s.state); v.push_back(std::move(s));
                }} else {
                    size_t captured = 0;
                    while (captured < count) {
                        auto& s = reuse_effect(slot, effectSource, captured);
                        candidate_effect(f.effect.data(), s.state);
                    }
                    v.resize(captured);
                }
                escape(&v);
            });
    }
    // Retain dependency validation in both variants. Only cache total bytes;
    // cached offsets are unnecessary for the existing sequential copy loop.
    for (size_t count : {64, 512}) {
        std::vector<uint64_t> values(count, 123);
        RegionDependencies dependencies;
        std::vector<RollbackRegion> regions(count);
        size_t cachedTotal = 0;
        for (size_t i = 0; i < count; ++i) {
            dependencies.read<uint64_t>((uintptr_t)&values[i]);
            regions[i].size = 64 + i * 8;
            cachedTotal += regions[i].size;
        }
        require(RegionDependenciesValid(dependencies));
        ++values.back(); require(!RegionDependenciesValid(dependencies)); --values.back();
        measure("region_total_" + std::to_string(count), 2, iterations,
            [&](int m, size_t) {
                escape(values.data());
                require(RegionDependenciesValid(dependencies));
                size_t total = cachedTotal;
                if (!m) { total = 0; for (auto& r : regions) total += r.size; }
                require(total == cachedTotal); sink += total;
            });
    }
    std::fprintf(stderr, "checksum=%llu\n", (unsigned long long)sink);
}
