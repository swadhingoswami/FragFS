#include "commands.h"

#include <fragfs/version.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr const char* kProgramName = "fragfs";

void printUsage(std::FILE* out) {
    std::fprintf(out,
        "FragFS - zero-copy logical file aggregation\n"
        "\n"
        "Usage:\n"
        "  fragfs <command> [arguments]\n"
        "\n"
        "Commands:\n"
        "  create <output> <file>...             Create a logical file from physical files\n"
        "  info <logical-file>                   Show metadata for a logical file\n"
        "  read <logical-file> <offset> <size> [--output <file>]\n"
        "                                        Read a logical byte range\n"
        "  append <logical-file> <file>          Append a physical file to the mapping\n"
        "  add <logical-file> <file> --physical-offset <n> --length <n>\n"
        "                                        Add a partial physical range\n"
        "  remove <logical-file> <id>            Remove a fragment by id\n"
        "  verify <logical-file>                 Validate metadata and mappings\n"
        "  benchmark <logical-file> [--iterations <n>]\n"
        "                                        Benchmark logical reads\n"
        "  mount <logical-file> <dir>            Mount through a filesystem adapter\n"
        "  unmount <dir>                         Unmount a FragFS mount point\n"
        "\n"
        "Global options:\n"
        "  -h, --help                            Show this help\n"
        "  -V, --version                         Show version information\n");
}

void printVersion(std::FILE* out) {
    std::fprintf(out, "%s %s\n", kProgramName, fragfs::versionString().c_str());
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty()) {
        printUsage(stdout);
        return 0;
    }

    const std::string& command = args.front();

    if (command == "-h" || command == "--help" || command == "help") {
        printUsage(stdout);
        return 0;
    }
    if (command == "-V" || command == "--version" || command == "version") {
        printVersion(stdout);
        return 0;
    }

    if (command == "create") {
        return fragfs::cli::runCreate(args);
    }
    if (command == "info") {
        return fragfs::cli::runInfo(args);
    }
    if (command == "read") {
        return fragfs::cli::runRead(args);
    }
    if (command == "append") {
        return fragfs::cli::runAppend(args);
    }
    if (command == "add") {
        return fragfs::cli::runAdd(args);
    }
    if (command == "remove") {
        return fragfs::cli::runRemove(args);
    }
    if (command == "verify") {
        return fragfs::cli::runVerify(args);
    }
    if (command == "benchmark") {
        return fragfs::cli::runBenchmark(args);
    }

    if (command == "mount" || command == "unmount") {
        std::fprintf(stderr,
                     "fragfs: '%s' is not implemented yet (filesystem adapter "
                     "milestone)\n",
                     command.c_str());
        return 2;
    }

    std::fprintf(stderr, "fragfs: unknown command '%s'\n\n", command.c_str());
    printUsage(stderr);
    return 2;
}
