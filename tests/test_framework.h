#pragma once

// A deliberately tiny, dependency-free test framework.
//
// Rationale: FragFS is a systems project and its test suite must be able to
// build and run offline, with no package manager and no network access. A few
// dozen lines of registration + assertion macros give us exactly what we need
// without pulling in Catch2/GoogleTest and their transitive dependencies.
//
// Usage:
//   #include "test_framework.h"
//
//   TEST_CASE("addition works") {
//       FRAGFS_CHECK_EQ(2 + 2, 4);
//   }
//
//   FRAGFS_TEST_MAIN

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fragfs::test {

struct Failure {
    std::string message;
};

using TestFunction = std::function<void()>;

struct TestCase {
    std::string name;
    TestFunction function;
};

// Function-local static: avoids the static-initialisation-order fiasco, since
// each translation unit's Registrar runs before main() but the registry itself
// is created on first use.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

class Registrar {
public:
    Registrar(std::string name, TestFunction function) {
        registry().push_back({std::move(name), std::move(function)});
    }
};

inline int runAll(const std::string& suiteName) {
    int passed = 0;
    int failed = 0;

    for (const TestCase& test : registry()) {
        try {
            test.function();
            ++passed;
            std::cout << "[ PASS ] " << test.name << '\n';
        } catch (const Failure& failure) {
            ++failed;
            std::cout << "[ FAIL ] " << test.name
                      << "\n         " << failure.message << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[ FAIL ] " << test.name
                      << "\n         unexpected exception: " << error.what() << '\n';
        }
    }

    std::cout << suiteName << ": " << passed << " passed, "
              << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

} // namespace fragfs::test

#define FRAGFS_DETAIL_CONCAT_INNER(a, b) a##b
#define FRAGFS_DETAIL_CONCAT(a, b) FRAGFS_DETAIL_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                          \
    static void FRAGFS_DETAIL_CONCAT(fragfs_test_body_, __LINE__)();             \
    static const ::fragfs::test::Registrar FRAGFS_DETAIL_CONCAT(                 \
        fragfs_test_registrar_, __LINE__)(                                       \
        (name), &FRAGFS_DETAIL_CONCAT(fragfs_test_body_, __LINE__));             \
    static void FRAGFS_DETAIL_CONCAT(fragfs_test_body_, __LINE__)()

#define FRAGFS_CHECK(condition)                                                  \
    do {                                                                         \
        if (!(condition)) {                                                      \
            std::ostringstream fragfs_detail_stream;                             \
            fragfs_detail_stream << __FILE__ << ':' << __LINE__                  \
                                 << ": CHECK failed: " #condition;               \
            throw ::fragfs::test::Failure{fragfs_detail_stream.str()};           \
        }                                                                        \
    } while (false)

#define FRAGFS_CHECK_EQ(actual, expected)                                        \
    do {                                                                         \
        const auto& fragfs_detail_actual = (actual);                             \
        const auto& fragfs_detail_expected = (expected);                         \
        if (!(fragfs_detail_actual == fragfs_detail_expected)) {                 \
            std::ostringstream fragfs_detail_stream;                             \
            fragfs_detail_stream << __FILE__ << ':' << __LINE__                  \
                                 << ": CHECK_EQ failed: " #actual                \
                                 << " == " #expected                             \
                                 << " (actual: " << fragfs_detail_actual         \
                                 << ", expected: " << fragfs_detail_expected     \
                                 << ')';                                         \
            throw ::fragfs::test::Failure{fragfs_detail_stream.str()};           \
        }                                                                        \
    } while (false)

#define FRAGFS_TEST_MAIN                                                         \
    int main() { return ::fragfs::test::runAll(__FILE__); }
