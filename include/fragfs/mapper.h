#pragma once

#include <fragfs/metadata.h>

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

namespace fragfs {

// One physical read needed to satisfy part of a logical request.
struct ReadStep {
    std::size_t fragmentIndex = 0; // index into Metadata::fragments
    uint64_t physicalOffset = 0;   // where to read in that physical file
    uint64_t length = 0;           // how many bytes to read
};

// The ordered list of physical reads that satisfy a logical request. `error`
// is empty on success. `bytesPlanned` is the sum of the step lengths, which is
// the number of logical bytes actually available (it is smaller than the
// requested size only when the request runs past the end of the logical file).
struct ReadPlan {
    std::error_code error;
    std::vector<ReadStep> steps;
    uint64_t bytesPlanned = 0;

    bool ok() const { return !error; }
};

// Translates logical reads into physical reads. The mapper performs no I/O and
// holds no file descriptors; it is a pure function of the metadata, which is
// what makes the offset-translation logic exhaustively testable.
//
// Precondition: the referenced Metadata is structurally valid and outlives the
// mapper. Callers obtain valid metadata from deserializeMetadata() or by
// running Metadata::validate().
class Mapper {
public:
    explicit Mapper(const Metadata& metadata) : metadata_(metadata) {}

    // Plans a read of up to `size` bytes starting at `logicalOffset`.
    //
    //   - a zero-size request, or one starting at or past logicalSize, yields
    //     an empty plan (the POSIX pread end-of-file convention);
    //   - a request that starts before logicalSize but overruns it is clamped
    //     to logicalSize, producing a short plan;
    //   - an offset that falls in no fragment (possible only with invalid
    //     metadata) is reported as invalid_range.
    ReadPlan plan(uint64_t logicalOffset, uint64_t size) const;

private:
    const Metadata& metadata_;
};

} // namespace fragfs
