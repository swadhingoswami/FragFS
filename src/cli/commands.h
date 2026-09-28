#pragma once

#include <string>
#include <vector>

namespace fragfs::cli {

// Each function receives the full argument vector, with the command name at
// index 0, and returns a process exit code (0 success, 1 runtime error,
// 2 usage error).

int runCreate(const std::vector<std::string>& args);
int runInfo(const std::vector<std::string>& args);
int runRead(const std::vector<std::string>& args);
int runAppend(const std::vector<std::string>& args);
int runAdd(const std::vector<std::string>& args);
int runRemove(const std::vector<std::string>& args);
int runVerify(const std::vector<std::string>& args);
int runBenchmark(const std::vector<std::string>& args);

} // namespace fragfs::cli
