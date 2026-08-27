#ifndef UTILS_CURL_H
#define UTILS_CURL_H

#include <cstdio>
#include <string>

#include <curl/curl.h>

class Curl
{
public:
    Curl();
    ~Curl();
    void setURL(std::string URL);
    void setHeaders(curl_slist *headers);
    // Stream the request body from a FILE*.  When size >= 0 the body length is
    // announced with Content-Length; without it libcurl falls back to chunked
    // transfer encoding, which the Dropbox content endpoints reject.
    void setReadData(void *pointer, curl_off_t size = -1);
    void setPostData(const std::string &data);
    void resetToGet();

    // Restore the handle to the constructor defaults, dropping every option set
    // since.  Options are sticky on a libcurl handle, so a request style that
    // differs from the previous one (NOBODY, UPLOAD, WILDCARDMATCH, a custom
    // method, …) must start from a clean slate.
    void reset();

    // Credentials for protocols that authenticate per request (FTP, WebDAV).
    void setUserPassword(const std::string &user, const std::string &password);
    // Override the HTTP method (PROPFIND, MKCOL, DELETE, …).
    void setCustomRequest(const char *method);
    // Request headers only, no body (HTTP HEAD, or FTP MDTM/SIZE).
    void setNoBody(bool enabled);
    // Ask the server for the file's modification time; read with getFileTime().
    void setFileTime(bool enabled);
    // Stream a request body from a FILE* as an upload (HTTP PUT, FTP STOR).
    void setUploadFile(FILE *fp, curl_off_t size);
    // TLS level for FTP: CURLUSESSL_NONE / _TRY / _CONTROL / _ALL.
    void setUseSSL(long level);
    // FTP: NLST instead of LIST (names only).
    void setDirListOnly(bool enabled);
    // FTP: create missing directories on upload.
    void setCreateMissingDirs(bool enabled);
    // FTP: use active mode instead of passive.
    void setActiveMode(bool enabled);
    // FTP: commands to run after a successful transfer (RNFR/RNTO, DELE, …).
    // Prefix a command with '*' to ignore its failure.  Pass NULL to clear.
    void setPostQuote(curl_slist *commands);

    // Wildcard listing (FTP): the callback runs once per matched entry with a
    // struct curl_fileinfo *, and returns CURL_CHUNK_BGN_FUNC_SKIP to list
    // without transferring.  Clear it before any other request on this handle.
    typedef long (*ChunkBeginFn)(const void *transferInfo, void *userdata, int remains);
    void setWildcardMatch(ChunkBeginFn fn, void *userdata);

    // Modification time from the last response, or -1 when unknown.
    long getFileTime() const;
    // Body length the server announced, or -1 when unknown.
    curl_off_t getContentLength() const;
    // Direct response body to a FILE* instead of the internal string buffer.
    // Call clearDownloadFile() after perform() to restore buffering mode.
    void setDownloadFile(FILE *fp);
    void clearDownloadFile();
    // Rewind and truncate the download target to 0 bytes.  Call before
    // retrying a streamed download so a fresh response isn't appended after
    // a partial or error body.
    void rewindDownloadFile();
    // Override the HTTP method to PATCH (body still set via setPostData).
    // Call clearCustomRequest() after perform() to reset.
    void setPatch();
    void clearCustomRequest();
    int perform();
    std::string getResponse() const;
    long getStatusCode() const;
    // Case-insensitive search in the last response's headers (e.g. "Date").
    std::string getResponseHeader(const std::string &name) const;

private:
    CURL *_curl;
    std::string _postData;
    std::string _userPassword;
    std::string _responseData;
    std::string _rawHeaders;
    FILE *_downloadFile;
    void _applyDefaults();
    static size_t _read_callback(void *ptr, size_t size, size_t nmemb, void *userdata);
    static size_t _write_callback(void *data, size_t size, size_t nmemb, void *userdata);
    static size_t _header_callback(void *data, size_t size, size_t nmemb, void *userdata);
};

#endif
