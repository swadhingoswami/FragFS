#include <fragfs/logical_file.h>
#include <fragfs/metadata_store.h>
#include <fragfs/operations.h>
#include <fragfs/posix_file.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int fileCount = 4;
    uint64_t fileSize = 8ull * 1024 * 1024;
    uint64_t randomIterations = 20000;
    uint64_t randomSize = 4096;
};

double secondsBetween(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double>(end - start).count();
}

std::filesystem::path makeTempDir() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("fragfs_bench_" + std::to_string(::getpid()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

void createFile(const std::filesystem::path& path, uint64_t size, char fill) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1024 * 1024, fill);
    uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<uint64_t>(buffer.size(), remaining));
        out.write(buffer.data(), static_cast<std::streamsize>(chunk));
        remaining -= chunk;
    }
}

// Copies every input file into a single output file, the traditional way.
void concatenate(const std::vector<std::filesystem::path>& inputs,
                 const std::filesystem::path& output) {
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1024 * 1024);
    for (const std::filesystem::path& input : inputs) {
        std::ifstream in(input, std::ios::binary);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got > 0) {
                out.write(buffer.data(), got);
            }
        }
    }
}

double sequentialReadThroughput(const std::filesystem::path& path, uint64_t expectedSize) {
    std::error_code error;
    auto file = fragfs::PosixFile::open(path, fragfs::OpenFlags::Read, error);
    if (!file.has_value()) {
        return 0.0;
    }
    std::vector<char> buffer(1024 * 1024);
    uint64_t offset = 0;
    const auto start = Clock::now();
    while (offset < expectedSize) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(buffer.size(), expectedSize - offset));
        std::size_t bytesRead = 0;
        error = file->pread(offset, buffer.data(), want, bytesRead);
        if (error || bytesRead == 0) {
            break;
        }
        offset += bytesRead;
    }
    const double seconds = secondsBetween(start, Clock::now());
    return seconds > 0.0
               ? static_cast<double>(offset) / seconds / (1024.0 * 1024.0)
               : 0.0;
}

double sequentialReadThroughput(fragfs::LogicalFile& file) {
    const uint64_t size = file.logicalSize();
    std::vector<char> buffer(1024 * 1024);
    uint64_t offset = 0;
    const auto start = Clock::now();
    while (offset < size) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(buffer.size(), size - offset));
        std::size_t bytesRead = 0;
        const std::error_code error = file.read(offset, buffer.data(), want, bytesRead);
        if (error || bytesRead == 0) {
            break;
        }
        offset += bytesRead;
    }
    const double seconds = secondsBetween(start, Clock::now());
    return seconds > 0.0
               ? static_cast<double>(offset) / seconds / (1024.0 * 1024.0)
               : 0.0;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--files" && i + 1 < argc) {
            options.fileCount = std::atoi(argv[++i]);
        } else if (arg == "--size-mib" && i + 1 < argc) {
            options.fileSize = static_cast<uint64_t>(std::atoi(argv[++i])) * 1024 * 1024;
        } else if (arg == "--random-iterations" && i + 1 < argc) {
            options.randomIterations = static_cast<uint64_t>(std::atoi(argv[++i]));
        }
    }
    if (options.fileCount < 1) {
        options.fileCount = 1;
    }
    if (options.fileSize == 0) {
        options.fileSize = 1;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    const std::filesystem::path directory = makeTempDir();

    std::printf("FragFS benchmark\n");
    std::printf("  files       : %d\n", options.fileCount);
    std::printf("  file size   : %llu bytes\n",
                static_cast<unsigned long long>(options.fileSize));
    std::printf("  total data  : %llu bytes\n\n",
                static_cast<unsigned long long>(options.fileSize *
                                                static_cast<uint64_t>(options.fileCount)));

    std::vector<std::filesystem::path> inputs;
    for (int i = 0; i < options.fileCount; ++i) {
        const std::filesystem::path path =
            directory / ("part" + std::to_string(i) + ".dat");
        createFile(path, options.fileSize, static_cast<char>('A' + (i % 26)));
        inputs.push_back(path);
    }

    // --- Creation ---------------------------------------------------------
    const std::filesystem::path concatenated = directory / "concatenated.dat";
    const auto concatStart = Clock::now();
    concatenate(inputs, concatenated);
    const double concatSeconds = secondsBetween(concatStart, Clock::now());

    const std::filesystem::path metadataPath = directory / "combined.ff.meta";
    const auto fragStart = Clock::now();
    const fragfs::CreateResult built = fragfs::buildMetadata(inputs, metadataPath);
    const double buildSeconds = secondsBetween(fragStart, Clock::now());
    if (!built.ok()) {
        std::fprintf(stderr, "buildMetadata failed: %s\n",
                     built.error.message().c_str());
        return 1;
    }
    const auto writeStart = Clock::now();
    const std::error_code writeError = fragfs::writeMetadataFile(metadataPath, built.metadata);
    const double writeSeconds = secondsBetween(writeStart, Clock::now());
    if (writeError) {
        std::fprintf(stderr, "writeMetadataFile failed: %s\n",
                     writeError.message().c_str());
        return 1;
    }

    uint64_t metadataBytes = 0;
    {
        std::error_code error;
        auto file = fragfs::PosixFile::open(metadataPath, fragfs::OpenFlags::Read, error);
        if (file.has_value()) {
            (void)file->size(metadataBytes);
        }
    }

    std::error_code error;
    auto logical = fragfs::LogicalFile::open(metadataPath, error);
    if (!logical.has_value()) {
        std::fprintf(stderr, "LogicalFile::open failed: %s\n", error.message().c_str());
        return 1;
    }

    const uint64_t totalBytes =
        options.fileSize * static_cast<uint64_t>(options.fileCount);

    std::printf("Creation\n");
    std::printf("  concatenation : %.3f s  (%llu bytes copied)\n", concatSeconds,
                static_cast<unsigned long long>(totalBytes));
    std::printf("  fragfs build  : %.6f s\n", buildSeconds);
    std::printf("  fragfs write  : %.6f s\n", writeSeconds);
    std::printf("  metadata size : %llu bytes\n", static_cast<unsigned long long>(metadataBytes));
    std::printf("  data copied   : 0 bytes\n\n");

    // --- Sequential reads -------------------------------------------------
    const double concatenatedThroughput =
        sequentialReadThroughput(concatenated, totalBytes);
    const double logicalThroughput = sequentialReadThroughput(*logical);

    std::printf("Sequential read throughput\n");
    std::printf("  concatenated  : %.1f MiB/s\n", concatenatedThroughput);
    std::printf("  fragfs logical: %.1f MiB/s\n\n", logicalThroughput);

    // --- Random reads -----------------------------------------------------
    std::mt19937_64 rng(0xBADC0FFEE);
    std::vector<char> buffer(static_cast<std::size_t>(options.randomSize));
    const uint64_t maxOffset =
        totalBytes > options.randomSize ? totalBytes - options.randomSize : 0;
    const auto randomStart = Clock::now();
    for (uint64_t i = 0; i < options.randomIterations; ++i) {
        const uint64_t offset = maxOffset > 0 ? rng() % maxOffset : 0;
        std::size_t bytesRead = 0;
        error = logical->read(offset, buffer.data(), buffer.size(), bytesRead);
        if (error) {
            std::fprintf(stderr, "read failed: %s\n", error.message().c_str());
            return 1;
        }
    }
    const double randomSeconds = secondsBetween(randomStart, Clock::now());
    const double averageMicros =
        randomSeconds * 1e6 / static_cast<double>(options.randomIterations);

    std::printf("Random reads\n");
    std::printf("  size          : %llu bytes\n",
                static_cast<unsigned long long>(options.randomSize));
    std::printf("  iterations    : %llu\n",
                static_cast<unsigned long long>(options.randomIterations));
    std::printf("  average       : %.2f us/read\n", averageMicros);
    std::printf("  throughput    : %.1f MiB/s\n\n",
                randomSeconds > 0.0
                    ? static_cast<double>(options.randomIterations * options.randomSize) /
                          randomSeconds / (1024.0 * 1024.0)
                    : 0.0);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return 0;
}
