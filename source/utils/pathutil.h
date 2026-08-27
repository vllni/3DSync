#ifndef UTILS_PATHUTIL_H
#define UTILS_PATHUTIL_H

#include <string>

// Suffix used for a transfer that is still in flight, locally and remotely.
extern const char *TEMP_SUFFIX;

// Clean a remote path: '\' becomes '/', repeated separators collapse, and
// leading and trailing separators are removed.  With leadingSlash a non-empty
// result is prefixed with '/' — Dropbox and WebDAV want "/a/b" where SMB and
// FTP want "a/b", and an empty path stays empty for every remote root.
std::string normalizeRemotePath(const std::string &path, bool leadingSlash = false);

// True if the path names one of our in-flight transfers, which listings skip.
bool isTempTransferName(const std::string &path);

#endif
