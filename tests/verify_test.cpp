#include "test_framework.h"
#include "test_helpers.h"

#include <fragfs/error.h>
#include <fragfs/metadata.h>
#include <fragfs/operations.h>
#include <fragfs/verify.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
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

TEST_CASE("verification passes when recorded identity matches") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    const fragfs::CreateResult built =
        fragfs::buildMetadata({dir.path / "a.dat"}, metadataPath);
    FRAGFS_CHECK(built.ok());

    const fragfs::VerifyReport report =
        fragfs::verifyMetadata(built.metadata, dir.path);

    FRAGFS_CHECK(report.valid());
    FRAGFS_CHECK_EQ(report.changedFiles, std::size_t{0});
}

TEST_CASE("verification detects a physical file modified after creation") {
    const TempDir dir;
    writeTextFile(dir.path / "a.dat", "AAAA");

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    const fragfs::CreateResult built =
        fragfs::buildMetadata({dir.path / "a.dat"}, metadataPath);
    FRAGFS_CHECK(built.ok());

    // Move the file's mtime forward; the recorded identity no longer matches.
    std::error_code error;
    const auto future =
        std::filesystem::file_time_type::clock::now() + std::chrono::hours(24);
    std::filesystem::last_write_time(dir.path / "a.dat", future, error);
    FRAGFS_CHECK(!error);

    const fragfs::VerifyReport report =
        fragfs::verifyMetadata(built.metadata, dir.path);

    // The file is present and in range, so the mapping is still usable...
    FRAGFS_CHECK(report.valid());
    // ...but the recorded identity no longer matches.
    FRAGFS_CHECK(!report.unchanged());
    FRAGFS_CHECK_EQ(report.changedFiles, std::size_t{1});
    FRAGFS_CHECK(report.fragments[0].changed);
}

FRAGFS_TEST_MAIN
