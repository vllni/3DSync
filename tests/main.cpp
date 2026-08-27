#include <stdio.h>
#include <string.h>

#include "framework.h"

std::vector<std::string> &currentFailures();

// Runs every registered test.  An optional argument filters by substring match
// against "suite.name", which is how a single case gets re-run while fixing it.
int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : NULL;

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    const char *lastSuite = "";

    for (auto &testCase : testRegistry())
    {
        std::string fullName = std::string(testCase.suite) + "." + testCase.name;
        if (filter != NULL && fullName.find(filter) == std::string::npos)
        {
            skipped++;
            continue;
        }

        if (strcmp(lastSuite, testCase.suite) != 0)
        {
            printf("\n%s\n", testCase.suite);
            lastSuite = testCase.suite;
        }

        currentFailures().clear();
        testCase.fn();

        if (currentFailures().empty())
        {
            printf("  ok    %s\n", testCase.name);
            passed++;
        }
        else
        {
            printf("  FAIL  %s\n", testCase.name);
            for (auto &failure : currentFailures())
                printf("    %s\n", failure.c_str());
            failed++;
        }
    }

    printf("\n%d passed, %d failed", passed, failed);
    if (skipped > 0)
        printf(", %d not matching filter", skipped);
    printf("\n");

#ifndef HAVE_MBEDTLS
    printf("\nNOTE: hash tests were left out of this build (no mbedtls headers).\n"
           "      Install libmbedtls-dev to include them.\n");
#endif

    return failed == 0 ? 0 : 1;
}
