#pragma once

#include <cstdint>

namespace EffectPending {

// Retire rejected entries and compact non-null kept entries in place.
// Read the live pointer/count so entries appended by retirement callbacks are processed too.
template <typename Keep, typename Retire>
void FlushRetaining(void**& entries, uint32_t& count, Keep keep, Retire retire)
{
    uint32_t retained = 0;
    for (uint32_t index = 0; index < count; ++index) {
        void* entry = entries[index];
        if (!entry) continue;
        if (keep(entry)) entries[retained++] = entry;
        else retire(entry);
    }
    for (uint32_t index = retained; index < count; ++index)
        entries[index] = nullptr;
    count = retained;
}

} // namespace EffectPending
