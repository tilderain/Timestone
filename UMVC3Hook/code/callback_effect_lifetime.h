#pragma once

#include <cstdint>
#include <cstring>

namespace CallbackLifetime {

struct State {
    bool known = false;
    uint8_t unitState = 0;
    bool emissionFinished = false;
    bool sleeping = false;
};

constexpr bool Alive(uint8_t state) { return state == 1 || state == 2; }

struct EmissionSummary {
    bool sawFinish = false;
    bool running = false;

    void Observe(uint32_t status)
    {
        sawFinish |= (status & 0x80000000u) != 0;
        running |= (status & 7u) != 0 && (status & 0x80000000u) == 0;
    }

    bool Finished(bool completeChain, uint32_t count, uint32_t expected) const
    {
        return completeChain && count != 0 && count == expected && sawFinish && !running;
    }
};

// A restored pool can still contain a legitimate particle tail after finish.
// A fresh fallback cannot represent that tail: allowing it to emit again loses
// the one-time finish event, especially when the shot is already in cleanup.
constexpr bool ShouldRetire(bool occupied, State saved, bool graphRestored,
                            bool waitingForCallbacks)
{
    if (!occupied) return true;
    if (!saved.known) return false;
    if (!Alive(saved.unitState)) return true;
    return !graphRestored &&
           (waitingForCallbacks || (saved.emissionFinished && !saved.sleeping));
}

constexpr bool NeedsFinish(State saved, bool graphRestored, bool waitingForCallbacks)
{
    return saved.known && Alive(saved.unitState) && graphRestored &&
           waitingForCallbacks && !saved.emissionFinished;
}

inline void RestoreFiniteClock(uint8_t* effect, const uint8_t* saved1D0,
                               const uint8_t* saved110)
{
    uint16_t configured;
    std::memcpy(&configured, saved1D0 + 6, sizeof(configured));
    if (!configured) return;
    // The countdown must continue even if a pool is unsupported/over budget.
    // Never restart the outer lifetime at its factory-configured duration.
    std::memcpy(effect + 0x1D4, saved1D0 + 4, 4);
    uint32_t liveModes, savedModes;
    std::memcpy(&liveModes, effect + 0x130, 4);
    std::memcpy(&savedModes, saved110 + 0x20, 4);
    liveModes = (liveModes & ~0xFu) | (savedModes & 0xFu);
    std::memcpy(effect + 0x130, &liveModes, 4);
}

inline void RestoreSleepMode(uint8_t* effect, bool sleeping)
{
    uint32_t flags;
    std::memcpy(&flags, effect + 0x100, sizeof(flags));
    flags = (flags & ~2u) | (sleeping ? 2u : 0u);
    std::memcpy(effect + 0x100, &flags, sizeof(flags));
}

} // namespace CallbackLifetime
