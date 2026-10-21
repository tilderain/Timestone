#include "../UMVC3Hook/code/rollback_character_restore.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {
constexpr uint64_t ironMan = 0x140D45518;
constexpr uint64_t taskmaster = 0x140D45E58;
using Body = std::array<uint8_t, 0x8000>;

struct Range { uint64_t dti; size_t offset; size_t size; };
constexpr Range generated[] = {
#include "../UMVC3Hook/code/generated_character_tail_fields.inc"
};

// Independent expectation from the documented constructor/consumer ledger.
// These are ownership spans, not a runtime bit-pattern filter.
constexpr Range owners[] = {
    {0x140D44E48, 0x68F0, 8}, {0x140D44E48, 0x6910, 0x10},
    {0x140D44E90, 0x68F0, 8}, {0x140D44F18, 0x68F8, 8},
    {0x140D44F60, 0x68F0, 8}, {0x140D44F60, 0x6900, 8},
    {0x140D44FA0, 0x68F0, 8}, {0x140D44FA0, 0x6908, 0x18},
    {0x140D44FA0, 0x6928, 0x10},
    {0x140D45020, 0x6900, 0x20}, {0x140D45068, 0x68F0, 8},
    {0x140D450A8, 0x68F0, 0x30}, {0x140D450F0, 0x68F0, 0x30},
    {0x140D45138, 0x68F1, 7}, {0x140D45200, 0x6A0C, 4},
    {0x140D45200, 0x6A28, 0x18}, {0x140D452B0, 0x68F0, 0x20},
    {0x140D45450, 0x68F8, 8}, {0x140D45450, 0x691C, 4},
    {0x140D454D0, 0x68F0, 0x10}, {0x140D454D0, 0x691C, 4},
    {0x140D45518, 0x68F0, 8}, {0x140D45518, 0x6900, 8},
    {0x140D45518, 0x691C, 4}, {0x140D45518, 0x6920, 8},
    {0x140D45578, 0x68F0, 8}, {0x140D45578, 0x6900, 8},
    {0x140D45578, 0x6910, 0x10},
    {0x140D455F8, 0x68F0, 0x0C}, {0x140D455F8, 0x6918, 0x18},
    {0x140D45700, 0x6910, 0x18}, {0x140D45748, 0x68F0, 8},
    {0x140D457C0, 0x68F0, 0x28}, {0x140D457C0, 0x6928, 0x10},
    {0x140D458D0, 0x68F0, 0x18}, {0x140D45910, 0x68F8, 0x20},
    {0x140D45910, 0x6920, 0x10}, {0x140D45958, 0x6900, 0x28},
    {0x140D45998, 0x6910, 8}, {0x140D459D8, 0x68F0, 0x60},
    {0x140D45A10, 0x6918, 8}, {0x140D45A10, 0x6928, 0x10},
    {0x140D45C58, 0x68F0, 8}, {0x140D45C58, 0x6900, 8},
    {0x140D45CF8, 0x68F8, 0x30}, {0x140D45CF8, 0x6930, 0x48},
    {0x140D45D80, 0x6900, 8}, {0x140D45DD8, 0x68F0, 0x18},
    {0x140D45E58, 0x6910, 8}, {0x140D45EA0, 0x68F0, 0x10},
    {0x140D45ED8, 0x68F0, 0x18}, {0x140D45F20, 0x68F0, 8},
    {0x140D45F68, 0x6920, 8}, {0x140D45F68, 0x6940, 0x10},
    {0x140D45FF8, 0x68FC, 4}, {0x140D45FF8, 0x6900, 8},
    {0x140D45FF8, 0x6910, 0x10}, {0x140D46038, 0x68F0, 8},
    {0x140D46038, 0x6910, 0x18}, {0x140D46080, 0x68F0, 8},
    {0x140D460C0, 0x6900, 0x10}, {0x140D460C0, 0x6920, 8},
    {0x140D460C0, 0x6930, 0x78},
    {0x140D460C0, 0x69A8, 0x20}, {0x140D46118, 0x68F0, 0x10},
    {0x140D45788, 0x68F0, 0x10},
};

constexpr Range verified[] = {
    // Independent writer/consumer ledger from the three native tail audits.
    {0x140D44F60, 0x68F8, 1}, {0x140D44FA0, 0x68F8, 1},
    {0x140D44FA0, 0x6920, 2}, {0x140D44FE0, 0x68F8, 4},
    {0x140D44FE0, 0x6900, 0x30}, {0x140D44FE0, 0x6930, 1},
    {0x140D450A8, 0x6928, 6}, {0x140D450F0, 0x692C, 2},
    {0x140D450A8, 0x6948, 4},
    {0x140D45138, 0x68F0, 1}, {0x140D45180, 0x68F8, 1},
    {0x140D45200, 0x69E0, 6}, {0x140D45200, 0x6A00, 0x0C},
    {0x140D45408, 0x68F4, 4}, {0x140D45450, 0x6910, 0x0C},
    {0x140D45488, 0x6904, 4}, {0x140D454D0, 0x6910, 0x0C},
    {0x140D45518, 0x6910, 0x0C}, {0x140D45518, 0x6928, 1},
    {0x140D45578, 0x6908, 1}, {0x140D455F8, 0x68FC, 4},
    {0x140D455F8, 0x6914, 4}, {0x140D45748, 0x68F8, 1},
    {0x140D45788, 0x6E90, 1}, {0x140D457C0, 0x6940, 2},
    {0x140D45910, 0x6930, 1}, {0x140D45958, 0x68F8, 4},
    {0x140D45A10, 0x6914, 4}, {0x140D45C58, 0x6908, 1},
    {0x140D45DD8, 0x6908, 2}, {0x140D45DD8, 0x690C, 0x11},
    {0x140D45DD8, 0x6920, 0x0E}, {0x140D45E18, 0x68F0, 0x0C},
    {0x140D45EA0, 0x690C, 1}, {0x140D45F68, 0x6910, 1},
    {0x140D45FF8, 0x68F0, 0x0C}, {0x140D46038, 0x6900, 0x0C},
    {0x140D46080, 0x68F8, 1}, {0x140D46158, 0x6900, 0x0C},
    {0x140D46158, 0x6910, 4},
    {0x140D45CF8, 0x6928, 4}, {0x140D45CF8, 0x7494, 0x10},
    {0x140D45CF8, 0x74A8, 1},
    {0x140D460C0, 0x68F8, 1}, {0x140D460C0, 0x6928, 1},
    {0x140D44F18, 0x6900, 8}, {0x140D44F60, 0x6908, 4},
    {0x140D45020, 0x68F4, 8}, {0x140D45020, 0x6920, 1},
    {0x140D45068, 0x68F8, 4}, {0x140D450A8, 0x6920, 8},
    {0x140D450A8, 0x6930, 0x0C}, {0x140D450F0, 0x6920, 8},
    {0x140D450F0, 0x6930, 0x0C}, {0x140D45180, 0x68F4, 4},
    {0x140D45180, 0x68F9, 1}, {0x140D451C0, 0x68F4, 4},
    {0x140D45200, 0x6990, 0x30}, {0x140D45200, 0x69C4, 4},
    {0x140D45200, 0x69DC, 4}, {0x140D45200, 0x69EC, 8},
    {0x140D45200, 0x6A48, 9}, {0x140D45248, 0x6930, 0x20},
    {0x140D45408, 0x68FC, 0x18}, {0x140D45408, 0x6918, 4},
    {0x140D45450, 0x6900, 1}, {0x140D45450, 0x6920, 5},
    {0x140D45450, 0x6928, 4}, {0x140D45488, 0x6900, 1},
    {0x140D45518, 0x68F8, 2}, {0x140D45578, 0x68F8, 1},
    {0x140D45680, 0x68F0, 0x20},
    {0x140D456C0, 0x68F4, 4}, {0x140D45700, 0x68F4, 0x1C},
    {0x140D45890, 0x68F0, 0x60}, {0x140D458D0, 0x6908, 9},
    {0x140D45958, 0x68F4, 1}, {0x140D45958, 0x68FC, 2},
    {0x140D45958, 0x6928, 4}, {0x140D45998, 0x68F4, 4},
    {0x140D45998, 0x68F8, 2}, {0x140D45998, 0x6908, 1},
    {0x140D45998, 0x6938, 4}, {0x140D459D8, 0x6950, 2},
    {0x140D459D8, 0x6954, 9}, {0x140D459D8, 0x6960, 4},
    {0x140D45A10, 0x68F4, 8}, {0x140D45A10, 0x6920, 1},
    {0x140D45A10, 0x6938, 8}, {0x140D45C58, 0x6909, 1},
    {0x140D45C58, 0x690C, 9}, {0x140D45D40, 0x68F0, 0x10},
    {0x140D45D80, 0x68FC, 4}, {0x140D45E18, 0x690C, 8},
    {0x140D45E58, 0x68F0, 0x20}, {0x140D45EA0, 0x6900, 1},
    {0x140D45EA0, 0x6908, 4}, {0x140D45EA0, 0x6910, 8},
    {0x140D45F68, 0x691C, 4}, {0x140D45F68, 0x6930, 0x0C},
    {0x140D45FB0, 0x6900, 0x0C}, {0x140D45FF8, 0x690C, 4},
    {0x140D46038, 0x68F8, 1}, {0x140D460C0, 0x6910, 1},
    {0x140D45788, 0x6900, 0x590}, {0x140D46118, 0x6900, 0x590},
    {0x140D46118, 0x6E90, 4}, {0x140D46158, 0x68F4, 8},
};

void ExpectBytes(const Body& body, size_t offset, size_t size, uint8_t value)
{
    for (size_t i = offset; i < offset + size; ++i) {
        if (body[i] != value)
            std::fprintf(stderr, "byte %zX: expected %02X got %02X (range %zX/%zX)\n",
                         i, value, body[i], offset, size);
        assert(body[i] == value);
    }
}

void Store(Body& body, size_t offset, uint64_t value)
{
    std::memcpy(body.data() + offset, &value, sizeof(value));
}

void Restore(Body& live, const Body& saved, uint64_t dti, bool roster,
             CharacterRestorePhase phase)
{
    assert(Rollback_RestoreCharacterFields(live.data(), live.size(),
        saved.data(), saved.size(), dti, roster, phase) ==
        CharacterRestoreStatus::ReviewedSubset);
}
} // namespace

int main()
{
    Body live, saved;
    // Indexed shot relationships preserve slot identity; none may be a raw copy.
    const std::pair<uint64_t, size_t> shotCounts[] = {
        {0x140D44E10, 1}, {0x140D45020, 1}, {0x140D45700, 2},
        {0x140D45910, 3}, {0x140D45CF8, 6}, {0x140D45D80, 1}, {0x140D45ED8, 2}};
    for (const auto& [dti, count] : shotCounts) {
        auto fields = Rollback_CharacterShotTailOffsets(dti);
        assert(fields.size() == count);
        live.fill(0x5A);
        saved.fill(0xA5);
        Restore(live, saved, dti, true, CharacterRestorePhase::Early);
        for (uint32_t field : fields) ExpectBytes(live, field, 8, 0x5A);
    }
    assert(Rollback_CharacterShotTailOffsets(0).empty());
    for (uint64_t dti : {uint64_t(0x140D455B0), uint64_t(0x140D45638)}) {
        live.fill(0x5A);
        saved.fill(0xA5);
        assert(Rollback_RestoreAuxiliaryActor(live.data(), 0x14CF, saved.data(),
            0x14D0, dti) == CharacterRestoreStatus::InvalidBuffer);
        ExpectBytes(live, 0, live.size(), 0x5A);
        assert(Rollback_RestoreAuxiliaryActor(live.data(), 0x14D0, saved.data(),
            0x14D0, dti) == CharacterRestoreStatus::ReviewedSubset);
        ExpectBytes(live, 0x1470, 9, 0xA5);
        ExpectBytes(live, 0x1479, 3, 0x5A);
        ExpectBytes(live, 0x147C, 9, 0xA5);
        ExpectBytes(live, 0x1485, 3, 0x5A);
        ExpectBytes(live, 0x1450, 0x20, 0x5A); // model and motion resource owners
        ExpectBytes(live, 0x1488, 8, 0x5A); // parent identity
        ExpectBytes(live, 0x1490, 0x3C, 0xA5);
        ExpectBytes(live, 0x14CC, 1, 0x5A);
        ExpectBytes(live, 0x14CD, 1, 0xA5);
        ExpectBytes(live, 0x14D0, live.size() - 0x14D0, 0x5A);
        std::memset(live.data() + 0x50, 0xCC, 0x10);
        std::memset(live.data() + 0x1470, 0xCC, 9);
        assert(Rollback_RestoreAuxiliaryActor(live.data(), 0x14D0, saved.data(),
            0x14D0, dti, CharacterRestorePhase::Late) == CharacterRestoreStatus::ReviewedSubset);
        ExpectBytes(live, 0x50, 0x10, 0xA5);
        ExpectBytes(live, 0x1470, 9, 0xCC); // no blanket late motion-history rewind
    }
    // Shuma's controllers restore payload only while both parent/handle match.
    live.fill(0x5A);
    saved.fill(0xA5);
    for (size_t field : {0x6970, 0x6988, 0x6998})
        std::memcpy(live.data() + field, saved.data() + field, 8);
    Restore(live, saved, 0x140D45CF8, true, CharacterRestorePhase::Early);
    ExpectBytes(live, 0x6990, 4, 0xA5);
    ExpectBytes(live, 0x69A0, 8, 0xA5);
    ExpectBytes(live, 0x6980, 8, 0x5A);
    ExpectBytes(live, 0x69B8, 4, 0x5A); // next controller has a different binding
    // Test every generated entry, including false inferred wall timers. A
    // constructor-refined narrow bool is not permission to copy its padding.
    size_t scalarBytesChecked = 0;
    for (const auto& range : generated) {
        for (uint64_t bits : {uint64_t(0), uint64_t(0x140000000), uint64_t(0x3F800000)}) {
            saved.fill(0xA5);
            for (size_t offset = 0x68F0; offset + 8 <= saved.size(); offset += 8)
                Store(saved, offset, bits);
            live.fill(0x5A);
            Restore(live, saved, range.dti, true, CharacterRestorePhase::Early);
            size_t confirmedEnd = range.offset + range.size;
            if (range.offset == 0x68F0) {
                if (range.dti == 0x140D45180 || range.dti == 0x140D45200 ||
                    range.dti == 0x140D45998) confirmedEnd = range.offset + 1;
                if (range.dti == 0x140D452F0 || range.dti == 0x140D46158)
                    confirmedEnd = range.offset + 2;
            }
            for (size_t offset = range.offset; offset < confirmedEnd; ++offset) {
                bool owner = false;
                for (const auto& span : owners)
                    owner |= span.dti == range.dti && offset >= span.offset &&
                        offset < span.offset + span.size;
                if (!owner) {
                    if (live[offset] != saved[offset]) {
                        std::fprintf(stderr, "missing generated scalar dti=%llX offset=%zX\n",
                                     (unsigned long long)range.dti, offset);
                        assert(false);
                    }
                    ++scalarBytesChecked;
                }
            }
        }
    }
    for (const auto& range : verified) {
        for (uint8_t value : {uint8_t(0), uint8_t(1), uint8_t(0xA5)}) {
            live.fill(0x5A);
            saved.fill(value);
            Restore(live, saved, range.dti, true, CharacterRestorePhase::Early);
            ExpectBytes(live, range.offset, range.size, value);
        }
    }
    for (uint8_t latch : {uint8_t(0), uint8_t(1)}) {
        saved.fill(0xA5);
        live.fill(0x5A);
        saved[0x68F8] = saved[0x6928] = latch;
        Restore(live, saved, 0x140D460C0, true, CharacterRestorePhase::Early);
        assert(live[0x68F8] == latch && live[0x6928] == latch);
        ExpectBytes(live, 0x68F9, 7, 0x5A);
        ExpectBytes(live, 0x6929, 7, 0x5A);
        ExpectBytes(live, 0x6900, 0x10, 0x5A); // charge/install effects
        ExpectBytes(live, 0x6920, 8, 0x5A); // shadow identity, not an effect
    }
    // Check entire owner spans, not just the low words that happened to look
    // mapped in one frame. Also exercise null/non-null ownership transitions.
    for (const auto& range : owners) {
        for (auto phase : {CharacterRestorePhase::Early, CharacterRestorePhase::Late}) {
            for (uint8_t sentinel : {uint8_t(0), uint8_t(0x5A)}) {
                live.fill(sentinel);
                saved.fill(0xA5);
                Restore(live, saved, range.dti, true, phase);
                ExpectBytes(live, range.offset, range.size, sentinel);
            }
        }
    }
    std::printf("checked %zu generated ranges, %zu verified ranges, %zu owner spans; "
                "%zu scalar-byte checks across 3 bit patterns\n",
                std::size(generated), std::size(verified), std::size(owners), scalarBytesChecked);
    for (auto phase : {CharacterRestorePhase::Early, CharacterRestorePhase::Late}) {
        live.fill(0x5A);
        saved.fill(0xA5);
        Restore(live, saved, ironMan, true, phase);
        for (size_t helper : {size_t(0x3BF0), size_t(0x3D60)}) {
            ExpectBytes(live, helper + 0x60, 8, 0x5A); // parent joint
            ExpectBytes(live, helper + 0x68, 0x10, 0xA5);
            ExpectBytes(live, helper + 0x78, 8, 0x5A); // model owner
            ExpectBytes(live, helper + 0x80, 0x10, 0xA5);
        }
    }
    saved.fill(0xA5);
    live.fill(0x5A);
    Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
    for (size_t command : {size_t(0x2738), size_t(0x2950)}) {
        ExpectBytes(live, command + 0x1A0, 4, 0xA5); // saved action-selection work
        ExpectBytes(live, command + 0x1E4, 4, 0x5A); // unresolved, not included
    }
    for (size_t message = 0x2C50; message < 0x2D70; message += 0x30) {
        ExpectBytes(live, message, 0x14, 0x5A); // vtable, owner and native type
        ExpectBytes(live, message + 0x14, 1, 0xA5); // stop gate
        ExpectBytes(live, message + 0x15, 3, 0x5A); // no proof these are state
        ExpectBytes(live, message + 0x18, 0x18, 0xA5); // IDs and full queued impulse
    }
    ExpectBytes(live, 0x12C0, 8, 0x5A); // toon vtable
    ExpectBytes(live, 0x12C8, 0x1C, 0xA5);
    ExpectBytes(live, 0x12E4, 0x0C, 0x5A);
    ExpectBytes(live, 0x12F0, 0x10, 0xA5);
    Store(saved, 0x13D8, 0);
    Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
    ExpectBytes(live, 0x13D8, 8, 0); // saved-null result must clear future object
    saved.fill(0xA5);
    for (size_t binding : {size_t(0x100), size_t(0x110), size_t(0x36E0)})
        std::memcpy(live.data() + binding, saved.data() + binding, 8);
    for (size_t controller = 0x36F0; controller < 0x3BF0; controller += 0x28) {
        for (size_t binding : {size_t(8), size_t(0x18)})
            std::memcpy(live.data() + controller + binding, saved.data() + controller + binding, 8);
    }
    Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
    ExpectBytes(live, 0x36E8, 1, 0xA5);
    for (size_t controller = 0x36F0; controller < 0x3BF0; controller += 0x28) {
        ExpectBytes(live, controller, 8, 0x5A); // vtable
        ExpectBytes(live, controller + 0x10, 4, 0xA5);
        ExpectBytes(live, controller + 0x14, 4, 0x5A); // unknown gap
        ExpectBytes(live, controller + 0x20, 8, 0xA5);
    }
    // Replacing a handle prevents mixing its saved clock/selectors with the new binding.
    Store(live, 0x36F0 + 0x18, 1);
    std::memset(live.data() + 0x36F0 + 0x20, 0xCC, 8);
    Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
    ExpectBytes(live, 0x36F0 + 0x20, 8, 0xCC);
    for (auto phase : {CharacterRestorePhase::Early, CharacterRestorePhase::Late}) {
        live.fill(0x5A);
        Restore(live, saved, ironMan, true, phase);
        // Native identity, intrusive lists, owners, resources and locks never
        // depend on whether either word looks mapped, null or pointer-shaped.
        for (size_t field : {0x00, 0x18, 0x20, 0x38, 0x40, 0x1308,
                             0x1348, 0x1360, 0x1380, 0x1408, 0x1478,
                             0x1488, 0x1490, 0x1498, 0x14B0, 0x1530,
                             0x1538, 0x1540, 0x21C8, 0x21D0, 0x3F40,
                             0x6428, 0x6490, 0x6498, 0x6740, 0x67F0,
                             0x68F0, 0x6900, 0x6920})
            ExpectBytes(live, field, 8, 0x5A);
        ExpectBytes(live, 0x6460, 0x28, 0x5A);
        ExpectBytes(live, 0x691C, 4, 0x5A); // unconsumed fourth vector lane
        ExpectBytes(live, 0x40A0, 0x10, 0x5A); // unresolved, not scalar by default
        ExpectBytes(live, 0x68F8, 2,
                    phase == CharacterRestorePhase::Early ? 0xA5 : 0x5A);
        for (size_t command : {0x2738, 0x2950}) {
            for (size_t field : {0x00, 0x08, 0x10, 0x138, 0x140, 0x160, 0x168, 0x178, 0x180})
                ExpectBytes(live, command + field, 8, 0x5A);
            ExpectBytes(live, command + 0x1E8, 0x30, 0x5A); // callbacks
        }
    }

    live.fill(0x5A);
    Store(saved, 0x1480, 0x000000003F800000); // packed float/zero resembles pointer
    Store(saved, 0x21D8, 0x0000000140000000); // input words resemble image address
    Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
    assert(std::memcmp(live.data() + 0x1480, saved.data() + 0x1480, 8) == 0);
    assert(std::memcmp(live.data() + 0x21D8, saved.data() + 0x21D8, 8) == 0);
    ExpectBytes(live, 0x10, 4, 0xA5);
    ExpectBytes(live, 0x2D80, 0x10, 0xA5); // RNG
    ExpectBytes(live, 0x2D90, 4, 0x5A); // setup marker/padding
    ExpectBytes(live, 0x2D98, 4, 0x5A); // immutable archetype

    // Native reconstruction intervenes: late repair is not redundant.
    std::memset(live.data() + 0x1480, 0xCC, 8);
    std::memset(live.data() + 0x21D8, 0xCC, 8);
    Restore(live, saved, ironMan, true, CharacterRestorePhase::Late);
    assert(std::memcmp(live.data() + 0x1480, saved.data() + 0x1480, 8) == 0);
    ExpectBytes(live, 0x21D8, 8, 0xCC); // input not replayed late

    live.fill(0x5A);
    Restore(live, saved, taskmaster, false, CharacterRestorePhase::Early);
    ExpectBytes(live, 0x10, 8, 0x5A); // helper lifecycle unchanged
    ExpectBytes(live, 0x68F0, 0x20, 0xA5); // scalar tail despite pointer patterns
    ExpectBytes(live, 0x6910, 8, 0x5A); // drone owner, not tail POD

    // Throw slot 3 is mutable: entering, leaving, or changing the thrower
    // must rewind the binding and execution state together.
    for (auto bindings : {std::pair<uint64_t, uint64_t>{0, 0x140100000},
                          {0x140100000, 0}, {0x140100000, 0x140200000}}) {
        live.fill(0x5A);
        for (size_t field : {0x1360, 0x1368, 0x1370})
            std::memcpy(live.data() + field, saved.data() + field, 8);
        Store(saved, 0x1378, bindings.first);
        Store(live, 0x1378, bindings.second);
        Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
        assert(std::memcmp(live.data() + 0x1378, saved.data() + 0x1378, 8) == 0);
        ExpectBytes(live, 0x1350, 0x10, 0xA5);
        ExpectBytes(live, 0x1388, 0x30, 0xA5);
        ExpectBytes(live, 0x1380, 8, 0x5A); // interpreter owner stays live

        Store(live, 0x1368, 1); // unrelated permanent resource replacement
        std::memset(live.data() + 0x1388, 0xCC, 0x30);
        Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
        ExpectBytes(live, 0x1388, 0x30, 0xCC);
    }

    // Both callbacks are valid with self-state zero. Preserve the snapshot's
    // actual selection in both phases; never import an arbitrary code pointer.
    for (auto phase : {CharacterRestorePhase::Early, CharacterRestorePhase::Late}) {
        for (uint64_t callback : {uint64_t(0x14010BB40), uint64_t(0x14010BC20),
                                  uint64_t(0), UINT64_MAX}) {
            live.fill(0x5A);
            for (size_t helper : {0x3BF0, 0x3D60}) {
                Store(saved, helper + 0x120, 0);
                Store(saved, helper + 0x128, callback);
            }
            Restore(live, saved, ironMan, true, phase);
            for (size_t helper : {0x3BF0, 0x3D60}) {
                if (callback == 0x14010BB40 || callback == 0x14010BC20)
                    assert(std::memcmp(live.data() + helper + 0x128,
                                       saved.data() + helper + 0x128, 8) == 0);
                else
                    ExpectBytes(live, helper + 0x128, 8, 0x5A);
            }
        }
    }

    // Counts and other borrowed references require exact owners.
    live.fill(0x5A);
    for (size_t field : {0x1360, 0x1368, 0x1370, 0x1378, 0x3F40,
                         0x2738 + 0x168, 0x2738 + 0x178})
        std::memcpy(live.data() + field, saved.data() + field, 8);
    Restore(live, saved, ironMan, true, CharacterRestorePhase::Early);
    ExpectBytes(live, 0x1390, 0x28, 0xA5);
    ExpectBytes(live, 0x3F38, 4, 0xA5);
    ExpectBytes(live, 0x2738 + 0x170, 4, 0xA5);
    ExpectBytes(live, 0x2738 + 0x180, 8, 0xA5);
    ExpectBytes(live, 0x2950 + 0x170, 4, 0x5A);
    ExpectBytes(live, 0x2950 + 0x180, 8, 0x5A);

    for (size_t size : {size_t(0), size_t(8), size_t(0x1B60), size_t(0x68EF), size_t(0x690F)}) {
        live.fill(0x5A);
        for (auto phase : {CharacterRestorePhase::Early, CharacterRestorePhase::Late}) {
            assert(Rollback_RestoreCharacterFields(live.data(), size,
                saved.data(), saved.size(), taskmaster, true, phase) ==
                CharacterRestoreStatus::InvalidBuffer);
            assert(Rollback_RestoreCharacterFields(live.data(), live.size(),
                saved.data(), size, taskmaster, true, phase) ==
                CharacterRestoreStatus::InvalidBuffer);
            ExpectBytes(live, 0, live.size(), 0x5A); // no partial writes
        }
    }
    assert(Rollback_RestoreCharacterFields(nullptr, live.size(), saved.data(),
        saved.size(), ironMan, true, CharacterRestorePhase::Early) ==
        CharacterRestoreStatus::InvalidBuffer);

    for (uint64_t unknownDti : {uint64_t(0), uint64_t(0x140D44E11), UINT64_MAX}) {
        for (auto phase : {CharacterRestorePhase::Early, CharacterRestorePhase::Late}) {
            live.fill(0x5A);
            assert(Rollback_RestoreCharacterFields(live.data(), live.size(), saved.data(),
                saved.size(), unknownDti, true, phase) == CharacterRestoreStatus::UnsupportedLayout);
            ExpectBytes(live, 0, live.size(), 0x5A); // common body must not mutate either
        }
    }

    // Exercise the same unchecked Motion routine used by fighters and weapons.
    // Animation A's descriptors/samplers must replace prediction B's bindings,
    // but neither the joint allocation nor the channel-work slice may change.
    live.fill(0x5A);
    saved.fill(0xA5);
    Rollback_RestoreModelMotions(live.data(), saved.data());
    for (size_t byte = 0; byte < live.size(); ++byte) {
        bool restored = false;
        if (byte >= 0x560 && byte < 0x1160) {
            const size_t field = (byte - 0x560) % 0x180;
            restored = (field >= 8 && field < 0x160) ||
                       (field >= 0x168 && field < 0x178);
        }
        assert(live[byte] == (restored ? 0xA5 : 0x5A));
    }
    // A second restore must also rewind the descriptor/count pair, not just time.
    for (size_t base = 0x560; base < 0x1160; base += 0x180) {
        Store(saved, base + 0x168, 3);
        Store(saved, base + 0x170, 0x12345678);
        Store(saved, base + 0xE0, 0x23456780);
        Store(saved, base + 0xF0, 0x34567890);
        Store(saved, base + 0x100, 0x456789A0);
        Store(saved, base + 0x110, 0x56789AB0);
    }
    Rollback_RestoreModelMotions(live.data(), saved.data());
    for (size_t base = 0x560; base < 0x1160; base += 0x180) {
        assert(std::memcmp(live.data() + base + 0x168, saved.data() + base + 0x168, 0x10) == 0);
        assert(std::memcmp(live.data() + base + 0xE0, saved.data() + base + 0xE0, 0x40) == 0);
        ExpectBytes(live, base, 8, 0x5A);
        ExpectBytes(live, base + 0x160, 8, 0x5A);
        ExpectBytes(live, base + 0x178, 8, 0x5A);
    }
    std::puts("character restore ownership, bounds, scalar, phase and Motion binding tests passed");
}
