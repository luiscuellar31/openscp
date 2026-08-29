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

class TestHarness {
    public:
    using TestCase = std::function<void(TestContext &)>;

    explicit TestHarness(std::string suiteName)
        : suiteName_(std::move(suiteName)) {}

    void add(std::string name, TestCase testCase) {
        cases_.push_back({std::move(name), std::move(testCase)});
    }

    int run() {
        TestContext context;
        for (const RegisteredCase &testCase : cases_) {
            try {
                testCase.function(context);
            } catch (const std::exception &error) {
                context.check(false, testCase.name + ": " + error.what());
            } catch (...) {
                context.check(false, testCase.name + ": unknown exception");
            }
        }
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
    struct RegisteredCase {
        std::string name;
        TestCase function;
    };

    std::string suiteName_;
    std::vector<RegisteredCase> cases_;
};

} // namespace openscp::test

using TestContext = openscp::test::TestContext;
