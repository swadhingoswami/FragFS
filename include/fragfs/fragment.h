#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fragfs {

// A single contiguous mapping from a logical byte range to a physical byte
// range within one file.
//
// Ranges are half-open: [start, start + length). The half-open convention
// makes boundaries unambiguous: a byte belongs to exactly one fragment, and
// the end offset of one fragment is the start offset of the next.
//
// This is a plain aggregate on purpose: it is a value type that is copied,
// serialized, and compared freely. Validation lives in explicit methods
// (and, later, in the metadata validator) rather than in the type itself, so
// that partially-built fragments can exist transiently while being parsed.
struct Fragment {
    // Where this fragment begins in the logical file.
    uint64_t logicalStart = 0;

    // Where the mapped data begins in the physical file.
    uint64_t physicalStart = 0;

    // How many bytes are mapped.
    uint64_t length = 0;

    // Path to the physical file. May be relative or absolute; path resolution
    // policy is decided by the metadata layer, not here.
    std::string path;

    // logicalStart + length, or std::nullopt if that would overflow uint64_t.
    std::optional<uint64_t> logicalEnd() const;

    // physicalStart + length, or std::nullopt if that would overflow uint64_t.
    std::optional<uint64_t> physicalEnd() const;

    // True when both ranges are representable without overflow.
    bool hasValidRanges() const;

    // True when offset lies in [logicalStart, logicalEnd()).
    // Always false when the logical range is invalid or empty.
    bool containsLogicalOffset(uint64_t offset) const;
};

} // namespace fragfs
