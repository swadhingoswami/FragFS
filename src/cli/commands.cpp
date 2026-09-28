#include "commands.h"

#include <fragfs/error.h>
#include <fragfs/logical_file.h>
#include <fragfs/metadata_store.h>
#include <fragfs/operations.h>
#include <fragfs/posix_file.h>
#include <fragfs/verify.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace fragfs::cli {
namespace {

int runtimeError(const std::string& command, const std::string& message) {
    std::fprintf(stderr, "fragfs %s: %s\n", command.c_str(), message.c_str());
    return 1;
}

int usageError(const std::string& command, const std::string& usage) {
    std::fprintf(stderr, "fragfs %s: %s\n", command.c_str(), usage.c_str());
    return 2;
}

bool parseU64(const std::string& text, uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    std::size_t consumed = 0;
    try {
        const unsigned long long parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size()) {
            return false;
        }
        value = static_cast<uint64_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string formatBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu %s",
                      static_cast<unsigned long long>(bytes), units[unit]);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
    }
    return buffer;
}

// Loads metadata for a logical file path, reporting a readable error.
std::optional<Metadata> loadMetadata(const std::string& command,
                                     const std::filesystem::path& logicalPath,
                                     std::filesystem::path& metadataPath) {
    metadataPath = metadataPathFor(logicalPath);
    Metadata metadata;
    const std::error_code error = readMetadataFile(metadataPath, metadata);
    if (error) {
        runtimeError(command, "cannot read '" + metadataPath.string() + "': " +
                                  error.message());
        return std::nullopt;
    }
    return metadata;
}

} // namespace

int runAggregate(const std::vector<std::string>& args) {
    // args[0] is the logical file; args[1..] are the physical chunks.
    if (args.size() < 2) {
        return usageError("", "usage: fragfs <logical-file> <chunk> [<chunk>...]");
    }

    const std::filesystem::path logical = args[0];
    const std::filesystem::path metadataPath = metadataPathFor(logical);

    // Hold the writer lock across load-modify-store so concurrent runs cannot
    // lose each other's chunks.
    std::error_code error;
    std::optional<MetadataLock> lock = MetadataLock::acquire(metadataPath, error);
    if (!lock.has_value()) {
        return runtimeError(logical.string(),
                            "cannot lock '" + metadataPath.string() + "': " +
                                error.message());
    }

    Metadata metadata;
    error = readMetadataFile(metadataPath, metadata);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            metadata = Metadata{}; // first run: start from an empty mapping
        } else {
            return runtimeError(logical.string(),
                                "cannot read '" + metadataPath.string() + "': " +
                                    error.message());
        }
    }

    // Append every chunk in the order given. If any chunk is missing or
    // invalid, abort without writing, leaving the previous mapping intact.
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::error_code appendError = appendFile(metadata, args[i], metadataPath);
        if (appendError) {
            return runtimeError(logical.string(),
                                "cannot map '" + args[i] + "': " + appendError.message());
        }
    }

    error = writeMetadataFile(metadataPath, metadata);
    if (error) {
        return runtimeError(logical.string(), error.message());
    }

    std::printf("Mapped %zu chunk(s) into %s\n", args.size() - 1, logical.c_str());
    std::printf("  fragments   : %zu\n", metadata.fragments.size());
    std::printf("  logical size: %llu bytes\n",
                static_cast<unsigned long long>(metadata.logicalSize));
    std::printf("  metadata    : %s\n", metadataPath.c_str());
    return 0;
}

int runCreate(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return usageError("create", "usage: fragfs create <output> <file>...");
    }
    const std::filesystem::path output = args[1];

    std::vector<std::filesystem::path> files;
    for (std::size_t i = 2; i < args.size(); ++i) {
        files.emplace_back(args[i]);
    }

    const std::filesystem::path metadataPath = metadataPathFor(output);
    const CreateResult built = buildMetadata(files, metadataPath);
    if (!built.ok()) {
        return runtimeError("create", built.error.message());
    }

    // Serialise with other writers, then publish the new metadata atomically.
    std::error_code error;
    std::optional<MetadataLock> lock = MetadataLock::acquire(metadataPath, error);
    if (!lock.has_value()) {
        return runtimeError("create", "cannot lock '" + metadataPath.string() +
                                          "': " + error.message());
    }
    error = writeMetadataFile(metadataPath, built.metadata);
    if (error) {
        return runtimeError("create", "cannot write '" + metadataPath.string() +
                                          "': " + error.message());
    }

    std::printf("Created %s\n", output.c_str());
    std::printf("  fragments   : %zu\n", built.metadata.fragments.size());
    std::printf("  logical size: %llu bytes\n",
                static_cast<unsigned long long>(built.metadata.logicalSize));
    std::printf("  metadata    : %s\n", metadataPath.c_str());
    return 0;
}

int runInfo(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return usageError("info", "usage: fragfs info <logical-file>");
    }

    std::filesystem::path metadataPath;
    const std::optional<Metadata> metadata =
        loadMetadata("info", args[1], metadataPath);
    if (!metadata.has_value()) {
        return 1;
    }

    std::printf("Logical file : %s\n", args[1].c_str());
    std::printf("Logical size : %llu bytes\n",
                static_cast<unsigned long long>(metadata->logicalSize));
    std::printf("Fragments    : %zu\n\n", metadata->fragments.size());

    std::printf("%-4s %-28s %-16s %-16s %s\n", "ID", "File", "Logical Start",
                "Physical Start", "Length");
    for (std::size_t i = 0; i < metadata->fragments.size(); ++i) {
        const Fragment& fragment = metadata->fragments[i];
        std::printf("%-4zu %-28s %-16llu %-16llu %llu\n", i,
                    fragment.path.c_str(),
                    static_cast<unsigned long long>(fragment.logicalStart),
                    static_cast<unsigned long long>(fragment.physicalStart),
                    static_cast<unsigned long long>(fragment.length));
    }
    return 0;
}

int runRead(const std::vector<std::string>& args) {
    if (args.size() < 4) {
        return usageError(
            "read",
            "usage: fragfs read <logical-file> <offset> <size> [--output <file>]");
    }

    uint64_t offset = 0;
    uint64_t size = 0;
    if (!parseU64(args[2], offset) || !parseU64(args[3], size)) {
        return usageError("read", "<offset> and <size> must be non-negative integers");
    }

    std::optional<std::filesystem::path> outputPath;
    for (std::size_t i = 4; i < args.size(); ++i) {
        if (args[i] == "--output" && i + 1 < args.size()) {
            outputPath = args[++i];
        } else {
            return usageError("read", "unexpected argument '" + args[i] + "'");
        }
    }

    if (!outputPath.has_value() && ::isatty(STDOUT_FILENO) != 0) {
        return runtimeError(
            "read",
            "refusing to write binary data to a terminal; use --output <file> "
            "or redirect stdout");
    }

    std::error_code error;
    auto file = LogicalFile::open(metadataPathFor(args[1]), error);
    if (!file.has_value()) {
        return runtimeError("read", error.message());
    }

    std::optional<PosixFile> outputFile;
    if (outputPath.has_value()) {
        auto opened = PosixFile::open(
            *outputPath,
            OpenFlags::Write | OpenFlags::Create | OpenFlags::Truncate, error);
        if (!opened.has_value()) {
            return runtimeError("read", "cannot write '" + outputPath->string() +
                                            "': " + error.message());
        }
        outputFile = std::move(*opened);
    }

    constexpr std::size_t kChunk = 64 * 1024;
    std::vector<char> buffer(kChunk);
    uint64_t current = offset;
    uint64_t remaining = size;
    uint64_t fileOffset = 0;

    while (remaining > 0) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<uint64_t>(kChunk, remaining));
        std::size_t bytesRead = 0;
        error = file->read(current, buffer.data(), want, bytesRead);
        if (error) {
            return runtimeError("read", error.message());
        }
        if (bytesRead == 0) {
            break;
        }

        if (outputFile.has_value()) {
            std::size_t written = 0;
            error = outputFile->pwrite(fileOffset, buffer.data(), bytesRead, written);
            if (error) {
                return runtimeError("read", error.message());
            }
            fileOffset += written;
        } else if (std::fwrite(buffer.data(), 1, bytesRead, stdout) != bytesRead) {
            return runtimeError("read", "error writing to stdout");
        }

        current += bytesRead;
        remaining -= bytesRead;
    }

    if (outputPath.has_value()) {
        std::fprintf(stderr, "read %llu bytes from %s at offset %llu\n",
                     static_cast<unsigned long long>(fileOffset), args[1].c_str(),
                     static_cast<unsigned long long>(offset));
    }
    return 0;
}

int runAppend(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        return usageError("append", "usage: fragfs append <logical-file> <file>");
    }

    const std::filesystem::path metadataPath = metadataPathFor(args[1]);
    uint64_t newSize = 0;
    std::size_t fragmentCount = 0;

    const std::error_code error = updateMetadata(
        metadataPath, [&](Metadata& metadata) -> std::error_code {
            const std::error_code mutation = appendFile(metadata, args[2], metadataPath);
            if (!mutation) {
                newSize = metadata.logicalSize;
                fragmentCount = metadata.fragments.size();
            }
            return mutation;
        });
    if (error) {
        return runtimeError("append", error.message());
    }

    std::printf("Appended %s; logical size is now %llu bytes (%zu fragments)\n",
                args[2].c_str(), static_cast<unsigned long long>(newSize),
                fragmentCount);
    return 0;
}

int runAdd(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        return usageError(
            "add",
            "usage: fragfs add <logical-file> <file> --physical-offset <n> --length <n>");
    }

    uint64_t physicalOffset = 0;
    uint64_t length = 0;
    bool haveOffset = false;
    bool haveLength = false;

    for (std::size_t i = 3; i < args.size(); ++i) {
        if (args[i] == "--physical-offset" && i + 1 < args.size()) {
            if (!parseU64(args[++i], physicalOffset)) {
                return usageError("add", "--physical-offset must be an integer");
            }
            haveOffset = true;
        } else if (args[i] == "--length" && i + 1 < args.size()) {
            if (!parseU64(args[++i], length)) {
                return usageError("add", "--length must be an integer");
            }
            haveLength = true;
        } else {
            return usageError("add", "unexpected argument '" + args[i] + "'");
        }
    }

    if (!haveOffset || !haveLength) {
        return usageError(
            "add", "both --physical-offset and --length are required");
    }

    const std::filesystem::path metadataPath = metadataPathFor(args[1]);
    uint64_t newSize = 0;

    const std::error_code error = updateMetadata(
        metadataPath, [&](Metadata& metadata) -> std::error_code {
            const std::error_code mutation =
                addRange(metadata, args[2], physicalOffset, length, metadataPath);
            if (!mutation) {
                newSize = metadata.logicalSize;
            }
            return mutation;
        });
    if (error) {
        return runtimeError("add", error.message());
    }

    std::printf("Added %llu bytes from %s; logical size is now %llu bytes\n",
                static_cast<unsigned long long>(length), args[2].c_str(),
                static_cast<unsigned long long>(newSize));
    return 0;
}

int runRemove(const std::vector<std::string>& args) {
    if (args.size() != 3) {
        return usageError("remove", "usage: fragfs remove <logical-file> <id>");
    }

    uint64_t index = 0;
    if (!parseU64(args[2], index)) {
        return usageError("remove", "<id> must be a non-negative integer");
    }

    const std::filesystem::path metadataPath = metadataPathFor(args[1]);
    uint64_t newSize = 0;

    const std::error_code error = updateMetadata(
        metadataPath, [&](Metadata& metadata) -> std::error_code {
            const std::error_code mutation =
                removeFragment(metadata, static_cast<std::size_t>(index));
            if (!mutation) {
                newSize = metadata.logicalSize;
            }
            return mutation;
        });
    if (error) {
        return runtimeError("remove", error.message());
    }

    std::printf("Removed fragment %llu; logical size is now %llu bytes\n",
                static_cast<unsigned long long>(index),
                static_cast<unsigned long long>(newSize));
    return 0;
}

int runVerify(const std::vector<std::string>& args) {
    if (args.size() != 2) {
        return usageError("verify", "usage: fragfs verify <logical-file>");
    }

    std::filesystem::path metadataPath;
    const std::optional<Metadata> metadata =
        loadMetadata("verify", args[1], metadataPath);
    if (!metadata.has_value()) {
        return 1;
    }

    const VerifyReport report =
        verifyMetadata(*metadata, baseDirectoryFor(metadataPath));

    std::printf("FragFS verification\n\n");
    std::printf("Metadata       : %s\n",
                report.metadataError ? report.metadataError.message().c_str() : "OK");
    std::printf("Fragments      : %zu\n", metadata->fragments.size());
    std::printf("Logical size   : %s\n", formatBytes(metadata->logicalSize).c_str());
    std::printf("Missing files  : %zu\n", report.missingFiles);
    std::printf("Invalid ranges : %zu\n", report.invalidRanges);
    std::printf("Changed files  : %zu\n", report.changedFiles);
    std::printf("\nStatus: %s\n", report.valid() ? "VALID" : "INVALID");

    if (!report.valid()) {
        for (const FragmentVerification& fragment : report.fragments) {
            if (!fragment.error) {
                continue;
            }
            std::fprintf(stderr, "  fragment %zu: %s\n", fragment.fragmentIndex,
                         fragment.error.message().c_str());
        }
        return 1;
    }
    return 0;
}

int runBenchmark(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        return usageError(
            "benchmark",
            "usage: fragfs benchmark <logical-file> [--iterations <n>]");
    }

    uint64_t iterations = 10000;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--iterations" && i + 1 < args.size()) {
            if (!parseU64(args[++i], iterations) || iterations == 0) {
                return usageError("benchmark", "--iterations must be a positive integer");
            }
        } else {
            return usageError("benchmark", "unexpected argument '" + args[i] + "'");
        }
    }

    std::error_code error;
    auto file = LogicalFile::open(metadataPathFor(args[1]), error);
    if (!file.has_value()) {
        return runtimeError("benchmark", error.message());
    }

    const uint64_t logicalSize = file->logicalSize();
    uint64_t metadataBytes = 0;
    {
        auto metadataFile =
            PosixFile::open(metadataPathFor(args[1]), OpenFlags::Read, error);
        if (metadataFile.has_value()) {
            (void)metadataFile->size(metadataBytes);
        }
    }

    // Sequential read of the whole logical file.
    constexpr std::size_t kChunk = 1024 * 1024;
    std::vector<char> buffer(kChunk);
    const auto sequentialStart = std::chrono::steady_clock::now();
    uint64_t offset = 0;
    while (offset < logicalSize) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(kChunk, logicalSize - offset));
        std::size_t bytesRead = 0;
        error = file->read(offset, buffer.data(), want, bytesRead);
        if (error) {
            return runtimeError("benchmark", error.message());
        }
        if (bytesRead == 0) {
            break;
        }
        offset += bytesRead;
    }
    const auto sequentialEnd = std::chrono::steady_clock::now();
    const double sequentialSeconds =
        std::chrono::duration<double>(sequentialEnd - sequentialStart).count();
    const double throughput =
        sequentialSeconds > 0.0
            ? static_cast<double>(logicalSize) / sequentialSeconds / (1024.0 * 1024.0)
            : 0.0;

    // Random 4 KiB reads.
    constexpr std::size_t kRandomSize = 4096;
    std::vector<char> randomBuffer(kRandomSize);
    std::mt19937_64 rng(0xC0FFEE);
    const auto randomStart = std::chrono::steady_clock::now();
    if (logicalSize > 0) {
        for (uint64_t i = 0; i < iterations; ++i) {
            const uint64_t randomOffset =
                (logicalSize > kRandomSize)
                    ? rng() % (logicalSize - kRandomSize)
                    : 0;
            std::size_t bytesRead = 0;
            error = file->read(randomOffset, randomBuffer.data(),
                               std::min<std::size_t>(kRandomSize,
                                                     static_cast<std::size_t>(logicalSize)),
                               bytesRead);
            if (error) {
                return runtimeError("benchmark", error.message());
            }
        }
    }
    const auto randomEnd = std::chrono::steady_clock::now();
    const double randomMicros =
        std::chrono::duration<double, std::micro>(randomEnd - randomStart).count() /
        static_cast<double>(iterations);

    std::printf("FragFS benchmark: %s\n\n", args[1].c_str());
    std::printf("Logical size     : %s (%llu bytes)\n",
                formatBytes(logicalSize).c_str(),
                static_cast<unsigned long long>(logicalSize));
    std::printf("Fragments        : %zu\n", file->metadata().fragments.size());
    std::printf("Metadata size    : %s\n", formatBytes(metadataBytes).c_str());
    std::printf("Sequential read  : %.3f s, %.1f MiB/s\n", sequentialSeconds,
                throughput);
    std::printf("Random 4 KiB read: %.1f us average over %llu reads\n", randomMicros,
                static_cast<unsigned long long>(iterations));
    return 0;
}

} // namespace fragfs::cli
