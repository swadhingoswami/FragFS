#pragma once

#include <string>
#include <vector>

namespace fragfs::cli {

// Each function receives the full argument vector, with the command name at
// index 0, and returns a process exit code (0 success, 1 runtime error,
// 2 usage error).

// The primary command: `fragfs <logical-file> <chunk> [<chunk>...]`. The first
// argument is the logical file (the reconstructed original); the remaining
// arguments are physical files mapped into it, in order. Creates the mapping if
// absent, appends to it otherwise. No data is copied.
int runAggregate(const std::vector<std::string>& args);

int runCreate(const std::vector<std::string>& args);
int runSplit(const std::vector<std::string>& args);
int runInfo(const std::vector<std::string>& args);
int runRead(const std::vector<std::string>& args);
int runAppend(const std::vector<std::string>& args);
int runAdd(const std::vector<std::string>& args);
int runRemove(const std::vector<std::string>& args);
int runVerify(const std::vector<std::string>& args);
int runBenchmark(const std::vector<std::string>& args);

} // namespace fragfs::cli
