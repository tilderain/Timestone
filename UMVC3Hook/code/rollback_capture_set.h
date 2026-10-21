#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

// Capacity must cover all candidates admitted by the caller's traversal limits.
template <typename T, size_t Capacity>
class RollbackCaptureSet {
    std::array<T, Capacity> values;
    size_t size = 0;

public:
    size_t count(T value) const {
        return std::find(values.begin(), values.begin() + size, value) !=
            values.begin() + size;
    }

    std::pair<size_t, bool> insert(T value) {
        auto found = std::find(values.begin(), values.begin() + size, value);
        if (found != values.begin() + size)
            return {static_cast<size_t>(found - values.begin()), false};
        // Bounds checking fails loudly if a future traversal exceeds its budget.
        values.at(size) = value;
        return {size++, true};
    }
};
