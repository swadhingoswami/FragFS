#include <fragfs/version.h>

#include <cstdio>

// Placeholder. The real benchmark suite (concatenation vs. FragFS creation,
// random/sequential read throughput, cross-fragment reads) arrives in the
// performance milestone.
int main() {
    std::printf("FragFS benchmark suite (placeholder) - version %s\n",
                fragfs::versionString().c_str());
    return 0;
}
