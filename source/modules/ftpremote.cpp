#include "ftpremote.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "../utils/fsutil.h"

FtpRemote::FtpRemote(const std::string &host, int port, const std::string &user,
                     const std::string &password, const std::string &basePath,
                     FtpTlsMode tls, bool activeMode)
    : _host(host), _port(port > 0 ? port : (tls == FTP_TLS_IMPLICIT ? 990 : 21)),
      _user(user), _password(password), _tls(tls), _activeMode(activeMode),
      _fatalError(false)
{
    // Strip the separators so paths can be joined without doubling them.
    _basePath = basePath;
    while (!_basePath.empty() && _basePath[0] == '/')
        _basePath.erase(0, 1);
    while (!_basePath.empty() && _basePath[_basePath.size() - 1] == '/')
        _basePath.erase(_basePath.size() - 1);
}

std::string FtpRemote::manifestPrefix() const
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", _port);
    return "ftp://" + _host + ":" + buf;
}

bool FtpRemote::hasFatalError() const { return _fatalError; }

std::string FtpRemote::_urlFor(const std::string &path) const
{
    char portBuf[32];
    snprintf(portBuf, sizeof(portBuf), "%d", _port);
    std::string url = (_tls == FTP_TLS_IMPLICIT ? "ftps://" : "ftp://") + _host +
                      ":" + portBuf + "/";
    return url + urlEncodePath(path);
}

void FtpRemote::_prepare()
{
    _curl.reset();
    _curl.setUserPassword(_user, _password);
    _curl.setActiveMode(_activeMode);

    switch (_tls)
    {
    case FTP_TLS_TRY:
        _curl.setUseSSL(CURLUSESSL_TRY);
        break;
    case FTP_TLS_REQUIRE:
        _curl.setUseSSL(CURLUSESSL_ALL);
        break;
    case FTP_TLS_IMPLICIT: // the ftps:// scheme already implies TLS
    case FTP_TLS_NONE:
    default:
        break;
    }
}

int FtpRemote::_performWithRetry(FILE *uploadFile)
{
    if (_fatalError)
        return -1;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (attempt > 0 && uploadFile != NULL)
            rewind(uploadFile); // the previous attempt consumed part of the file

        int res = _curl.perform();
        if (res == CURLE_OK)
        {
            std::string date = _curl.getResponseHeader("Date");
            if (!date.empty())
                _lastServerTime = date;
            return 0;
        }

        switch (res)
        {
        // Wrong credentials or no access to the tree — nothing else will work.
        case CURLE_LOGIN_DENIED:
            printf(CONSOLE_RED "  FATAL: FTP login denied." CONSOLE_RESET "\n");
            printf("  Check User and Password in 3DSync.ini.\n");
            _fatalError = true;
            return res;
        case CURLE_REMOTE_ACCESS_DENIED:
            printf(CONSOLE_RED "  FATAL: FTP server denied access to the path." CONSOLE_RESET "\n");
            _fatalError = true;
            return res;
        case CURLE_USE_SSL_FAILED:
            printf(CONSOLE_RED "  FATAL: the server does not support FTPS." CONSOLE_RESET "\n");
            printf("  Set TLS=try or TLS=none in 3DSync.ini to allow plaintext.\n");
            _fatalError = true;
            return res;
        case CURLE_COULDNT_RESOLVE_HOST:
            printf(CONSOLE_RED "  FATAL: cannot resolve %s." CONSOLE_RESET "\n", _host.c_str());
            _fatalError = true;
            return res;

        // Missing file or directory — affects this entry only.
        case CURLE_REMOTE_FILE_NOT_FOUND:
        case CURLE_FTP_COULDNT_RETR_FILE:
            return res;

        // Transient: retry.
        default:
            printf("  FTP error: %s (attempt %d/3)\n", curl_easy_strerror((CURLcode)res),
                   attempt + 1);
            if (attempt < 2)
            {
                svcSleepThread(2000000000LL);
                continue;
            }
            return res;
        }
    }
    return -1;
}

bool FtpRemote::connect()
{
    // A listing of the base directory is the cheapest way to prove the
    // credentials work before any file is touched.
    _prepare();
    _curl.setURL(_urlFor(_basePath.empty() ? "" : _basePath + "/"));
    _curl.setDirListOnly(true);

    int res = _performWithRetry();
    if (res == CURLE_OK)
        return true;

    // The base directory may simply not exist yet; that is not a login problem.
    if (!_fatalError && (res == CURLE_REMOTE_FILE_NOT_FOUND ||
                         res == CURLE_FTP_COULDNT_RETR_FILE))
        return true;

    printf("FTP: cannot reach %s\n", _host.c_str());
    return false;
}

std::string FtpRemote::ensureRoot(const std::string &remoteName)
{
    std::string root = remoteName;
    while (!root.empty() && root[0] == '/')
        root.erase(0, 1);
    if (!_basePath.empty())
        root = _basePath + "/" + root;
    while (!root.empty() && root[root.size() - 1] == '/')
        root.erase(root.size() - 1);

    // Directories are created lazily on upload (CURLOPT_FTP_CREATE_MISSING_DIRS),
    // so nothing to do here beyond normalising the path.
    return root;
}

long FtpRemote::_onListEntry(const void *transferInfo, void *userdata, int remains)
{
    (void)remains;
    const struct curl_fileinfo *info = (const struct curl_fileinfo *)transferInfo;
    ListingContext *ctx = (ListingContext *)userdata;
    if (!info || !ctx || !info->filename)
        return CURL_CHUNK_BGN_FUNC_SKIP;

    std::string entryName(info->filename);
    if (entryName == "." || entryName == "..")
        return CURL_CHUNK_BGN_FUNC_SKIP;

    if (info->filetype == CURLFILETYPE_DIRECTORY)
        ctx->dirs.push_back(entryName);
    else if (info->filetype == CURLFILETYPE_FILE)
        ctx->files.push_back(entryName);

    // Listing only — never let libcurl start downloading the entry.
    return CURL_CHUNK_BGN_FUNC_SKIP;
}

std::string FtpRemote::_tagFor(const std::string &path)
{
    _prepare();
    _curl.setURL(_urlFor(path));
    _curl.setNoBody(true);
    _curl.setFileTime(true);

    if (_performWithRetry() != 0)
        return "";

    curl_off_t size = _curl.getContentLength();
    long mtime = _curl.getFileTime();
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld:%ld", (long long)(size < 0 ? -1 : size), mtime);
    return buf;
}

bool FtpRemote::_listDir(const std::string &dirPath, const std::string &prefix,
                         std::map<std::string, RemoteFileInfo> &out)
{
    ListingContext ctx;

    _prepare();
    _curl.setURL(_urlFor(dirPath.empty() ? "*" : dirPath + "/*"));
    _curl.setWildcardMatch(_onListEntry, &ctx);

    int res = _performWithRetry();
    _curl.setWildcardMatch(NULL, NULL);

    if (res != 0)
    {
        // A directory that does not exist yet simply has nothing in it.
        if (res == CURLE_REMOTE_FILE_NOT_FOUND || res == CURLE_FTP_COULDNT_RETR_FILE)
            return true;
        return false;
    }

    for (auto &fileName : ctx.files)
    {
        std::string childPath = dirPath.empty() ? fileName : dirPath + "/" + fileName;
        std::string childRel = prefix + "/" + fileName;

        // Skip our own interrupted transfers.
        if (childRel.size() > 7 &&
            childRel.compare(childRel.size() - 7, 7, ".3dstmp") == 0)
            continue;

        RemoteFileInfo info;
        info.id = childPath;
        info.relPath = childRel;
        info.tag = _tagFor(childPath);
        out[childRel] = info;
    }

    for (auto &dirName : ctx.dirs)
    {
        std::string childPath = dirPath.empty() ? dirName : dirPath + "/" + dirName;
        if (!_listDir(childPath, prefix + "/" + dirName, out))
            return false;
    }
    return true;
}

bool FtpRemote::list(const std::string &root,
                     std::map<std::string, RemoteFileInfo> &out)
{
    return _listDir(root, "", out);
}

bool FtpRemote::download(const RemoteFileInfo &file, const std::string &localPath)
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
        printf("FTP: download failed for %s\n", file.id.c_str());
        remove(tmpPath.c_str());
        return false;
    }
    return replaceLocalFile(tmpPath, localPath);
}

bool FtpRemote::upload(const std::string &root, const std::string &relPath,
                       const std::string &localPath, const RemoteFileInfo *existing,
                       std::string &outTag, std::string &outId)
{
    (void)existing;

    std::string remotePath = root + relPath;
    std::string tmpRemote = remotePath + ".3dstmp";

    FILE *fp = fopen(localPath.c_str(), "rb");
    if (!fp)
    {
        printf("FTP: cannot read %s\n", localPath.c_str());
        return false;
    }

    curl_off_t size = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
    {
        size = (curl_off_t)ftell(fp);
        rewind(fp);
    }

    // Upload beside the target and rename on success, so an interrupted
    // transfer cannot leave a truncated save on the server.  RNTO does not
    // replace on every server, so the old file is removed first; '*' marks the
    // DELE as allowed to fail (it is absent on a first upload).
    std::string dele = "*DELE " + remotePath;
    std::string rnfr = "RNFR " + tmpRemote;
    std::string rnto = "RNTO " + remotePath;
    struct curl_slist *quote = NULL;
    quote = curl_slist_append(quote, dele.c_str());
    quote = curl_slist_append(quote, rnfr.c_str());
    quote = curl_slist_append(quote, rnto.c_str());

    _prepare();
    _curl.setURL(_urlFor(tmpRemote));
    _curl.setUploadFile(fp, size);
    _curl.setCreateMissingDirs(true);
    _curl.setPostQuote(quote);

    int res = _performWithRetry(fp);

    _curl.setPostQuote(NULL);
    curl_slist_free_all(quote);
    fclose(fp);

    if (res != 0)
    {
        printf("FTP: upload failed for %s\n", remotePath.c_str());
        return false;
    }

    outTag = _tagFor(remotePath);
    outId = remotePath;
    return !outTag.empty();
}
