#pragma once

#include <fragfs/metadata.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fragfs {

// Per-fragment result of verification.
struct FragmentVerification {
    std::size_t fragmentIndex = 0;
    bool fileExists = false;
    bool rangeInBounds = false;
    bool changed = false; // identity (size/mtime/device/inode) differs
    uint64_t physicalFileSize = 0;
    std::error_code error;
};

// Full verification report.
//
// `valid()` means the mapping is usable: the metadata is structurally sound and
// every fragment's physical file exists with a range inside it. `unchanged()`
// additionally requires the recorded identity to match, which is stricter than
// most workflows need — copying or moving a chunk legitimately changes its
// mtime/device/inode without changing its bytes.
struct VerifyReport {
    std::error_code metadataError;
    std::vector<FragmentVerification> fragments;
    std::size_t missingFiles = 0;
    std::size_t invalidRanges = 0;
    std::size_t changedFiles = 0;

    bool valid() const {
        return !metadataError && missingFiles == 0 && invalidRanges == 0;
    }

    bool unchanged() const {
        return valid() && changedFiles == 0;
    }
};

// Validates metadata structure and checks every fragment against its physical
// file (existence, regular-file-ness, and that the mapped range fits).
VerifyReport verifyMetadata(const Metadata& metadata,
                            const std::filesystem::path& baseDirectory);

} // namespace fragfs
