#pragma once

#include <string>

namespace fragfs {

// The version is compiled into the binary and reported by `fragfs --version`.
// Keep these in sync with the `project(... VERSION ...)` line in CMakeLists.txt.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

// Human-readable version, e.g. "0.1.0".
std::string versionString();

} // namespace fragfs
