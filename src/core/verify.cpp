#include <fragfs/verify.h>

#include <fragfs/error.h>
#include <fragfs/metadata_store.h>
#include <fragfs/posix_file.h>

namespace fragfs {

VerifyReport verifyMetadata(const Metadata& metadata,
                            const std::filesystem::path& baseDirectory) {
    VerifyReport report;

    // Structural problems make the rest of the checks meaningless.
    const ValidationResult validation = metadata.validate();
    if (!validation.ok()) {
        report.metadataError = validation.error;
        return report;
    }

    report.fragments.reserve(metadata.fragments.size());

    for (std::size_t i = 0; i < metadata.fragments.size(); ++i) {
        const Fragment& fragment = metadata.fragments[i];
        FragmentVerification verification;
        verification.fragmentIndex = i;

        const std::filesystem::path resolved =
            resolvePhysicalPath(fragment.path, baseDirectory);

        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::status(resolved, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && !std::filesystem::exists(status))) {
            verification.error = make_error_code(ErrorCode::missing_physical_file);
            ++report.missingFiles;
            report.fragments.push_back(verification);
            continue;
        }
        if (error) {
            verification.error = error;
            ++report.missingFiles;
            report.fragments.push_back(verification);
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            verification.error = make_error_code(ErrorCode::not_a_regular_file);
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }

        verification.fileExists = true;

        auto opened = PosixFile::open(resolved, OpenFlags::Read, error);
        if (!opened) {
            verification.error = error ? error : make_error_code(ErrorCode::io_error);
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }

        uint64_t fileSize = 0;
        error = opened->size(fileSize);
        if (error) {
            verification.error = error;
            ++report.invalidRanges;
            report.fragments.push_back(verification);
            continue;
        }
        verification.physicalFileSize = fileSize;

        const std::optional<uint64_t> physicalEnd = fragment.physicalEnd();
        if (!physicalEnd.has_value()) {
            verification.error = make_error_code(ErrorCode::invalid_range);
            ++report.invalidRanges;
        } else if (*physicalEnd > fileSize) {
            verification.error = make_error_code(ErrorCode::physical_range_out_of_bounds);
            ++report.invalidRanges;
        } else {
            verification.rangeInBounds = true;
        }

        report.fragments.push_back(verification);
    }

    return report;
}

} // namespace fragfs
