#include "webdavremote.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "../utils/debug.h"
#include "../utils/fsutil.h"
#include "../utils/pathutil.h"
#include "../utils/urlutil.h"
#include "../utils/xmlutil.h"
#include "remoteparse.h"

static const char *PROPFIND_BODY =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
    "<d:resourcetype/><d:getetag/><d:getcontentlength/><d:getlastmodified/>"
    "</d:prop></d:propfind>";

WebDavRemote::WebDavRemote(const std::string &baseUrl, const std::string &user,
                           const std::string &password)
    : _baseUrl(baseUrl), _user(user), _password(password), _fatalError(false),
      _noInfiniteDepth(false)
{
    if (_baseUrl.empty() || _baseUrl[_baseUrl.size() - 1] != '/')
        _baseUrl += '/';

    // Remember the path part so hrefs, which are usually server-relative, can
    // be turned back into paths relative to the sync root.
    size_t schemeEnd = _baseUrl.find("://");
    size_t hostStart = (schemeEnd == std::string::npos) ? 0 : schemeEnd + 3;
    size_t pathStart = _baseUrl.find('/', hostStart);
    _basePath = (pathStart == std::string::npos) ? "/" : _baseUrl.substr(pathStart);
}

bool WebDavRemote::hasFatalError() const { return _fatalError; }

std::string WebDavRemote::_urlFor(const std::string &path) const
{
    // _baseUrl already ends with '/', so the path must not start with one.
    return _baseUrl + urlEncodePath(normalizeRemotePath(path));
}

void WebDavRemote::_prepare()
{
    _curl.reset();
    if (!_user.empty())
        _curl.setUserPassword(_user, _password);
}

int WebDavRemote::_performWithRetry(FILE *uploadFile)
{
    if (_fatalError)
        return -1;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (attempt > 0 && uploadFile != NULL)
            rewind(uploadFile);

        int curlRes = _curl.perform();
        long status = _curl.getStatusCode();

        if (curlRes != 0)
        {
            printf("  Network error (attempt %d/3)\n", attempt + 1);
            debugf("  %s\n", _curl.getURL().c_str());
            if (attempt < 2)
            {
                _curl.rewindDownloadFile();
                svcSleepThread(2000000000LL);
                continue;
            }
            return curlRes;
        }

        if (status >= 200 && status < 300)
            return 0;

        if (status == 401 || status == 403)
        {
            printf(CONSOLE_RED "  FATAL: WebDAV authentication failed (HTTP %ld)."
                   CONSOLE_RESET "\n", status);
            printf("  Check Url, User and Password in 3DSync.ini.\n");
            _fatalError = true;
            return (int)status;
        }

        if (status == 429 || (status >= 500 && status < 600))
        {
            if (attempt < 2)
            {
                printf("  WebDAV busy (HTTP %ld), waiting 5 seconds...\n", status);
                _curl.rewindDownloadFile();
                svcSleepThread(5000000000LL);
                continue;
            }
        }

        printf(CONSOLE_RED "  WebDAV error: HTTP %ld" CONSOLE_RESET "\n", status);
        debugf("  %s\n", _curl.getURL().c_str());
        return (int)status;
    }
    debugf("  giving up on %s after 3 attempts\n", _curl.getURL().c_str());
    return -1;
}

bool WebDavRemote::connect()
{
    std::vector<DavEntry> entries;
    if (!_propfind("", "0", entries))
    {
        printf("WebDAV: cannot reach %s\n", _baseUrl.c_str());
        return false;
    }
    return true;
}

bool WebDavRemote::_propfind(const std::string &path, const char *depth,
                             std::vector<DavEntry> &out)
{
    std::string depthHeader = std::string("Depth: ") + depth;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, depthHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");

    // The root of the listing, as the server will spell it in the hrefs.
    std::string requestPath = path;
    while (!requestPath.empty() && requestPath[0] == '/')
        requestPath.erase(0, 1);
    std::string rootHref = _basePath + requestPath;
    if (!rootHref.empty() && rootHref[rootHref.size() - 1] != '/')
        rootHref += '/';

    _prepare();
    _curl.setURL(_urlFor(requestPath.empty() ? "" : requestPath + "/"));
    _curl.setHeaders(headers);
    _curl.setCustomRequest("PROPFIND");
    _curl.setPostData(PROPFIND_BODY);

    int res = _performWithRetry();
    long status = _curl.getStatusCode();
    std::string body = _curl.getResponse();
    curl_slist_free_all(headers);

    if (res != 0)
    {
        // Apache and Nextcloud refuse infinite depth; remember and let the
        // caller walk the tree one level at a time instead.
        if (status == 403 || status == 507)
        {
            debugf("PROPFIND Depth: %s on \"%s\" refused with HTTP %ld\n", depth,
                   requestPath.c_str(), status);
            _noInfiniteDepth = true;
            return false;
        }
        debugf("PROPFIND Depth: %s on \"%s\" failed with %d (HTTP %ld)\n", depth,
               requestPath.c_str(), res, status);
        return false;
    }

    size_t before = out.size();
    parseDavResponses(body, rootHref, out);
    // Nothing parsed out of a 207 means the hrefs did not start with the root
    // this code expects, which would otherwise look like an empty remote.
    if (out.size() == before)
        debugf("PROPFIND on \"%s\" parsed no entries below \"%s\" from %d bytes\n",
               requestPath.c_str(), rootHref.c_str(), (int)body.size());
    return true;
}

bool WebDavRemote::_mkcol(const std::string &path)
{
    _prepare();
    _curl.setURL(_urlFor(path + "/"));
    _curl.setCustomRequest("MKCOL");

    int res = _performWithRetry();
    long status = _curl.getStatusCode();

    // 405 means it is already there, which is exactly what we wanted.
    if (res == 0 || status == 405)
        return true;
    printf("WebDAV: cannot create %s (HTTP %ld)\n", path.c_str(), status);
    debugf("MKCOL returned %d\n", res);
    return false;
}

std::string WebDavRemote::ensureRoot(const std::string &remoteName)
{
    std::string root = normalizeRemotePath(remoteName);
    if (root.empty())
        return root;

    // MKCOL only creates one level at a time.
    size_t pos = 0;
    while (pos != std::string::npos)
    {
        pos = root.find('/', pos + 1);
        std::string part = (pos == std::string::npos) ? root : root.substr(0, pos);
        if (!_mkcol(part))
            return "";
    }
    return root;
}

bool WebDavRemote::_listDepthOne(const std::string &root, const std::string &prefix,
                                 std::map<std::string, RemoteFileInfo> &out)
{
    std::vector<DavEntry> entries;
    if (!_propfind(root, "1", entries))
        return false;

    for (auto &entry : entries)
    {
        std::string childPath = root + entry.relPath;
        std::string childRel = prefix + entry.relPath;

        if (entry.isCollection)
        {
            if (!_listDepthOne(childPath, childRel, out))
                return false;
            continue;
        }

        if (isTempTransferName(childRel))
            continue;

        RemoteFileInfo info;
        info.id = childPath;
        info.relPath = childRel;
        info.tag = entry.tag;
        out[childRel] = info;
    }
    return true;
}

bool WebDavRemote::list(const std::string &root,
                        std::map<std::string, RemoteFileInfo> &out)
{
    if (!_noInfiniteDepth)
    {
        std::vector<DavEntry> entries;
        if (_propfind(root, "infinity", entries))
        {
            for (auto &entry : entries)
            {
                if (entry.isCollection)
                    continue;
                if (isTempTransferName(entry.relPath))
                    continue;

                RemoteFileInfo info;
                info.id = root + entry.relPath;
                info.relPath = entry.relPath;
                info.tag = entry.tag;
                out[entry.relPath] = info;
            }
            return true;
        }
        if (_fatalError)
            return false;
        if (!_noInfiniteDepth)
        {
            debugf("infinite-depth listing of \"%s\" failed for another reason "
                   "than depth\n", root.c_str());
            return false; // a real failure, not a depth restriction
        }
        printf("  Server refuses Depth: infinity, walking one level at a time\n");
    }
    return _listDepthOne(root, "", out);
}

bool WebDavRemote::download(const RemoteFileInfo &file, const std::string &localPath)
{
    std::string tmpPath;
    FILE *fp = openTempFor(localPath, tmpPath);
    if (!fp)
        return false;

    _prepare();
    _curl.setURL(_urlFor(file.id));
    _curl.setDownloadFile(fp);

    int res = _performWithRetry();
    _curl.clearDownloadFile();
    fclose(fp);

    if (res != 0)
    {
        printf("WebDAV: download failed for %s\n", file.id.c_str());
        debugf("  %d for %s\n", res, _curl.getURL().c_str());
        // A server error page was streamed into the temp file.
        debugFileBody("  error body", tmpPath);
        remove(tmpPath.c_str());
        return false;
    }
    return replaceLocalFile(tmpPath, localPath);
}

bool WebDavRemote::upload(const std::string &root, const std::string &relPath,
                          const std::string &localPath, const RemoteFileInfo *existing,
                          std::string &outTag, std::string &outId)
{
    (void)existing;

    std::string remotePath = root + relPath;

    // Parent collections have to exist before a PUT.
    size_t slash = remotePath.rfind('/');
    if (slash != std::string::npos && slash > 0)
    {
        std::string parent = remotePath.substr(0, slash);
        if (ensureRoot(parent).empty())
            return false;
    }

    FILE *fp = fopen(localPath.c_str(), "rb");
    if (!fp)
    {
        printf("WebDAV: cannot read %s\n", localPath.c_str());
        debugErrno("fopen", localPath);
        return false;
    }

    curl_off_t size = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
    {
        size = (curl_off_t)ftell(fp);
        rewind(fp);
    }

    // PUT beside the target, then MOVE over it: MOVE with Overwrite: T is
    // atomic server-side, so an interrupted upload never leaves a half-written
    // save in place.
    std::string tmpPath = remotePath + TEMP_SUFFIX;

    _prepare();
    _curl.setURL(_urlFor(tmpPath));
    _curl.setUploadFile(fp, size);

    int res = _performWithRetry(fp);
    fclose(fp);

    if (res != 0)
    {
        printf("WebDAV: upload failed for %s\n", remotePath.c_str());
        debugf("  PUT of %s returned %d\n", tmpPath.c_str(), res);
        return false;
    }

    std::string destination = "Destination: " + _urlFor(remotePath);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, destination.c_str());
    headers = curl_slist_append(headers, "Overwrite: T");

    _prepare();
    _curl.setURL(_urlFor(tmpPath));
    _curl.setHeaders(headers);
    _curl.setCustomRequest("MOVE");

    res = _performWithRetry();
    curl_slist_free_all(headers);

    if (res != 0)
    {
        printf("WebDAV: cannot move %s into place\n", tmpPath.c_str());
        debugf("  MOVE returned %d; the upload is left as %s\n", res,
               tmpPath.c_str());
        return false;
    }

    // Read the new ETag back so the manifest records what the server now holds.
    std::vector<DavEntry> entries;
    outTag = "";
    if (_propfind(remotePath, "0", entries) && !entries.empty())
        outTag = entries[0].tag;
    if (outTag.empty())
    {
        // Depth 0 on a file reports the file itself, so an empty list means the
        // server answered oddly; fall back to a local stamp so the next sync
        // still has something to compare.
        debugf("no ETag for %s after upload, using a local size:mtime stamp\n",
               remotePath.c_str());
        struct stat st = {};
        if (stat(localPath.c_str(), &st) == 0)
        {
            char buf[48];
            snprintf(buf, sizeof(buf), "%lld:%lld", (long long)st.st_size,
                     (long long)st.st_mtime);
            outTag = buf;
        }
    }
    outId = remotePath;
    return true;
}
