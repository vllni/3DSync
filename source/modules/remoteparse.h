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

// ---------------------------------------------------------------------------
// What a Dropbox answer says about a path
// ---------------------------------------------------------------------------
// get_metadata and create_folder_v2 both describe the same thing in two
// shapes: a 2xx body naming what is there, or a 409 whose "error_summary" is a
// slash-separated reason.  Reading either one wrong is how a sync ends up
// writing into the wrong place, so the reading is here rather than inline.
enum DropboxPathState
{
    DROPBOX_PATH_FOLDER,     // a folder is there — created just now or already
    DROPBOX_PATH_NOT_FOLDER, // something else occupies the path (a file)
    DROPBOX_PATH_MISSING,    // nothing there yet (409 path/not_found)
    DROPBOX_PATH_ERROR       // malformed_path, no_write_permission, unreadable…
};

// status is what Dropbox::_performWithRetry() returned: 0 for a 2xx, otherwise
// the HTTP status.  response is the body that came with it.
DropboxPathState dropboxPathState(int status, const std::string &response);

// Build an FTP URL for a path relative to the login directory.
std::string buildFtpUrl(bool implicitTls, const std::string &host, int port,
                        const std::string &path);

#endif
