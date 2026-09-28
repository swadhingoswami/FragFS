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

namespace {

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

TEST_CASE("append adds a fragment and grows the logical size") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");
    writeTextFile(dir.path / "b.dat", "BB");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    Metadata metadata;
    FRAGFS_CHECK(!fragfs::appendFile(metadata, dir.path / "a.dat", metadataPath));
    FRAGFS_CHECK_EQ(metadata.logicalSize, uint64_t{4});

    FRAGFS_CHECK(!fragfs::appendFile(metadata, dir.path / "b.dat", metadataPath));
    FRAGFS_CHECK_EQ(metadata.logicalSize, uint64_t{6});
    FRAGFS_CHECK_EQ(metadata.fragments.size(), std::size_t{2});
    FRAGFS_CHECK_EQ(metadata.fragments[1].logicalStart, uint64_t{4});
}

TEST_CASE("append rejects a missing file") {
    const TempDir dir;
    Metadata metadata;

    FRAGFS_CHECK_EQ(fragfs::appendFile(metadata, dir.path / "nope.dat", dir.path / "m.meta"),
                    fragfs::make_error_code(ErrorCode::missing_physical_file));
}

TEST_CASE("append rejects an empty file") {
    const TempDir dir;
    writeTextFile(dir.path / "empty.dat", "");
    Metadata metadata;

    FRAGFS_CHECK_EQ(fragfs::appendFile(metadata, dir.path / "empty.dat", dir.path / "m.meta"),
                    fragfs::make_error_code(ErrorCode::empty_physical_file));
}

TEST_CASE("addRange maps a partial physical range") {
    const TempDir dir;
    writeTextFile(dir.path / "video.dat", "0123456789");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    Metadata metadata;
    FRAGFS_CHECK(!fragfs::addRange(metadata, dir.path / "video.dat", 2, 5, metadataPath));

    FRAGFS_CHECK_EQ(metadata.logicalSize, uint64_t{5});
    FRAGFS_CHECK_EQ(metadata.fragments[0].physicalStart, uint64_t{2});
    FRAGFS_CHECK_EQ(metadata.fragments[0].length, uint64_t{5});

    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, metadata));
    std::error_code error;
    auto file = LogicalFile::open(metadataPath, error);
    FRAGFS_CHECK(file.has_value());
    FRAGFS_CHECK_EQ(readRange(*file, 0, 5), std::string("23456"));
}

TEST_CASE("addRange rejects a range past the end of the file") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "0123456789");
    Metadata metadata;

    FRAGFS_CHECK_EQ(fragfs::addRange(metadata, dir.path / "a.dat", 8, 5, dir.path / "m.meta"),
                    fragfs::make_error_code(ErrorCode::physical_range_out_of_bounds));
}

TEST_CASE("addRange rejects a zero length") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "0123456789");
    Metadata metadata;

    FRAGFS_CHECK_EQ(fragfs::addRange(metadata, dir.path / "a.dat", 0, 0, dir.path / "m.meta"),
                    fragfs::make_error_code(ErrorCode::invalid_range));
}

TEST_CASE("remove shifts later fragments down and preserves their data") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");
    writeTextFile(dir.path / "b.dat", "BBBBBB");
    writeTextFile(dir.path / "c.dat", "CC");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    Metadata metadata;
    FRAGFS_CHECK(!fragfs::appendFile(metadata, dir.path / "a.dat", metadataPath));
    FRAGFS_CHECK(!fragfs::appendFile(metadata, dir.path / "b.dat", metadataPath));
    FRAGFS_CHECK(!fragfs::appendFile(metadata, dir.path / "c.dat", metadataPath));

    FRAGFS_CHECK(!fragfs::removeFragment(metadata, 1)); // drop "BBBBBB"

    FRAGFS_CHECK_EQ(metadata.logicalSize, uint64_t{6});
    FRAGFS_CHECK_EQ(metadata.fragments.size(), std::size_t{2});
    FRAGFS_CHECK_EQ(metadata.fragments[1].logicalStart, uint64_t{4});
    FRAGFS_CHECK_EQ(metadata.fragments[1].path, std::string("c.dat"));

    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, metadata));
    std::error_code error;
    auto file = LogicalFile::open(metadataPath, error);
    FRAGFS_CHECK(file.has_value());
    FRAGFS_CHECK_EQ(readRange(*file, 0, 6), std::string("AAAACC"));
}

TEST_CASE("remove rejects an out-of-range index") {
    Metadata metadata;
    FRAGFS_CHECK_EQ(fragfs::removeFragment(metadata, 0),
                    fragfs::make_error_code(ErrorCode::fragment_index_out_of_range));
}

TEST_CASE("removing every fragment yields an empty logical file") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");

    Metadata metadata;
    FRAGFS_CHECK(!fragfs::appendFile(metadata, dir.path / "a.dat", dir.path / "m.meta"));
    FRAGFS_CHECK(!fragfs::removeFragment(metadata, 0));

    FRAGFS_CHECK_EQ(metadata.logicalSize, uint64_t{0});
    FRAGFS_CHECK_EQ(metadata.fragments.size(), std::size_t{0});
}

FRAGFS_TEST_MAIN
