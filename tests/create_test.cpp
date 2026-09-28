#include "test_framework.h"
#include "test_helpers.h"

#include <fragfs/error.h>
#include <fragfs/logical_file.h>
#include <fragfs/metadata_store.h>
#include <fragfs/operations.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using fragfs::CreateResult;
using fragfs::ErrorCode;
using fragfs::LogicalFile;
using fragfs::Metadata;
using fragfs::testutil::TempDir;
using fragfs::testutil::writeTextFile;

std::string readRange(LogicalFile& file, uint64_t offset, std::size_t size) {
    std::string buffer(size, '\0');
    std::size_t bytesRead = 0;
    const std::error_code error = file.read(offset, buffer.data(), size, bytesRead);
    FRAGFS_CHECK(!error);
    buffer.resize(bytesRead);
    return buffer;
}

} // namespace

TEST_CASE("buildMetadata maps each file in full and in order") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");
    writeTextFile(dir.path / "b.dat", "BBBBBB");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    const CreateResult result =
        fragfs::buildMetadata({dir.path / "a.dat", dir.path / "b.dat"}, metadataPath);

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(result.metadata.logicalSize, uint64_t{10});
    FRAGFS_CHECK_EQ(result.metadata.fragments.size(), std::size_t{2});
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].logicalStart, uint64_t{0});
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].length, uint64_t{4});
    FRAGFS_CHECK_EQ(result.metadata.fragments[1].logicalStart, uint64_t{4});
    FRAGFS_CHECK_EQ(result.metadata.fragments[1].length, uint64_t{6});
    FRAGFS_CHECK_EQ(result.metadata.fragments[1].physicalStart, uint64_t{0});
}

TEST_CASE("buildMetadata stores paths relative to the metadata directory") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");

    const CreateResult result =
        fragfs::buildMetadata({dir.path / "a.dat"}, dir.path / "combined.ff.meta");

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].path, std::string("a.dat"));
}

TEST_CASE("buildMetadata rejects a missing file") {
    const TempDir dir;

    const CreateResult result =
        fragfs::buildMetadata({dir.path / "nope.dat"}, dir.path / "m.meta");

    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::missing_physical_file));
}

TEST_CASE("buildMetadata rejects a directory") {
    const TempDir dir;
    std::filesystem::create_directories(dir.path / "sub");

    const CreateResult result =
        fragfs::buildMetadata({dir.path / "sub"}, dir.path / "m.meta");

    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::not_a_regular_file));
}

TEST_CASE("buildMetadata rejects an empty file") {
    const TempDir dir;
    writeTextFile(dir.path / "empty.dat", "");

    const CreateResult result =
        fragfs::buildMetadata({dir.path / "empty.dat"}, dir.path / "m.meta");

    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::empty_physical_file));
}

TEST_CASE("metadata written to disk reads back identically") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");
    writeTextFile(dir.path / "b.dat", "BBBBBB");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    const CreateResult built =
        fragfs::buildMetadata({dir.path / "a.dat", dir.path / "b.dat"}, metadataPath);
    FRAGFS_CHECK(built.ok());
    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, built.metadata));

    Metadata loaded;
    FRAGFS_CHECK(!fragfs::readMetadataFile(metadataPath, loaded));
    FRAGFS_CHECK_EQ(loaded.logicalSize, built.metadata.logicalSize);
    FRAGFS_CHECK_EQ(loaded.fragments.size(), built.metadata.fragments.size());
    FRAGFS_CHECK_EQ(loaded.fragments[0].path, built.metadata.fragments[0].path);
}

TEST_CASE("a crash-safe write leaves no temporary file behind") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    const CreateResult built = fragfs::buildMetadata({dir.path / "a.dat"}, metadataPath);
    FRAGFS_CHECK(built.ok());
    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, built.metadata));

    FRAGFS_CHECK(std::filesystem::exists(metadataPath));
    FRAGFS_CHECK(!std::filesystem::exists(metadataPath.string() + ".tmp"));
}

TEST_CASE("a created logical file can be read end to end") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");
    writeTextFile(dir.path / "b.dat", "BBBBBB");
    writeTextFile(dir.path / "c.dat", "CC");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    const CreateResult built = fragfs::buildMetadata(
        {dir.path / "a.dat", dir.path / "b.dat", dir.path / "c.dat"}, metadataPath);
    FRAGFS_CHECK(built.ok());
    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, built.metadata));

    std::error_code error;
    auto file = LogicalFile::open(metadataPath, error);
    FRAGFS_CHECK(file.has_value());
    FRAGFS_CHECK_EQ(readRange(*file, 0, 12), std::string("AAAABBBBBBCC"));
}

FRAGFS_TEST_MAIN
