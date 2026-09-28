#include <fragfs/crc32.h>

#include <array>

namespace fragfs {
namespace {

// A 256-entry lookup table, built once on first use.
const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            }
            result[i] = value;
        }
        return result;
    }();
    return table;
}

uint32_t updateCrc(uint32_t crc, const unsigned char* bytes, std::size_t size) {
    const std::array<uint32_t, 256>& table = crcTable();
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

} // namespace

uint32_t crc32(const void* data, std::size_t size) {
    return updateCrc(0xFFFFFFFFu, static_cast<const unsigned char*>(data), size) ^
           0xFFFFFFFFu;
}

void Crc32::update(const void* data, std::size_t size) {
    state_ = updateCrc(state_, static_cast<const unsigned char*>(data), size);
}

} // namespace fragfs
