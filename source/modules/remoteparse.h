#ifndef MODULES_REMOTEPARSE_H
#define MODULES_REMOTEPARSE_H

#include <map>
#include <string>
#include <vector>

#include "syncprovider.h"

// Response parsing and path building for the remotes, kept free of libcurl,
// libsmb2 and <3ds.h> so it can be exercised directly against recorded server
// responses in the host test build.  The providers are thin wrappers around
// these: everything that decides *what* a response means lives here.

// Change tag for a remote that offers no checksum, only a size and a time.
std::string sizeTimeTag(long long size, long long mtime);

// One <response> from a WebDAV multistatus body.
struct DavEntry
{
    std::string relPath; // relative to the PROPFIND root, '/'-prefixed
    std::string tag;
    bool isCollection;
};

// Parse a multistatus body.  rootHref is the listing root as the server spells
// it in hrefs (path only, trailing slash), used to make paths relative; the
// root's own entry is skipped, as is anything outside it.
void parseDavResponses(const std::string &xml, const std::string &rootHref,
                       std::vector<DavEntry> &out);

// Parse the "entries" array of a Dropbox list_folder response.  root is the
// sync root as an absolute Dropbox path; entries outside it, folders, deleted
// entries and in-flight transfers are skipped.  Comparison of the root is
// case-insensitive because Dropbox paths are.
void parseDropboxEntries(const std::string &response, const std::string &root,
                         std::map<std::string, RemoteFileInfo> &out);

// Build an FTP URL for a path relative to the login directory.
std::string buildFtpUrl(bool implicitTls, const std::string &host, int port,
                        const std::string &path);

#endif
