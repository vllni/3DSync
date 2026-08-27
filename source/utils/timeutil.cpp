#include "timeutil.h"

#include <stdio.h>
#include <string.h>

time_t parseServerTime(const std::string &value)
{
    static const char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm t = {};
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    char monthName[8] = {};

    if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        // RFC 3339
    }
    else if (sscanf(value.c_str(), "%*3s, %d %3s %d %d:%d:%d",
                    &day, monthName, &year, &hour, &min, &sec) == 6)
    {
        month = 0;
        for (int i = 0; i < 12; i++)
            if (strncmp(monthName, months[i], 3) == 0)
                month = i + 1;
        if (month == 0)
            return 0;
    }
    else
    {
        return 0;
    }

    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = 0;
    return mktime(&t);
}
