#pragma once

#include <fragfs/metadata.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace fragfs::testutil {

inline std::filesystem::path makeTempDir(const std::string& tag) {
    static int counter = 0;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("fragfs_" + tag + "_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    return directory;
}

// A directory that removes itself, so tests leave no residue on failure.
class TempDir {
public:
    explicit TempDir(const std::string& tag = "tmp") : path(makeTempDir(tag)) {}
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path path;
};

inline void writeTextFile(const std::filesystem::path& path,
                          const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

inline void writeRawMetadataFile(const std::filesystem::path& path,
                                 const Metadata& metadata) {
    const std::vector<std::byte> bytes = serializeMetadata(metadata);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

} // namespace fragfs::testutil
