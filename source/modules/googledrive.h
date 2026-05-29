#ifndef MODULES_GOOGLEDRIVE_H
#define MODULES_GOOGLEDRIVE_H

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "../utils/curl.h"

// Metadata for one file returned by the Drive files.list API.
struct DriveFileInfo
{
    std::string id;
    std::string name;          // bare file name (no path)
    std::string relPath;       // relative path from the listing root (set by listFolderRecursive)
    std::string modifiedTime;  // RFC 3339 string from Drive
    std::string md5;           // md5Checksum (empty for Google-native types)
    bool        isFolder;
};

class GoogleDrive
{
public:
    // Refresh-token based construction (preferred): exchanges refresh token for
    // access token on first use.  clientId, clientSecret, and refreshToken come
    // from the [GoogleDrive] section of the INI.  For backwards compatibility a
    // pre-existing access token can be supplied via directToken.
    GoogleDrive(const std::string &clientId, const std::string &clientSecret,
                const std::string &refreshToken, const std::string &folderId = std::string(),
                const std::string &directToken = std::string());
    ~GoogleDrive() {}

    // Ensures the access token is valid; call once before any sync operations.
    bool ensureToken();

    // Returns true if a fatal, unrecoverable API error has occurred (e.g. 401
    // auth failure or 403 API-disabled).  When true, all subsequent Drive calls
    // will be skipped and the sync loop should be aborted.
    bool hasFatalError() const;

    // Legacy flat-name upload (unchanged behaviour).
    bool upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths);

    // Resolve (and create if missing) a '/'-separated folder path under _folderId.
    // Returns the Drive folder ID of the deepest component, or "" on failure.
    std::string ensureFolderPath(const std::string &path);

    // Recursively list all non-folder files under folderId.
    // Returns map<relPath, DriveFileInfo> where relPath starts with '/'.
    std::map<std::string, DriveFileInfo> listFolderRecursive(const std::string &folderId);

    // Upload a local file to a Drive folder (creates or overwrites).
    // relPath is the path relative to the sync root (e.g. "/saves/Game/001.sav").
    // rootFolderId is the ID of the sync-root Drive folder.
    // existingId: if non-empty, PATCH the existing Drive file instead of creating.
    // Returns the Drive file ID and fills outMd5 with the md5Checksum from Drive.
    std::string syncUpload(const std::string &rootFolderId,
                           const std::string &relPath,
                           const std::string &localPath,
                           const std::string &existingId,
                           std::string &outMd5);

    // Download a Drive file to localPath using a temp file + atomic rename.
    bool downloadFile(const DriveFileInfo &file, const std::string &localPath);

    // Date header value from the most recent API response (RFC 7231 format).
    // Use to detect 3DS clock skew.
    std::string getServerTime() const;

private:
    std::string _token;
    std::string _clientId;
    std::string _clientSecret;
    std::string _refreshToken;
    std::string _folderId;
    std::string _lastServerTime;
    int         _uploadCount;
    Curl        _curl;
    std::map<std::string, std::string> _folderCache;

    bool        _refreshAccessToken();
    bool        _fatalError;
    // Find or create a single folder segment; returns folder ID or "".
    std::string _findOrCreateFolder(const std::string &name, const std::string &parentId);
    // Raw Drive files.list for one folder (non-recursive, handles pagination).
    std::vector<DriveFileInfo> _listFolderContents(const std::string &folderId);
    // Recursive helper used by listFolderRecursive.
    void        _listFolderRecursiveImpl(const std::string &folderId,
                                         const std::string &prefix,
                                         std::map<std::string, DriveFileInfo> &result);
    // perform() with automatic 429 retry (up to 3 attempts, 10 s wait each).
    int         _performWithRetry();

    std::string _extractJsonString(const std::string &json, const std::string &key);
    std::string _urlEncode(const std::string &value);
    std::string _jsonEscape(std::string value);
    std::string _driveFileName(std::string path);
    std::string _readFile(FILE *file);
};

#endif
