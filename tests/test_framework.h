#pragma once

// A ~60-line test framework.
//
// VCache has no third-party dependencies, and pulling in GoogleTest for
// assert-and-report would be the largest dependency in the project. If the suite
// ever outgrows this, swapping in Catch2 or GoogleTest is a contained change.
//
// Usage:
//     VCACHE_TEST(MyThing_DoesTheThing) {
//         CHECK(condition);
//         CHECK_EQ(actual, expected);
//     }
//     int main() { return testing::runAll(); }

#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace testing {

struct AssertionFailure : std::exception {
    std::string message;

    explicit AssertionFailure(std::string m) : message(std::move(m)) {}

    const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
    std::string name;
    void (*fn)();
};

// Function-local static so registration order across translation units is safe:
// the vector is created on first use, before any Registrar can touch it.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

// Constructed at static-init time by the VCACHE_TEST macro, which is how a test
// adds itself to the registry without main() naming it.
struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back(TestCase{name, fn}); }
};

// Comparison lives in a function, not in the CHECK_EQ macro body, for a lifetime
// reason. A macro that did
//
//     const auto& actual = (expr);   // expr == *db.get("key")
//
// would bind a reference into a temporary (the std::optional that get() returned
// by value). That temporary dies at the end of *that* statement, so the very next
// line -- the comparison -- would read freed memory. Passing the expression as a
// function argument instead keeps every temporary alive for the whole call, which
// is exactly what is needed, and it still avoids copying.
template <typename Actual, typename Expected>
void checkEqual(const Actual& actual,
                const Expected& expected,
                const char* actualExpr,
                const char* expectedExpr,
                const char* file,
                int line) {
    if (actual == expected) {
        return;
    }

    std::ostringstream oss;
    oss << "           at " << file << ":" << line
        << "\n           CHECK_EQ(" << actualExpr << ", " << expectedExpr << ") failed"
        << "\n             actual:   " << actual
        << "\n             expected: " << expected;
    throw AssertionFailure(oss.str());
}

inline int runAll() {
    int failed = 0;
    std::cout << "Running " << registry().size() << " tests\n\n";

    for (const TestCase& test : registry()) {
        try {
            test.fn();
            std::cout << "  [ PASS  ] " << test.name << "\n";
        } catch (const AssertionFailure& failure) {
            std::cout << "  [ FAIL  ] " << test.name << "\n" << failure.message << "\n";
            ++failed;
        } catch (const std::exception& error) {
            std::cout << "  [ ERROR ] " << test.name << " threw: " << error.what() << "\n";
            ++failed;
        }
    }

    const int total = static_cast<int>(registry().size());
    std::cout << "\n" << (total - failed) << "/" << total << " tests passed\n";
    return failed == 0 ? 0 : 1;  // non-zero exit code is what CTest checks
}

}  // namespace testing

#define VCACHE_TEST(name)                                               \
    static void name();                                                 \
    static ::testing::Registrar vcache_registrar_##name(#name, name);   \
    static void name()

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            std::ostringstream vcache_oss;                              \
            vcache_oss << "           at " << __FILE__ << ":" << __LINE__ \
                       << "\n           CHECK(" #condition ") failed";  \
            throw ::testing::AssertionFailure(vcache_oss.str());         \
        }                                                               \
    } while (false)

#define CHECK_EQ(actual, expected)                                      \
    ::testing::checkEqual((actual), (expected), #actual, #expected, __FILE__, __LINE__)
