#ifndef UTILS_TIMEUTIL_H
#define UTILS_TIMEUTIL_H

#include <ctime>
#include <string>

// Convert a server timestamp to time_t (UTC), or 0 when it cannot be parsed.
//
// Two formats turn up: RFC 3339 ("2024-05-28T14:32:00.000Z") in Drive's JSON,
// and RFC 7231 ("Tue, 28 May 2024 14:32:00 GMT") in the HTTP Date header that
// every provider returns.  mktime() reads the fields as local time; the 3DS
// clock is kept as UTC, so the two line up.
time_t parseServerTime(const std::string &value);

#endif
