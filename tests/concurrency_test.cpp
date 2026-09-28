#include "test_framework.h"
#include "test_helpers.h"

#include <fragfs/logical_file.h>
#include <fragfs/metadata_store.h>
#include <fragfs/operations.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using fragfs::CreateResult;
using fragfs::LogicalFile;
using fragfs::testutil::TempDir;
using fragfs::testutil::writeTextFile;

} // namespace

// Concurrent readers must not race on the shared descriptor cache, and each
// must observe exactly the same bytes a single-threaded reader would. pread()
// carries its own offset, so no per-read locking is required.
TEST_CASE("many threads can read disjoint and overlapping ranges safely") {
    const TempDir dir;

    std::vector<std::string> parts = {"AAAA", "BBBBBB", "CC", "DDDDDDDD",
                                      "EEEEEEEEEEEE", "FF"};
    std::vector<std::filesystem::path> files;
    std::string expected;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::filesystem::path path =
            dir.path / ("part" + std::to_string(i) + ".dat");
        writeTextFile(path, parts[i]);
        files.push_back(path);
        expected += parts[i];
    }

    const std::filesystem::path metadataPath = dir.path / "combined.ff.meta";
    const CreateResult built = fragfs::buildMetadata(files, metadataPath);
    FRAGFS_CHECK(built.ok());
    FRAGFS_CHECK(!fragfs::writeMetadataFile(metadataPath, built.metadata));

    std::error_code error;
    auto file = LogicalFile::open(metadataPath, error);
    FRAGFS_CHECK(file.has_value());

    const std::uint64_t logicalSize = file->logicalSize();
    FRAGFS_CHECK_EQ(logicalSize, static_cast<std::uint64_t>(expected.size()));

    std::atomic<bool> failed{false};
    constexpr int kThreads = 4;
    constexpr int kIterations = 4000;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937_64 rng(static_cast<std::uint64_t>(t) * 2654435761u + 12345);
            for (int i = 0; i < kIterations && !failed.load(); ++i) {
                const std::uint64_t offset = rng() % logicalSize;
                const std::size_t size = static_cast<std::size_t>(
                    rng() % (logicalSize - offset + 1));

                std::string buffer(size, '\0');
                std::size_t bytesRead = 0;
                const std::error_code readError =
                    file->read(offset, buffer.data(), size, bytesRead);
                if (readError || bytesRead != size ||
                    buffer != expected.substr(static_cast<std::size_t>(offset), size)) {
                    failed.store(true);
                }
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    FRAGFS_CHECK(!failed.load());
}

FRAGFS_TEST_MAIN
