#include "test_framework.h"
#include "test_helpers.h"

#include <fragfs/assemble.h>
#include <fragfs/crc32.h>
#include <fragfs/error.h>
#include <fragfs/metadata_store.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace {

using fragfs::ErrorCode;
using fragfs::Fragment;
using fragfs::Metadata;
using fragfs::testutil::TempDir;
using fragfs::testutil::writeTextFile;

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

Fragment frag(uint64_t logicalStart, uint64_t length, std::string path,
              uint32_t checksum) {
    Fragment fragment;
    fragment.logicalStart = logicalStart;
    fragment.physicalStart = 0;
    fragment.length = length;
    fragment.path = std::move(path);
    fragment.checksum = checksum;
    return fragment;
}

// A 12-byte original: "AAAA" (chunk a) + "BBBBBBBB" (chunk b), with CRCs.
Metadata manifestFixture() {
    Metadata metadata;
    metadata.originalName = "big.dat";
    metadata.logicalSize = 12;
    metadata.fragments = {frag(0, 4, "a", fragfs::crc32("AAAA", 4)),
                          frag(4, 8, "b", fragfs::crc32("BBBBBBBB", 8))};
    return metadata;
}

} // namespace

TEST_CASE("assemble rebuilds the original from the manifest") {
    const TempDir dir;
    writeTextFile(dir.path / "a", "AAAA");
    writeTextFile(dir.path / "b", "BBBBBBBB");

    const fragfs::AssembleResult result = fragfs::assemble(
        manifestFixture(), dir.path, dir.path / "big.dat", {});

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(readFile(dir.path / "big.dat"), std::string("AAAABBBBBBBB"));
}

TEST_CASE("assemble reports a missing chunk by name") {
    const TempDir dir;
    writeTextFile(dir.path / "a", "AAAA"); // b is missing

    const fragfs::AssembleResult result = fragfs::assemble(
        manifestFixture(), dir.path, dir.path / "big.dat", {});

    FRAGFS_CHECK(!result.ok());
    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::missing_physical_file));
    FRAGFS_CHECK_EQ(result.detail, std::string("b"));
}

TEST_CASE("assemble detects a corrupted chunk when verifying") {
    const TempDir dir;
    writeTextFile(dir.path / "a", "AAAA");
    writeTextFile(dir.path / "b", "BBBBBBBB");
    // Flip one byte of chunk b.
    {
        std::fstream out(dir.path / "b", std::ios::in | std::ios::out | std::ios::binary);
        out.seekp(0);
        out.put('X');
    }

    fragfs::AssembleOptions options;
    options.verify = true;
    const fragfs::AssembleResult result =
        fragfs::assemble(manifestFixture(), dir.path, dir.path / "big.dat", options);

    FRAGFS_CHECK(!result.ok());
    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::checksum_mismatch));
    FRAGFS_CHECK_EQ(result.detail, std::string("b"));
}

TEST_CASE("assemble with consume removes the chunks after success") {
    const TempDir dir;
    writeTextFile(dir.path / "a", "AAAA");
    writeTextFile(dir.path / "b", "BBBBBBBB");

    fragfs::AssembleOptions options;
    options.consume = true;
    const fragfs::AssembleResult result =
        fragfs::assemble(manifestFixture(), dir.path, dir.path / "big.dat", options);

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK(!std::filesystem::exists(dir.path / "a"));
    FRAGFS_CHECK(!std::filesystem::exists(dir.path / "b"));
    FRAGFS_CHECK_EQ(readFile(dir.path / "big.dat"), std::string("AAAABBBBBBBB"));
}

FRAGFS_TEST_MAIN
