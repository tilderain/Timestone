#pragma once

#include <cstdint>

namespace EffectPending {

// Compact the native queue in place. Both header fields are references because
// a retirement callback may append entries and reallocate the native array.
// Retained entries stay owned by the queue for later ordinary native cleanup.
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
