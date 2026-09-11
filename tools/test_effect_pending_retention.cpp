#include "../UMVC3Hook/code/effect_pending_retention.h"

#include <cassert>
#include <cstdio>
#include <memory>

struct Unit {
    bool transition;
    bool alive = true;
    unsigned retirements = 0;
};

static bool KeepTransition(void* entry)
{
    const auto& unit = *static_cast<Unit*>(entry);
    return unit.transition && unit.alive;
}

static void Retire(void* entry)
{
    auto& unit = *static_cast<Unit*>(entry);
    if (unit.alive) {
        unit.alive = false;
        ++unit.retirements;
    }
}

int main()
{
    // Mixed native kill queue: retain both tear entries, discard null/dead
    // entries, and retire ordinary effects without changing the allocation.
    Unit normal{false}, tear{true}, companion{true}, dead{false, false};
    void* storage[] = {&normal, &tear, nullptr, &dead, &companion};
    void** entries = storage;
    uint32_t count = 5;
    EffectPending::FlushRetaining(entries, count, KeepTransition, Retire);
    assert(entries == storage && count == 2);
    assert(entries[0] == &tear && entries[1] == &companion);
    assert(!entries[2] && !entries[3] && !entries[4]);
    assert(normal.retirements == 1 && dead.retirements == 0);
    for (unsigned restore = 0; restore < 16; ++restore)
        EffectPending::FlushRetaining(entries, count, KeepTransition, Retire);
    assert(count == 2 && tear.retirements == 0 && companion.retirements == 0);

    // Ordinary native cleanup must still have both retained pointers available.
    EffectPending::FlushRetaining(entries, count, [](void*) { return false; }, Retire);
    assert(count == 0 && tear.retirements == 1 && companion.retirements == 1);
    assert(entries[0] == nullptr && entries[1] == nullptr);

    // Retirement may append another tracked effect and move the native array.
    // The flush must use the updated header, not the freed original buffer.
    Unit spawner{false}, existingTear{true}, appendedTear{true};
    auto allocation = std::make_unique<void*[]>(2);
    allocation[0] = &spawner;
    allocation[1] = &existingTear;
    entries = allocation.get();
    count = 2;
    EffectPending::FlushRetaining(entries, count, KeepTransition, [&](void* entry) {
        assert(entry == &spawner);
        Retire(entry);
        auto replacement = std::make_unique<void*[]>(3);
        for (uint32_t index = 0; index < count; ++index)
            replacement[index] = entries[index];
        replacement[count++] = &appendedTear;
        allocation = std::move(replacement);
        entries = allocation.get();
    });
    assert(count == 2 && entries[0] == &existingTear && entries[1] == &appendedTear);
    assert(entries[2] == nullptr && spawner.retirements == 1);
    assert(existingTear.alive && appendedTear.alive);

    entries = nullptr;
    count = 0;
    EffectPending::FlushRetaining(entries, count, KeepTransition, Retire);
    assert(entries == nullptr && count == 0);
    std::puts("tracked effect retention: regression tests passed");
}
