#ifndef UTILS_URLUTIL_H
#define UTILS_URLUTIL_H

#include <string>

// Percent-encode a '/'-separated path, leaving the separators intact.
std::string urlEncodePath(const std::string &path);

// Reverse of urlEncodePath.
std::string urlDecode(const std::string &value);

// "https://user:pass@host/dav/" -> "https://host/dav/": a URL with any
// credentials in it removed, for showing it or keying state by it.  A URL
// without them comes back unchanged.
std::string urlWithoutCredentials(const std::string &url);

#endif
