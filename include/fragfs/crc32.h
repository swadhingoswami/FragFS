#pragma once

#include <cstddef>
#include <cstdint>

namespace fragfs {

// CRC-32 (IEEE 802.3, the zlib/PKZIP polynomial 0xEDB88320). Used to detect
// accidental corruption of serialized metadata. It is a checksum, not a
// cryptographic hash: it detects random damage, not deliberate tampering.
uint32_t crc32(const void* data, std::size_t size);

} // namespace fragfs
