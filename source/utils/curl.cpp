#include "curl.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <time.h>

#include "../version.h"
#include "debug.h"

// ---------------------------------------------------------------------------
// Closing a socket gracefully
// ---------------------------------------------------------------------------
// A plain close on the 3DS ends a TCP connection with a RST rather than a FIN,
// discarding whatever is still in the send buffer.  An FTP upload in stream
// mode has no end-of-file marker but the client's close, so vsftpd read the
// RST as a failed transfer: "426 Failure reading network stream", with the
// file short by whatever had not left the console yet.  So: send the FIN
// after the buffered data (shutdown), wait briefly for the peer to close its
// side, then close.  The wait is bounded, so a peer that never answers costs
// at most CLOSE_DRAIN_MS.
//
// libcurl calls this for every socket it opened — the control connection,
// the passive-mode data connection, every HTTP connection — but not for one
// it accepted, so the data connection of Mode=active still closes the old
// way.  For HTTP, whose responses carry their own length, it only makes the
// close polite.  SMB goes through libsmb2, not libcurl.
static const int CLOSE_DRAIN_MS = 1000;

static int _closeGracefully(void *clientp, curl_socket_t fd)
{
    (void)clientp;
    if (shutdown(fd, SHUT_WR) != 0)
        debugf("socket %d: shutdown failed (errno %d), closing anyway\n", (int)fd, errno);
    else
    {
        // Drain until the peer's FIN (recv() == 0), an error, or the deadline.
        // Unread data at close is the other thing that makes a stack send RST.
        char buf[512];
        int left = CLOSE_DRAIN_MS;
        while (left > 0)
        {
            struct pollfd p;
            p.fd = fd;
            p.events = POLLIN;
            p.revents = 0;
            u64 start = osGetTime();
            int ready = poll(&p, 1, left);
            left -= (int)(osGetTime() - start);
            if (ready < 0)
            {
                debugf("socket %d: poll failed (errno %d) while draining\n", (int)fd, errno);
                break;
            }
            if (ready == 0)
                continue; // timed out; the loop condition ends it
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                if (n < 0)
                    debugf("socket %d: recv failed (errno %d) while draining\n", (int)fd,
                           errno);
                break;
            }
        }
        if (left <= 0)
            debugf("socket %d: peer did not close within %d ms\n", (int)fd, CLOSE_DRAIN_MS);
    }
    return closesocket(fd);
}

// ---------------------------------------------------------------------------
// Certificate checking
// ---------------------------------------------------------------------------
// The CA bundle is curl's extract of Mozilla's (romfs/cacert.pem), parsed once
// per app run and kept: libcurl's mbedTLS backend would otherwise parse the
// whole file again for every connection, and FTPS opens one per transfer.  It
// is handed to each connection through CURLOPT_SSL_CTX_FUNCTION.  Only the
// worker thread uses libcurl, so the lazy load needs no lock.
static const char *const CA_BUNDLE = "romfs:/cacert.pem";
static mbedtls_x509_crt g_caChain;
static int g_caState = 0; // 0 not tried, 1 loaded, -1 failed

static const mbedtls_x509_crt *caChain()
{
    if (g_caState == 0)
    {
        mbedtls_x509_crt_init(&g_caChain);
        int ret = mbedtls_x509_crt_parse_file(&g_caChain, CA_BUNDLE);
        // A positive result counts certificates that did not parse; the rest
        // are usable.  A negative one means nothing was read.
        g_caState = (ret >= 0 && g_caChain.version != 0) ? 1 : -1;
        if (g_caState > 0)
        {
            // Said once per run: the one line in a debug log that shows the
            // certificates are being checked, and against what.
            int roots = 0;
            for (const mbedtls_x509_crt *c = &g_caChain; c && c->version != 0; c = c->next)
                roots++;
            debugf("TLS: checking certificates against %d trusted roots\n", roots);
        }
        if (ret > 0)
            debugf("CA bundle: %d certificate(s) skipped\n", ret);
        if (g_caState < 0)
            debugf("CA bundle %s could not be loaded (mbedTLS -0x%04x)\n", CA_BUNDLE,
                   (unsigned)-ret);
    }
    return g_caState > 0 ? &g_caChain : NULL;
}

// Runs for every new TLS connection, after libcurl has set mbedTLS up and
// before the handshake: the place to give it the trusted roots.
static CURLcode _sslContext(CURL *handle, void *sslConfig, void *userdata)
{
    (void)handle;
    (void)userdata;
    const mbedtls_x509_crt *ca = caChain();
    if (!ca)
        return CURLE_SSL_CACERT_BADFILE;
    mbedtls_ssl_conf_ca_chain((mbedtls_ssl_config *)sslConfig,
                              const_cast<mbedtls_x509_crt *>(ca), NULL);
    return CURLE_OK;
}

bool Curl::isCertificateError(int curlCode)
{
    return curlCode == CURLE_PEER_FAILED_VERIFICATION || curlCode == CURLE_SSL_CACERT_BADFILE;
}

void Curl::printCertificateHelp(const char *remoteName, int curlCode, bool canOptOut)
{
    if (curlCode == CURLE_SSL_CACERT_BADFILE)
    {
        printf(CONSOLE_RED "%s: the list of trusted certificates\n" CONSOLE_RESET, remoteName);
        printf(CONSOLE_RED "  is missing, so 3DSync did not connect.\n" CONSOLE_RESET);
        printf("  Reinstall 3DSync.\n");
        return;
    }
    printf(CONSOLE_RED "%s: the server's certificate could not\n" CONSOLE_RESET, remoteName);
    printf(CONSOLE_RED "  be verified, so 3DSync did not connect.\n" CONSOLE_RESET);
    // A clock that was never set (2000, 2011) makes every certificate look
    // not yet valid: the most likely cause on a 3DS, and easy to fix.
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);
    if (t)
        printf("  The 3DS date is %04d-%02d-%02d; if that is\n  wrong, set it in System Settings.\n",
               t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    if (canOptOut)
    {
        printf("  A server with its own certificate (a\n");
        printf("  NAS)? Add VerifyTLS=false to its section.\n");
    }
}

Curl::Curl() : _downloadFile(nullptr), _verifyTls(true)
{
    curl_global_init(CURL_GLOBAL_ALL);
    _curl = curl_easy_init();
    if (!_curl)
    {
        printf("Failed to init libcurl.\n");
        return; // every setopt below would be a null dereference
    }
    _applyDefaults();
}

void Curl::_applyDefaults()
{
    curl_easy_setopt(_curl, CURLOPT_USERAGENT, "3DSync/" APP_VERSION);
    curl_easy_setopt(_curl, CURLOPT_CONNECTTIMEOUT, 50L);
    curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(_curl, CURLOPT_NOPROGRESS, 1L);
    if (_verifyTls)
    {
        curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYHOST, 2L);
        // devkitPro's libcurl defaults to sdmc:/config/ssl/cacert.pem, which
        // few SD cards have: without this every connection would try to load
        // it, and fail.  The bundle comes from the callback below instead.
        curl_easy_setopt(_curl, CURLOPT_CAINFO, (char *)NULL);
        curl_easy_setopt(_curl, CURLOPT_CAPATH, (char *)NULL);
        // A libcurl without SSL_CTX_FUNCTION for its TLS backend refuses the
        // option; it then reads the bundle itself, once per connection.
        if (curl_easy_setopt(_curl, CURLOPT_SSL_CTX_FUNCTION, _sslContext) != CURLE_OK)
            curl_easy_setopt(_curl, CURLOPT_CAINFO, CA_BUNDLE);
    }
    else
    {
        curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(_curl, CURLOPT_SSL_CTX_FUNCTION, (void *)NULL);
    }
    curl_easy_setopt(_curl, CURLOPT_PIPEWAIT, 1L);
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, _write_callback);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(_curl, CURLOPT_HEADERFUNCTION, _header_callback);
    curl_easy_setopt(_curl, CURLOPT_HEADERDATA, this);
    // Re-applied after every reset(), so no connection is opened without it.
    curl_easy_setopt(_curl, CURLOPT_CLOSESOCKETFUNCTION, _closeGracefully);

    // Options are re-applied after every reset(), so a run started in debug
    // mode traces every request without the handle having to be rebuilt.
    if (debugEnabled())
    {
        curl_easy_setopt(_curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(_curl, CURLOPT_DEBUGFUNCTION, _trace_callback);
        curl_easy_setopt(_curl, CURLOPT_DEBUGDATA, this);
    }
}

Curl::~Curl()
{
    curl_easy_cleanup(_curl);
}

void Curl::setVerifyTls(bool enabled)
{
    _verifyTls = enabled;
    if (_curl)
        _applyDefaults();
}

void Curl::setURL(std::string URL)
{
    _lastUrl = URL;
    curl_easy_setopt(_curl, CURLOPT_URL, URL.c_str());
}

std::string Curl::getURL() const
{
    return _lastUrl;
}

void Curl::setHeaders(curl_slist *headers)
{
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headers);
}

void Curl::setReadData(void *pointer, curl_off_t size)
{
    curl_easy_setopt(_curl, CURLOPT_READFUNCTION, _read_callback);
    curl_easy_setopt(_curl, CURLOPT_READDATA, pointer);
    curl_easy_setopt(_curl, CURLOPT_SEEKFUNCTION, _seek_callback);
    curl_easy_setopt(_curl, CURLOPT_SEEKDATA, pointer);
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE_LARGE, size);
}

void Curl::setPostData(const std::string &data)
{
    _postData = data;
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, _postData.data());
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, _postData.size());
    curl_easy_setopt(_curl, CURLOPT_POST, 1L);
}

void Curl::resetToGet()
{
    _postData.clear();
    curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1L);
}

int Curl::perform()
{
    _responseData.clear();
    _rawHeaders.clear();

    if (!_curl)
    {
        printf("Network error: libcurl was not initialised.\n");
        return CURLE_FAILED_INIT;
    }

    CURLcode rescode = curl_easy_perform(_curl);
    long status = getStatusCode();

    // No printf here: the caller decides what a failure means.  A refused
    // FTP CWD is "nothing there yet" while listing, and announcing it as a
    // network error would contradict the provider.  Every _performWithRetry()
    // prints the reason itself; the code is in the debug output below.

    if (debugEnabled())
    {
        double seconds = 0;
        curl_easy_getinfo(_curl, CURLINFO_TOTAL_TIME, &seconds);
        debugf("status %ld, %d bytes, %d ms  <-  %s\n", status,
               (int)_responseData.size(), (int)(seconds * 1000),
               _lastUrl.c_str());

        if (rescode != CURLE_OK)
        {
            // The transport failure is the one error worth spelling out twice:
            // the code above is what a bug report needs, and the OS errno says
            // whether the console got as far as a socket.
            debugf("curl error %d (%s)\n", (int)rescode, curl_easy_strerror(rescode));
            long oserrno = 0;
            if (curl_easy_getinfo(_curl, CURLINFO_OS_ERRNO, &oserrno) == CURLE_OK &&
                oserrno != 0)
                debugf("  socket errno %ld: %s\n", oserrno, strerror((int)oserrno));
        }

        // A non-2xx body is the remote explaining itself, and the callers that
        // only look at the status code throw it away.  Status 0 with no error
        // is a protocol that reports none (an FTP listing), not a failure —
        // dumping there would print the listing itself.
        if (rescode != CURLE_OK || status >= 300 || (status != 0 && status < 200))
        {
            if (_downloadFile != nullptr)
                debugf("  body went to the download file\n");
            else
                debugBody("  response", _responseData);
        }
    }
    return rescode;
}

std::string Curl::getResponse() const
{
    return _responseData;
}

long Curl::getStatusCode() const
{
    long code = 0;
    curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

long Curl::getAuthOffered() const
{
    // Collected from every WWW-Authenticate of the transfer and cleared only
    // when the next one starts, so it still holds the 401's answer after the
    // retried request has succeeded.
    long methods = 0;
    curl_easy_getinfo(_curl, CURLINFO_HTTPAUTH_AVAIL, &methods);
    return methods;
}

std::string Curl::getResponseHeader(const std::string &name) const
{
    // Build a lowercase copy for case-insensitive matching
    std::string lowerHeaders = _rawHeaders;
    std::string lowerName = name;
    for (char &c : lowerHeaders) c = (char)tolower((unsigned char)c);
    for (char &c : lowerName)    c = (char)tolower((unsigned char)c);
    std::string search = lowerName + ":";
    size_t pos = lowerHeaders.find(search);
    if (pos == std::string::npos)
        return "";
    pos += search.size();
    while (pos < _rawHeaders.size() && _rawHeaders[pos] == ' ') pos++;
    size_t end = _rawHeaders.find("\r\n", pos);
    if (end == std::string::npos) end = _rawHeaders.find('\n', pos);
    if (end == std::string::npos) end = _rawHeaders.size();
    return _rawHeaders.substr(pos, end - pos);
}

void Curl::setDownloadFile(FILE *fp)
{
    _downloadFile = fp;
}

void Curl::clearDownloadFile()
{
    _downloadFile = nullptr;
}

void Curl::rewindDownloadFile()
{
    if (_downloadFile)
    {
        rewind(_downloadFile);
        ftruncate(fileno(_downloadFile), 0);
    }
}

void Curl::setPatch()
{
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "PATCH");
}

void Curl::clearCustomRequest()
{
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, NULL);
}

void Curl::reset()
{
    curl_easy_reset(_curl);
    _postData.clear();
    _downloadFile = nullptr;
    _applyDefaults();
}

void Curl::setUserPassword(const std::string &user, const std::string &password,
                           long methods)
{
    _userPassword = user + ":" + password;
    curl_easy_setopt(_curl, CURLOPT_USERPWD, _userPassword.c_str());
    curl_easy_setopt(_curl, CURLOPT_HTTPAUTH, methods);
}

void Curl::setCustomRequest(const char *method)
{
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, method);
}

void Curl::setNoBody(bool enabled)
{
    curl_easy_setopt(_curl, CURLOPT_NOBODY, enabled ? 1L : 0L);
}

void Curl::setFileTime(bool enabled)
{
    curl_easy_setopt(_curl, CURLOPT_FILETIME, enabled ? 1L : 0L);
}

void Curl::setUploadFile(FILE *fp, curl_off_t size)
{
    curl_easy_setopt(_curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(_curl, CURLOPT_READFUNCTION, _read_callback);
    curl_easy_setopt(_curl, CURLOPT_READDATA, fp);
    // CURLAUTH_ANY sends the first request without credentials to learn the
    // scheme, so the body goes out once, gets a 401, and has to be sent
    // again.  Without a way to rewind it libcurl gives up with "Send failed
    // since rewinding of the data stream failed" (curl 65) — on every WebDAV
    // PUT, until a retry with the scheme already known got through.
    curl_easy_setopt(_curl, CURLOPT_SEEKFUNCTION, _seek_callback);
    curl_easy_setopt(_curl, CURLOPT_SEEKDATA, fp);
    curl_easy_setopt(_curl, CURLOPT_INFILESIZE_LARGE, size);
}

void Curl::setUseSSL(long level)
{
    curl_easy_setopt(_curl, CURLOPT_USE_SSL, level);
}

void Curl::setFreshConnect(bool enabled)
{
    curl_easy_setopt(_curl, CURLOPT_FRESH_CONNECT, enabled ? 1L : 0L);
}

void Curl::setDirListOnly(bool enabled)
{
    curl_easy_setopt(_curl, CURLOPT_DIRLISTONLY, enabled ? 1L : 0L);
}

void Curl::setCreateMissingDirs(bool enabled)
{
    curl_easy_setopt(_curl, CURLOPT_FTP_CREATE_MISSING_DIRS,
                     enabled ? (long)CURLFTP_CREATE_DIR_RETRY : (long)CURLFTP_CREATE_DIR_NONE);
}

void Curl::setActiveMode(bool enabled)
{
    // "-" makes libcurl listen on its own address instead of issuing PASV.
    curl_easy_setopt(_curl, CURLOPT_FTPPORT, enabled ? "-" : NULL);
}

void Curl::setPostQuote(curl_slist *commands)
{
    curl_easy_setopt(_curl, CURLOPT_POSTQUOTE, commands);
}

void Curl::setWildcardMatch(ChunkBeginFn fn, void *userdata)
{
    curl_easy_setopt(_curl, CURLOPT_WILDCARDMATCH, fn ? 1L : 0L);
    curl_easy_setopt(_curl, CURLOPT_CHUNK_BGN_FUNCTION, fn);
    curl_easy_setopt(_curl, CURLOPT_CHUNK_DATA, userdata);
}

long Curl::getFileTime() const
{
    long filetime = -1;
    curl_easy_getinfo(_curl, CURLINFO_FILETIME, &filetime);
    return filetime;
}

curl_off_t Curl::getContentLength() const
{
    curl_off_t length = -1;
    curl_easy_getinfo(_curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    return length;
}

size_t Curl::_read_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *readhere = (FILE *)userdata;
    size_t read = fread(ptr, size, nmemb, readhere);
    // A short read is normal at the end of the file; an error is not, and
    // libcurl can only report it as a vague "read function failure".
    if (read < nmemb && ferror(readhere))
        debugf("upload: reading the local file failed: %s\n", strerror(errno));
    return read;
}

int Curl::_seek_callback(void *userdata, curl_off_t offset, int origin)
{
    FILE *fp = (FILE *)userdata;
    if (fseek(fp, (long)offset, origin) != 0)
    {
        debugf("upload: cannot rewind the local file: %s\n", strerror(errno));
        return CURL_SEEKFUNC_CANTSEEK;
    }
    return CURL_SEEKFUNC_OK;
}

size_t Curl::_write_callback(void *data, size_t size, size_t nmemb, void *userdata)
{
    Curl *self = static_cast<Curl *>(userdata);
    size_t totalSize = size * nmemb;
    if (self->_downloadFile != nullptr)
    {
        size_t written = fwrite(data, size, nmemb, self->_downloadFile);
        // Returning less than asked aborts the transfer with CURLE_WRITE_ERROR,
        // which says nothing about the SD card being full or write-protected.
        if (written < nmemb)
            debugf("download: writing to the SD card failed after %d of %d "
                   "bytes: %s\n", (int)(written * size), (int)totalSize,
                   strerror(errno));
        return written;
    }
    self->_responseData.append(static_cast<char *>(data), totalSize);
    return totalSize;
}

size_t Curl::_header_callback(void *data, size_t size, size_t nmemb, void *userdata)
{
    Curl *self = static_cast<Curl *>(userdata);
    size_t totalSize = size * nmemb;
    self->_rawHeaders.append(static_cast<char *>(data), totalSize);
    return totalSize;
}

// ---------------------------------------------------------------------------
// _trace_callback  — libcurl's protocol trace, debug mode only
// ---------------------------------------------------------------------------
// Only the informational and header lines are shown: the payload would bury
// them, and a save file printed to the console is noise.  Header lines go
// through debugRedact() because the request ones carry the bearer token.
// ---------------------------------------------------------------------------
int Curl::_trace_callback(CURL *handle, curl_infotype type, char *data,
                          size_t size, void *userdata)
{
    (void)handle;
    (void)userdata;

    const char *prefix;
    switch (type)
    {
    case CURLINFO_TEXT:
        prefix = "*";
        break;
    case CURLINFO_HEADER_OUT:
        prefix = ">";
        break;
    case CURLINFO_HEADER_IN:
        prefix = "<";
        break;
    default:
        return 0; // DATA_IN / DATA_OUT / SSL_DATA_*
    }

    // One call can carry several lines, and every one wants its own prefix.
    std::string text = debugRedact(std::string(data, size));
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, (end == std::string::npos)
                                                  ? std::string::npos
                                                  : end - start);
        while (!line.empty() && (line[line.size() - 1] == '\r' ||
                                 line[line.size() - 1] == ' '))
            line.erase(line.size() - 1);
        if (!line.empty())
            debugf("%s %s\n", prefix, line.c_str());
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return 0;
}
