#include <fragfs/fragment.h>

#include <fragfs/checked_arithmetic.h>

namespace fragfs {

std::optional<uint64_t> Fragment::logicalEnd() const {
    return checkedAdd(logicalStart, length);
}

std::optional<uint64_t> Fragment::physicalEnd() const {
    return checkedAdd(physicalStart, length);
}

bool Fragment::hasValidRanges() const {
    return logicalEnd().has_value() && physicalEnd().has_value();
}

bool Fragment::containsLogicalOffset(uint64_t offset) const {
    const std::optional<uint64_t> end = logicalEnd();
    if (!end.has_value()) {
        // An overflowing range cannot contain any valid offset.
        return false;
    }
    return offset >= logicalStart && offset < *end;
}

} // namespace fragfs
