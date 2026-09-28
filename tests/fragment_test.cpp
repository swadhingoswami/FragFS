#include "test_framework.h"

#include <fragfs/fragment.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace {

using fragfs::Fragment;

constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

Fragment makeFragment(uint64_t logicalStart, uint64_t physicalStart, uint64_t length) {
    Fragment fragment;
    fragment.logicalStart = logicalStart;
    fragment.physicalStart = physicalStart;
    fragment.length = length;
    fragment.path = "part.dat";
    return fragment;
}

} // namespace

TEST_CASE("fragment reports its logical and physical end") {
    const Fragment fragment = makeFragment(100, 200, 50);

    FRAGFS_CHECK_EQ(fragment.logicalEnd(), std::optional<uint64_t>(150));
    FRAGFS_CHECK_EQ(fragment.physicalEnd(), std::optional<uint64_t>(250));
    FRAGFS_CHECK(fragment.hasValidRanges());
}

TEST_CASE("containsLogicalOffset treats the range as half-open") {
    const Fragment fragment = makeFragment(100, 0, 50);

    FRAGFS_CHECK(fragment.containsLogicalOffset(100));
    FRAGFS_CHECK(fragment.containsLogicalOffset(149));
    FRAGFS_CHECK(!fragment.containsLogicalOffset(99));
    FRAGFS_CHECK(!fragment.containsLogicalOffset(150));
}

TEST_CASE("a zero-length fragment maps nothing but is otherwise valid") {
    const Fragment fragment = makeFragment(100, 0, 0);

    FRAGFS_CHECK_EQ(fragment.logicalEnd(), std::optional<uint64_t>(100));
    FRAGFS_CHECK(fragment.hasValidRanges());
    FRAGFS_CHECK(!fragment.containsLogicalOffset(100));
    FRAGFS_CHECK(!fragment.containsLogicalOffset(99));
}

TEST_CASE("a fragment may span almost the whole 64-bit space") {
    const Fragment fragment = makeFragment(0, 0, kMax);

    FRAGFS_CHECK_EQ(fragment.logicalEnd(), std::optional<uint64_t>(kMax));
    FRAGFS_CHECK(fragment.hasValidRanges());
    FRAGFS_CHECK(fragment.containsLogicalOffset(kMax - 1));
    FRAGFS_CHECK(!fragment.containsLogicalOffset(kMax));
}

TEST_CASE("logical range overflow is detected at the exact boundary") {
    const Fragment boundary = makeFragment(kMax - 5, 0, 5);
    FRAGFS_CHECK_EQ(boundary.logicalEnd(), std::optional<uint64_t>(kMax));
    FRAGFS_CHECK(boundary.hasValidRanges());

    const Fragment overflowing = makeFragment(kMax - 5, 0, 6);
    FRAGFS_CHECK_EQ(overflowing.logicalEnd(), std::nullopt);
    FRAGFS_CHECK(!overflowing.hasValidRanges());
    FRAGFS_CHECK(!overflowing.containsLogicalOffset(kMax - 5));
}

TEST_CASE("physical range overflow is detected independently of the logical range") {
    const Fragment fragment = makeFragment(0, kMax, 1);

    FRAGFS_CHECK_EQ(fragment.logicalEnd(), std::optional<uint64_t>(1));
    FRAGFS_CHECK_EQ(fragment.physicalEnd(), std::nullopt);
    FRAGFS_CHECK(!fragment.hasValidRanges());
    FRAGFS_CHECK(fragment.containsLogicalOffset(0));
}

TEST_CASE("logicalStart at the maximum with zero length is representable") {
    const Fragment fragment = makeFragment(kMax, 0, 0);

    FRAGFS_CHECK_EQ(fragment.logicalEnd(), std::optional<uint64_t>(kMax));
    FRAGFS_CHECK(fragment.hasValidRanges());
    FRAGFS_CHECK(!fragment.containsLogicalOffset(kMax));
}

TEST_CASE("fragment preserves its physical path") {
    Fragment fragment;
    fragment.path = "relative/part1.dat";

    FRAGFS_CHECK_EQ(fragment.path, std::string("relative/part1.dat"));
}

FRAGFS_TEST_MAIN
