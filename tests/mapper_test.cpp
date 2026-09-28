#include "test_framework.h"

#include <fragfs/error.h>
#include <fragfs/mapper.h>
#include <fragfs/metadata.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace {

using fragfs::Fragment;
using fragfs::Mapper;
using fragfs::Metadata;
using fragfs::ReadPlan;

constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

Fragment frag(uint64_t logicalStart, uint64_t physicalStart, uint64_t length) {
    Fragment fragment;
    fragment.logicalStart = logicalStart;
    fragment.physicalStart = physicalStart;
    fragment.length = length;
    fragment.path = "part.dat";
    return fragment;
}

// F1: [0,100)   -> physical 1000
// F2: [100,150) -> physical 0
// F3: [150,350) -> physical 5000
Metadata fixture() {
    Metadata metadata;
    metadata.logicalSize = 350;
    metadata.fragments = {frag(0, 1000, 100), frag(100, 0, 50),
                          frag(150, 5000, 200)};
    return metadata;
}

} // namespace

TEST_CASE("a read inside the first fragment maps to one step") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(10, 20);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{0});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{1010});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{20});
    FRAGFS_CHECK_EQ(plan.bytesPlanned, uint64_t{20});
}

TEST_CASE("a read inside a middle fragment maps to one step") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(120, 10);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{20});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{10});
}

TEST_CASE("a read inside the last fragment maps to one step") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(200, 50);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{2});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{5050});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{50});
}

TEST_CASE("a read starting exactly on a boundary enters the next fragment") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(100, 10);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{0});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{10});
}

TEST_CASE("a read ending exactly on a boundary does not spill over") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(90, 10);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{0});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{1090});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{10});
}

TEST_CASE("the byte before a boundary stays in the earlier fragment") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(99, 1);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{0});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{1099});
}

TEST_CASE("the byte after a boundary is in the later fragment") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(101, 1);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{1});
}

TEST_CASE("a read crossing two fragments produces two steps") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(80, 40);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{2});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{0});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{1080});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{20});
    FRAGFS_CHECK_EQ(plan.steps[1].fragmentIndex, std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[1].physicalOffset, uint64_t{0});
    FRAGFS_CHECK_EQ(plan.steps[1].length, uint64_t{20});
    FRAGFS_CHECK_EQ(plan.bytesPlanned, uint64_t{40});
}

TEST_CASE("a read crossing every fragment produces one step per fragment") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(0, 350);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{3});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{100});
    FRAGFS_CHECK_EQ(plan.steps[1].length, uint64_t{50});
    FRAGFS_CHECK_EQ(plan.steps[2].length, uint64_t{200});
    FRAGFS_CHECK_EQ(plan.bytesPlanned, uint64_t{350});
}

TEST_CASE("a read that overruns the logical end is clamped") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(300, 100);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].fragmentIndex, std::size_t{2});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{5150});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{50});
    FRAGFS_CHECK_EQ(plan.bytesPlanned, uint64_t{50});
}

TEST_CASE("an adversarial size cannot overflow the offset arithmetic") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(340, kMax);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{1});
    FRAGFS_CHECK_EQ(plan.steps[0].physicalOffset, uint64_t{5190});
    FRAGFS_CHECK_EQ(plan.steps[0].length, uint64_t{10});
    FRAGFS_CHECK_EQ(plan.bytesPlanned, uint64_t{10});
}

TEST_CASE("a read exactly at the end of the logical file yields nothing") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(350, 10);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{0});
    FRAGFS_CHECK_EQ(plan.bytesPlanned, uint64_t{0});
}

TEST_CASE("a read past the end of the logical file yields nothing") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(1000, 10);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{0});
}

TEST_CASE("a zero-size read yields nothing") {
    const Metadata metadata = fixture();
    const ReadPlan plan = Mapper(metadata).plan(50, 0);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{0});
}

TEST_CASE("an empty logical file yields nothing") {
    const Metadata metadata;
    const ReadPlan plan = Mapper(metadata).plan(0, 10);

    FRAGFS_CHECK(plan.ok());
    FRAGFS_CHECK_EQ(plan.steps.size(), std::size_t{0});
}

TEST_CASE("an offset that falls in no fragment is reported as invalid") {
    Metadata metadata;
    metadata.logicalSize = 250;
    metadata.fragments = {frag(0, 0, 100), frag(150, 0, 100)}; // deliberate gap

    const ReadPlan plan = Mapper(metadata).plan(120, 10);

    FRAGFS_CHECK(!plan.ok());
    FRAGFS_CHECK_EQ(plan.error,
                    fragfs::make_error_code(fragfs::ErrorCode::invalid_range));
}

FRAGFS_TEST_MAIN
