#pragma once

#include <fragfs/metadata.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <vector>

namespace fragfs {

// Result of building metadata from a list of physical files.
struct CreateResult {
    std::error_code error;
    Metadata metadata;

    bool ok() const { return !error; }
};

// Builds metadata that maps each file in full, in the order given. Paths are
// stored relative to the metadata file's directory where possible. Nothing is
// written to disk; use writeMetadataFile() for that.
//
// Fails if a file is missing, is not a regular file, is empty, or if the
// cumulative logical size would overflow uint64_t.
CreateResult buildMetadata(const std::vector<std::filesystem::path>& files,
                           const std::filesystem::path& metadataPath);

// Appends a whole physical file to the end of the logical file. Physical data
// is never copied: only metadata changes.
std::error_code appendFile(Metadata& metadata,
                           const std::filesystem::path& file,
                           const std::filesystem::path& metadataPath);

// Appends the physical range [physicalOffset, physicalOffset + length) of
// `file`. The range must lie within the file.
std::error_code addRange(Metadata& metadata,
                         const std::filesystem::path& file,
                         uint64_t physicalOffset,
                         uint64_t length,
                         const std::filesystem::path& metadataPath);

// Removes the fragment at `fragmentIndex` and shifts the logical start of all
// later fragments down by its length. Physical data is untouched.
std::error_code removeFragment(Metadata& metadata, std::size_t fragmentIndex);

// Result of splitting a file into chunks.
struct SplitResult {
    std::error_code error;
    std::vector<std::filesystem::path> parts;

    bool ok() const { return !error; }
};

// Splits `input` into consecutive chunks of `chunkSize` bytes, writing them to
// "<outputPrefix><index>" (zero-padded, at least 3 digits). The final chunk may
// be shorter. This physically copies data, because the chunks are new files;
// the zero-copy step is aggregating them back, not producing them.
SplitResult splitFile(const std::filesystem::path& input,
                      const std::filesystem::path& outputPrefix,
                      uint64_t chunkSize);

} // namespace fragfs
