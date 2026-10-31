#ifndef MODULES_FTPREMOTE_H
#define MODULES_FTPREMOTE_H

#include <set>
#include <string>
#include <vector>

#include "../utils/curl.h"
#include "remoteparse.h" // FtpTlsMode
#include "syncprovider.h"

// FTP / FTPS remote.  libcurl on the 3DS is built with FTP and FTPS but without
// libssh2, so SFTP is not reachable from here.
//
// FTP has no checksums, and the timestamps in a LIST response are not parsed by
// libcurl (curl_fileinfo::time is documented "always zero"), so listing runs in
// two passes: a wildcard LIST for names and types, then one MDTM/SIZE request
// per file to build the change tag.
class FtpRemote : public SyncProvider
{
public:
    FtpRemote(const std::string &host, int port, const std::string &user,
              const std::string &password, const std::string &basePath,
              FtpTlsMode tls, bool activeMode, bool verifyTls = true);

    const char *name() const override { return "FTP"; }
    bool isExperimental() const override { return false; }
    std::string manifestPrefix() const override;
    bool connect() override;
    bool hasFatalError() const override;
    std::string ensureRoot(const std::string &remoteName) override;
    bool list(const std::string &root,
              std::map<std::string, RemoteFileInfo> &out) override;
    bool download(const RemoteFileInfo &file, const std::string &localPath) override;
    bool upload(const std::string &root, const std::string &relPath,
                const std::string &localPath, const RemoteFileInfo *existing,
                std::string &outTag, std::string &outId) override;
    std::string serverTime() const override { return _lastServerTime; }

private:
    std::string _host;
    int _port;
    std::string _user;
    std::string _password;
    std::string _basePath;
    FtpTlsMode _tls;
    // Set once an anonymous TLS=try login was refused over TLS and retried in
    // plaintext; every later request of the run then goes in plaintext too.
    bool _tlsDropped;
    bool _activeMode;
    bool _fatalError;
    // Folders an upload could not create or enter, explained once each.
    std::set<std::string> _refusedDirs;
    std::string _lastServerTime;
    Curl _curl;

    // One directory's worth of entries gathered by the wildcard callback.
    struct ListingContext
    {
        std::vector<std::string> files;
        std::vector<std::string> dirs;
    };
    static long _onListEntry(const void *transferInfo, void *userdata, int remains);

    // _tls, or FTP_TLS_NONE once _tlsDropped.
    FtpTlsMode _effectiveTls() const;
    // Reset the handle and apply the connection options every request needs.
    void _prepare();
    // "ftp://host:port/" + url-encoded path.
    std::string _urlFor(const std::string &path) const;
    // perform() with back-off; maps curl codes to fatal / transient / per-file.
    // request says what the call was for, which decides whether a refused
    // directory is fatal — see ftpRefusalOutcome().
    int _performWithRetry(FtpRequest request, FILE *uploadFile = NULL);
    bool _listDir(const std::string &dirPath, const std::string &prefix,
                  std::map<std::string, RemoteFileInfo> &out);
    // MDTM + SIZE for one file, as "<size>:<mtime>".  "" when unavailable.
    std::string _tagFor(const std::string &path);
};

#endif
