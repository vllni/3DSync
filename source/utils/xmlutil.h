#ifndef UTILS_XMLUTIL_H
#define UTILS_XMLUTIL_H

#include <string>

// Just enough XML reading for a WebDAV multistatus response.  No XML library is
// available on the 3DS, and servers vary in how they spell the DAV namespace
// (d:, D:, lp1:, none at all), so prefixes are removed first and everything
// afterwards matches on local names.

// Remove namespace prefixes from tag names: <d:href> becomes <href>.
// Attribute values are left alone.
std::string stripXmlNamespaces(const std::string &xml);

// Text content of the first <tag>…</tag> between from and to.  Returns "" when
// the tag is absent, self-closing, or its closing tag falls outside the range.
std::string xmlTagText(const std::string &xml, const std::string &tag,
                       size_t from, size_t to);

// Strip the W/ prefix and surrounding quotes from an ETag so the value compares
// equal however the server chose to quote it.
std::string normalizeEtag(std::string etag);

#endif
