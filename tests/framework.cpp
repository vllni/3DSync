#include "framework.h"

#include <stdio.h>
#include <string.h>

std::vector<TestCase> &testRegistry()
{
    static std::vector<TestCase> registry;
    return registry;
}

TestRegistrar::TestRegistrar(const char *suite, const char *name, void (*fn)())
{
    TestCase testCase = {suite, name, fn};
    testRegistry().push_back(testCase);
}

// Failures of the test currently running, collected by main().
std::vector<std::string> &currentFailures()
{
    static std::vector<std::string> failures;
    return failures;
}

void testFail(const char *file, int line, const std::string &message)
{
    char location[512];
    snprintf(location, sizeof(location), "%s:%d: ", file, line);
    currentFailures().push_back(std::string(location) + message);
}
