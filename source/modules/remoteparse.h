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

// MKCOL on a collection that already exists answers 405 Method Not Allowed
// (RFC 4918, 9.3.1).  For a sync that is the ordinary case — every run after
// the first, and every upload into an existing folder — so the request passes
// it to WebDavRemote::_performWithRetry() as expected, not as an error.
const long WEBDAV_MKCOL_EXISTS_STATUS = 405;

enum WebDavMkcolResult
{
    WEBDAV_MKCOL_CREATED,
    WEBDAV_MKCOL_EXISTS,
    WEBDAV_MKCOL_FAILED // 409 (a parent is missing), 403, 507, a network error…
};

// res is what WebDavRemote::_performWithRetry() returned: 0 for a 2xx,
// otherwise the HTTP status or a curl code.  status is the HTTP status.
WebDavMkcolResult webdavMkcolResult(int res, long status);

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

// How much of the FTP session is protected.
enum FtpTlsMode
{
    FTP_TLS_NONE,     // plaintext — credentials cross the network in the clear
    FTP_TLS_TRY,      // AUTH TLS when the server offers it, plaintext otherwise
    FTP_TLS_REQUIRE,  // AUTH TLS or fail
    FTP_TLS_IMPLICIT  // ftps:// on port 990
};

// "anonymous" or "ftp", any case: the two names FTP servers take as an
// anonymous login, whose password is by convention not a secret.
bool isAnonymousFtpUser(const std::string &user);

// TLS=try asks for AUTH TLS and libcurl falls back to plaintext only when the
// server refuses AUTH itself.  Stock vsftpd (allow_anon_ssl=NO) accepts AUTH
// and then refuses an anonymous login inside the TLS session with 530, which
// reaches the provider as "login denied".  Whether to reconnect once without
// TLS: only for TLS=try, only for an anonymous login, only on a denied login.
// A named user never falls back — that would resend a real password in
// plaintext right after the server turned it down over TLS.  Pass the mode
// currently in effect, so once TLS has been dropped this says no again.
bool ftpShouldRetryWithoutTls(FtpTlsMode mode, const std::string &user, bool loginDenied);

// What an FTP request was for, as far as a refusal is concerned.
enum FtpRequest
{
    FTP_REQUEST_LIST,   // listing a directory, including the Path= check in connect()
    FTP_REQUEST_UPLOAD, // STOR, which creates missing directories on the way
    FTP_REQUEST_FILE    // RETR of a listed file, or its MDTM/SIZE
};

// The two refusals that are about one path rather than the whole session.
enum FtpPathRefusal
{
    // A directory could not be entered or created.  libcurl CWDs into every
    // directory on the way and reports a 550 to any of them, or to the MKD it
    // sends for CURLOPT_FTP_CREATE_MISSING_DIRS, as CURLE_REMOTE_ACCESS_DENIED
    // — never as "not found".  FTP gives a missing directory and a forbidden
    // one the same 550, so this cannot tell them apart.
    FTP_REFUSED_DIRECTORY,
    // The directory is there but the file is not: CURLE_REMOTE_FILE_NOT_FOUND
    // or CURLE_FTP_COULDNT_RETR_FILE.
    FTP_REFUSED_FILE
};

enum FtpRefusalOutcome
{
    FTP_OUTCOME_EMPTY,     // nothing there yet: an empty listing, said only through debugf()
    FTP_OUTCOME_THIS_FILE, // this file fails and the run goes on; the caller reports it
    FTP_OUTCOME_FATAL      // stop the run
};

// A listing reads a refused directory as "nothing there yet": the sync folder
// is missing on every first sync, since FTP creates directories only when it
// uploads into them.  A forbidden folder then surfaces at upload, where the MKD
// or the STOR fails, and costs that one file — the other files and the other
// [Paths] entries may well be writable.  Reading a file that was just listed
// from a directory the server refuses is not a first-sync case, so it stops.
FtpRefusalOutcome ftpRefusalOutcome(FtpRequest request, FtpPathRefusal refusal);

// The commands that move an uploaded temp file over the real one: DELE (marked
// '*' so its failure is ignored — the file is absent on a first upload), RNFR,
// RNTO.  remotePath and tempPath are relative to the login directory; both are
// in the same directory.
//
// They are sent as CURLOPT_POSTQUOTE, which libcurl sends *after* it has CWD'd
// into the directory of the request's URL, so they name the file alone: a path
// from the login directory would be looked up below the directory the session
// is already in, and RNFR fails.  Nor can a path from the login directory be
// sent before a CWD (CURLOPT_QUOTE): a reused connection starts the request
// wherever the last one left it.
std::vector<std::string> ftpReplaceCommands(const std::string &remotePath,
                                            const std::string &tempPath);

// vsftpd 3.x (strict_ssl_read_eof, on by default) refuses an upload whose TLS
// data connection ends without a close_notify, with 426 after the data has
// been stored — and devkitPro's libcurl 8.4 never sends one (its mbedTLS
// backend has no call to mbedtls_ssl_close_notify).  Whether a failed upload
// is that case: TLS in effect, libcurl's "partial file" (CURLE_PARTIAL_FILE),
// and the final reply 426.  It fails the same way on every attempt, so it is
// not retried; the provider checks what arrived instead.
bool ftpUploadEndedUnclean(FtpTlsMode mode, bool partialTransfer, long replyCode);

// Whether the temp file the server kept is the whole upload.  TLS
// authenticates every record, so what arrived is what was sent; close_notify
// only guards against the stream being cut short, and the length is that
// check made by hand.  -1 (unknown) on either side is never intact.
bool ftpUploadArrivedIntact(long long remoteSize, long long localSize);

// Build an FTP URL for a path relative to the login directory.
std::string buildFtpUrl(bool implicitTls, const std::string &host, int port,
                        const std::string &path);

#endif
