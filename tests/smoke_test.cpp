#include "test_framework.h"

#include <fragfs/version.h>

#include <string>

TEST_CASE("version string is not empty") {
    FRAGFS_CHECK(!fragfs::versionString().empty());
}

TEST_CASE("version string matches the version components") {
    const std::string expected =
        std::to_string(fragfs::kVersionMajor) + "." +
        std::to_string(fragfs::kVersionMinor) + "." +
        std::to_string(fragfs::kVersionPatch);

    FRAGFS_CHECK_EQ(fragfs::versionString(), expected);
}

FRAGFS_TEST_MAIN
