#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <initializer_list>

// Compact ordinary-generator control state, not a particle-pool snapshot.
namespace EffectGenerator {
struct State {
    uint64_t dti = 0;
    uint64_t resource = 0;
    bool ordinary = false;
    bool hadParticles = false;
    uint8_t phase = 0;
    uint16_t countdown = 0;
    uint32_t status = 0;
    uint32_t modes = 0;
    uint8_t timing78[12] = {};
    uint32_t elapsed = 0;
    uint8_t emission1F0[0x18] = {};
};

template<class T> inline T Read(const void* object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(value));
    return value;
}
template<class T> inline void Write(void* object, size_t offset, T value)
{
    std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(value));
}

inline void CaptureOrdinary(const void* object, State& saved)
{
    saved.hadParticles = Read<uint64_t>(object, 0xE0) != 0;
    saved.phase = Read<uint8_t>(object, 0x77);
    saved.countdown = Read<uint16_t>(object, 0x122);
    saved.status = Read<uint32_t>(object, 0x20);
    saved.modes = Read<uint32_t>(object, 0x70);
    std::memcpy(saved.timing78, static_cast<const uint8_t*>(object) + 0x78, 12);
    saved.elapsed = Read<uint32_t>(object, 0xB8);
    // Includes subframe distribution, successful-spawn/event serials and the
    // repeat countdown at +0x206. Stops before the owned auxiliary pointer +0x210.
    std::memcpy(saved.emission1F0, static_cast<const uint8_t*>(object) + 0x1F0, 0x18);
}

inline bool RecreateSingle(const State& saved, const void* fresh, bool liveShot)
{
    return liveShot && saved.ordinary && saved.hadParticles && saved.phase == 5 &&
        (saved.modes & 0xF00u) == 0x100u && (saved.status & 4u) &&
        Read<uint64_t>(fresh, 0xE0) == 0;
}

inline void RestoreOrdinary(void* object, const State& saved, bool liveShot)
{
    if (!saved.ordinary) return; // Node/Infinite use different storage here.
    const bool recreate = RecreateSingle(saved, object, liveShot);
    auto* bytes = static_cast<uint8_t*>(object);
    uint32_t status = (Read<uint32_t>(object, 0x20) & ~0x80000007u) |
                      (saved.status & 0x80000007u);
    if (recreate) status = (status & ~0x80000000u) | 4u;
    Write(object, 0x20, status);
    Write<uint8_t>(object, 0x77, recreate ? 3 : saved.phase);
    Write(object, 0x122, saved.countdown);
    std::memcpy(bytes + 0x78, saved.timing78, 12);
    Write(object, 0xB8, saved.elapsed);
    std::memcpy(bytes + 0x1F0, saved.emission1F0, 0x18);
    if (recreate) {
        // The normal phase-3 single-spawn update increments these again. No
        // extra update, age-dependent replay, or historical pointer is needed.
        for (size_t offset : {size_t(0x1F4), size_t(0x1F8)}) {
            uint32_t serial = Read<uint32_t>(object, offset);
            if (serial) Write(object, offset, serial - 1);
        }
    }
}
} // namespace EffectGenerator
