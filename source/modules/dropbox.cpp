#include "dropbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "../utils/debug.h"
#include "../utils/fsutil.h"
#include "../utils/hash.h"
#include "../utils/json.h"
#include "../utils/pathutil.h"
#include "remoteparse.h"

// Dropbox rejects a single-request upload above 150 MB; larger files need the
// upload_session endpoints, which nothing on a 3DS SD card should require.
static const long long MAX_UPLOAD_BYTES = 150LL * 1024 * 1024;

Dropbox::Dropbox(const std::string &appKey, const std::string &appSecret,
                 const std::string &refreshToken, const std::string &basePath,
                 const std::string &directToken)
    : _appKey(appKey), _appSecret(appSecret), _refreshToken(refreshToken),
      _token(directToken), _fatalError(false)
{
    // Normalise to "" or "/a/b" — Dropbox wants the root as an empty string.
    _basePath = normalizeRemotePath(basePath, true);
}

bool Dropbox::hasFatalError() const { return _fatalError; }

std::string Dropbox::_pathFor(const std::string &path) const
{
    return _basePath + normalizeRemotePath(path, true);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

bool Dropbox::_refreshAccessToken()
{
    printf("Refreshing Dropbox access token...\n");

    std::string body = "grant_type=refresh_token&refresh_token=" + _refreshToken;
    // A PKCE app authenticates with its key alone; a confidential app also
    // sends the secret.
    body += "&client_id=" + _appKey;
    if (!_appSecret.empty())
        body += "&client_secret=" + _appSecret;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    _curl.reset();
    _curl.setURL(std::string("https://api.dropbox.com/oauth2/token"));
    _curl.setHeaders(headers);
    _curl.setPostData(body);
    int res = _performWithRetry();
    curl_slist_free_all(headers);

    if (res != 0)
    {
        printf(CONSOLE_RED "  FATAL: Dropbox token refresh failed." CONSOLE_RESET "\n");
        printf("  The refresh token may have been revoked.\n");
        printf("  Re-run the configurator to obtain a new one.\n");
        debugf("token refresh returned %d\n", res);
        _fatalError = true;
        return false;
    }

    std::string accessToken = jsonString(_curl.getResponse(), "access_token");
    if (accessToken.empty())
    {
        printf("Failed to parse access_token from refresh response\n");
        debugBody("refresh response", _curl.getResponse());
        _fatalError = true;
        return false;
    }
    debugf("access token refreshed, %d chars\n", (int)accessToken.size());

    _token = accessToken;
    return true;
}

bool Dropbox::connect()
{
    if (!_refreshToken.empty())
    {
        if (_appKey.empty())
        {
            printf(CONSOLE_RED "Dropbox: RefreshToken needs AppKey." CONSOLE_RESET "\n");
            _fatalError = true;
            return false;
        }
        if (!_refreshAccessToken())
            return false;
    }

    if (_token.empty())
    {
        printf(CONSOLE_RED "Dropbox: no token configured." CONSOLE_RESET "\n");
        _fatalError = true;
        return false;
    }

    return validateToken();
}

bool Dropbox::validateToken()
{
    if (_rpc("https://api.dropboxapi.com/2/check/user", "{\"query\":\"3DSync\"}") != 0)
    {
        printf(CONSOLE_RED "Dropbox token rejected." CONSOLE_RESET "\n");
        if (_refreshToken.empty())
            printf("  Static tokens expire after ~4 hours. Re-run the\n"
                   "  configurator to get a refresh token instead.\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

int Dropbox::_rpc(const std::string &endpoint, const std::string &jsonBody,
                  long expectedStatus)
{
    std::string auth("Authorization: Bearer " + _token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    _curl.reset();
    _curl.setURL(endpoint);
    _curl.setHeaders(headers);
    _curl.setPostData(jsonBody);
    int res = _performWithRetry(NULL, expectedStatus);
    curl_slist_free_all(headers);
    return res;
}

int Dropbox::_performWithRetry(FILE *uploadFile, long expectedStatus)
{
    if (_fatalError)
        return -1;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (attempt > 0 && uploadFile != NULL)
            rewind(uploadFile); // the previous attempt consumed part of the file

        int curlRes = _curl.perform();
        long status = _curl.getStatusCode();

        std::string date = _curl.getResponseHeader("Date");
        if (!date.empty())
            _lastServerTime = date;

        // Network-level failure (timeout, DNS, TLS, …)
        if (curlRes != 0)
        {
            printf("  Network error (attempt %d/3)\n", attempt + 1);
            debugf("  %s\n", _curl.getURL().c_str());
            if (attempt < 2)
            {
                _curl.rewindDownloadFile(); // discard any partial body
                svcSleepThread(2000000000LL);
                continue;
            }
            return curlRes;
        }

        if (status >= 200 && status < 300)
            return 0;

        // A status the caller asked about is its business to report, not an
        // error to announce here — see expectedStatus in the header.
        bool expected = (expectedStatus != 0 && status == expectedStatus);
        if (!expected)
            printf(CONSOLE_RED "  Dropbox API error: HTTP %ld" CONSOLE_RESET "\n", status);
        debugf("  HTTP %ld%s for %s (attempt %d/3)\n", status,
               expected ? " (expected by the caller)" : "",
               _curl.getURL().c_str(), attempt + 1);

        // Rate limited — Dropbox states the wait in Retry-After.
        if (status == 429 && attempt < 2)
        {
            long wait = strtol(_curl.getResponseHeader("Retry-After").c_str(), NULL, 10);
            if (wait < 1)
                wait = 10;
            if (wait > 30)
                wait = 30;
            printf("  Rate limited, waiting %ld seconds...\n", wait);
            _curl.rewindDownloadFile();
            svcSleepThread((s64)wait * 1000000000LL);
            continue;
        }

        if (status >= 500 && status < 600 && attempt < 2)
        {
            printf("  Server error, waiting 5 seconds...\n");
            _curl.rewindDownloadFile();
            svcSleepThread(5000000000LL);
            continue;
        }

        // Fatal: the token is invalid, expired or revoked.
        if (status == 401)
        {
            printf(CONSOLE_RED "  FATAL: Authentication failed." CONSOLE_RESET "\n");
            printf("  The Dropbox token is invalid or has expired.\n");
            _fatalError = true;
            return -401;
        }

        // Fatal: the app lacks the permission for this call.
        if (status == 403)
        {
            printf(CONSOLE_RED "  FATAL: Access denied (HTTP 403)." CONSOLE_RESET "\n");
            printf("  The Dropbox app is missing the required permissions.\n");
            _fatalError = true;
            return -403;
        }

        // 400 (malformed request) and 409 (path conflict, insufficient space,
        // …) carry the reason in the body and affect only this call.
        std::string body = _curl.getResponse();
        // Cut at 200 characters so one error cannot fill the screen; debug
        // mode has already printed the whole body from perform().
        if (!expected && !body.empty())
            printf(CONSOLE_RED "  %s" CONSOLE_RESET "\n", body.substr(0, 200).c_str());

        return (int)status;
    }
    debugf("  giving up on %s after 3 attempts\n", _curl.getURL().c_str());
    return -1; // exhausted retries
}

// ---------------------------------------------------------------------------
// Sync operations
// ---------------------------------------------------------------------------

// The reason behind a 409 that _rpc() was told to expect, and so kept off the
// console.  Called only where the expectation turned out to be wrong.
void Dropbox::_report409Reason()
{
    printf(CONSOLE_RED "  Dropbox API error: HTTP 409" CONSOLE_RESET "\n");
    std::string reason = jsonString(_curl.getResponse(), "error_summary");
    if (!reason.empty())
        printf(CONSOLE_RED "  %s" CONSOLE_RESET "\n", reason.c_str());
}

std::string Dropbox::ensureRoot(const std::string &remoteName)
{
    std::string root = _pathFor(remoteName);
    if (root.empty())
        return root; // the Dropbox root always exists

    // Ask before creating.  The folder is already there on every run but the
    // first, so this is a read where create_folder_v2 was a write that came
    // back 409 — and it reports what is at the path as a field rather than as
    // the wording of an error.  The cost is one extra round trip, first run
    // only.  409 is expected here: it is how "not there yet" arrives.
    std::string body = "{\"path\":\"" + jsonEscape(root) + "\"}";
    int res = _rpc("https://api.dropboxapi.com/2/files/get_metadata", body, 409);

    switch (dropboxPathState(res, _curl.getResponse()))
    {
    case DROPBOX_PATH_FOLDER:
        return root;

    case DROPBOX_PATH_MISSING:
        debugf("%s does not exist yet\n", root.c_str());
        return _createFolder(root);

    case DROPBOX_PATH_NOT_FOLDER:
        // A file sits where the sync folder belongs.  It cannot be created
        // over and every later call would fail on it, so stop on this entry.
        printf(CONSOLE_RED "Dropbox: %s is not a folder\n" CONSOLE_RESET, root.c_str());
        debugf("get_metadata described %s as something other than a folder\n",
               root.c_str());
        return "";

    case DROPBOX_PATH_ERROR:
        break;
    }

    if (_fatalError)
        return "";
    if (res == 409)
        _report409Reason(); // suppressed on the assumption it was not_found
    printf("Dropbox: cannot use %s\n", root.c_str());
    debugf("get_metadata returned %d for %s\n", res, root.c_str());
    return "";
}

// A root that ensureRoot's probe found missing.  409 path/conflict/folder is
// still success: a second console syncing the same account can create it
// between the probe and this call, and that is the outcome we wanted anyway.
std::string Dropbox::_createFolder(const std::string &root)
{
    std::string body = "{\"path\":\"" + jsonEscape(root) + "\",\"autorename\":false}";
    int res = _rpc("https://api.dropboxapi.com/2/files/create_folder_v2", body, 409);

    if (dropboxPathState(res, _curl.getResponse()) == DROPBOX_PATH_FOLDER)
    {
        if (res == 409)
            debugf("%s appeared between the probe and the create\n", root.c_str());
        return root;
    }

    if (_fatalError)
        return "";
    if (res == 409)
        _report409Reason(); // suppressed on the assumption it was a conflict
    printf("Dropbox: cannot create %s\n", root.c_str());
    debugf("create_folder_v2 returned %d for %s\n", res, root.c_str());
    return "";
}

bool Dropbox::list(const std::string &root,
                   std::map<std::string, RemoteFileInfo> &out)
{
    std::string body = "{\"path\":\"" + jsonEscape(root) +
                       "\",\"recursive\":true,\"include_deleted\":false}";
    std::string endpoint = "https://api.dropboxapi.com/2/files/list_folder";

    while (true)
    {
        int res = _rpc(endpoint, body);
        if (res != 0)
        {
            if (_fatalError)
                return false;
            // 409 path/not_found: nothing has been uploaded here yet.  It is
            // also what a wrong Path= looks like, so say which it was.
            if (res == 409)
            {
                debugf("list_folder 409 for \"%s\" — treated as an empty "
                       "folder\n", root.c_str());
                return true;
            }
            debugf("list_folder failed with %d for \"%s\"\n", res, root.c_str());
            return false;
        }

        std::string response = _curl.getResponse();
        size_t before = out.size();
        parseDropboxEntries(response, root, out);
        // A listing that parses to nothing while the response was not empty
        // means the entries did not match the root — a path-case problem.
        if (out.size() == before)
            debugf("list_folder page added no entries under \"%s\" (%d bytes of "
                   "response)\n", root.c_str(), (int)response.size());

        if (!jsonTrue(response, "has_more"))
            break;

        std::string cursor = jsonString(response, "cursor");
        if (cursor.empty())
        {
            printf("Dropbox: has_more set but no cursor returned\n");
            break;
        }
        body = "{\"cursor\":\"" + jsonEscape(cursor) + "\"}";
        endpoint = "https://api.dropboxapi.com/2/files/list_folder/continue";
    }
    return true;
}

bool Dropbox::download(const RemoteFileInfo &file, const std::string &localPath)
{
    std::string tmpPath;
    FILE *fp = openTempFor(localPath, tmpPath);
    if (!fp)
        return false;

    std::string args("Dropbox-API-Arg: {\"path\":\"" + jsonEscapeAscii(file.id) + "\"}");
    std::string auth("Authorization: Bearer " + _token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, args.c_str());
    // The argument travels in the header, so the body must be empty.
    headers = curl_slist_append(headers, "Content-Type:");

    _curl.reset();
    _curl.setURL(std::string("https://content.dropboxapi.com/2/files/download"));
    _curl.setHeaders(headers);
    _curl.setPostData("");
    _curl.setDownloadFile(fp);

    int res = _performWithRetry();
    _curl.clearDownloadFile();
    curl_slist_free_all(headers);
    fclose(fp);

    if (res != 0)
    {
        // On an error the body is the JSON error, which went into the temp file
        // and is about to be deleted with the reason still in it.
        printf("Dropbox: download failed for %s\n", file.relPath.c_str());
        debugf("download of %s (%s) returned %d\n", file.relPath.c_str(),
               file.id.c_str(), res);
        debugFileBody("  error body", tmpPath);
        remove(tmpPath.c_str());
        return false;
    }
    return replaceLocalFile(tmpPath, localPath);
}

bool Dropbox::upload(const std::string &root, const std::string &relPath,
                     const std::string &localPath, const RemoteFileInfo *existing,
                     std::string &outTag, std::string &outId)
{
    (void)existing;

    std::string remotePath = root + relPath;

    FILE *fp = fopen(localPath.c_str(), "rb");
    if (!fp)
    {
        printf("Dropbox: cannot read %s\n", localPath.c_str());
        debugErrno("fopen", localPath);
        return false;
    }

    curl_off_t size = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
    {
        size = (curl_off_t)ftell(fp);
        rewind(fp);
    }
    if (size > MAX_UPLOAD_BYTES)
    {
        printf("Dropbox: %s is larger than 150 MB — skipping\n", relPath.c_str());
        fclose(fp);
        return false;
    }

    // "overwrite" replaces the remote file in place; the API default "add"
    // silently renames the upload to "name (1).ext" instead.
    std::string args("Dropbox-API-Arg: {\"path\":\"" + jsonEscapeAscii(remotePath) +
                     "\",\"mode\":\"overwrite\",\"mute\":true}");
    std::string auth("Authorization: Bearer " + _token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, args.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, "Expect:");

    _curl.reset();
    _curl.setURL(std::string("https://content.dropboxapi.com/2/files/upload"));
    _curl.setHeaders(headers);
    // Dropbox's content endpoints reject a chunked body, so the size is
    // announced up front.
    _curl.setReadData((void *)fp, size);
    int res = _performWithRetry(fp);

    fclose(fp);
    curl_slist_free_all(headers);

    if (res != 0)
    {
        printf("Dropbox: upload failed for %s\n", relPath.c_str());
        debugf("upload of %s returned %d\n", remotePath.c_str(), res);
        return false;
    }

    // The upload response is the new file's metadata, so the tag needs no
    // extra round trip.
    std::string response = _curl.getResponse();
    outTag = jsonString(response, "content_hash");
    std::string rev = jsonString(response, "rev");
    outId = rev.empty() ? remotePath : ("rev:" + rev);

    if (outTag.empty())
    {
        // Fall back to hashing what we just sent rather than leaving the
        // manifest without a tag, which would re-upload on every run.
        debugf("upload response carried no content_hash, hashing locally\n");
        debugBody("  upload response", response);
        outTag = computeDropboxHash(localPath);
    }
    return true;
}

std::string Dropbox::localTag(const std::string &localPath)
{
    return computeDropboxHash(localPath);
}
