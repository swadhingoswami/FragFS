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
        "  fragfs <logical-file> <chunk> [<chunk>...]\n"
        "                                        Map physical chunks into a logical file\n"
        "  fragfs <command> [arguments]\n"
        "\n"
        "The first form is the primary operation: the chunks are mapped, in order,\n"
        "into the logical file without copying any data. Running it again appends\n"
        "more chunks.\n"
        "\n"
        "Commands:\n"
        "  split <input> (--chunk-size <size> | --chunks <n>) [--output-prefix <p>]\n"
        "                                        Split a file into chunks (KB/MB/GB)\n"
        "  get <original> <chunk>...             Rebuild the original from its chunks\n"
        "  info <logical-file>                   Show metadata for a logical file\n"
        "  read <logical-file> <offset> <size> [--output <file>] [--no-check]\n"
        "                                        Read a logical byte range\n"
        "  append <logical-file> <file>          Append one physical file\n"
        "  add <logical-file> <file> --physical-offset <n> --length <n>\n"
        "                                        Add a partial physical range\n"
        "  remove <logical-file> <id>            Remove a fragment by id\n"
        "  verify <logical-file>                 Validate metadata and mappings\n"
        "  benchmark <logical-file> [--iterations <n>]\n"
        "                                        Benchmark logical reads\n"
        "  create <output> <file>...             Alias for the primary form\n"
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
    if (command == "split") {
        return fragfs::cli::runSplit(args);
    }
    if (command == "get") {
        return fragfs::cli::runGet(args);
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

    // Anything else is the primary form: `fragfs <logical-file> <chunk>...`.
    return fragfs::cli::runAggregate(args);
}
