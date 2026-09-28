#include "test_framework.h"

#include <fragfs/error.h>
#include <fragfs/logical_file.h>
#include <fragfs/metadata.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using fragfs::ErrorCode;
using fragfs::Fragment;
using fragfs::LogicalFile;
using fragfs::Metadata;

std::filesystem::path makeTempDir() {
    static int counter = 0;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("fragfs_lf_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

struct TempDir {
    std::filesystem::path path;
    TempDir() : path(makeTempDir()) {}
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void writeMetadataFile(const std::filesystem::path& path, const Metadata& metadata) {
    const std::vector<std::byte> bytes = fragfs::serializeMetadata(metadata);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

Fragment frag(uint64_t logicalStart,
              uint64_t physicalStart,
              uint64_t length,
              std::string path) {
    Fragment fragment;
    fragment.logicalStart = logicalStart;
    fragment.physicalStart = physicalStart;
    fragment.length = length;
    fragment.path = std::move(path);
    return fragment;
}

// A 12-byte logical file: "AAAA" + "BBBBBB" + "CC".
Metadata concatenationFixture() {
    Metadata metadata;
    metadata.logicalSize = 12;
    metadata.fragments = {frag(0, 0, 4, "A.dat"), frag(4, 0, 6, "B.dat"),
                          frag(10, 0, 2, "C.dat")};
    return metadata;
}

std::string readRange(LogicalFile& file, uint64_t offset, std::size_t size) {
    std::string buffer(size, '\0');
    std::size_t bytesRead = 0;
    const std::error_code error =
        file.read(offset, buffer.data(), size, bytesRead);
    FRAGFS_CHECK(!error);
    buffer.resize(bytesRead);
    return buffer;
}

} // namespace

TEST_CASE("a logical read across fragments returns concatenated content") {
    const TempDir dir;
    writeTextFile(dir.path / "A.dat", "AAAA");
    writeTextFile(dir.path / "B.dat", "BBBBBB");
    writeTextFile(dir.path / "C.dat", "CC");
    writeMetadataFile(dir.path / "combined.ff.meta", concatenationFixture());

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    FRAGFS_CHECK_EQ(readRange(*file, 0, 12), std::string("AAAABBBBBBCC"));
}

TEST_CASE("a read that crosses a boundary returns the right bytes") {
    const TempDir dir;
    writeTextFile(dir.path / "A.dat", "AAAA");
    writeTextFile(dir.path / "B.dat", "BBBBBB");
    writeTextFile(dir.path / "C.dat", "CC");
    writeMetadataFile(dir.path / "combined.ff.meta", concatenationFixture());

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    FRAGFS_CHECK_EQ(readRange(*file, 2, 5), std::string("AABBB"));
}

TEST_CASE("a partial physical mapping reads from the right offset") {
    const TempDir dir;
    writeTextFile(dir.path / "A.dat", "0123456789");

    Metadata metadata;
    metadata.logicalSize = 5;
    metadata.fragments = {frag(0, 2, 5, "A.dat")}; // maps "23456"
    writeMetadataFile(dir.path / "combined.ff.meta", metadata);

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    FRAGFS_CHECK_EQ(readRange(*file, 0, 5), std::string("23456"));
}

TEST_CASE("reading at end of file returns zero bytes without error") {
    const TempDir dir;
    writeTextFile(dir.path / "A.dat", "AAAA");
    writeTextFile(dir.path / "B.dat", "BBBBBB");
    writeTextFile(dir.path / "C.dat", "CC");
    writeMetadataFile(dir.path / "combined.ff.meta", concatenationFixture());

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    std::string buffer(4, '\0');
    std::size_t bytesRead = 99;
    error = file->read(12, buffer.data(), buffer.size(), bytesRead);

    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(bytesRead, std::size_t{0});
}

TEST_CASE("a read past end of file is clamped to the logical size") {
    const TempDir dir;
    writeTextFile(dir.path / "A.dat", "AAAA");
    writeTextFile(dir.path / "B.dat", "BBBBBB");
    writeTextFile(dir.path / "C.dat", "CC");
    writeMetadataFile(dir.path / "combined.ff.meta", concatenationFixture());

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    FRAGFS_CHECK_EQ(readRange(*file, 10, 100), std::string("CC"));
}

TEST_CASE("a missing physical file is reported on read, not on open") {
    const TempDir dir;

    Metadata metadata;
    metadata.logicalSize = 4;
    metadata.fragments = {frag(0, 0, 4, "missing.dat")};
    writeMetadataFile(dir.path / "combined.ff.meta", metadata);

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    std::string buffer(4, '\0');
    std::size_t bytesRead = 0;
    error = file->read(0, buffer.data(), buffer.size(), bytesRead);

    FRAGFS_CHECK_EQ(error, fragfs::make_error_code(ErrorCode::missing_physical_file));
}

TEST_CASE("a physical file shorter than the mapping is an error") {
    const TempDir dir;
    writeTextFile(dir.path / "A.dat", "AAAA");

    Metadata metadata;
    metadata.logicalSize = 100;
    metadata.fragments = {frag(0, 0, 100, "A.dat")}; // claims 100 bytes
    writeMetadataFile(dir.path / "combined.ff.meta", metadata);

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    std::string buffer(100, '\0');
    std::size_t bytesRead = 0;
    error = file->read(0, buffer.data(), buffer.size(), bytesRead);

    FRAGFS_CHECK_EQ(error,
                    fragfs::make_error_code(ErrorCode::physical_range_out_of_bounds));
}

TEST_CASE("relative fragment paths resolve against the metadata directory") {
    const TempDir dir;
    writeTextFile(dir.path / "A.dat", "hello");
    writeTextFile(dir.path / "B.dat", "world");

    Metadata metadata;
    metadata.logicalSize = 10;
    metadata.fragments = {frag(0, 0, 5, "A.dat"), frag(5, 0, 5, "B.dat")};
    writeMetadataFile(dir.path / "combined.ff.meta", metadata);

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());

    // The test process runs from the build directory, so this only succeeds if
    // the relative paths are resolved against the metadata file's directory.
    FRAGFS_CHECK_EQ(readRange(*file, 0, 10), std::string("helloworld"));
}

TEST_CASE("absolute fragment paths are used as-is") {
    const TempDir dir;
    const std::filesystem::path dataDir = dir.path / "data";
    std::filesystem::create_directories(dataDir);
    writeTextFile(dataDir / "A.dat", "absolute");

    Metadata metadata;
    metadata.logicalSize = 8;
    metadata.fragments = {frag(0, 0, 8, (dataDir / "A.dat").string())};
    writeMetadataFile(dir.path / "meta", metadata);

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "meta", error);
    FRAGFS_CHECK(file.has_value());

    FRAGFS_CHECK_EQ(readRange(*file, 0, 8), std::string("absolute"));
}

TEST_CASE("an empty logical file reads zero bytes") {
    const TempDir dir;
    writeMetadataFile(dir.path / "combined.ff.meta", Metadata{});

    std::error_code error;
    auto file = LogicalFile::open(dir.path / "combined.ff.meta", error);
    FRAGFS_CHECK(file.has_value());
    FRAGFS_CHECK_EQ(file->logicalSize(), uint64_t{0});

    std::string buffer(4, '\0');
    std::size_t bytesRead = 99;
    error = file->read(0, buffer.data(), buffer.size(), bytesRead);

    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(bytesRead, std::size_t{0});
}

TEST_CASE("the metadata sidecar path is derived from the logical path") {
    FRAGFS_CHECK_EQ(fragfs::metadataPathFor("combined.ff").string(),
                    std::string("combined.ff.meta"));
}

FRAGFS_TEST_MAIN
