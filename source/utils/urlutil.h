#ifndef UTILS_URLUTIL_H
#define UTILS_URLUTIL_H

#include <string>

// Percent-encode a '/'-separated path, leaving the separators intact.
std::string urlEncodePath(const std::string &path);

// Reverse of urlEncodePath.
std::string urlDecode(const std::string &value);

#endif
