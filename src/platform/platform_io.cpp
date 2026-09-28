#include <fragfs/platform_io.h>

#include <cerrno>
#include <cstddef>
#include <vector>

#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif

namespace fragfs {
namespace {

std::error_code lastError() {
    return std::error_code(errno, std::generic_category());
}

// Portable fallback: read into a buffer and write it out. This does copy the
// bytes, but it is the only option when the kernel/FS offers nothing better.
std::error_code copyViaBuffers(int destFd,
                               uint64_t destOffset,
                               int srcFd,
                               uint64_t srcOffset,
                               uint64_t length,
                               uint64_t& bytesCopied) {
    std::vector<char> buffer(1024 * 1024);
    uint64_t done = 0;
    while (done < length) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(buffer.size(), length - done));

        ssize_t got = ::pread(srcFd, buffer.data(), want,
                              static_cast<off_t>(srcOffset + done));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return lastError();
        }
        if (got == 0) {
            break; // source ended early
        }

        ssize_t put = 0;
        do {
            put = ::pwrite(destFd, buffer.data(), static_cast<std::size_t>(got),
                           static_cast<off_t>(destOffset + done));
        } while (put < 0 && errno == EINTR);
        if (put < 0) {
            return lastError();
        }
        done += static_cast<uint64_t>(put);
    }

    bytesCopied = done;
    return {};
}

} // namespace

std::error_code cloneRange(int destFd,
                           uint64_t destOffset,
                           int srcFd,
                           uint64_t srcOffset,
                           uint64_t length) {
#if defined(__linux__)
    struct file_clone_range range {};
    range.src_fd = srcFd;
    range.src_offset = srcOffset;
    range.src_length = length;
    range.dest_offset = destOffset;

    if (::ioctl(destFd, FICLONERANGE, &range) != 0) {
        return lastError();
    }
    return {};
#else
    (void)destFd;
    (void)destOffset;
    (void)srcFd;
    (void)srcOffset;
    (void)length;
    // macOS and others expose no range-clone primitive.
    return std::make_error_code(std::errc::not_supported);
#endif
}

std::error_code copyRange(int destFd,
                          uint64_t destOffset,
                          int srcFd,
                          uint64_t srcOffset,
                          uint64_t length,
                          uint64_t& bytesCopied) {
    bytesCopied = 0;
    if (length == 0) {
        return {};
    }

#if defined(__linux__)
    // copy_file_range stays in the kernel and can be offloaded by the
    // filesystem (reflink) or the server (NFS/SMB). It may copy less than
    // requested, so loop; on any indication that it is unsupported, fall back.
    uint64_t done = 0;
    while (done < length) {
        loff_t in = static_cast<loff_t>(srcOffset + done);
        loff_t out = static_cast<loff_t>(destOffset + done);
        const std::size_t want =
            static_cast<std::size_t>(std::min<uint64_t>(length - done, 1u << 30));
        const ssize_t n = ::copy_file_range(srcFd, &in, destFd, &out, want, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOSYS || errno == EXDEV || errno == EINVAL ||
                errno == EOPNOTSUPP || errno == ENOTSUP) {
                return copyViaBuffers(destFd, destOffset, srcFd, srcOffset, length,
                                      bytesCopied);
            }
            return lastError();
        }
        if (n == 0) {
            break;
        }
        done += static_cast<uint64_t>(n);
    }
    bytesCopied = done;
    return {};
#else
    return copyViaBuffers(destFd, destOffset, srcFd, srcOffset, length, bytesCopied);
#endif
}

} // namespace fragfs
