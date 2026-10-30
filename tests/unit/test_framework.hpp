#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace zeromesh::testing {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

class TestRegistry {
public:
    static TestRegistry& Instance() {
        static TestRegistry instance;
        return instance;
    }

    void Add(const std::string& name, std::function<void()> func) { tests_.push_back({name, std::move(func)}); }

    int RunAll() {
        int passed = 0;
        int failed = 0;

        std::cout << "\n=========================================\n";
        std::cout << "       ZeroMesh C++20 Test Suite         \n";
        std::cout << "=========================================\n";

        for (const auto& test : tests_) {
            std::cout << "[ RUN      ] " << test.name << std::flush;
            auto start = std::chrono::steady_clock::now();
            try {
                test.func();
                auto end = std::chrono::steady_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                std::cout << "\r\033[32m[       OK ]\033[0m " << test.name << " (" << ms / 1000.0 << " ms)\n";
                passed++;
            } catch (const std::exception& ex) {
                std::cout << "\r\033[31m[  FAILED  ]\033[0m " << test.name << "\n";
                std::cout << "             Error: " << ex.what() << "\n";
                failed++;
            } catch (...) {
                std::cout << "\r\033[31m[  FAILED  ]\033[0m " << test.name << "\n";
                std::cout << "             Error: Unknown exception\n";
                failed++;
            }
        }

        std::cout << "-----------------------------------------\n";
        std::cout << "Total Tests: " << tests_.size() << " | "
                  << "\033[32mPassed: " << passed << "\033[0m | " << (failed > 0 ? "\033[31m" : "")
                  << "Failed: " << failed << "\033[0m\n";
        std::cout << "=========================================\n\n";

        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> tests_;
};

#define TEST(test_name)                                                                                                \
    void test_name();                                                                                                  \
    struct test_name##_registrar {                                                                                     \
        test_name##_registrar() { ::zeromesh::testing::TestRegistry::Instance().Add(#test_name, test_name); }          \
    } test_name##_instance;                                                                                            \
    void test_name()

#define ASSERT_TRUE(condition)                                                                                         \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::ostringstream ss;                                                                                     \
            ss << "Assertion failed: " << #condition << " (" << __FILE__ << ":" << __LINE__ << ")";                    \
            throw std::runtime_error(ss.str());                                                                        \
        }                                                                                                              \
    } while (0)

#define ASSERT_FALSE(condition) ASSERT_TRUE(!(condition))

#define ASSERT_EQ(val1, val2)                                                                                          \
    do {                                                                                                               \
        if (!((val1) == (val2))) {                                                                                     \
            std::ostringstream ss;                                                                                     \
            ss << "Equality assertion failed: " << #val1 << " == " << #val2 << " (" << __FILE__ << ":" << __LINE__     \
               << ")";                                                                                                 \
            throw std::runtime_error(ss.str());                                                                        \
        }                                                                                                              \
    } while (0)

#define ASSERT_NE(val1, val2)                                                                                          \
    do {                                                                                                               \
        if ((val1) == (val2)) {                                                                                        \
            std::ostringstream ss;                                                                                     \
            ss << "Inequality assertion failed: " << #val1 << " != " << #val2 << " (" << __FILE__ << ":" << __LINE__   \
               << ")";                                                                                                 \
            throw std::runtime_error(ss.str());                                                                        \
        }                                                                                                              \
    } while (0)

} // namespace zeromesh::testing
