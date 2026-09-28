#include "test_framework.h"
#include "test_helpers.h"

#include <fragfs/metadata_store.h>
#include <fragfs/operations.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using fragfs::Metadata;
using fragfs::testutil::TempDir;
using fragfs::testutil::writeTextFile;

} // namespace

// Without writer serialisation, concurrent read-modify-write updates would
// lose all but the last append. The lock inside updateMetadata covers the load
// as well as the store, so every append must survive.
TEST_CASE("concurrent appends do not lose updates") {
    const TempDir dir;
    constexpr std::size_t kThreads = 8;

    std::vector<std::filesystem::path> files;
    std::size_t totalBytes = 0;
    for (std::size_t i = 0; i < kThreads; ++i) {
        const std::filesystem::path path =
            dir.path / ("part" + std::to_string(i) + ".dat");
        const std::string content(i + 1, 'x');
        writeTextFile(path, content);
        files.push_back(path);
        totalBytes += content.size();
    }

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, Metadata{}));

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i]() {
            const std::error_code error = fragfs::updateMetadata(
                metadataPath, [&](Metadata& metadata) -> std::error_code {
                    return fragfs::appendFile(metadata, files[i], metadataPath);
                });
            FRAGFS_CHECK(!error);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    Metadata loaded;
    FRAGFS_CHECK(!fragfs::readMetadataFile(metadataPath, loaded));
    FRAGFS_CHECK(loaded.validate().ok());
    FRAGFS_CHECK_EQ(loaded.fragments.size(), kThreads);
    FRAGFS_CHECK_EQ(loaded.logicalSize, static_cast<uint64_t>(totalBytes));

    std::set<std::string> paths;
    for (const fragfs::Fragment& fragment : loaded.fragments) {
        paths.insert(fragment.path);
    }
    FRAGFS_CHECK_EQ(paths.size(), kThreads);
}

FRAGFS_TEST_MAIN
