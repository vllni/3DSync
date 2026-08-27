// Server clock parsing.  This is what the clock-skew warning depends on, and
// it silently did nothing while it only understood one of the two formats.

#include "framework.h"

#include "../source/utils/timeutil.h"

TEST(time, parses_rfc3339_from_drive_json)
{
    time_t parsed = parseServerTime("2024-05-28T14:32:00.000Z");
    CHECK(parsed != 0);

    struct tm expected = {};
    expected.tm_year = 2024 - 1900;
    expected.tm_mon = 4;
    expected.tm_mday = 28;
    expected.tm_hour = 14;
    expected.tm_min = 32;
    expected.tm_isdst = 0;
    CHECK_EQ((long long)parsed, (long long)mktime(&expected));
}

TEST(time, parses_rfc7231_from_the_date_header)
{
    // Every provider returns this form; feeding it to an RFC 3339 parser is the
    // bug this function exists to prevent.
    time_t parsed = parseServerTime("Tue, 28 May 2024 14:32:00 GMT");
    CHECK(parsed != 0);
    CHECK_EQ((long long)parsed, (long long)parseServerTime("2024-05-28T14:32:00.000Z"));
}

TEST(time, parses_every_month_name)
{
    const char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; i++)
    {
        std::string header = std::string("Mon, 01 ") + months[i] + " 2024 00:00:00 GMT";
        time_t parsed = parseServerTime(header);
        CHECK(parsed != 0);

        struct tm converted = {};
        time_t copy = parsed;
        gmtime_r(&copy, &converted);
        // gmtime of a value built with mktime shifts by the local offset, so
        // compare against the same round trip rather than a fixed month.
        struct tm expected = {};
        expected.tm_year = 2024 - 1900;
        expected.tm_mon = i;
        expected.tm_mday = 1;
        expected.tm_isdst = 0;
        CHECK_EQ((long long)parsed, (long long)mktime(&expected));
    }
}

TEST(time, unparseable_values_return_zero)
{
    // 0 means "unknown", which suppresses the skew warning rather than
    // reporting a 55-year skew against the epoch.
    CHECK_EQ((long long)parseServerTime(""), (long long)0);
    CHECK_EQ((long long)parseServerTime("not a date"), (long long)0);
    CHECK_EQ((long long)parseServerTime("Tue, 28 Foo 2024 14:32:00 GMT"), (long long)0);
    CHECK_EQ((long long)parseServerTime("2024-05-28"), (long long)0);
}
