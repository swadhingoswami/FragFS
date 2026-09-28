#pragma once

#include <fragfs/metadata.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace fragfs {

struct AssembleOptions {
    // Delete each chunk after the reassembled file is durable.
    bool consume = false;
    // Verify every chunk's CRC-32 even when reflink was used (which does not
    // otherwise read the data).
    bool verify = false;
};

struct AssembleResult {
    std::error_code error;
    std::string detail;   // names the offending chunk, when relevant
    bool usedReflink = false;
    bool checksumsVerified = false;

    bool ok() const { return !error; }
};

// Reassembles the original file described by `metadata` into `outputPath`.
//
// For each fragment it clones the chunk's range with a copy-on-write reflink
// when the platform/filesystem supports it; otherwise it copies. The result is
// written to a temporary file and atomically renamed into place, so a partial
// reassembly never appears as the final file. With `consume`, the chunks are
// unlinked only after the result is durable.
AssembleResult assemble(const Metadata& metadata,
                        const std::filesystem::path& baseDirectory,
                        const std::filesystem::path& outputPath,
                        const AssembleOptions& options);

} // namespace fragfs
