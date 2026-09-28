#include "test_framework.h"
#include "test_helpers.h"

#include <fragfs/error.h>
#include <fragfs/metadata.h>
#include <fragfs/verify.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

namespace {

using fragfs::ErrorCode;
using fragfs::Fragment;
using fragfs::Metadata;
using fragfs::testutil::TempDir;
using fragfs::testutil::writeTextFile;

Fragment frag(uint64_t logicalStart, uint64_t physicalStart, uint64_t length,
              std::string path) {
    Fragment fragment;
    fragment.logicalStart = logicalStart;
    fragment.physicalStart = physicalStart;
    fragment.length = length;
    fragment.path = std::move(path);
    return fragment;
}

} // namespace

TEST_CASE("valid metadata with present files verifies clean") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");
    writeTextFile(dir.path / "b.dat", "BBBBBB");

    Metadata metadata;
    metadata.logicalSize = 10;
    metadata.fragments = {frag(0, 0, 4, "a.dat"), frag(4, 0, 6, "b.dat")};

    const fragfs::VerifyReport report = fragfs::verifyMetadata(metadata, dir.path);

    FRAGFS_CHECK(report.valid());
    FRAGFS_CHECK_EQ(report.missingFiles, std::size_t{0});
    FRAGFS_CHECK_EQ(report.invalidRanges, std::size_t{0});
    FRAGFS_CHECK_EQ(report.fragments.size(), std::size_t{2});
    FRAGFS_CHECK(report.fragments[0].fileExists);
    FRAGFS_CHECK(report.fragments[0].rangeInBounds);
}

TEST_CASE("verification reports a missing physical file") {
    const TempDir dir;

    Metadata metadata;
    metadata.logicalSize = 4;
    metadata.fragments = {frag(0, 0, 4, "missing.dat")};

    const fragfs::VerifyReport report = fragfs::verifyMetadata(metadata, dir.path);

    FRAGFS_CHECK(!report.valid());
    FRAGFS_CHECK_EQ(report.missingFiles, std::size_t{1});
    FRAGFS_CHECK(!report.fragments[0].fileExists);
}

TEST_CASE("verification reports a range outside the physical file") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");

    Metadata metadata;
    metadata.logicalSize = 100;
    metadata.fragments = {frag(0, 0, 100, "a.dat")}; // claims 100 bytes, file has 4

    const fragfs::VerifyReport report = fragfs::verifyMetadata(metadata, dir.path);

    FRAGFS_CHECK(!report.valid());
    FRAGFS_CHECK_EQ(report.invalidRanges, std::size_t{1});
    FRAGFS_CHECK_EQ(report.fragments[0].physicalFileSize, uint64_t{4});
    FRAGFS_CHECK_EQ(report.fragments[0].error,
                    fragfs::make_error_code(ErrorCode::physical_range_out_of_bounds));
}

TEST_CASE("verification reports structurally invalid metadata first") {
    Metadata metadata;
    metadata.logicalSize = 150;
    metadata.fragments = {frag(0, 0, 100, "a.dat"), frag(50, 0, 100, "b.dat")};

    const fragfs::VerifyReport report = fragfs::verifyMetadata(metadata, ".");

    FRAGFS_CHECK(!report.valid());
    FRAGFS_CHECK_EQ(report.metadataError,
                    fragfs::make_error_code(ErrorCode::overlapping_fragments));
    FRAGFS_CHECK_EQ(report.fragments.size(), std::size_t{0});
}

FRAGFS_TEST_MAIN
