#ifndef MODULES_DROPBOX_H
#define MODULES_DROPBOX_H

#include <string>
#include <vector>

#include "../utils/curl.h"
#include "syncprovider.h"

// Dropbox remote with full bidirectional sync.
//
// Dropbox publishes no MD5: its change token is content_hash, a SHA-256 over
// the SHA-256 of each 4 MiB block, which computeDropboxHash() reproduces
// locally.  That makes Dropbox the only remote besides Drive that can verify a
// file by content rather than by timestamp.
//
// Files are addressed by "rev:<rev>" when downloading, so the bytes that arrive
// are the revision that was listed even if the file changes mid-sync.
class Dropbox : public SyncProvider
{
public:
    // appKey/appSecret/refreshToken come from the [Dropbox] section.  A
    // refresh token is what allows unattended sync: Dropbox access tokens
    // expire after about four hours.  A bare short-lived token is still
    // accepted via directToken for configs written before that flow existed.
    Dropbox(const std::string &appKey, const std::string &appSecret,
            const std::string &refreshToken, const std::string &basePath,
            const std::string &directToken = std::string());
    ~Dropbox() {}

    const char *name() const override { return "Dropbox"; }
    std::string manifestPrefix() const override { return "dropbox"; }
    bool connect() override;
    bool hasFatalError() const override;
    std::string ensureRoot(const std::string &remoteName) override;
    bool list(const std::string &root,
              std::map<std::string, RemoteFileInfo> &out) override;
    bool download(const RemoteFileInfo &file, const std::string &localPath) override;
    bool upload(const std::string &root, const std::string &relPath,
                const std::string &localPath, const RemoteFileInfo *existing,
                std::string &outTag, std::string &outId) override;
    // content_hash is derivable from local file contents, so an unchanged
    // mtime can still be checked properly.
    std::string localTag(const std::string &localPath) override;
    std::string serverTime() const override { return _lastServerTime; }

    // Verify the token before any transfer is attempted (POST /2/check/user,
    // which needs no scopes).  Public so a caller can probe credentials.
    bool validateToken();

private:
    std::string _appKey;
    std::string _appSecret;
    std::string _refreshToken;
    std::string _basePath;
    std::string _token;
    std::string _lastServerTime;
    bool _fatalError;
    Curl _curl;

    bool _refreshAccessToken();

    // perform() with back-off on network errors, 429 (honouring Retry-After)
    // and 5xx.  Returns 0 on HTTP 2xx, the curl error code on a network
    // failure, or the HTTP status for a per-file error.  Sets _fatalError on
    // 401/403.  uploadFile, when given, is rewound before each retry so the
    // streamed request body starts from the beginning.
    //
    // expectedStatus names one status the caller handles as a normal outcome
    // rather than a failure: it is still returned, and still explained through
    // debugf(), but it is not announced on the console.  Dropbox answers
    // "the folder is already there" with 409, and a red API error for the
    // most ordinary thing a sync can do is only alarming.
    int _performWithRetry(FILE *uploadFile = NULL, long expectedStatus = 0);

    // POST a JSON body to an api.dropboxapi.com RPC endpoint.
    int _rpc(const std::string &endpoint, const std::string &jsonBody,
             long expectedStatus = 0);

    // create_folder_v2 for a root that ensureRoot's probe found missing.
    // Returns root, or "" if it could not be created.
    std::string _createFolder(const std::string &root);

    // Print the reason behind a 409 that _rpc() was told to expect, for the
    // cases where the expectation turned out to be wrong.
    void _report409Reason();

    // Absolute Dropbox path for a sync-relative path ("" for the root).
    std::string _pathFor(const std::string &path) const;
};

#endif
