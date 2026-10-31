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

// True if a relative path from a remote listing ("/Game/001.sav") is safe to
// append to a local folder: it starts with '/', and has no empty, "." or ".."
// segment, no backslash and no control character.  Anything else could name
// a file outside the folder it is synced into ("/../../boot.firm"), so the
// engine refuses it before deciding anything.
bool isSafeRelativePath(const std::string &relPath);

#endif
