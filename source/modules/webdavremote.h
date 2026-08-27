#ifndef MODULES_WEBDAVREMOTE_H
#define MODULES_WEBDAVREMOTE_H

#include <string>
#include <vector>

#include "../utils/curl.h"
#include "syncprovider.h"

// WebDAV remote — Nextcloud/ownCloud, a NAS with WebDAV enabled, or any server
// speaking PROPFIND/MKCOL/PUT/MOVE.
//
// Listing uses PROPFIND with Depth: infinity where the server allows it and
// falls back to a Depth: 1 walk where it does not (Apache's mod_dav and
// Nextcloud both refuse infinite depth by default).  Change detection prefers
// the ETag and falls back to size plus Last-Modified.
class WebDavRemote : public SyncProvider
{
public:
    WebDavRemote(const std::string &baseUrl, const std::string &user,
                 const std::string &password);

    const char *name() const override { return "WebDAV"; }
    std::string manifestPrefix() const override { return _baseUrl; }
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
    std::string _baseUrl;  // always ends with '/'
    std::string _basePath; // path component of _baseUrl, for href matching
    std::string _user;
    std::string _password;
    bool _fatalError;
    bool _noInfiniteDepth; // set once a server rejects Depth: infinity
    std::string _lastServerTime;
    Curl _curl;

    struct DavEntry
    {
        std::string relPath; // relative to the PROPFIND root
        std::string tag;
        bool isCollection;
    };

    void _prepare();
    std::string _urlFor(const std::string &path) const;
    // perform() with back-off; maps HTTP status to fatal / transient / per-file.
    int _performWithRetry(FILE *uploadFile = NULL);
    // One PROPFIND at the given depth ("0", "1" or "infinity").
    bool _propfind(const std::string &path, const char *depth,
                   std::vector<DavEntry> &out);
    bool _listDepthOne(const std::string &root, const std::string &prefix,
                       std::map<std::string, RemoteFileInfo> &out);
    bool _mkcol(const std::string &path);
    // Drop XML namespace prefixes so tags can be matched by local name alone.
    static std::string _stripNamespaces(const std::string &xml);
    static std::string _tagBetween(const std::string &xml, const std::string &tag,
                                   size_t from, size_t to);
    static std::string _normalizeEtag(std::string etag);
};

#endif
