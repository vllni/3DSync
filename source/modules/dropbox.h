#ifndef MODULES_DROPBOX_H
#define MODULES_DROPBOX_H

#include <string>
#include <vector>
#include <map>

#include "../utils/curl.h"

class Dropbox
{
public:
    Dropbox(std::string token);
    ~Dropbox() {};

    // Verify the token before any transfer is attempted (POST /2/check/user,
    // which needs no scopes).  Dropbox tokens from the configurator are
    // short-lived, so a stale one is the most common failure.
    bool validateToken();

    bool upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths);

    // True once an unrecoverable API error occurred (401 / 403).  All further
    // Dropbox calls are skipped and the caller should stop uploading.
    bool hasFatalError() const;

private:
    std::string _token;
    Curl _curl;
    bool _fatalError;

    // perform() with back-off on network errors, 429 (honouring Retry-After)
    // and 5xx.  Returns 0 on HTTP 2xx, the curl error code on a network
    // failure, or the HTTP status for a per-file error.  Sets _fatalError on
    // 401/403.  uploadFile, when given, is rewound before each retry so the
    // streamed request body starts from the beginning.
    int _performWithRetry(FILE *uploadFile = NULL);

    // Dropbox-API-Arg is JSON carried in an HTTP header, so it has to be pure
    // ASCII: escape the JSON specials and emit non-ASCII as \uXXXX.
    static std::string _headerJsonEscape(const std::string &value);
};

#endif
