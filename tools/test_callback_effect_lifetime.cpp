#include "../UMVC3Hook/code/callback_effect_lifetime.h"

#include <array>
#include <cassert>
#include <cstdio>

using namespace CallbackLifetime;

template<class T, size_t N> void Store(std::array<uint8_t, N>& bytes, size_t offset, T value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<class T, size_t N> T Load(const std::array<uint8_t, N>& bytes, size_t offset)
{
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

int main()
{
    State running{true, 2, false};
    State draining{true, 2, true};
    State retiring{true, 3, true};
    State unknown{};

    // Doom/Big Thunder: restore after the once-only finish handler, while
    // +0x212c is 2/3 and the shot waits for callbacks. Unsupported fresh graphs
    // cannot remain active just because the saved unit itself was still alive.
    assert(ShouldRetire(true, draining, false, true));
    assert(ShouldRetire(true, running, false, true));
    // Same failure for an independent one-shot with no shot cleanup handler.
    assert(ShouldRetire(true, draining, false, false));
    assert(!ShouldRetire(true, running, false, false));

    // Preserve the working full-pool fix: visible draining particles must NOT
    // disappear merely because emission has finished or the shot was hit.
    assert(!ShouldRetire(true, draining, true, true));
    assert(!NeedsFinish(draining, true, true));
    // An inconsistent old snapshot with a running emitter in cleanup should
    // stop emitting but retain its restored particles to drain naturally.
    assert(!ShouldRetire(true, running, true, true));
    assert(NeedsFinish(running, true, true));
    assert(!NeedsFinish(running, true, false));
    assert(!NeedsFinish(running, false, true));

    assert(ShouldRetire(false, unknown, false, false));
    assert(ShouldRetire(true, retiring, true, false));
    assert(!ShouldRetire(true, unknown, false, true));
    State sleeping{true, 2, true, true};
    assert(!ShouldRetire(true, sleeping, false, false)); // intentional sleep/reuse
    assert(ShouldRetire(true, sleeping, false, true));  // owner actually ended

    // Shared prefix supports Node/Infinite/EffectUnit without interpreting
    // matrix data as particle endpoints. Partially finished effects still run.
    EmissionSummary terminal;
    terminal.Observe(0x80000007); // draining ordinary/node generator
    terminal.Observe(0x80000000); // completed, low activity bits cleared
    terminal.Observe(0);          // disabled sibling
    assert(terminal.Finished(true, 3, 3));
    assert(!terminal.Finished(false, 3, 3)); // truncated/cyclic list
    assert(!terminal.Finished(true, 3, 4));  // partial construction
    terminal.Observe(7); // another emitter is still running
    assert(!terminal.Finished(true, 4, 4));
    EmissionSummary disabled;
    disabled.Observe(0);
    assert(!disabled.Finished(true, 1, 1));

    // Failed/unsupported pool restoration must not reset a finite lifetime.
    // End mode is copied without overwriting graph-ready/configuration bits.
    std::array<uint8_t, 0x250> effect{};
    std::array<uint8_t, 8> saved1D0{};
    std::array<uint8_t, 0x28> saved110{};
    Store<uint32_t>(effect, 0x100, 0xDEADBEEF);
    Store<uint32_t>(effect, 0x130, 0x12345672);
    Store<uint16_t>(effect, 0x208, 17);
    Store<uint16_t>(saved1D0, 6, 60);
    Store<uint32_t>(saved110, 0x20, 1);
    for (uint16_t remaining : {3, 2, 1, 0}) {
        Store<uint16_t>(effect, 0x1D4, 60); // fresh factory each restore
        Store<uint16_t>(saved1D0, 4, remaining);
        RestoreFiniteClock(effect.data(), saved1D0.data(), saved110.data());
        assert(Load<uint16_t>(effect, 0x1D4) == remaining);
        assert(Load<uint16_t>(effect, 0x1D6) == 60);
        assert(Load<uint32_t>(effect, 0x130) == 0x12345671);
        assert(Load<uint32_t>(effect, 0x100) == 0xDEADBEEF);
        assert(Load<uint16_t>(effect, 0x208) == 17);
    }
    Store<uint16_t>(saved1D0, 6, 0); // unlimited original: don't invent a deadline
    Store<uint16_t>(effect, 0x1D4, 42);
    RestoreFiniteClock(effect.data(), saved1D0.data(), saved110.data());
    assert(Load<uint16_t>(effect, 0x1D4) == 42);
    RestoreSleepMode(effect.data(), false);
    assert(Load<uint32_t>(effect, 0x100) == (0xDEADBEEFu & ~2u));
    RestoreSleepMode(effect.data(), true);
    assert(Load<uint32_t>(effect, 0x100) == 0xDEADBEEF);
    std::puts("callback effect lifetime tests passed");
}
