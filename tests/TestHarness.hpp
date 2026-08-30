#pragma once

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
        std::cerr << "[FAIL] " << message << '\n';
    }

    void checkContains(std::string_view haystack, std::string_view needle,
                       std::string_view message) {
        check(haystack.find(needle) != std::string_view::npos, message);
    }

    int failures = 0;
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

    void add(std::string name, TestCase testCase) {
        cases_.push_back({std::move(name), std::move(testCase)});
    }

    int run() {
        TestContext context;
        for (const RegisteredCase &testCase : cases_)
            runCase(testCase, context);
        for (const RegisteredCase &testCase : registeredTestCases())
            runCase(testCase, context);
        if (context.failures == 0)
            std::cout << "All " << suiteName_ << " tests passed\n";
        return context.failures == 0 ? 0 : 1;
    }

    template <typename Application>
    int runWithApplication(int &argc, char **argv) {
        Application application(argc, argv);
        return run();
    }

    private:
    static void runCase(const RegisteredCase &testCase, TestContext &context) {
        try {
            testCase.function(context);
        } catch (const std::exception &error) {
            context.check(false, testCase.name + ": " + error.what());
        } catch (...) {
            context.check(false, testCase.name + ": unknown exception");
        }
    }

    std::string suiteName_;
    std::vector<RegisteredCase> cases_;
};

} // namespace openscp::test

using TestContext = openscp::test::TestContext;

#define OPENSCP_TEST(testName, contextName)                                    \
    static void testName(TestContext &contextName);                            \
    [[maybe_unused]] const ::openscp::test::TestRegistrar                      \
        openscp_test_registrar_##testName{#testName, testName};                \
    static void testName(TestContext &contextName)
