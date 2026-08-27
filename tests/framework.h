#ifndef TESTS_FRAMEWORK_H
#define TESTS_FRAMEWORK_H

// A test framework small enough to vendor: the build image has no gtest, and
// pulling one in for a homebrew project is more dependency than it is worth.
//
//   TEST(json, decodes_escapes) { CHECK_EQ(jsonString(doc, "a"), "b\n"); }
//
// Assertions record a failure and keep going, so one broken case reports every
// mismatch in that test rather than only the first.

#include <sstream>
#include <string>
#include <vector>

struct TestCase
{
    const char *suite;
    const char *name;
    void (*fn)();
};

std::vector<TestCase> &testRegistry();
void testFail(const char *file, int line, const std::string &message);

struct TestRegistrar
{
    TestRegistrar(const char *suite, const char *name, void (*fn)());
};

// Render a value for a failure message.
template <typename T>
std::string testDescribe(const T &value)
{
    std::ostringstream out;
    out << value;
    return out.str();
}
inline std::string testDescribe(const std::string &value) { return "\"" + value + "\""; }
inline std::string testDescribe(bool value) { return value ? "true" : "false"; }

#define TEST(suite_name, test_name)                                              \
    static void suite_name##_##test_name();                                       \
    static TestRegistrar registrar_##suite_name##_##test_name(                    \
        #suite_name, #test_name, suite_name##_##test_name);                        \
    static void suite_name##_##test_name()

#define CHECK(condition)                                                         \
    do                                                                            \
    {                                                                             \
        if (!(condition))                                                         \
            testFail(__FILE__, __LINE__, "CHECK failed: " #condition);             \
    } while (0)

#define CHECK_EQ(actual, expected)                                               \
    do                                                                            \
    {                                                                             \
        auto actualValue = (actual);                                              \
        auto expectedValue = (expected);                                          \
        if (!(actualValue == expectedValue))                                      \
            testFail(__FILE__, __LINE__,                                          \
                     std::string(#actual) + " == " + #expected + "\n      actual:   " + \
                         testDescribe(actualValue) + "\n      expected: " +        \
                         testDescribe(expectedValue));                            \
    } while (0)

#define CHECK_STR_EQ(actual, expected) CHECK_EQ(std::string(actual), std::string(expected))

#endif
