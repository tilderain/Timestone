#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// CPU billboard / lite-polyline graph snapshots. The caller supplies the
// concrete-family check; this is deliberately not an arbitrary MtObject copier.
// One bounded blob per effect, stack-only validation, no native update calls.
namespace CallbackParticles {

constexpr size_t MaxBytes = 64 * 1024;
constexpr size_t MaxGenerators = 64;
constexpr size_t MaxManagers = 64;
constexpr size_t MaxParticles = 1024;
constexpr size_t ManagerSize = 0x150;
constexpr size_t GeneratorSize = 0x240;

template<class T> inline T Read(const void* p, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(p) + offset, sizeof(value));
    return value;
}

template<class T> inline void Write(void* p, size_t offset, T value)
{
    std::memcpy(static_cast<uint8_t*>(p) + offset, &value, sizeof(value));
}

inline uint8_t* Pointer(const void* p, size_t offset)
{
    return reinterpret_cast<uint8_t*>(Read<uint64_t>(p, offset));
}

struct Header {
    uint16_t managers;
    uint16_t generators;
    uint32_t bytes;
    uint16_t activeManagers;
    uint16_t reserved;
};

inline bool PoolIndex(uint64_t pointer, uint64_t base, size_t stride,
                      size_t capacity, size_t& index)
{
    if (pointer < base || !stride) return false;
    uint64_t offset = pointer - base;
    index = offset / stride;
    return offset % stride == 0 && index < capacity;
}

// Free-list reverse links are NOT conventionally maintained by native
// activateParticleFromFreeList. Validate forward lists and active prev links;
// canonicalize free prev links during relocation instead of trusting them.
inline bool ValidPool(const uint8_t* generator, const uint8_t* pool)
{
    size_t capacity = Read<uint16_t>(generator, 0x110);
    size_t stride = Read<uint16_t>(generator, 0x116);
    uint64_t base = Read<uint64_t>(generator, 0x38);
    size_t bytes = Read<uint32_t>(generator, 0x34);
    if (!base || !capacity || capacity > MaxParticles || stride < 0x80 ||
        capacity * stride > bytes) return false;
    std::array<uint64_t, MaxParticles / 64> visited{};
    size_t count = 0;
    for (size_t list = 0; list < 2; ++list) {
        uint64_t entry = Read<uint64_t>(generator, 0xE0 + list * 0x10);
        uint64_t tail = Read<uint64_t>(generator, 0xE8 + list * 0x10);
        uint64_t previous = 0;
        while (entry) {
            size_t index;
            if (!PoolIndex(entry, base, stride, capacity, index)) return false;
            uint64_t bit = uint64_t(1) << (index & 63);
            if (visited[index / 64] & bit) return false;
            visited[index / 64] |= bit;
            ++count;
            const uint8_t* particle = pool + index * stride;
            if (Read<uint16_t>(particle, 0x10) != index) return false;
            if (list == 0 && (Read<uint64_t>(particle, 0) != previous ||
                             Read<uint64_t>(particle, 0x60) != 0)) return false;
            // Among the exact supported families only LitePolyline has this
            // base size. Its mode-0 history needs three arrays of count vec4s.
            if (list == 0 && Read<uint16_t>(generator, 0x114) == 0xF0) {
                size_t history = Read<uint8_t>(particle, 0x9D);
                if (Read<uint8_t>(particle, 0x9C) != 0 || !history ||
                    0xF0 + history * 0x30 > stride) return false;
            }
            previous = entry;
            entry = Read<uint64_t>(particle, 8);
        }
        if (previous != tail) return false;
    }
    return count == capacity;
}

inline bool PlainGenerator(const uint8_t* generator)
{
    // GPU handles, child emitters and auxiliary allocations need their own
    // ownership policy. Limit motion/track storage to the audited scalar modes.
    uint32_t modes = Read<uint32_t>(generator, 0x70);
    return Read<uint64_t>(generator, 0x48) == 0 &&
           Read<uint32_t>(generator, 0xC4) == 0 &&
           (Read<uint32_t>(generator, 0x12C) & 0x400) == 0 &&
           ((modes >> 16) & 0xF) <= 2 &&
           ((modes >> 20) & 0xF) <= 2 &&
           ((modes >> 12) & 0xF) <= 8;
}

template<class Accessible> bool ValidManager(const uint8_t* manager,
                                             Accessible accessible)
{
    size_t size = Read<uint32_t>(manager, 0x128);
    auto* buffer = Pointer(manager, 0x130);
    if (size != 0 && size != 0x20 && size != 0x30 && size != 0x50) return false;
    if (size && (!buffer || !accessible(buffer, size))) return false;
    if (!size && buffer) return false;
    for (size_t offset : {size_t(0x138), size_t(0x140)}) {
        uint64_t view = Read<uint64_t>(manager, offset);
        size_t viewSize = offset == 0x138 ? 0x20 : 0x30;
        uint64_t base = reinterpret_cast<uint64_t>(buffer);
        if (view && (size < viewSize || view < base ||
                     view - base > size - viewSize)) return false;
    }
    return true;
}

template<class Accessible, class Supported>
bool Capture(const uint8_t* effect, std::vector<uint8_t>& out,
             Accessible accessible, Supported supported, size_t budget = MaxBytes)
{
    out.clear();
    if (budget <= sizeof(Header)) return false;
    // A linked child/root requires a coherent scheduler census and child graph.
    if (!(Read<uint32_t>(effect, 0x130) & 0x1000000) ||
        (Read<uint32_t>(effect, 0x100) & 0x80000) ||
        Read<uint64_t>(effect, 0x1F0)) return false;
    size_t managers = Read<uint16_t>(effect, 0x208);
    auto* managerBase = Pointer(effect, 0x220);
    if (!managers || managers > MaxManagers || !managerBase ||
        !accessible(managerBase, managers * ManagerSize)) return false;
    size_t total = sizeof(Header);
    for (size_t i = 0; i < managers; ++i) {
        auto* manager = managerBase + i * ManagerSize;
        if (!ValidManager(manager, accessible)) return false;
        total += ManagerSize + Read<uint32_t>(manager, 0x128);
    }
    std::array<const uint8_t*, MaxGenerators> generators{};
    size_t count = 0;
    for (auto* node = Pointer(effect, 0x218); node; node = Pointer(node, 0x18)) {
        if (count == MaxGenerators || !accessible(node, GeneratorSize) ||
            !supported(node) || !PlainGenerator(node)) return false;
        size_t manager = Read<uint16_t>(node, 0x30);
        if (manager >= managers || Pointer(node, 0x28) != managerBase + manager * ManagerSize)
            return false;
        size_t bytes = Read<uint32_t>(node, 0x34);
        total += GeneratorSize + bytes;
        if (total > budget || total > MaxBytes || !Pointer(node, 0x38) ||
            !accessible(Pointer(node, 0x38), bytes) ||
            !ValidPool(node, Pointer(node, 0x38))) return false;
        generators[count++] = node;
    }
    if (!count || count != Read<uint16_t>(effect, 0x204)) return false;
    out.resize(total);
    Header header{static_cast<uint16_t>(managers), static_cast<uint16_t>(count),
                  static_cast<uint32_t>(total), Read<uint16_t>(effect, 0x20A), 0};
    std::memcpy(out.data(), &header, sizeof(header));
    auto* dst = out.data() + sizeof(header);
    for (size_t i = 0; i < managers; ++i) {
        const auto* manager = managerBase + i * ManagerSize;
        size_t size = Read<uint32_t>(manager, 0x128);
        std::memcpy(dst, manager, ManagerSize);
        dst += ManagerSize;
        if (size) std::memcpy(dst, Pointer(manager, 0x130), size);
        dst += size;
    }
    for (size_t i = 0; i < count; ++i) {
        const auto* node = generators[i];
        size_t bytes = Read<uint32_t>(node, 0x34);
        std::memcpy(dst, node, GeneratorSize);
        dst += GeneratorSize;
        std::memcpy(dst, Pointer(node, 0x38), bytes);
        dst += bytes;
    }
    return true;
}

template<class Accessible>
bool Restore(uint8_t* effect, const std::vector<uint8_t>& saved, Accessible accessible)
{
    if (saved.size() < sizeof(Header) || saved.size() > MaxBytes ||
        !(Read<uint32_t>(effect, 0x130) & 0x1000000) ||
        (Read<uint32_t>(effect, 0x100) & 0x80000) ||
        Read<uint64_t>(effect, 0x1F0)) return false;
    Header header = Read<Header>(saved.data(), 0);
    if (header.bytes != saved.size() || !header.managers || !header.generators ||
        header.managers > MaxManagers || header.generators > MaxGenerators ||
        header.activeManagers > header.managers ||
        header.managers != Read<uint16_t>(effect, 0x208) ||
        header.generators != Read<uint16_t>(effect, 0x204)) return false;
    auto* managers = Pointer(effect, 0x220);
    if (!managers || !accessible(managers, header.managers * ManagerSize)) return false;
    std::array<const uint8_t*, MaxManagers> savedManagers{};
    std::array<const uint8_t*, MaxGenerators> savedGenerators{};
    std::array<uint8_t*, MaxGenerators> generators{};
    size_t cursor = sizeof(Header);
    for (size_t i = 0; i < header.managers; ++i) {
        if (saved.size() - cursor < ManagerSize) return false;
        const auto* old = saved.data() + cursor;
        auto* live = managers + i * ManagerSize;
        size_t bytes = Read<uint32_t>(old, 0x128);
        if (Read<uint64_t>(old, 0x108) != Read<uint64_t>(live, 0x108) ||
            bytes != Read<uint32_t>(live, 0x128) || !ValidManager(live, accessible) ||
            saved.size() - cursor - ManagerSize < bytes) return false;
        for (size_t offset : {size_t(0x138), size_t(0x140)}) {
            uint64_t oldView = Read<uint64_t>(old, offset);
            uint64_t liveView = Read<uint64_t>(live, offset);
            if ((oldView == 0) != (liveView == 0) ||
                (oldView && oldView - Read<uint64_t>(old, 0x130) !=
                            liveView - Read<uint64_t>(live, 0x130))) return false;
        }
        savedManagers[i] = old;
        cursor += ManagerSize + bytes;
    }
    auto* node = Pointer(effect, 0x218);
    for (size_t i = 0; i < header.generators; ++i) {
        if (!node || !accessible(node, GeneratorSize) ||
            saved.size() - cursor < GeneratorSize) return false;
        const auto* old = saved.data() + cursor;
        size_t bytes = Read<uint32_t>(old, 0x34);
        size_t manager = Read<uint16_t>(old, 0x30);
        if (Read<uint64_t>(old, 0) != Read<uint64_t>(node, 0) ||
            !PlainGenerator(node) || !PlainGenerator(old) ||
            manager >= header.managers || manager != Read<uint16_t>(node, 0x30) ||
            Pointer(node, 0x28) != managers + manager * ManagerSize ||
            bytes != Read<uint32_t>(node, 0x34) ||
            std::memcmp(old + 0x40, node + 0x40, 0x30) != 0 ||
            Read<uint16_t>(old, 0x110) != Read<uint16_t>(node, 0x110) ||
            std::memcmp(old + 0x104, node + 0x104, 0x0C) != 0 ||
            std::memcmp(old + 0x114, node + 0x114, 0x0A) != 0 ||
            saved.size() - cursor - GeneratorSize < bytes ||
            !Pointer(node, 0x38) || !accessible(Pointer(node, 0x38), bytes) ||
            !ValidPool(old, old + GeneratorSize) ||
            !ValidPool(node, Pointer(node, 0x38))) return false;
        // These three CPU-runtime pointers select resource-backed scalar/color
        // tracks. They are not relocatable pool addresses or owned allocations.
        for (size_t offset : {size_t(0x1D0), size_t(0x1D8), size_t(0x1E8)}) {
            if (Read<uint64_t>(old, offset) != Read<uint64_t>(node, offset))
                return false;
        }
        // Never orphan children from a fresh graph when overwriting its pool.
        savedGenerators[i] = old;
        generators[i] = node;
        cursor += GeneratorSize + bytes;
        node = Pointer(node, 0x18);
    }
    if (node || cursor != saved.size()) return false;

    // Validation is complete before the first write: no mixed outer/fresh graph
    // on rejection, and no calls which spawn, consume RNG, or advance lifetime.
    for (size_t i = 0; i < header.managers; ++i) {
        const auto* old = savedManagers[i];
        auto* live = managers + i * ManagerSize;
        std::memcpy(live, old, 0x100);             // transform/scalar records
        std::memcpy(live + 0x110, old + 0x110, 0x18); // state, seed, age, wait
        size_t bytes = Read<uint32_t>(live, 0x128);
        if (bytes) std::memcpy(Pointer(live, 0x130), old + ManagerSize, bytes);
    }
    for (size_t i = 0; i < header.generators; ++i) {
        const auto* old = savedGenerators[i];
        auto* live = generators[i];
        auto* pool = Pointer(live, 0x38);
        uint64_t base = reinterpret_cast<uint64_t>(pool);
        uint64_t oldBase = Read<uint64_t>(old, 0x38);
        size_t bytes = Read<uint32_t>(old, 0x34);
        auto relocate = [=](uint64_t p) { return p ? base + (p - oldBase) : uint64_t(0); };
        std::memcpy(pool, old + GeneratorSize, bytes);
        // Preserve factory sound/vibration ownership, all object/resource/pool
        // pointers and auxiliary allocations. Restore the complete CPU state.
        uint32_t sound = Read<uint32_t>(live, 0x20) & 0x70;
        Write(live, 0x20, (Read<uint32_t>(old, 0x20) & ~0x70u) | sound);
        std::memcpy(live + 0x70, old + 0x70, 0x54);
        std::memcpy(live + 0xE0, old + 0xE0, 0x130);
        std::memcpy(live + 0x238, old + 0x238, 8);
        for (size_t offset = 0xE0; offset < 0x100; offset += 8)
            Write(live, offset, relocate(Read<uint64_t>(old, offset)));
        size_t stride = Read<uint16_t>(old, 0x116);
        // Rebuild both prev/next chains from validated forward indices. This
        // also avoids transplanting native's stale free-list reverse pointers.
        for (size_t list = 0; list < 2; ++list) {
            uint64_t entry = Read<uint64_t>(old, 0xE0 + list * 0x10);
            uint64_t previous = 0;
            while (entry) {
                size_t index = (entry - oldBase) / stride;
                auto* particle = pool + index * stride;
                uint64_t next = Read<uint64_t>(particle, 8);
                Write(particle, 0, previous);
                Write(particle, 8, relocate(next));
                Write<uint64_t>(particle, 0x60, 0);
                previous = base + index * stride;
                entry = next;
            }
        }
    }
    Write(effect, 0x20A, header.activeManagers);
    return true;
}

} // namespace CallbackParticles
