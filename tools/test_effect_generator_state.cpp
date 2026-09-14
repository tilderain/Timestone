#include "../UMVC3Hook/code/concrete_snapshot.h"

#include <array>
#include <cassert>
#include <cstdio>

using namespace EffectGenerator;

int main()
{
    std::array<uint8_t, 0x240> old{}, fresh{};
    fresh.fill(0x5A);
    Write<uint64_t>(fresh.data(), 0xE0, 0);
    const auto factory = fresh;
    Write<uint64_t>(old.data(), 0xE0, 0x12340000);
    Write<uint32_t>(old.data(), 0x20, 0x80000007);
    Write<uint32_t>(old.data(), 0x70, 0x100); // native single-spawn state machine
    Write<uint8_t>(old.data(), 0x77, 5);
    Write<uint16_t>(old.data(), 0x122, 9);
    Write<uint32_t>(old.data(), 0x78, 0x12345678);
    Write<uint32_t>(old.data(), 0xB8, 37);
    Write<uint8_t>(old.data(), 0x1F1, 2); // subframe distribution state
    Write<uint32_t>(old.data(), 0x1F4, 1);
    Write<uint32_t>(old.data(), 0x1F8, 1);
    Write<uint16_t>(old.data(), 0x206, 3); // repeat countdown, previously omitted
    State saved;
    saved.ordinary = true;
    CaptureOrdinary(old.data(), saved);

    // Standalone one-shots retain their emission phase, not a new first emission.
    RestoreOrdinary(fresh.data(), saved, false);
    assert(Read<uint8_t>(fresh.data(), 0x77) == 5);
    assert(Read<uint32_t>(fresh.data(), 0x20) & 0x80000000u);
    assert(Read<uint16_t>(fresh.data(), 0x122) == 9);
    assert(Read<uint16_t>(fresh.data(), 0x206) == 3);
    assert(Read<uint8_t>(fresh.data(), 0x1F1) == 2);

    // A live shot whose main particle is missing gets exactly the native
    // phase-3 creation opportunity, instead of staying empty in phase 5.
    for (int rollback = 0; rollback < 16; ++rollback) {
        fresh = factory;
        assert(RecreateSingle(saved, fresh.data(), true));
        RestoreOrdinary(fresh.data(), saved, true);
        assert(Read<uint8_t>(fresh.data(), 0x77) == 3);
        assert(!(Read<uint32_t>(fresh.data(), 0x20) & 0x80000000u));
        assert(Read<uint32_t>(fresh.data(), 0x20) & 4u);
        assert(Read<uint32_t>(fresh.data(), 0x1F4) == 0);
        assert(Read<uint32_t>(fresh.data(), 0x1F8) == 0);
        assert(Read<uint32_t>(fresh.data(), 0xB8) == 37);
        assert(Read<uint32_t>(fresh.data(), 0x78) == 0x12345678);
        // Every byte outside the audited scalar ranges remains factory-owned.
        for (size_t i = 0; i < fresh.size(); ++i) {
            if ((i >= 0x20 && i < 0x24) || (i >= 0x77 && i < 0x84) ||
                (i >= 0xB8 && i < 0xBC) || (i >= 0x122 && i < 0x124) ||
                (i >= 0x1F0 && i < 0x208)) continue;
            assert(fresh[i] == factory[i]);
        }
        assert((Read<uint32_t>(fresh.data(), 0x20) & ~0x80000007u) ==
               (Read<uint32_t>(factory.data(), 0x20) & ~0x80000007u));
    }

    // Never duplicate a particle the fresh graph already has.
    Write<uint64_t>(fresh.data(), 0xE0, 0x56780000);
    assert(!RecreateSingle(saved, fresh.data(), true));
    RestoreOrdinary(fresh.data(), saved, true);
    assert(Read<uint8_t>(fresh.data(), 0x77) == 5);
    assert(Read<uint64_t>(fresh.data(), 0xE0) == 0x56780000);

    fresh = factory;
    saved.hadParticles = false;
    assert(!RecreateSingle(saved, fresh.data(), true));
    saved.hadParticles = true;
    saved.modes = 0x200; // continuing trail: restore phase and actual wait/repeat state
    saved.phase = 3;
    RestoreOrdinary(fresh.data(), saved, true);
    assert(Read<uint16_t>(fresh.data(), 0x122) == 9);
    assert(Read<uint16_t>(fresh.data(), 0x206) == 3);
    assert(Read<uint32_t>(fresh.data(), 0x1F4) == 1);
    saved.phase = 5;
    assert(!RecreateSingle(saved, fresh.data(), true));
    saved.modes = 0x100;
    saved.phase = 1; // delayed first spawn must keep its delay
    assert(!RecreateSingle(saved, fresh.data(), true));

    // Node/Infinite matrices must never be overwritten as ordinary counters.
    saved.ordinary = false;
    const auto beforeUnknown = fresh;
    RestoreOrdinary(fresh.data(), saved, true);
    assert(fresh == beforeUnknown);
    // Native single-spawn phase 3 finishes emission and returns one spawn.
    // The resulting phase-5 particle may remain alive for many more updates.
    // Capture that combination: it must survive the independent-recipe gate.
    ConcreteShotSnapshot::CallbackEffectState burst;
    State tail;
    tail.ordinary = true;
    CaptureOrdinary(old.data(), tail);
    assert(tail.phase == 5 && tail.hadParticles);
    assert((tail.status & 0x80000000u) && (tail.status & 4u));
    burst.occupied = true;
    burst.hasState = true;
    burst.unitState = 2;
    burst.emissionFinished = true;
    burst.hasGeneratorEmissionState = true;
    burst.generatorEmissionStates.push_back(tail);
    for (int rollback = 0; rollback < 16; ++rollback)
        assert(!burst.CompletedIndependent());

    // Reaching native retirement is authoritative even if the captured
    // generator still has particle bytes or emission was not marked finished.
    burst.unitState = 3;
    assert(burst.CompletedIndependent());

    // Match native isAlive (states 1/2) for both awake and sleeping effects.
    // A partial/unavailable capture must never establish completion.
    for (uint8_t state = 0; state < 8; ++state) {
        for (bool finished : {false, true}) {
            for (uint8_t sleep : {uint8_t(0), uint8_t(2)}) {
                burst.unitState = state;
                burst.emissionFinished = finished;
                burst.flags100[0] = sleep;
                burst.hasState = true;
                assert(burst.CompletedIndependent() == (state != 1 && state != 2));
                burst.hasState = false;
                assert(!burst.CompletedIndependent());
            }
        }
    }
    std::printf("compact generator state: %zu bytes; regression tests passed\n", sizeof(State));
}
