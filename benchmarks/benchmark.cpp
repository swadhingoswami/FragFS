#include <fragfs/assemble.h>
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
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int fileCount = 8;
    uint64_t fileSize = 16ull * 1024 * 1024;
    uint64_t randomIterations = 20000;
    uint64_t randomSize = 4096;
};

double seconds(Clock::time_point start, Clock::time_point end) {
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

uint64_t fileSize(const std::filesystem::path& path) {
    std::error_code error;
    const uint64_t size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

// The traditional approach: copy every chunk into one combined file.
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

double sequentialThroughput(const std::filesystem::path& path, uint64_t size) {
    std::error_code error;
    auto file = fragfs::PosixFile::open(path, fragfs::OpenFlags::Read, error);
    if (!file.has_value()) {
        return 0.0;
    }
    std::vector<char> buffer(1024 * 1024);
    uint64_t offset = 0;
    const auto start = Clock::now();
    while (offset < size) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(buffer.size(), size - offset));
        std::size_t got = 0;
        error = file->pread(offset, buffer.data(), want, got);
        if (error || got == 0) {
            break;
        }
        offset += got;
    }
    const double s = seconds(start, Clock::now());
    return s > 0.0 ? static_cast<double>(offset) / s / (1024.0 * 1024.0) : 0.0;
}

double sequentialThroughput(fragfs::LogicalFile& file) {
    const uint64_t size = file.logicalSize();
    std::vector<char> buffer(1024 * 1024);
    uint64_t offset = 0;
    const auto start = Clock::now();
    while (offset < size) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(buffer.size(), size - offset));
        std::size_t got = 0;
        const std::error_code error = file.read(offset, buffer.data(), want, got);
        if (error || got == 0) {
            break;
        }
        offset += got;
    }
    const double s = seconds(start, Clock::now());
    return s > 0.0 ? static_cast<double>(offset) / s / (1024.0 * 1024.0) : 0.0;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--files" && i + 1 < argc) {
            options.fileCount = std::atoi(argv[++i]);
        } else if (arg == "--size-mib" && i + 1 < argc) {
            options.fileSize = static_cast<uint64_t>(std::atoi(argv[++i])) * 1024 * 1024;
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
    const std::filesystem::path dir = makeTempDir();
    const uint64_t total = options.fileSize * static_cast<uint64_t>(options.fileCount);

    std::printf("FragFS scenario benchmark\n");
    std::printf("  chunks     : %d\n", options.fileCount);
    std::printf("  chunk size : %llu bytes\n",
                static_cast<unsigned long long>(options.fileSize));
    std::printf("  total data : %llu bytes (%.1f MiB)\n\n",
                static_cast<unsigned long long>(total),
                static_cast<double>(total) / (1024.0 * 1024.0));

    // Create the chunks.
    std::vector<std::filesystem::path> chunks;
    for (int i = 0; i < options.fileCount; ++i) {
        const std::filesystem::path path =
            dir / ("chunk_" + std::to_string(i));
        createFile(path, options.fileSize, static_cast<char>('A' + (i % 26)));
        chunks.push_back(path);
    }

    // ---- Normal approach: concatenate (copy) ----
    const std::filesystem::path combined = dir / "combined.dat";
    const auto concatStart = Clock::now();
    concatenate(chunks, combined);
    const double concatSeconds = seconds(concatStart, Clock::now());

    // ---- FragFS approach: build the manifest (no copy) ----
    const std::filesystem::path manifestPath = dir / "big.dat.meta";
    const auto mapStart = Clock::now();
    const fragfs::CreateResult mapped = fragfs::buildMetadata(chunks, manifestPath);
    const double mapSeconds = seconds(mapStart, Clock::now());
    if (!mapped.ok()) {
        std::fprintf(stderr, "buildMetadata failed: %s\n", mapped.error.message().c_str());
        return 1;
    }
    const std::error_code writeError = fragfs::writeMetadataFile(manifestPath, mapped.metadata);
    if (writeError) {
        std::fprintf(stderr, "writeMetadataFile failed: %s\n", writeError.message().c_str());
        return 1;
    }

    // ---- FragFS reassembly ----
    const std::filesystem::path reassembled = dir / "reassembled.dat";
    const auto getStart = Clock::now();
    const fragfs::AssembleResult assembled =
        fragfs::assemble(mapped.metadata, dir, reassembled, {});
    const double getSeconds = seconds(getStart, Clock::now());
    if (!assembled.ok()) {
        std::fprintf(stderr, "assemble failed: %s\n", assembled.error.message().c_str());
        return 1;
    }

    const uint64_t manifestBytes = fileSize(manifestPath);

    std::printf("-------------------------------------------------------------\n");
    std::printf("%-28s %-16s %-16s\n", "operation", "normal (copy)", "fragfs");
    std::printf("-------------------------------------------------------------\n");
    std::printf("%-28s %-16s %-16s\n", "map / concatenate",
                (std::to_string(concatSeconds).substr(0, 6) + " s").c_str(),
                (std::to_string(mapSeconds).substr(0, 6) + " s").c_str());
    std::printf("%-28s %-16s %-16s\n", "data copied (map step)",
                (std::to_string(total) + " B").c_str(), "0 B");
    std::printf("%-28s %-16s %-16s\n", "extra storage",
                (std::to_string(total) + " B").c_str(),
                (std::to_string(manifestBytes) + " B").c_str());
    std::printf("%-28s %-16s %-16s\n", "reassemble",
                (std::to_string(concatSeconds).substr(0, 6) + " s").c_str(),
                (std::to_string(getSeconds).substr(0, 6) + " s").c_str());
    std::printf("%-28s %-16s %-16s\n", "reassembly method", "read+write",
                assembled.usedReflink ? "reflink (0 copy)" : "copy");
    std::printf("-------------------------------------------------------------\n\n");

    // ---- Read throughput ----
    const double combinedThroughput = sequentialThroughput(combined, total);

    std::error_code error;
    auto logical = fragfs::LogicalFile::open(manifestPath, error);
    const double logicalThroughput =
        logical.has_value() ? sequentialThroughput(*logical) : 0.0;

    std::printf("Sequential read throughput\n");
    std::printf("  combined file : %.1f MiB/s\n", combinedThroughput);
    std::printf("  fragfs logical: %.1f MiB/s\n\n", logicalThroughput);

    // ---- Random reads ----
    if (logical.has_value()) {
        std::vector<char> buffer(static_cast<std::size_t>(options.randomSize));
        const uint64_t maxOffset =
            total > options.randomSize ? total - options.randomSize : 0;
        const auto randomStart = Clock::now();
        for (uint64_t i = 0; i < options.randomIterations; ++i) {
            const uint64_t offset = maxOffset > 0 ? (i * 2654435761u) % maxOffset : 0;
            std::size_t got = 0;
            error = logical->read(offset, buffer.data(), buffer.size(), got);
            if (error) {
                break;
            }
        }
        const double randomSeconds = seconds(randomStart, Clock::now());
        std::printf("Random %llu-byte reads : %.2f us/read over %llu reads\n",
                    static_cast<unsigned long long>(options.randomSize),
                    randomSeconds * 1e6 / static_cast<double>(options.randomIterations),
                    static_cast<unsigned long long>(options.randomIterations));
    }

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    return 0;
}
