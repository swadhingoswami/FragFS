#include "test_framework.h"

#include <fragfs/error.h>
#include <fragfs/metadata.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using fragfs::DecodeResult;
using fragfs::ErrorCode;
using fragfs::Fragment;
using fragfs::Metadata;

constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

Fragment frag(uint64_t logicalStart,
              uint64_t physicalStart,
              uint64_t length,
              std::string path = "part.dat") {
    Fragment fragment;
    fragment.logicalStart = logicalStart;
    fragment.physicalStart = physicalStart;
    fragment.length = length;
    fragment.path = std::move(path);
    return fragment;
}

void writeU32(std::vector<std::byte>& buffer, std::size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        buffer[offset + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

void writeU64(std::vector<std::byte>& buffer, std::size_t offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        buffer[offset + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
    }
}

DecodeResult decode(const std::vector<std::byte>& buffer) {
    return fragfs::deserializeMetadata(buffer.data(), buffer.size());
}

} // namespace

TEST_CASE("metadata survives a serialize/deserialize round trip") {
    Metadata metadata;
    metadata.logicalSize = 350;
    metadata.fragments = {frag(0, 0, 100, "part1.dat"),
                          frag(100, 4096, 50, "part2.dat"),
                          frag(150, 0, 200, "part3.dat")};

    const DecodeResult result = decode(fragfs::serializeMetadata(metadata));

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(result.metadata.logicalSize, uint64_t{350});
    FRAGFS_CHECK_EQ(result.metadata.fragments.size(), std::size_t{3});
    FRAGFS_CHECK_EQ(result.metadata.fragments[1].logicalStart, uint64_t{100});
    FRAGFS_CHECK_EQ(result.metadata.fragments[1].physicalStart, uint64_t{4096});
    FRAGFS_CHECK_EQ(result.metadata.fragments[1].length, uint64_t{50});
    FRAGFS_CHECK_EQ(result.metadata.fragments[1].path, std::string("part2.dat"));
    FRAGFS_CHECK_EQ(result.metadata.fragments[2].physicalStart, uint64_t{0});
}

TEST_CASE("empty metadata survives a round trip") {
    Metadata metadata;

    const DecodeResult result = decode(fragfs::serializeMetadata(metadata));

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(result.metadata.logicalSize, uint64_t{0});
    FRAGFS_CHECK_EQ(result.metadata.fragments.size(), std::size_t{0});
}

TEST_CASE("paths with spaces and slashes survive a round trip") {
    Metadata metadata;
    metadata.logicalSize = 10;
    metadata.fragments = {frag(0, 0, 10, "a directory/part 1.dat")};

    const DecodeResult result = decode(fragfs::serializeMetadata(metadata));

    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].path,
                    std::string("a directory/part 1.dat"));
}

TEST_CASE("the encoded size matches the documented layout") {
    Metadata metadata;
    metadata.logicalSize = 10;
    metadata.fragments = {frag(0, 0, 10, "a"), frag(10, 0, 10, "bb")};

    // 36-byte header + 2 * 28-byte fixed records + 1 + 2 path bytes.
    FRAGFS_CHECK_EQ(fragfs::serializeMetadata(metadata).size(), std::size_t{95});
}

TEST_CASE("the header starts with the expected magic and version") {
    const std::vector<std::byte> encoded = fragfs::serializeMetadata(Metadata{});

    for (std::size_t i = 0; i < fragfs::kMetadataMagic.size(); ++i) {
        FRAGFS_CHECK_EQ(std::to_integer<unsigned char>(encoded[i]),
                        static_cast<unsigned char>(fragfs::kMetadataMagic[i]));
    }
    FRAGFS_CHECK_EQ(std::to_integer<unsigned char>(encoded[8]),
                    static_cast<unsigned char>(fragfs::kMetadataFormatVersion));
}

TEST_CASE("a corrupted magic is rejected") {
    std::vector<std::byte> encoded = fragfs::serializeMetadata(Metadata{});
    encoded[0] = static_cast<std::byte>('X');

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::invalid_magic));
}

TEST_CASE("an unsupported format version is rejected") {
    std::vector<std::byte> encoded = fragfs::serializeMetadata(Metadata{});
    writeU32(encoded, 8, fragfs::kMetadataFormatVersion + 1);

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::unsupported_version));
}

TEST_CASE("a buffer smaller than the header is rejected") {
    std::vector<std::byte> encoded = fragfs::serializeMetadata(Metadata{});
    encoded.resize(16);

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::truncated_metadata));
}

TEST_CASE("a null buffer is rejected") {
    FRAGFS_CHECK_EQ(fragfs::deserializeMetadata(nullptr, 0).error,
                    fragfs::make_error_code(ErrorCode::truncated_metadata));
}

TEST_CASE("a truncated fragment body is rejected") {
    Metadata metadata;
    metadata.logicalSize = 10;
    metadata.fragments = {frag(0, 0, 10, "part.dat")};

    std::vector<std::byte> encoded = fragfs::serializeMetadata(metadata);
    encoded.resize(encoded.size() - 3); // cut into the fragment record

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::truncated_metadata));
}

TEST_CASE("a path length larger than the remaining buffer is rejected") {
    Metadata metadata;
    metadata.logicalSize = 10;
    metadata.fragments = {frag(0, 0, 10, "part.dat")};

    std::vector<std::byte> encoded = fragfs::serializeMetadata(metadata);
    // path_length lives at offset 36 + 24 = 60 in the first fragment.
    writeU32(encoded, 60, 0xFFFFFFFFu);

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::truncated_metadata));
}

TEST_CASE("a corrupted fragment region is caught by the checksum") {
    Metadata metadata;
    metadata.logicalSize = 10;
    metadata.fragments = {frag(0, 0, 10, "a.dat")};

    std::vector<std::byte> encoded = fragfs::serializeMetadata(metadata);
    // First path byte is at 36 + 28 = 64; flipping it keeps the record
    // structurally valid, so only the checksum can detect the damage.
    encoded[64] = static_cast<std::byte>('Z');

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::checksum_mismatch));
}

TEST_CASE("physical identity is written as version 3 and survives a round trip") {
    Metadata metadata;
    metadata.logicalSize = 4;
    Fragment fragment = frag(0, 0, 4, "a.dat");
    fragfs::FileIdentity identity;
    identity.device = 11;
    identity.inode = 22;
    identity.size = 4;
    identity.mtimeSeconds = 1234567;
    identity.mtimeNanoseconds = 890;
    fragment.identity = identity;
    metadata.fragments = {fragment};

    const std::vector<std::byte> encoded = fragfs::serializeMetadata(metadata);
    FRAGFS_CHECK_EQ(std::to_integer<unsigned char>(encoded[8]),
                    static_cast<unsigned char>(3));

    const DecodeResult result = decode(encoded);
    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK(result.metadata.fragments[0].identity.has_value());
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].identity->device, uint64_t{11});
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].identity->inode, uint64_t{22});
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].identity->size, uint64_t{4});
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].identity->mtimeSeconds, int64_t{1234567});
    FRAGFS_CHECK_EQ(result.metadata.fragments[0].identity->mtimeNanoseconds, uint32_t{890});
}

TEST_CASE("version 1 metadata without a checksum is still readable") {
    // Hand-build a minimal version-1 empty-metadata buffer.
    std::vector<std::byte> encoded(32, std::byte{0});
    for (std::size_t i = 0; i < fragfs::kMetadataMagic.size(); ++i) {
        encoded[i] = static_cast<std::byte>(fragfs::kMetadataMagic[i]);
    }
    writeU32(encoded, 8, 1);  // version 1
    writeU32(encoded, 12, 0); // flags
    writeU64(encoded, 16, 0); // logical size
    writeU64(encoded, 24, 0); // fragment count

    const DecodeResult result = decode(encoded);
    FRAGFS_CHECK(result.ok());
    FRAGFS_CHECK_EQ(result.metadata.logicalSize, uint64_t{0});
    FRAGFS_CHECK_EQ(result.metadata.fragments.size(), std::size_t{0});
}

TEST_CASE("an excessive fragment count is rejected before allocating") {
    std::vector<std::byte> encoded = fragfs::serializeMetadata(Metadata{});
    writeU64(encoded, 24, fragfs::kMaxFragments + 1);

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::invalid_fragment_count));
}

TEST_CASE("a count that cannot fit in the buffer is treated as truncated") {
    std::vector<std::byte> encoded = fragfs::serializeMetadata(Metadata{});
    writeU64(encoded, 24, 2); // claims two fragments, buffer holds none

    FRAGFS_CHECK_EQ(decode(encoded).error,
                    fragfs::make_error_code(ErrorCode::truncated_metadata));
}

TEST_CASE("deserialization rejects structurally invalid metadata") {
    Metadata overlapping;
    overlapping.logicalSize = 150;
    overlapping.fragments = {frag(0, 0, 100), frag(50, 0, 100)};

    FRAGFS_CHECK_EQ(decode(fragfs::serializeMetadata(overlapping)).error,
                    fragfs::make_error_code(ErrorCode::overlapping_fragments));
}

TEST_CASE("deserialization rejects overflowing fragment ranges") {
    Metadata overflowing;
    overflowing.logicalSize = 100;
    overflowing.fragments = {frag(0, kMax, 100)};

    FRAGFS_CHECK_EQ(decode(fragfs::serializeMetadata(overflowing)).error,
                    fragfs::make_error_code(ErrorCode::invalid_range));
}

FRAGFS_TEST_MAIN
