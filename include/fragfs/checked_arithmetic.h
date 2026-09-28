#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace fragfs {

// Overflow-checked arithmetic helpers.
//
// FragFS treats metadata as untrusted input: a corrupted or hostile file can
// contain offsets and lengths that overflow when combined. Silently wrapping
// would turn a validation failure into an out-of-bounds read, so every
// addition on offsets/lengths goes through a helper that reports overflow
// instead of producing a bogus value.

// Returns a + b, or std::nullopt if the true sum does not fit in uint64_t.
inline std::optional<uint64_t> checkedAdd(uint64_t a, uint64_t b) {
    if (a > std::numeric_limits<uint64_t>::max() - b) {
        return std::nullopt;
    }
    return a + b;
}

} // namespace fragfs
