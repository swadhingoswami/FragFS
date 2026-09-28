#include <fragfs/version.h>

#include <string>

namespace fragfs {

std::string versionString() {
    return std::to_string(kVersionMajor) + "." +
           std::to_string(kVersionMinor) + "." +
           std::to_string(kVersionPatch);
}

} // namespace fragfs
