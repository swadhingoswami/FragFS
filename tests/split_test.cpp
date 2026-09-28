#include "test_framework.h"
#include "test_helpers.h"

#include <fragfs/error.h>
#include <fragfs/logical_file.h>
#include <fragfs/metadata_store.h>
#include <fragfs/operations.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using fragfs::ErrorCode;
using fragfs::LogicalFile;
using fragfs::Metadata;
using fragfs::SplitResult;
using fragfs::testutil::TempDir;
using fragfs::testutil::writeTextFile;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("split writes consecutive chunks with zero-padded names") {
    const TempDir dir;
    writeTextFile(dir.path / "big.bin", "0123456789");

    const SplitResult result =
        fragfs::splitFile(dir.path / "big.bin", dir.path / "big.bin.part", 4);

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(result.parts.size(), std::size_t{3});
    FRAGFS_CHECK_EQ(result.parts[0].filename().string(), std::string("big.bin.part000"));
    FRAGFS_CHECK_EQ(result.parts[2].filename().string(), std::string("big.bin.part002"));
    FRAGFS_CHECK_EQ(readFile(result.parts[0]), std::string("0123"));
    FRAGFS_CHECK_EQ(readFile(result.parts[1]), std::string("4567"));
    FRAGFS_CHECK_EQ(readFile(result.parts[2]), std::string("89")); // shorter tail
}

TEST_CASE("split rejects an empty input") {
    const TempDir dir;
    writeTextFile(dir.path / "empty", "");

    const SplitResult result =
        fragfs::splitFile(dir.path / "empty", dir.path / "empty.part", 4);

    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::empty_physical_file));
}

TEST_CASE("split rejects a zero chunk size") {
    const TempDir dir;
    writeTextFile(dir.path / "big", "abcd");

    const SplitResult result =
        fragfs::splitFile(dir.path / "big", dir.path / "big.part", 0);

    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::invalid_range));
}

TEST_CASE("splitting then aggregating reconstructs the original") {
    const TempDir dir;
    const std::string original = "The quick brown fox jumps over the lazy dog.";
    writeTextFile(dir.path / "big.bin", original);

    const SplitResult split =
        fragfs::splitFile(dir.path / "big.bin", dir.path / "big.bin.part", 7);
    FRAGFS_CHECK(split.ok());

    // Map the parts back into a logical file, then read it whole.
    const std::filesystem::path metadataPath = dir.path / "restored.meta";
    const fragfs::CreateResult built = fragfs::buildMetadata(split.parts, metadataPath);
    FRAGFS_CHECK(built.ok());
    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, built.metadata));

    std::error_code error;
    auto file = LogicalFile::open(metadataPath, error);
    FRAGFS_CHECK(file.has_value());

    std::string buffer(original.size(), '\0');
    std::size_t bytesRead = 0;
    error = file->read(0, buffer.data(), buffer.size(), bytesRead);
    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(bytesRead, original.size());
    FRAGFS_CHECK_EQ(buffer, original);
}

FRAGFS_TEST_MAIN
