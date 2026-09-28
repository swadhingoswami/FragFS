#pragma once

#include <cstddef>
#include <cstdint>

namespace fragfs {

// CRC-32 (IEEE 802.3, polynomial 0xEDB88320). A checksum, not a cryptographic
// hash: it detects accidental corruption, not deliberate tampering.
uint32_t crc32(const void* data, std::size_t size);

// Incremental CRC-32, for checksumming a stream in pieces (e.g. while writing
// a chunk to disk) without buffering the whole thing.
class Crc32 {
public:
    Crc32() = default;

    void update(const void* data, std::size_t size);
    void reset() { state_ = 0xFFFFFFFFu; }

    uint32_t value() const { return state_ ^ 0xFFFFFFFFu; }

private:
    uint32_t state_ = 0xFFFFFFFFu;
};

} // namespace fragfs
