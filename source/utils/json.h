#ifndef UTILS_JSON_H
#define UTILS_JSON_H

#include <string>
#include <vector>

// Minimal JSON reading for the shapes the APIs actually return.  No JSON
// library is available on the 3DS, and every provider was hand-rolling the same
// three or four lookups.  These are deliberately lookups rather than a parser:
// they find "key": value pairs anywhere in the document, which is enough for
// flat API responses and cannot be confused by nesting in practice.

// Value of a string member, with escapes decoded ("" when absent).
std::string jsonString(const std::string &json, const std::string &key);

// True only when the member is present and literally true.
bool jsonTrue(const std::string &json, const std::string &key);

// Value of an integer member; returns false and leaves out untouched when the
// member is absent.
bool jsonInt(const std::string &json, const std::string &key, long long &out);

// Objects of a named array, one string each.  Brace counting is string-aware,
// so a value containing '{' or '}' cannot split an object in the wrong place.
void jsonSplitArray(const std::string &json, const std::string &key,
                    std::vector<std::string> &out);

// Escape a string for a JSON body: quotes, backslashes and control characters.
std::string jsonEscape(const std::string &value);

// Escape for a JSON value carried in an HTTP header (Dropbox-API-Arg), which
// must be pure ASCII: every non-ASCII code point becomes \uXXXX, using a
// surrogate pair above the BMP.
std::string jsonEscapeAscii(const std::string &value);

// Decode the escapes jsonEscape produces, \uXXXX included.
std::string jsonUnescape(const std::string &value);

#endif
