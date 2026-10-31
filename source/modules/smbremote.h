#ifndef MODULES_SMBREMOTE_H
#define MODULES_SMBREMOTE_H

#include <string>

#include "syncprovider.h"

struct smb2_context;
struct smb2_stat_64;

// SMB2/SMB3 remote, for a NAS or a Windows share.  Backed by libsmb2, whose
// synchronous API blocks on poll(); libctru provides it, so no event loop of
// our own is needed.  SMB1 is deliberately not supported — libsmb2 speaks 2.02
// and up, which is what modern servers accept out of the box.
//
// The session is held for the whole run and may sit idle for minutes (a
// conflict prompt, another remote's turn, the lid shut).  Before a request
// after such a pause, or after one that failed, it is checked with an ECHO and
// reopened if the server or the network dropped it — see smbShouldProbe().
class SmbRemote : public SyncProvider
{
public:
    SmbRemote(const std::string &server, const std::string &share,
              const std::string &user, const std::string &password,
              const std::string &domain, const std::string &basePath);
    ~SmbRemote();

    const char *name() const override { return "SMB"; }
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

private:
    std::string _server;
    std::string _share;
    std::string _user;
    std::string _password;
    std::string _domain;
    std::string _basePath;
    struct smb2_context *_smb2;
    bool _connected;
    bool _fatalError;
    long long _lastActivity; // time() of the last request's end
    bool _lastFailed;        // ...and whether it failed

    // Open a context and connect the share; on failure error says why and
    // nothing is left open.
    bool _openSession(std::string &error);
    // polite: tell the server first.  Not for a session that is already dead,
    // where that would only wait out the timeout.
    void _closeSession(bool polite);
    // Make sure the session is usable before a request, reconnecting once if
    // it was dropped.  False (with _fatalError set) when it cannot be.
    bool _ensureSession();
    // Note how a request ended, for the next _ensureSession().
    void _settle(bool ok);
    // libsmb2's last error, without the trailing newline it sometimes has.
    // Valid until the next call.
    const char *_error();
    std::string _lastError;

    bool _download(const RemoteFileInfo &file, const std::string &localPath);
    bool _upload(const std::string &root, const std::string &relPath,
                 const std::string &localPath, std::string &outTag, std::string &outId);

    // Create every missing component of a share-relative directory path.
    bool _mkdirs(const std::string &dirPath);
    // Recursive worker for list(); prefix is the relPath built so far.
    bool _listInto(const std::string &dirPath, const std::string &prefix,
                   std::map<std::string, RemoteFileInfo> &out);
    // "<size>:<mtime>" — SMB offers no server-side checksum.
    static std::string _tagFor(const struct smb2_stat_64 &st);
};

#endif
