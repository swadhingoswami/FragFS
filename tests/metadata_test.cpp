#include "test_framework.h"

#include <fragfs/error.h>
#include <fragfs/metadata.h>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace {

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

} // namespace

TEST_CASE("metadata with a single contiguous fragment is valid") {
    Metadata metadata;
    metadata.logicalSize = 100;
    metadata.fragments = {frag(0, 0, 100)};

    const auto result = metadata.validate();
    FRAGFS_CHECK(result.ok());
    // A default-constructed std::error_code is system:0, which is falsy but not
    // equal to fragfs:0 (operator== compares the category as well).
    FRAGFS_CHECK(!result.error);
    FRAGFS_CHECK_EQ(result.error.value(), 0);
}

TEST_CASE("metadata with several contiguous fragments is valid") {
    Metadata metadata;
    metadata.logicalSize = 350;
    metadata.fragments = {frag(0, 0, 100, "a"), frag(100, 0, 50, "b"),
                          frag(150, 500, 200, "c")};

    FRAGFS_CHECK(metadata.validate().ok());
}

TEST_CASE("empty metadata with logical size zero is valid") {
    Metadata metadata;

    FRAGFS_CHECK(metadata.validate().ok());
}

TEST_CASE("empty metadata with non-zero logical size is invalid") {
    Metadata metadata;
    metadata.logicalSize = 1;

    const auto result = metadata.validate();
    FRAGFS_CHECK(!result.ok());
    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::logical_size_mismatch));
}

TEST_CASE("logical size smaller than the fragments is invalid") {
    Metadata metadata;
    metadata.logicalSize = 50;
    metadata.fragments = {frag(0, 0, 100)};

    FRAGFS_CHECK_EQ(metadata.validate().error,
                    fragfs::make_error_code(ErrorCode::logical_size_mismatch));
}

TEST_CASE("logical size larger than the fragments is invalid") {
    Metadata metadata;
    metadata.logicalSize = 101;
    metadata.fragments = {frag(0, 0, 100)};

    FRAGFS_CHECK_EQ(metadata.validate().error,
                    fragfs::make_error_code(ErrorCode::logical_size_mismatch));
}

TEST_CASE("overlapping fragments are rejected") {
    Metadata metadata;
    metadata.logicalSize = 150;
    metadata.fragments = {frag(0, 0, 100), frag(50, 0, 100)};

    const auto result = metadata.validate();
    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::overlapping_fragments));
    FRAGFS_CHECK_EQ(result.fragmentIndex, std::size_t{1});
}

TEST_CASE("a gap between fragments is rejected") {
    Metadata metadata;
    metadata.logicalSize = 300;
    metadata.fragments = {frag(0, 0, 100), frag(150, 0, 150)};

    const auto result = metadata.validate();
    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::gap_between_fragments));
    FRAGFS_CHECK_EQ(result.fragmentIndex, std::size_t{1});
}

TEST_CASE("the first fragment must start at logical offset zero") {
    Metadata metadata;
    metadata.logicalSize = 200;
    metadata.fragments = {frag(10, 0, 200)};

    const auto result = metadata.validate();
    FRAGFS_CHECK_EQ(result.error,
                    fragfs::make_error_code(ErrorCode::gap_between_fragments));
    FRAGFS_CHECK_EQ(result.fragmentIndex, std::size_t{0});
}

TEST_CASE("a zero-length fragment is rejected") {
    Metadata metadata;
    metadata.logicalSize = 100;
    metadata.fragments = {frag(0, 0, 100), frag(100, 0, 0)};

    const auto result = metadata.validate();
    FRAGFS_CHECK_EQ(result.error, fragfs::make_error_code(ErrorCode::invalid_range));
    FRAGFS_CHECK_EQ(result.fragmentIndex, std::size_t{1});
}

TEST_CASE("a fragment with an empty path is rejected") {
    Metadata metadata;
    metadata.logicalSize = 100;
    metadata.fragments = {frag(0, 0, 100, "")};

    FRAGFS_CHECK_EQ(metadata.validate().error,
                    fragfs::make_error_code(ErrorCode::empty_path));
}

TEST_CASE("a fragment whose physical range overflows is rejected") {
    Metadata metadata;
    metadata.logicalSize = 100;
    metadata.fragments = {frag(0, kMax, 100)};

    FRAGFS_CHECK_EQ(metadata.validate().error,
                    fragfs::make_error_code(ErrorCode::invalid_range));
}

TEST_CASE("the fragfs error category has a name and messages") {
    const std::error_code code = ErrorCode::invalid_magic;

    FRAGFS_CHECK(static_cast<bool>(code));
    FRAGFS_CHECK_EQ(std::string(code.category().name()), std::string("fragfs"));
    FRAGFS_CHECK(!code.message().empty());
}

FRAGFS_TEST_MAIN
