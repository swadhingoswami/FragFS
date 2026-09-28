#include <fragfs/metadata.h>

#include <utility>

namespace fragfs {
namespace {

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

} // namespace fragfs
