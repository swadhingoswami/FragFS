#pragma once

#include <system_error>

namespace fragfs {

// Error conditions reported by FragFS. These are exposed as std::error_code so
// that callers can compare against a stable value, print a message, and later
// compose with system_category() when a POSIX call fails.
enum class ErrorCode {
    success = 0,

    // Metadata parsing
    invalid_magic,
    unsupported_version,
    truncated_metadata,
    invalid_fragment_count,
    checksum_mismatch,

    // Structural validation
    invalid_range,
    empty_path,
    overlapping_fragments,
    gap_between_fragments,
    logical_size_mismatch,

    // Physical file verification
    missing_physical_file,
    physical_range_out_of_bounds,
    physical_file_changed,
    not_a_regular_file,
    empty_physical_file,
    fragment_index_out_of_range,

    // I/O
    io_error,
};

const std::error_category& errorCategory();
std::error_code make_error_code(ErrorCode code);

} // namespace fragfs

namespace std {
template <>
struct is_error_code_enum<fragfs::ErrorCode> : std::true_type {};
} // namespace std
