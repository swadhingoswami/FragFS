#include <fragfs/posix_file.h>

#include <cerrno>
#include <climits>
#include <limits>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fragfs {
namespace {

std::error_code lastError() {
    return std::error_code(errno, std::generic_category());
}

// POSIX pread/pwrite take an off_t (signed) and a count that the kernel limits
// to SSIZE_MAX. Guard both before calling so an adversarial offset or size
// cannot wrap into a negative argument.
bool offsetFits(off_t& out, uint64_t offset) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return false;
    }
    out = static_cast<off_t>(offset);
    return true;
}

constexpr std::size_t kMaxTransfer =
    static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());

} // namespace

PosixFile::~PosixFile() {
    // Errors are ignored here by necessity: a destructor cannot report them,
    // and there is nothing meaningful a caller could do at this point.
    (void)close();
}

PosixFile::PosixFile(PosixFile&& other) noexcept : descriptor_(other.descriptor_) {
    other.descriptor_ = -1;
}

PosixFile& PosixFile::operator=(PosixFile&& other) noexcept {
    if (this != &other) {
        (void)close();
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
    }
    return *this;
}

std::optional<PosixFile> PosixFile::open(const std::filesystem::path& path,
                                         OpenFlags flags,
                                         std::error_code& error) {
    error.clear();

    const bool read = hasFlag(flags, OpenFlags::Read);
    const bool write = hasFlag(flags, OpenFlags::Write);

    int access = 0;
    if (read && write) {
        access = O_RDWR;
    } else if (write) {
        access = O_WRONLY;
    } else if (read) {
        access = O_RDONLY;
    } else {
        error = std::make_error_code(std::errc::invalid_argument);
        return std::nullopt;
    }

    int oflags = access;
    if (hasFlag(flags, OpenFlags::Create)) {
        oflags |= O_CREAT;
    }
    if (hasFlag(flags, OpenFlags::Truncate)) {
        oflags |= O_TRUNC;
    }
#ifdef O_CLOEXEC
    // Do not leak the descriptor across exec().
    oflags |= O_CLOEXEC;
#endif

    const mode_t mode = static_cast<mode_t>(0644);
    const int descriptor = ::open(path.c_str(), oflags, mode);
    if (descriptor < 0) {
        error = lastError();
        return std::nullopt;
    }
    return PosixFile(descriptor);
}

std::error_code PosixFile::close() {
    if (descriptor_ < 0) {
        return {};
    }
    const int descriptor = descriptor_;
    descriptor_ = -1;
    // Deliberately not retried on EINTR: on Linux the descriptor is in an
    // unspecified state after an interrupted close, so retrying risks closing
    // a descriptor that has been reused.
    if (::close(descriptor) != 0) {
        return lastError();
    }
    return {};
}

std::error_code PosixFile::size(uint64_t& out) const {
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    struct stat info {};
    if (::fstat(descriptor_, &info) != 0) {
        return lastError();
    }
    if (info.st_size < 0) {
        return std::make_error_code(std::errc::io_error);
    }
    out = static_cast<uint64_t>(info.st_size);
    return {};
}

std::error_code PosixFile::truncate(uint64_t size) const {
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    if (size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::make_error_code(std::errc::value_too_large);
    }
    if (::ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
        return lastError();
    }
    return {};
}

std::error_code PosixFile::identity(FileIdentity& out) const {
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    struct stat info {};
    if (::fstat(descriptor_, &info) != 0) {
        return lastError();
    }
    if (info.st_size < 0) {
        return std::make_error_code(std::errc::io_error);
    }

    out.device = static_cast<uint64_t>(info.st_dev);
    out.inode = static_cast<uint64_t>(info.st_ino);
    out.size = static_cast<uint64_t>(info.st_size);
    out.mtimeSeconds = static_cast<int64_t>(info.st_mtime);
#if defined(__APPLE__)
    out.mtimeNanoseconds = static_cast<uint32_t>(info.st_mtimespec.tv_nsec);
#else
    out.mtimeNanoseconds = static_cast<uint32_t>(info.st_mtim.tv_nsec);
#endif
    return {};
}

std::error_code PosixFile::pread(uint64_t offset,
                                 void* buffer,
                                 std::size_t size,
                                 std::size_t& bytesRead) const {
    bytesRead = 0;
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    off_t position = 0;
    if (!offsetFits(position, offset)) {
        return std::make_error_code(std::errc::value_too_large);
    }
    const std::size_t count = size < kMaxTransfer ? size : kMaxTransfer;

    for (;;) {
        const ssize_t result = ::pread(descriptor_, buffer, count, position);
        if (result >= 0) {
            bytesRead = static_cast<std::size_t>(result);
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        return lastError();
    }
}

std::error_code PosixFile::pwrite(uint64_t offset,
                                  const void* buffer,
                                  std::size_t size,
                                  std::size_t& bytesWritten) const {
    bytesWritten = 0;
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    off_t position = 0;
    if (!offsetFits(position, offset)) {
        return std::make_error_code(std::errc::value_too_large);
    }
    const std::size_t count = size < kMaxTransfer ? size : kMaxTransfer;

    for (;;) {
        const ssize_t result = ::pwrite(descriptor_, buffer, count, position);
        if (result >= 0) {
            bytesWritten = static_cast<std::size_t>(result);
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        return lastError();
    }
}

std::error_code PosixFile::sync() {
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    for (;;) {
        if (::fsync(descriptor_) == 0) {
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        return lastError();
    }
}

std::error_code PosixFile::lockExclusive() {
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    for (;;) {
        if (::flock(descriptor_, LOCK_EX) == 0) {
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        return lastError();
    }
}

std::error_code PosixFile::unlock() {
    if (descriptor_ < 0) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    if (::flock(descriptor_, LOCK_UN) == 0) {
        return {};
    }
    return lastError();
}

std::error_code syncDirectory(const std::filesystem::path& directory) {
    const int descriptor = ::open(directory.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return lastError();
    }

    std::error_code result;
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        // Some platforms/filesystems reject fsync on a directory descriptor.
        // The rename is still atomic; only the durability hint is lost, so do
        // not turn that into a failure.
        if (error != EINVAL && error != ENOTSUP && error != EBADF) {
            result = std::error_code(error, std::generic_category());
        }
    }
    (void)::close(descriptor);
    return result;
}

} // namespace fragfs
