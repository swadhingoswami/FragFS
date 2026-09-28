#include <fragfs/mapper.h>

#include <algorithm>

namespace fragfs {

ReadPlan Mapper::plan(uint64_t logicalOffset, uint64_t size) const {
    ReadPlan plan;

    // Nothing to do for a zero-length request or a read at/after EOF. This
    // matches pread(), which returns 0 rather than an error at end of file.
    if (size == 0 || logicalOffset >= metadata_.logicalSize) {
        return plan;
    }

    // logicalOffset < logicalSize, so this subtraction cannot underflow. It
    // also lets us clamp the request without ever computing offset + size,
    // which could overflow uint64_t for an adversarial size.
    const uint64_t available = metadata_.logicalSize - logicalOffset;
    uint64_t remaining = std::min(size, available);
    uint64_t current = logicalOffset;

    // Linear scan for the fragment containing the first byte. Fragments are
    // ordered by logicalStart, so this can become a binary search later
    // without changing the interface.
    std::size_t index = 0;
    while (index < metadata_.fragments.size() &&
           !metadata_.fragments[index].containsLogicalOffset(current)) {
        ++index;
    }
    if (index == metadata_.fragments.size()) {
        // Unreachable for valid metadata (which tiles the logical space); a
        // defensive error for metadata that was not validated.
        plan.error = make_error_code(ErrorCode::invalid_range);
        return plan;
    }

    // Walk forward, emitting one physical read per fragment until the request
    // is satisfied.
    while (remaining > 0 && index < metadata_.fragments.size()) {
        const Fragment& fragment = metadata_.fragments[index];

        // containsLogicalOffset guaranteed current is within this fragment, so
        // relative < length and the subtraction below cannot underflow.
        const uint64_t relative = current - fragment.logicalStart;
        const uint64_t fragmentAvailable = fragment.length - relative;
        const uint64_t chunk = std::min(remaining, fragmentAvailable);

        plan.steps.push_back(
            ReadStep{index, fragment.physicalStart + relative, chunk});
        plan.bytesPlanned += chunk;

        current += chunk;
        remaining -= chunk;
        ++index;
    }

    if (remaining != 0) {
        // The fragments ran out before the request was satisfied, which means
        // logicalSize overstated the mapped range.
        plan.error = make_error_code(ErrorCode::invalid_range);
        plan.steps.clear();
        plan.bytesPlanned = 0;
        return plan;
    }

    return plan;
}

} // namespace fragfs
