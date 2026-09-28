#include <fragfs/metadata.h>

#include <cstddef>
#include <utility>

namespace fragfs {
namespace {

// On-disk layout:
//   header:   magic[8] | version u32 | flags u32 | logical_size u64 | count u64
//   fragment: logical_start u64 | physical_start u64 | length u64
//             | path_length u32 | path bytes
// All integers are little-endian regardless of host endianness.

constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kFragmentFixedSize = 28; // 8 + 8 + 8 + 4

void appendU32(std::vector<std::byte>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
}

void appendU64(std::vector<std::byte>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
}

uint32_t readU32(const std::byte* data) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(std::to_integer<unsigned char>(data[i]))
                 << (8 * i);
    }
    return value;
}

uint64_t readU64(const std::byte* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(std::to_integer<unsigned char>(data[i]))
                 << (8 * i);
    }
    return value;
}

ValidationResult failure(ErrorCode code, std::size_t fragmentIndex, std::string detail) {
    return ValidationResult{make_error_code(code), fragmentIndex, std::move(detail)};
}

} // namespace

ValidationResult Metadata::validate() const {
    // The first fragment must begin at logical offset 0 and each subsequent
    // fragment must begin exactly where the previous one ended. Tracking the
    // expected start is therefore enough to detect both gaps and overlaps.
    uint64_t expectedStart = 0;

    for (std::size_t i = 0; i < fragments.size(); ++i) {
        const Fragment& fragment = fragments[i];

        if (!fragment.hasValidRanges()) {
            return failure(ErrorCode::invalid_range, i,
                           "fragment range overflows uint64_t");
        }
        if (fragment.length == 0) {
            return failure(ErrorCode::invalid_range, i,
                           "fragment length must be greater than zero");
        }
        if (fragment.path.empty()) {
            return failure(ErrorCode::empty_path, i,
                           "fragment path must not be empty");
        }
        if (fragment.logicalStart < expectedStart) {
            return failure(ErrorCode::overlapping_fragments, i,
                           "fragment overlaps the previous fragment");
        }
        if (fragment.logicalStart > expectedStart) {
            return failure(ErrorCode::gap_between_fragments, i,
                           "gap between this fragment and the previous one");
        }

        expectedStart = *fragment.logicalEnd();
    }

    if (fragments.empty()) {
        if (logicalSize != 0) {
            return failure(ErrorCode::logical_size_mismatch, 0,
                           "empty metadata must have logicalSize 0");
        }
        return ValidationResult{};
    }

    if (logicalSize != expectedStart) {
        return failure(ErrorCode::logical_size_mismatch, fragments.size() - 1,
                       "logicalSize does not match the end of the last fragment");
    }

    return ValidationResult{};
}

std::vector<std::byte> serializeMetadata(const Metadata& metadata) {
    std::vector<std::byte> out;
    out.reserve(kHeaderSize + metadata.fragments.size() * kFragmentFixedSize);

    for (const char byte : kMetadataMagic) {
        out.push_back(static_cast<std::byte>(byte));
    }
    appendU32(out, kMetadataFormatVersion);
    appendU32(out, 0); // reserved flags, must be zero in version 1
    appendU64(out, metadata.logicalSize);
    appendU64(out, static_cast<uint64_t>(metadata.fragments.size()));

    for (const Fragment& fragment : metadata.fragments) {
        appendU64(out, fragment.logicalStart);
        appendU64(out, fragment.physicalStart);
        appendU64(out, fragment.length);
        appendU32(out, static_cast<uint32_t>(fragment.path.size()));
        for (const char byte : fragment.path) {
            out.push_back(static_cast<std::byte>(byte));
        }
    }

    return out;
}

DecodeResult deserializeMetadata(const std::byte* data, std::size_t size) {
    DecodeResult result;

    if (data == nullptr || size < kHeaderSize) {
        result.error = make_error_code(ErrorCode::truncated_metadata);
        return result;
    }

    for (std::size_t i = 0; i < kMetadataMagic.size(); ++i) {
        const char actual =
            static_cast<char>(std::to_integer<unsigned char>(data[i]));
        if (actual != kMetadataMagic[i]) {
            result.error = make_error_code(ErrorCode::invalid_magic);
            return result;
        }
    }

    if (readU32(data + 8) != kMetadataFormatVersion) {
        result.error = make_error_code(ErrorCode::unsupported_version);
        return result;
    }

    const uint64_t logicalSize = readU64(data + 16);
    const uint64_t fragmentCount = readU64(data + 24);

    // Reject absurd counts before allocating.
    if (fragmentCount > kMaxFragments) {
        result.error = make_error_code(ErrorCode::invalid_fragment_count);
        return result;
    }

    // A count is only plausible if the buffer could physically contain that
    // many minimum-sized fragments; otherwise the buffer is truncated.
    const std::size_t remaining = size - kHeaderSize;
    if (fragmentCount > remaining / kFragmentFixedSize) {
        result.error = make_error_code(ErrorCode::truncated_metadata);
        return result;
    }

    Metadata metadata;
    metadata.logicalSize = logicalSize;
    metadata.fragments.reserve(static_cast<std::size_t>(fragmentCount));

    std::size_t cursor = kHeaderSize;
    for (uint64_t i = 0; i < fragmentCount; ++i) {
        // The fast check above is only a lower bound: earlier fragments may
        // carry paths that consume space the fixed-size estimate ignored. The
        // per-fragment check is the authoritative bounds test.
        if (size - cursor < kFragmentFixedSize) {
            result.error = make_error_code(ErrorCode::truncated_metadata);
            return result;
        }

        Fragment fragment;
        fragment.logicalStart = readU64(data + cursor);
        fragment.physicalStart = readU64(data + cursor + 8);
        fragment.length = readU64(data + cursor + 16);
        const uint32_t pathLength = readU32(data + cursor + 24);
        cursor += kFragmentFixedSize;

        if (pathLength > size - cursor) {
            result.error = make_error_code(ErrorCode::truncated_metadata);
            return result;
        }
        fragment.path.assign(reinterpret_cast<const char*>(data + cursor), pathLength);
        cursor += pathLength;

        metadata.fragments.push_back(std::move(fragment));
    }

    const ValidationResult validation = metadata.validate();
    if (!validation.ok()) {
        result.error = validation.error;
        return result;
    }

    result.metadata = std::move(metadata);
    return result;
}

} // namespace fragfs
