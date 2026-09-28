#include <fragfs/metadata.h>

#include <fragfs/crc32.h>

#include <cstddef>
#include <utility>

namespace fragfs {
namespace {

// On-disk layout:
//   header v1: magic[8] | version u32 | flags u32 | logical_size u64 | count u64
//   header v2: v1 fields | checksum u32 (CRC-32 of the fragment region)
//   header v3: same as v2; fragments additionally carry physical identity
//   fragment:  logical_start u64 | physical_start u64 | length u64
//              | path_length u32 | path bytes
//              | [v3 only] device u64 | inode u64 | size u64
//                          | mtime_sec i64 | mtime_nsec u32
// All integers are little-endian regardless of host endianness.

constexpr std::size_t kHeaderSizeV1 = 32;
constexpr std::size_t kHeaderSizeV2 = 36; // v2 and v3 share a header layout
constexpr std::size_t kFragmentFixedSize = 28;       // v1/v2 fragment
constexpr std::size_t kIdentitySize = 36;            // v3 identity block
constexpr std::size_t kFragmentFixedSizeV3 = kFragmentFixedSize + kIdentitySize;

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

// Walks the fragment region starting at `headerSize` and fills `metadata`.
// Shared by all supported format versions.
std::error_code decodeFragments(const std::byte* data,
                                std::size_t size,
                                std::size_t headerSize,
                                bool withIdentity,
                                uint64_t logicalSize,
                                uint64_t fragmentCount,
                                Metadata& metadata) {
    if (fragmentCount > kMaxFragments) {
        return make_error_code(ErrorCode::invalid_fragment_count);
    }

    const std::size_t minimum = withIdentity ? kFragmentFixedSizeV3
                                             : kFragmentFixedSize;
    const std::size_t remaining = size - headerSize;
    if (fragmentCount > remaining / minimum) {
        return make_error_code(ErrorCode::truncated_metadata);
    }

    metadata.logicalSize = logicalSize;
    metadata.fragments.reserve(static_cast<std::size_t>(fragmentCount));

    std::size_t cursor = headerSize;
    for (uint64_t i = 0; i < fragmentCount; ++i) {
        if (size - cursor < kFragmentFixedSize) {
            return make_error_code(ErrorCode::truncated_metadata);
        }

        Fragment fragment;
        fragment.logicalStart = readU64(data + cursor);
        fragment.physicalStart = readU64(data + cursor + 8);
        fragment.length = readU64(data + cursor + 16);
        const uint32_t pathLength = readU32(data + cursor + 24);
        cursor += kFragmentFixedSize;

        if (pathLength > size - cursor) {
            return make_error_code(ErrorCode::truncated_metadata);
        }
        fragment.path.assign(reinterpret_cast<const char*>(data + cursor), pathLength);
        cursor += pathLength;

        if (withIdentity) {
            if (size - cursor < kIdentitySize) {
                return make_error_code(ErrorCode::truncated_metadata);
            }
            FileIdentity identity;
            identity.device = readU64(data + cursor);
            identity.inode = readU64(data + cursor + 8);
            identity.size = readU64(data + cursor + 16);
            identity.mtimeSeconds = static_cast<int64_t>(readU64(data + cursor + 24));
            identity.mtimeNanoseconds = readU32(data + cursor + 32);
            cursor += kIdentitySize;
            fragment.identity = identity;
        }

        metadata.fragments.push_back(std::move(fragment));
    }

    return {};
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
    // Version 3 is used only when every fragment carries physical identity;
    // otherwise the data is written as version 2 (which cannot represent it).
    // This keeps hand-built metadata and pre-identity files round-tripping.
    bool withIdentity = true;
    for (const Fragment& fragment : metadata.fragments) {
        if (!fragment.identity.has_value()) {
            withIdentity = false;
            break;
        }
    }

    // Encode the fragment region first so its checksum can go in the header.
    std::vector<std::byte> body;
    body.reserve(metadata.fragments.size() * kFragmentFixedSize);
    for (const Fragment& fragment : metadata.fragments) {
        appendU64(body, fragment.logicalStart);
        appendU64(body, fragment.physicalStart);
        appendU64(body, fragment.length);
        appendU32(body, static_cast<uint32_t>(fragment.path.size()));
        for (const char byte : fragment.path) {
            body.push_back(static_cast<std::byte>(byte));
        }
        if (withIdentity) {
            const FileIdentity& identity = *fragment.identity;
            appendU64(body, identity.device);
            appendU64(body, identity.inode);
            appendU64(body, identity.size);
            appendU64(body, static_cast<uint64_t>(identity.mtimeSeconds));
            appendU32(body, identity.mtimeNanoseconds);
        }
    }

    const uint32_t checksum = crc32(body.data(), body.size());

    std::vector<std::byte> out;
    out.reserve(kHeaderSizeV2 + body.size());
    for (const char byte : kMetadataMagic) {
        out.push_back(static_cast<std::byte>(byte));
    }
    appendU32(out, withIdentity ? 3u : 2u);
    appendU32(out, 0); // reserved flags, must be zero
    appendU64(out, metadata.logicalSize);
    appendU64(out, static_cast<uint64_t>(metadata.fragments.size()));
    appendU32(out, checksum);
    out.insert(out.end(), body.begin(), body.end());

    return out;
}

DecodeResult deserializeMetadata(const std::byte* data, std::size_t size) {
    DecodeResult result;

    // The magic plus the version field occupy the first 12 bytes in every
    // version, so they can be validated before choosing a parser.
    if (data == nullptr || size < 12) {
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

    const uint32_t version = readU32(data + 8);
    std::size_t headerSize = 0;
    bool withIdentity = false;
    if (version == 1) {
        headerSize = kHeaderSizeV1;
    } else if (version == 2 || version == 3) {
        headerSize = kHeaderSizeV2;
        withIdentity = (version == 3);
    } else {
        result.error = make_error_code(ErrorCode::unsupported_version);
        return result;
    }

    if (size < headerSize) {
        result.error = make_error_code(ErrorCode::truncated_metadata);
        return result;
    }

    const uint64_t logicalSize = readU64(data + 16);
    const uint64_t fragmentCount = readU64(data + 24);

    Metadata metadata;
    // Decode first: this performs the bounds checks that distinguish a
    // truncated buffer from a merely corrupt one, without trusting any field.
    const std::error_code decodeError = decodeFragments(
        data, size, headerSize, withIdentity, logicalSize, fragmentCount, metadata);
    if (decodeError) {
        result.error = decodeError;
        return result;
    }

    // Versions 2 and 3 protect the fragment region with a checksum. It is
    // verified after decoding (which is bounds-safe) so that truncation is
    // reported as truncation rather than as a checksum failure.
    if (version >= 2) {
        const uint32_t expected = readU32(data + 32);
        const uint32_t actual = crc32(data + headerSize, size - headerSize);
        if (actual != expected) {
            result.error = make_error_code(ErrorCode::checksum_mismatch);
            return result;
        }
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
