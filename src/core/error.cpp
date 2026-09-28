#include <fragfs/error.h>

#include <string>

namespace fragfs {
namespace {

const char* messageFor(ErrorCode code) {
    switch (code) {
        case ErrorCode::success:                   return "success";
        case ErrorCode::invalid_magic:             return "invalid metadata magic";
        case ErrorCode::unsupported_version:       return "unsupported metadata format version";
        case ErrorCode::truncated_metadata:        return "metadata is truncated";
        case ErrorCode::invalid_fragment_count:    return "invalid fragment count";
        case ErrorCode::invalid_range:             return "fragment range is invalid";
        case ErrorCode::empty_path:                return "fragment path is empty";
        case ErrorCode::overlapping_fragments:     return "fragments overlap";
        case ErrorCode::gap_between_fragments:     return "gap between fragments";
        case ErrorCode::logical_size_mismatch:     return "logical size does not match fragments";
        case ErrorCode::missing_physical_file:     return "physical file is missing";
        case ErrorCode::physical_range_out_of_bounds:
            return "physical range is outside the file";
        case ErrorCode::io_error:                  return "I/O error";
    }
    return "unknown fragfs error";
}

class FragfsErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override {
        return "fragfs";
    }

    std::string message(int condition) const override {
        return messageFor(static_cast<ErrorCode>(condition));
    }
};

} // namespace

const std::error_category& errorCategory() {
    static const FragfsErrorCategory category;
    return category;
}

std::error_code make_error_code(ErrorCode code) {
    return {static_cast<int>(code), errorCategory()};
}

} // namespace fragfs
