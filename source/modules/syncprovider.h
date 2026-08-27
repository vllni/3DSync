#ifndef MODULES_SYNCPROVIDER_H
#define MODULES_SYNCPROVIDER_H

#include <map>
#include <string>
#include <vector>

// One remote file, as reported by SyncProvider::list().
struct RemoteFileInfo
{
    // Provider handle for the file: the Drive file ID, or the remote path for
    // the path-addressed backends (SMB, FTP, WebDAV).
    std::string id;
    // '/'-prefixed path relative to the sync root, e.g. "/Game/001.sav".
    std::string relPath;
    // Opaque change token.  Compared against ManifestEntry::remoteTag to decide
    // whether the remote side changed: an MD5 for Drive, an ETag for WebDAV, or
    // "<size>:<mtime>" where the server offers nothing better.
    std::string tag;
};

// A remote that performSync() can sync a local folder against.  Everything the
// sync engine needs is here; anything provider-specific stays in the module.
class SyncProvider
{
public:
    virtual ~SyncProvider() {}

    // Short name for log lines, e.g. "Drive" or "SMB".
    virtual const char *name() const = 0;

    // Whether this backend is still experimental.  Defaults to true so a newly
    // added remote warns until someone has deliberately marked it stable: the
    // 3DS prints a notice with the issue tracker URL before syncing, and the
    // docs badge it.  Only Google Drive returns false.
    virtual bool isExperimental() const { return true; }

    // Stable identifier for *this* remote, used to scope manifest keys so two
    // providers syncing the same local folder cannot overwrite each other's
    // state.  Include the host where one exists: "drive", "smb://nas/3ds",
    // "https://host/dav/".  Changing it for an existing remote makes the next
    // sync treat every file as unseen.
    virtual std::string manifestPrefix() const = 0;

    // Authenticate / open the connection.  Called once before any transfer.
    virtual bool connect() = 0;

    // True once an unrecoverable error occurred (bad credentials, API
    // disabled, …).  The sync loop stops and remaining entries are skipped.
    virtual bool hasFatalError() const = 0;

    // Resolve (creating if missing) the remote folder for a configured entry.
    // Returns an opaque root handle — a Drive folder ID or a remote path — or
    // "" on failure.  The handle is passed back to list() and upload().
    virtual std::string ensureRoot(const std::string &remoteName) = 0;

    // List every file (not folder) under root, keyed by relPath.
    virtual bool list(const std::string &root,
                      std::map<std::string, RemoteFileInfo> &out) = 0;

    // Download file to localPath.  Must land atomically: write to a temp file
    // and swap it in, so an interrupted transfer cannot destroy a save.
    virtual bool download(const RemoteFileInfo &file,
                          const std::string &localPath) = 0;

    // Upload localPath to relPath under root, creating parent folders.
    // existing is the matching entry from list(), or NULL when the remote file
    // is new.  On success outTag/outId receive the new change token and handle.
    virtual bool upload(const std::string &root,
                        const std::string &relPath,
                        const std::string &localPath,
                        const RemoteFileInfo *existing,
                        std::string &outTag,
                        std::string &outId) = 0;

    // Local equivalent of a remote tag, when the two are comparable (Drive
    // returns an MD5, so a local MD5 can be checked against it).  Used only as
    // a fallback when the local mtime looks unchanged but may be stale — FAT
    // has 2-second granularity and some emulators never update it at all.
    // Return "" (the default) when the provider's tags are not derivable from
    // local file contents.
    virtual std::string localTag(const std::string &localPath)
    {
        (void)localPath;
        return "";
    }

    // Upload-only entries (UploadPaths=) on a provider that shipped its own
    // one-way upload before this interface existed.  Returning false — the
    // default — makes the engine mirror the folder with performSync() instead,
    // which is manifest-aware and skips unchanged files.  Only Google Drive
    // overrides this, to keep the flat remote naming its users already have.
    virtual bool legacyUpload(const std::string &localBase,
                              const std::string &remoteName,
                              const std::vector<std::string> &files)
    {
        (void)localBase;
        (void)remoteName;
        (void)files;
        return false;
    }

    // Server clock as an RFC 3339 or RFC 7231 string, for skew detection.
    // "" when the provider does not report one.
    virtual std::string serverTime() const { return ""; }
};

#endif
