#pragma once

#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openscp::test {

class TestContext {
    public:
    void check(bool condition, std::string_view message) {
        if (condition)
            return;
        ++failures;
        std::cerr << "[FAIL] " << caseLabel() << message << '\n';
    }

    void checkContains(std::string_view haystack, std::string_view needle,
                       std::string_view message) {
        check(haystack.find(needle) != std::string_view::npos, message);
    }

    // Checks an operation that reports its failure through `error`, which is
    // read only here, once the operation has run. In
    // check(op(error), "...: " + error) the order in which C++ evaluates the
    // two arguments is unspecified, and MSVC and GCC build the message first.
    void checkWithError(bool condition, std::string_view message,
                        const std::string &error) {
        if (!condition)
            check(false, std::string(message) + error);
    }

    // Notes a case this system cannot run, such as one that needs symbolic
    // links where only administrators may make them. It is not a failure.
    void skip(std::string_view message) {
        ++skipped;
        std::cerr << "[SKIP] " << caseLabel() << message << '\n';
    }

    int failures = 0;
    int skipped = 0;
    // The case that is running, which each failure names.
    std::string caseName;

    private:
    std::string caseLabel() const {
        return caseName.empty() ? std::string() : caseName + ": ";
    }
};

using TestCase = std::function<void(TestContext &)>;

struct RegisteredCase {
    std::string name;
    TestCase function;
};

inline std::vector<RegisteredCase> &registeredTestCases() {
    static std::vector<RegisteredCase> cases;
    return cases;
}

class TestRegistrar final {
    public:
    TestRegistrar(std::string name, TestCase testCase) {
        registeredTestCases().push_back({std::move(name), std::move(testCase)});
    }
};

class TestHarness {
    public:
    explicit TestHarness(std::string suiteName)
        : suiteName_(std::move(suiteName)) {}

    // Runs every case, or only the cases named on the command line;
    // --list prints their names instead.
    int run(int argc, char **argv) {
        std::vector<std::string> names;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]) == "--list") {
                for (const RegisteredCase &testCase : registeredTestCases())
                    std::cout << testCase.name << '\n';
                return 0;
            }
            names.emplace_back(argv[index]);
        }
        TestContext context;
        std::vector<std::string> unknown;
        for (const RegisteredCase *testCase : casesNamed(names, unknown))
            runCase(*testCase, context);
        for (const std::string &name : unknown)
            context.check(false, "no test case is named " + name);
        if (context.failures == 0) {
            std::cout << "All " << suiteName_ << " tests passed";
            if (context.skipped > 0)
                std::cout << " (" << context.skipped << " cases skipped)";
            std::cout << '\n';
        }
        return context.failures == 0 ? 0 : 1;
    }

    template <typename Application>
    int runWithApplication(int &argc, char **argv) {
        // Qt takes its own options out of the arguments.
        Application application(argc, argv);
        return run(argc, argv);
    }

    // The registered cases with these names, in their order, or all of them
    // when no name is given; the names no case has go to unknown.
    static std::vector<const RegisteredCase *>
    casesNamed(const std::vector<std::string> &names,
               std::vector<std::string> &unknown) {
        std::vector<const RegisteredCase *> cases;
        for (const RegisteredCase &testCase : registeredTestCases()) {
            if (names.empty() || std::find(names.begin(), names.end(),
                                           testCase.name) != names.end())
                cases.push_back(&testCase);
        }
        for (const std::string &name : names) {
            if (std::none_of(cases.begin(), cases.end(),
                             [&name](const RegisteredCase *testCase) {
                                 return testCase->name == name;
                             }))
                unknown.push_back(name);
        }
        return cases;
    }

    private:
    static void runCase(const RegisteredCase &testCase, TestContext &context) {
        context.caseName = testCase.name;
        try {
            testCase.function(context);
        } catch (const std::exception &error) {
            context.check(false,
                          std::string("unexpected exception: ") + error.what());
        } catch (...) {
            context.check(false, "unknown exception");
        }
        context.caseName.clear();
    }

    std::string suiteName_;
};

} // namespace openscp::test

using TestContext = openscp::test::TestContext;

#define OPENSCP_TEST(testName, contextName)                                    \
    static void testName(TestContext &contextName);                            \
    [[maybe_unused]] const ::openscp::test::TestRegistrar                      \
        openscp_test_registrar_##testName{#testName, testName};                \
    static void testName(TestContext &contextName)
