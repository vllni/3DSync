#include "ftpremote.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "../utils/debug.h"
#include "../utils/fsutil.h"
#include "../utils/pathutil.h"
#include "../utils/urlutil.h"
#include "remoteparse.h"

FtpRemote::FtpRemote(const std::string &host, int port, const std::string &user,
                     const std::string &password, const std::string &basePath,
                     FtpTlsMode tls, bool activeMode)
    : _host(host), _port(port > 0 ? port : (tls == FTP_TLS_IMPLICIT ? 990 : 21)),
      _user(user), _password(password), _tls(tls), _tlsDropped(false), _activeMode(activeMode),
      _fatalError(false)
{
    // Strip the separators so paths can be joined without doubling them.
    _basePath = normalizeRemotePath(basePath);
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
    return buildFtpUrl(_tls == FTP_TLS_IMPLICIT, _host, _port, path);
}

FtpTlsMode FtpRemote::_effectiveTls() const
{
    return _tlsDropped ? FTP_TLS_NONE : _tls;
}

void FtpRemote::_prepare()
{
    _curl.reset();
    _curl.setUserPassword(_user, _password);
    _curl.setActiveMode(_activeMode);

    switch (_effectiveTls())
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

// The curl codes that refuse one path rather than the session.
static bool pathRefusal(int res, FtpPathRefusal &out)
{
    switch (res)
    {
    case CURLE_REMOTE_ACCESS_DENIED:
        out = FTP_REFUSED_DIRECTORY;
        return true;
    case CURLE_REMOTE_FILE_NOT_FOUND:
    case CURLE_FTP_COULDNT_RETR_FILE:
        out = FTP_REFUSED_FILE;
        return true;
    default:
        return false;
    }
}

// A listing that failed only because the path is not there yet.
static bool nothingThereYet(int res)
{
    FtpPathRefusal refusal;
    return pathRefusal(res, refusal) &&
           ftpRefusalOutcome(FTP_REQUEST_LIST, refusal) == FTP_OUTCOME_EMPTY;
}

int FtpRemote::_performWithRetry(FtpRequest request, FILE *uploadFile)
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
        // Wrong credentials or no access to the tree — nothing else will work,
        // with one exception: see ftpShouldRetryWithoutTls().
        case CURLE_LOGIN_DENIED:
            if (ftpShouldRetryWithoutTls(_effectiveTls(), _user, true))
            {
                // Stock vsftpd: AUTH TLS accepted, the anonymous login then
                // refused inside it.  Reconnect once without TLS — the handle
                // keeps this request's options, only USE_SSL changes — and
                // stay in plaintext for the run, since _prepare() reads
                // _tlsDropped from here on.  That also makes the check above
                // say no next time, so this cannot repeat.  Not a transient
                // failure, so it does not use up one of the three attempts.
                debugf("FTP: anonymous login refused over TLS on %s:%d; "
                       "using plaintext for the rest of this run\n",
                       _host.c_str(), _port);
                _tlsDropped = true;
                _curl.setUseSSL(CURLUSESSL_NONE);
                // The refused session negotiated TLS; never reuse it.
                _curl.setFreshConnect(true);
                if (uploadFile != NULL)
                    rewind(uploadFile);
                attempt--;
                continue;
            }
            printf(CONSOLE_RED "  FATAL: FTP login denied." CONSOLE_RESET "\n");
            if (isAnonymousFtpUser(_user))
            {
                // No User= in the INI means "anonymous": telling this user to
                // check a User and Password they never set says nothing.
                printf("  The server refused an anonymous login.\n");
                printf("  Set User and Password if it needs one.\n");
            }
            else
            {
                printf("  Check User and Password in 3DSync.ini.\n");
                if (_tls == FTP_TLS_TRY)
                {
                    printf("  The server may refuse logins over TLS;\n");
                    printf("  TLS=none allows plaintext (unencrypted).\n");
                }
            }
            debugf("  curl %d on %s, TLS %s\n", res, _curl.getURL().c_str(),
                   _tlsDropped ? "dropped" : "as configured");
            _fatalError = true;
            return res;
        case CURLE_USE_SSL_FAILED:
            printf(CONSOLE_RED "  FATAL: the server does not support FTPS." CONSOLE_RESET "\n");
            printf("  Set TLS=try or TLS=none in 3DSync.ini\n");
            printf("  to allow plaintext.\n");
            _fatalError = true;
            return res;
        case CURLE_COULDNT_RESOLVE_HOST:
            printf(CONSOLE_RED "  FATAL: cannot resolve %s." CONSOLE_RESET "\n", _host.c_str());
            _fatalError = true;
            return res;

        // A path refused — a CWD, the MKD before a STOR, or the file itself.
        // libcurl reports a refused CWD as "access denied", which is also
        // what a directory that does not exist yet looks like; what it means
        // depends on the request, see ftpRefusalOutcome().
        case CURLE_REMOTE_ACCESS_DENIED:
        case CURLE_REMOTE_FILE_NOT_FOUND:
        case CURLE_FTP_COULDNT_RETR_FILE:
        {
            FtpPathRefusal refusal = FTP_REFUSED_FILE;
            pathRefusal(res, refusal);
            switch (ftpRefusalOutcome(request, refusal))
            {
            case FTP_OUTCOME_EMPTY:
                debugf("%s: %s; read as nothing there yet\n", _curl.getURL().c_str(),
                       curl_easy_strerror((CURLcode)res));
                return res;
            case FTP_OUTCOME_THIS_FILE:
                // The caller says which file failed.
                debugf("%s: %s\n", _curl.getURL().c_str(),
                       curl_easy_strerror((CURLcode)res));
                return res;
            case FTP_OUTCOME_FATAL:
            default:
                printf(CONSOLE_RED "  FATAL: FTP server denied access to" CONSOLE_RESET "\n");
                printf(CONSOLE_RED "  a folder it just listed." CONSOLE_RESET "\n");
                debugf("  curl %d on %s\n", res, _curl.getURL().c_str());
                _fatalError = true;
                return res;
            }
        }

        // Transient: retry.
        default:
            if (request == FTP_REQUEST_UPLOAD &&
                ftpUploadEndedUnclean(_effectiveTls(), res == CURLE_PARTIAL_FILE,
                                      _curl.getStatusCode()))
            {
                // Would fail identically every time; upload() checks what
                // the server kept instead.
                debugf("FTP: 426 after a TLS upload; the server wants a TLS "
                       "close this libcurl does not send\n");
                return res;
            }
            printf("  FTP error: %s (attempt %d/3)\n", curl_easy_strerror((CURLcode)res),
                   attempt + 1);
            debugf("  curl %d on %s\n", res, _curl.getURL().c_str());
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

    int res = _performWithRetry(FTP_REQUEST_LIST);
    if (res == CURLE_OK)
        return true;

    // The base directory may simply not exist yet; that is not a login
    // problem, and the first upload creates it.
    if (!_fatalError && nothingThereYet(res))
    {
        debugf("base directory \"%s\" is not listable yet, continuing\n",
               _basePath.c_str());
        return true;
    }

    printf("FTP: cannot reach %s\n", _host.c_str());
    return false;
}

std::string FtpRemote::ensureRoot(const std::string &remoteName)
{
    std::string root = normalizeRemotePath(remoteName);
    if (!_basePath.empty())
        root = normalizeRemotePath(_basePath + "/" + root);

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

    int res = _performWithRetry(FTP_REQUEST_FILE);
    if (res != 0)
    {
        // The caller turns this into "upload failed" with nothing to explain it.
        debugf("MDTM/SIZE failed for %s (%d)\n", path.c_str(), res);
        return "";
    }

    curl_off_t size = _curl.getContentLength();
    long mtime = _curl.getFileTime();
    if (size < 0 || mtime < 0)
        debugf("%s: server reported size=%lld mtime=%ld — the change tag will "
               "not be reliable\n", path.c_str(), (long long)size, mtime);
    return sizeTimeTag((long long)(size < 0 ? -1 : size), (long long)mtime);
}

bool FtpRemote::_listDir(const std::string &dirPath, const std::string &prefix,
                         std::map<std::string, RemoteFileInfo> &out)
{
    ListingContext ctx;

    // The wildcard is appended after encoding: percent-escaping it would leave
    // libcurl matching a literal "%2A" instead of listing the directory.
    std::string listUrl = dirPath.empty() ? _urlFor("") + "*"
                                          : _urlFor(dirPath) + "/*";

    _prepare();
    _curl.setURL(listUrl);
    _curl.setWildcardMatch(_onListEntry, &ctx);

    int res = _performWithRetry(FTP_REQUEST_LIST);
    _curl.setWildcardMatch(NULL, NULL);

    if (res != 0)
    {
        // A directory that does not exist yet simply has nothing in it.  The
        // sync folder is missing on every first sync: FTP creates directories
        // only when it uploads into them.
        if (!_fatalError && nothingThereYet(res))
        {
            debugf("nothing to list under \"%s\", treated as empty\n",
                   dirPath.c_str());
            return true;
        }
        debugf("listing \"%s\" failed with %d\n", dirPath.c_str(), res);
        return false;
    }
    debugf("\"%s\": %d file(s), %d dir(s)\n", dirPath.c_str(), (int)ctx.files.size(),
           (int)ctx.dirs.size());

    for (auto &fileName : ctx.files)
    {
        std::string childPath = dirPath.empty() ? fileName : dirPath + "/" + fileName;
        std::string childRel = prefix + "/" + fileName;

        // Skip our own interrupted transfers.
        if (isTempTransferName(childRel))
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

    int res = _performWithRetry(FTP_REQUEST_FILE);
    _curl.clearDownloadFile();
    fclose(fp);

    if (res != 0)
    {
        printf("FTP: download failed for %s\n", file.id.c_str());
        debugf("  curl %d for %s\n", res, _curl.getURL().c_str());
        debugFileBody("  partial download", tmpPath);
        remove(tmpPath.c_str());
        return false;
    }
    return replaceLocalFile(tmpPath, localPath);
}

long long FtpRemote::_remoteSize(const std::string &path)
{
    _prepare();
    _curl.setURL(_urlFor(path));
    _curl.setNoBody(true);

    int res = _performWithRetry(FTP_REQUEST_FILE);
    curl_off_t size = (res == 0) ? _curl.getContentLength() : -1;
    if (res != 0)
        debugf("SIZE failed for %s (%d)\n", path.c_str(), res);
    return (long long)(size < 0 ? -1 : size);
}

bool FtpRemote::_finishUncleanUpload(const std::string &remotePath,
                                     const std::string &tmpRemote, long long localSize)
{
    long long arrived = _remoteSize(tmpRemote);
    if (!ftpUploadArrivedIntact(arrived, localSize))
    {
        debugf("%s: %lld of %lld bytes on the server, not moving it into place\n",
               tmpRemote.c_str(), arrived, localSize);
        return false;
    }

    // The post-quote never ran, so send the same DELE/RNFR/RNTO on their own:
    // a QUOTE on the login directory, before any CWD, so full paths.
    struct curl_slist *quote = NULL;
    for (auto &command : ftpReplaceCommands(remotePath, tmpRemote, false))
        quote = curl_slist_append(quote, command.c_str());

    _prepare();
    _curl.setURL(_urlFor(""));
    _curl.setNoBody(true);
    _curl.setQuote(quote);
    int res = _performWithRetry(FTP_REQUEST_FILE);
    _curl.setQuote(NULL);
    curl_slist_free_all(quote);

    debugf("%s: all %lld bytes arrived despite the 426; rename %s\n", tmpRemote.c_str(),
           arrived, res == 0 ? "done" : "failed");
    return res == 0;
}

bool FtpRemote::upload(const std::string &root, const std::string &relPath,
                       const std::string &localPath, const RemoteFileInfo *existing,
                       std::string &outTag, std::string &outId)
{
    (void)existing;

    std::string remotePath = root + relPath;
    std::string tmpRemote = remotePath + TEMP_SUFFIX;

    FILE *fp = fopen(localPath.c_str(), "rb");
    if (!fp)
    {
        printf("FTP: cannot read %s\n", localPath.c_str());
        debugErrno("fopen", localPath);
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
    // replace on every server, so the old file is removed first.  These run
    // after the transfer, in the file's directory — see ftpReplaceCommands().
    struct curl_slist *quote = NULL;
    for (auto &command : ftpReplaceCommands(remotePath, tmpRemote, true))
        quote = curl_slist_append(quote, command.c_str());

    _prepare();
    _curl.setURL(_urlFor(tmpRemote));
    _curl.setUploadFile(fp, size);
    _curl.setCreateMissingDirs(true);
    _curl.setPostQuote(quote);

    int res = _performWithRetry(FTP_REQUEST_UPLOAD, fp);
    long reply = _curl.getStatusCode();

    _curl.setPostQuote(NULL);
    curl_slist_free_all(quote);
    fclose(fp);

    bool unclean = res != 0 && ftpUploadEndedUnclean(_effectiveTls(), res == CURLE_PARTIAL_FILE,
                                                     reply);
    if (unclean && _finishUncleanUpload(remotePath, tmpRemote, (long long)size))
        res = 0;

    if (res != 0)
    {
        printf("FTP: upload failed for\n  %s\n", remotePath.c_str());
        if (res == CURLE_REMOTE_ACCESS_DENIED)
        {
            // A CWD or MKD on the way was refused.  Listing read the same 550
            // as "not there yet", so this is where a folder the account may
            // not write to shows up.  Once per folder: every file in it fails
            // the same way.
            size_t slash = remotePath.rfind('/');
            std::string dir = (slash == std::string::npos) ? "" : remotePath.substr(0, slash);
            if (_refusedDirs.insert(dir).second)
            {
                printf(CONSOLE_RED "  The server would not create or enter" CONSOLE_RESET "\n");
                printf(CONSOLE_RED "  the folder:" CONSOLE_RESET "\n");
                printf("    %s\n", dir.c_str());
                printf("  Check the FTP account may write there.\n");
            }
        }
        if (unclean)
        {
            printf("  The server refused the end of the\n");
            printf("  encrypted transfer (FTP 426).\n");
        }
        debugf("  curl %d; the DELE/RNFR/RNTO quote may also have failed\n", res);
        return false;
    }

    outTag = _tagFor(remotePath);
    outId = remotePath;
    if (outTag.empty())
        printf("FTP: could not read the timestamp of\n  %s\n  after uploading it.\n",
               remotePath.c_str());
    return !outTag.empty();
}
