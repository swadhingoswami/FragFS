#include "test_framework.h"

#include <fragfs/posix_file.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace {

using fragfs::OpenFlags;
using fragfs::PosixFile;

std::filesystem::path uniquePath(const std::string& tag) {
    static int counter = 0;
    const std::filesystem::path directory = std::filesystem::temp_directory_path();
    return directory / ("fragfs_test_" + std::to_string(::getpid()) + "_" + tag +
                        "_" + std::to_string(counter++) + ".tmp");
}

// Creates a file on construction and removes it on destruction, so tests leave
// no residue behind even if an assertion fails.
class TempFile {
public:
    explicit TempFile(const std::string& tag) : path_(uniquePath(tag)) {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    const std::filesystem::path& path() const { return path_; }

    void write(const std::string& content) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

private:
    std::filesystem::path path_;
};

std::string readAll(PosixFile& file) {
    uint64_t size = 0;
    const std::error_code sizeError = file.size(size);
    FRAGFS_CHECK(!sizeError);

    std::string buffer(static_cast<std::size_t>(size), '\0');
    std::size_t bytesRead = 0;
    const std::error_code readError =
        file.pread(0, buffer.data(), buffer.size(), bytesRead);
    FRAGFS_CHECK(!readError);
    buffer.resize(bytesRead);
    return buffer;
}

} // namespace

TEST_CASE("opening a missing file fails with ENOENT") {
    const TempFile file("missing");

    std::error_code error;
    const auto opened = PosixFile::open(file.path(), OpenFlags::Read, error);

    FRAGFS_CHECK(!opened.has_value());
    FRAGFS_CHECK_EQ(error.value(), static_cast<int>(ENOENT));
}

TEST_CASE("a file can be read in full") {
    const TempFile file("read_all");
    file.write("hello world");

    std::error_code error;
    auto opened = PosixFile::open(file.path(), OpenFlags::Read, error);
    FRAGFS_CHECK(opened.has_value());

    FRAGFS_CHECK_EQ(readAll(*opened), std::string("hello world"));
}

TEST_CASE("pread reads a slice at an explicit offset") {
    const TempFile file("slice");
    file.write("hello world");

    std::error_code error;
    auto opened = PosixFile::open(file.path(), OpenFlags::Read, error);
    FRAGFS_CHECK(opened.has_value());

    std::string buffer(5, '\0');
    std::size_t bytesRead = 0;
    error = opened->pread(6, buffer.data(), buffer.size(), bytesRead);

    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(bytesRead, std::size_t{5});
    FRAGFS_CHECK_EQ(buffer, std::string("world"));
}

TEST_CASE("reading past end of file returns zero bytes, not an error") {
    const TempFile file("eof");
    file.write("abc");

    std::error_code error;
    auto opened = PosixFile::open(file.path(), OpenFlags::Read, error);
    FRAGFS_CHECK(opened.has_value());

    std::string buffer(4, '\0');
    std::size_t bytesRead = 99;
    error = opened->pread(100, buffer.data(), buffer.size(), bytesRead);

    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(bytesRead, std::size_t{0});
}

TEST_CASE("size reports the file length") {
    const TempFile file("size");
    file.write("0123456789");

    std::error_code error;
    auto opened = PosixFile::open(file.path(), OpenFlags::Read, error);
    FRAGFS_CHECK(opened.has_value());

    uint64_t size = 0;
    error = opened->size(size);

    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(size, uint64_t{10});
}

TEST_CASE("move construction transfers ownership") {
    const TempFile file("move");
    file.write("data");

    std::error_code error;
    auto opened = PosixFile::open(file.path(), OpenFlags::Read, error);
    FRAGFS_CHECK(opened.has_value());

    PosixFile moved = std::move(*opened);
    FRAGFS_CHECK(moved.isOpen());
    FRAGFS_CHECK(!opened->isOpen());
    FRAGFS_CHECK_EQ(readAll(moved), std::string("data"));
}

TEST_CASE("close releases the descriptor and is idempotent") {
    const TempFile file("close");
    file.write("x");

    std::error_code error;
    auto opened = PosixFile::open(file.path(), OpenFlags::Read, error);
    FRAGFS_CHECK(opened.has_value());

    FRAGFS_CHECK(opened->isOpen());
    FRAGFS_CHECK(!opened->close());
    FRAGFS_CHECK(!opened->isOpen());
    FRAGFS_CHECK(!opened->close());
}

TEST_CASE("pwrite writes at an explicit offset and the result is readable") {
    const TempFile file("write");

    std::error_code error;
    auto opened = PosixFile::open(
        file.path(), OpenFlags::Read | OpenFlags::Write | OpenFlags::Create, error);
    FRAGFS_CHECK(opened.has_value());

    std::size_t written = 0;
    error = opened->pwrite(0, "hello", 5, written);
    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(written, std::size_t{5});

    error = opened->pwrite(5, " world", 6, written);
    FRAGFS_CHECK(!error);
    FRAGFS_CHECK_EQ(written, std::size_t{6});

    FRAGFS_CHECK(!opened->sync());
    FRAGFS_CHECK_EQ(readAll(*opened), std::string("hello world"));
}

TEST_CASE("the Create flag creates a new file") {
    const TempFile file("create");
    FRAGFS_CHECK(!std::filesystem::exists(file.path()));

    std::error_code error;
    auto opened =
        PosixFile::open(file.path(), OpenFlags::Write | OpenFlags::Create, error);

    FRAGFS_CHECK(opened.has_value());
    FRAGFS_CHECK(std::filesystem::exists(file.path()));
}

TEST_CASE("opening without an access mode is rejected") {
    const TempFile file("noflags");
    file.write("x");

    std::error_code error;
    const auto opened = PosixFile::open(file.path(), OpenFlags::None, error);

    FRAGFS_CHECK(!opened.has_value());
    FRAGFS_CHECK_EQ(error,
                    std::make_error_code(std::errc::invalid_argument));
}

TEST_CASE("reading a write-only file is refused by the kernel") {
    const TempFile file("writeonly");
    file.write("x");

    std::error_code error;
    auto opened =
        PosixFile::open(file.path(), OpenFlags::Write, error);
    FRAGFS_CHECK(opened.has_value());

    char byte = '\0';
    std::size_t bytesRead = 0;
    error = opened->pread(0, &byte, 1, bytesRead);

    FRAGFS_CHECK_EQ(error.value(), static_cast<int>(EBADF));
}

TEST_CASE("operations on a closed file report a bad descriptor") {
    PosixFile file;
    FRAGFS_CHECK(!file.isOpen());

    char byte = '\0';
    std::size_t bytesRead = 0;
    const std::error_code error = file.pread(0, &byte, 1, bytesRead);

    FRAGFS_CHECK_EQ(error,
                    std::make_error_code(std::errc::bad_file_descriptor));
}

FRAGFS_TEST_MAIN
