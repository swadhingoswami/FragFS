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

    // Fragments are ordered by logicalStart and tile the logical space, so the
    // containing fragment is the last one whose logicalStart is <= current.
    // upper_bound finds the first fragment starting after current; one step
    // back is the candidate.
    const auto after = std::upper_bound(
        metadata_.fragments.begin(), metadata_.fragments.end(), current,
        [](uint64_t value, const Fragment& fragment) {
            return value < fragment.logicalStart;
        });

    if (after == metadata_.fragments.begin()) {
        // current precedes the first fragment: only possible with invalid
        // metadata (which does not start at logical offset 0).
        plan.error = make_error_code(ErrorCode::invalid_range);
        return plan;
    }
    std::size_t index =
        static_cast<std::size_t>(after - metadata_.fragments.begin()) - 1;

    if (!metadata_.fragments[index].containsLogicalOffset(current)) {
        // The candidate does not actually contain current: a gap in metadata
        // that was not validated.
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
