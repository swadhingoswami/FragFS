#pragma once

#include <fragfs/error.h>
#include <fragfs/fragment.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

namespace fragfs {

// 8-byte ASCII magic at the start of a serialized metadata file. Stored as
// bytes (not an integer) so its meaning does not depend on host endianness.
inline constexpr std::array<char, 8> kMetadataMagic = {
    'F', 'R', 'A', 'G', 'F', 'S', 'M', '1'};

// On-disk layout version. Bumped whenever the byte layout changes; a parser
// rejects versions it does not understand rather than guessing.
inline constexpr uint32_t kMetadataFormatVersion = 1;

// Upper bound on fragment count, enforced when parsing untrusted metadata so a
// corrupt count cannot trigger a huge allocation.
inline constexpr uint64_t kMaxFragments = 1000000;

// Outcome of a validation pass. `error` is empty when the metadata is valid;
// otherwise `fragmentIndex` identifies the offending fragment and `detail`
// carries a human-readable explanation.
struct ValidationResult {
    std::error_code error;
    std::size_t fragmentIndex = 0;
    std::string detail;

    bool ok() const { return !error; }
};

// In-memory description of a logical file: its logical size plus the ordered
// list of physical mappings.
//
// Magic and format version are intentionally absent: they describe the on-disk
// encoding, not the in-memory value. They live as constants above and are
// written by the serializer.
struct Metadata {
    uint64_t logicalSize = 0;
    std::vector<Fragment> fragments;

    // Checks the structural invariants:
    //   - every fragment has a non-overflowing range and a non-empty path
    //   - every fragment has non-zero length
    //   - fragments are ordered and tile [0, logicalSize) exactly, with no
    //     gaps and no overlaps
    //   - logicalSize equals the end of the last fragment
    ValidationResult validate() const;
};

} // namespace fragfs
