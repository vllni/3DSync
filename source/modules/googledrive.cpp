#include "googledrive.h"
#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

GoogleDrive::GoogleDrive(const std::string &clientId, const std::string &clientSecret,
                         const std::string &refreshToken, const std::string &folderId,
                         const std::string &directToken)
    : _token(directToken), _clientId(clientId), _clientSecret(clientSecret),
      _refreshToken(refreshToken), _folderId(folderId), _uploadCount(0)
{
}

bool GoogleDrive::ensureToken()
{
    if (!_token.empty()) return true;
    if (_refreshToken.empty()) return false;
    return _refreshAccessToken();
}

void GoogleDrive::upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths)
{
    if (!ensureToken())
    {
        printf("Cannot upload: failed to obtain Google Drive access token\n");
        return;
    }

    for (auto item : paths)
    {
        // Determine the target Drive folder for this path group.
        // The name (item.first.second) is used as a subfolder on Drive;
        // if empty, files land directly in _folderId (or Drive root).
        std::string groupName = item.first.second;
        std::string targetFolderId = _folderId.empty() ? "root" : _folderId;
        if (!groupName.empty())
        {
            size_t start = 0;
            while (start < groupName.size())
            {
                size_t end = groupName.find('/', start);
                if (end == std::string::npos)
                    end = groupName.size();
                std::string segment = groupName.substr(start, end - start);
                if (!segment.empty())
                {
                    std::string segId = _findOrCreateFolder(segment, targetFolderId);
                    if (segId.empty())
                        break; // keep whatever level we reached
                    targetFolderId = segId;
                }
                start = end + 1;
            }
        }

        for (auto path : item.second)
        {
            std::string localPath = item.first.first + path;
            printf("Uploading %s to Google Drive\n", localPath.c_str());
            FILE *file = fopen(localPath.c_str(), "rb");
            if (file == NULL)
            {
                printf("Failed to open %s: %s\n", localPath.c_str(), strerror(errno));
                continue;
            }

            std::string fileContents = _readFile(file);
            std::string boundary = "3DSyncGoogleDriveBoundary" + std::to_string(_uploadCount++);
            while (fileContents.find(boundary) != std::string::npos)
            {
                boundary += "x";
            }
            // File is placed inside the group folder; use only its own name (flat).
            std::string fileName = _driveFileName(path);
            std::string metadata = "{\"name\":\"" + _jsonEscape(fileName) + "\"";
            if (!targetFolderId.empty())
            {
                metadata += ",\"parents\":[\"" + _jsonEscape(targetFolderId) + "\"]";
            }
            metadata += "}";

            std::string body = "--" + boundary + "\r\n";
            body += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
            body += metadata + "\r\n";
            body += "--" + boundary + "\r\n";
            body += "Content-Type: application/octet-stream\r\n\r\n";
            body += fileContents;
            body += "\r\n--" + boundary + "--\r\n";

            std::string auth("Authorization: Bearer " + _token);
            std::string contentType("Content-Type: multipart/related; boundary=" + boundary);
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, auth.c_str());
            headers = curl_slist_append(headers, contentType.c_str());
            headers = curl_slist_append(headers, "Expect:");
            _curl.setURL(std::string("https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart"));
            _curl.setHeaders(headers);
            _curl.setPostData(body);
            if (_performWithRetry() != 0)
            {
                printf("Google Drive upload failed for %s\n", localPath.c_str());
            }
            curl_slist_free_all(headers);
            fclose(file);
            printf("\n");
        }
    }
}

std::string GoogleDrive::_findOrCreateFolder(const std::string &name, const std::string &parentId)
{
    std::string cacheKey = parentId + "/" + name;
    auto it = _folderCache.find(cacheKey);
    if (it != _folderCache.end())
        return it->second;

    // Escape single quotes in the name for Drive query language
    std::string safeName = name;
    size_t pos = 0;
    while ((pos = safeName.find('\'', pos)) != std::string::npos)
    {
        safeName.replace(pos, 1, "\\'");
        pos += 2;
    }

    std::string query = "name='" + safeName + "'"
                                              " and mimeType='application/vnd.google-apps.folder'"
                                              " and '" +
                        parentId + "' in parents"
                                   " and trashed=false";
    std::string url = "https://www.googleapis.com/drive/v3/files"
                      "?fields=files(id)&q=" +
                      _urlEncode(query);

    std::string auth = "Authorization: Bearer " + _token;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    _curl.setURL(url);
    _curl.setHeaders(headers);
    _curl.resetToGet();
    int result = _performWithRetry();
    curl_slist_free_all(headers);

    std::string folderId;
    if (result == 0)
        folderId = _extractJsonString(_curl.getResponse(), "id");

    if (folderId.empty())
    {
        // Not found (or search failed) — create it
        printf("Creating Drive folder: %s\n", name.c_str());
        std::string body = "{\"name\":\"" + _jsonEscape(name) + "\""
                                                                ",\"mimeType\":\"application/vnd.google-apps.folder\""
                                                                ",\"parents\":[\"" +
                           _jsonEscape(parentId) + "\"]}";

        headers = NULL;
        headers = curl_slist_append(headers, auth.c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        _curl.setURL("https://www.googleapis.com/drive/v3/files");
        _curl.setHeaders(headers);
        _curl.setPostData(body);
        result = _performWithRetry();
        curl_slist_free_all(headers);

        if (result == 0)
            folderId = _extractJsonString(_curl.getResponse(), "id");
    }

    if (folderId.empty())
        printf("Warning: could not find or create Drive folder '%s', uploading to parent\n", name.c_str());

    _folderCache[cacheKey] = folderId;
    return folderId;
}

bool GoogleDrive::_refreshAccessToken()
{
    printf("Refreshing Google Drive access token...\n");
    std::string body = "refresh_token=" + _urlEncode(_refreshToken) + "&client_id=" + _urlEncode(_clientId) + "&client_secret=" + _urlEncode(_clientSecret) + "&grant_type=refresh_token";

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    _curl.setURL("https://oauth2.googleapis.com/token");
    _curl.setHeaders(headers);
    _curl.setPostData(body);
    int result = _performWithRetry();
    curl_slist_free_all(headers);

    if (result != 0)
    {
        printf("Token refresh request failed\n");
        return false;
    }

    std::string accessToken = _extractJsonString(_curl.getResponse(), "access_token");
    if (accessToken.empty())
    {
        printf("Failed to parse access_token from refresh response\n");
        return false;
    }

    _token = accessToken;
    printf("Google Drive access token refreshed successfully\n");
    return true;
}

std::string GoogleDrive::_extractJsonString(const std::string &json, const std::string &key)
{
    // Handle both compact ("key":"value") and spaced ("key": "value") JSON
    for (const char *sep : {"\":\"", "\": \""})
    {
        std::string search = "\"" + key + sep;
        size_t pos = json.find(search);
        if (pos != std::string::npos)
        {
            pos += search.size();
            size_t end = json.find('"', pos);
            if (end != std::string::npos)
                return json.substr(pos, end - pos);
        }
    }
    return "";
}

std::string GoogleDrive::_urlEncode(const std::string &value)
{
    std::string result;
    for (unsigned char c : value)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            result += c;
        }
        else
        {
            char buffer[4];
            snprintf(buffer, sizeof(buffer), "%%%02X", c);
            result += buffer;
        }
    }
    return result;
}

std::string GoogleDrive::_jsonEscape(std::string value)
{
    std::string escaped;
    for (auto character : value)
    {
        switch (character)
        {
        case '"':
        case '\\':
            escaped += '\\';
            escaped += character;
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if ((unsigned char)character < 0x20)
            {
                char buffer[7];
                snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned char)character);
                escaped += buffer;
            }
            else
            {
                escaped += character;
            }
            break;
        }
    }
    return escaped;
}

std::string GoogleDrive::_driveFileName(std::string path)
{
    while (path.size() > 0 && path[0] == '/')
    {
        path.erase(0, 1);
    }
    for (auto &character : path)
    {
        if (character == '/')
        {
            character = '_';
        }
    }
    return path;
}

std::string GoogleDrive::_readFile(FILE *file)
{
    std::string contents;
    if (fseek(file, 0, SEEK_END) == 0)
    {
        long size = ftell(file);
        if (size != -1 && size > 0)
        {
            contents.reserve(size);
        }
        if (fseek(file, 0, SEEK_SET) != 0)
        {
            printf("Failed to seek file: %s\n", strerror(errno));
            return contents;
        }
    }

    char buffer[4096];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        contents.append(buffer, read);
    }
    return contents;
}

// ---------------------------------------------------------------------------
// _performWithRetry  — wraps _curl.perform() with up to 3 attempts and a 10 s
// back-off when the server returns HTTP 429 (rate limit exceeded).
// ---------------------------------------------------------------------------
int GoogleDrive::_performWithRetry()
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        int res = _curl.perform();
        if (res == 0) return 0;

        long status = _curl.getStatusCode();
        if (status == 429 && attempt < 2)
        {
            printf("Rate limited (429), waiting 10 seconds...\n");
            svcSleepThread(10000000000LL); // 10 seconds in nanoseconds
            continue;
        }
        return res;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// ensureFolderPath  — resolves or creates each '/'-separated segment of path
// under _folderId, returning the Drive ID of the deepest folder.
// ---------------------------------------------------------------------------
std::string GoogleDrive::ensureFolderPath(const std::string &path)
{
    std::string current = _folderId.empty() ? "root" : _folderId;
    if (path.empty()) return current;

    size_t start = 0;
    while (start < path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string segment = path.substr(start, end - start);
        if (!segment.empty())
        {
            std::string id = _findOrCreateFolder(segment, current);
            if (id.empty()) break;
            current = id;
        }
        start = end + 1;
    }
    return current;
}

// ---------------------------------------------------------------------------
// _listFolderContents  — Drive files.list for a single folder, with
// nextPageToken pagination.  Returns all children (files + sub-folders).
// ---------------------------------------------------------------------------
std::vector<DriveFileInfo> GoogleDrive::_listFolderContents(const std::string &folderId)
{
    std::vector<DriveFileInfo> result;
    std::string pageToken;

    do
    {
        std::string query = "'" + folderId + "' in parents and trashed=false";
        std::string url = "https://www.googleapis.com/drive/v3/files"
                          "?fields=nextPageToken,files(id,name,md5Checksum,modifiedTime,mimeType)"
                          "&pageSize=1000"
                          "&q=" + _urlEncode(query);
        if (!pageToken.empty())
            url += "&pageToken=" + _urlEncode(pageToken);

        std::string auth = "Authorization: Bearer " + _token;
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, auth.c_str());
        _curl.setURL(url);
        _curl.setHeaders(headers);
        _curl.resetToGet();
        int res = _performWithRetry();
        curl_slist_free_all(headers);

        if (res != 0)
        {
            printf("Drive list failed for folder %s\n", folderId.c_str());
            break;
        }

        // Capture the Date header from the first page for clock-skew detection
        if (pageToken.empty())
        {
            std::string date = _curl.getResponseHeader("Date");
            if (!date.empty()) _lastServerTime = date;
        }

        std::string response = _curl.getResponse();
        pageToken = _extractJsonString(response, "nextPageToken");

        // Parse the "files" array: find each '{' that opens a file object
        size_t arrPos = response.find("\"files\"");
        if (arrPos == std::string::npos) break;
        arrPos = response.find('[', arrPos);
        if (arrPos == std::string::npos) break;

        size_t objPos = arrPos;
        while ((objPos = response.find('{', objPos + 1)) != std::string::npos)
        {
            size_t objEnd = response.find('}', objPos + 1);
            if (objEnd == std::string::npos) break;
            std::string obj = response.substr(objPos, objEnd - objPos + 1);

            DriveFileInfo info;
            info.id           = _extractJsonString(obj, "id");
            info.name         = _extractJsonString(obj, "name");
            info.md5          = _extractJsonString(obj, "md5Checksum");
            info.modifiedTime = _extractJsonString(obj, "modifiedTime");
            std::string mime  = _extractJsonString(obj, "mimeType");
            info.isFolder     = (mime == "application/vnd.google-apps.folder");

            if (!info.id.empty() && !info.name.empty())
                result.push_back(info);

            objPos = objEnd;
        }
    } while (!pageToken.empty());

    return result;
}

// ---------------------------------------------------------------------------
// _listFolderRecursiveImpl  — depth-first traversal; prefix is the relative
// path built so far (starts empty, gains "/<name>" per level).
// ---------------------------------------------------------------------------
void GoogleDrive::_listFolderRecursiveImpl(const std::string &folderId,
                                            const std::string &prefix,
                                            std::map<std::string, DriveFileInfo> &result)
{
    auto items = _listFolderContents(folderId);
    for (auto &item : items)
    {
        std::string rel = prefix + "/" + item.name;
        if (item.isFolder)
        {
            _listFolderRecursiveImpl(item.id, rel, result);
        }
        else
        {
            item.relPath = rel;
            result[rel]  = item;
        }
    }
}

std::map<std::string, DriveFileInfo> GoogleDrive::listFolderRecursive(const std::string &folderId)
{
    std::map<std::string, DriveFileInfo> result;
    _listFolderRecursiveImpl(folderId, "", result);
    return result;
}

// ---------------------------------------------------------------------------
// syncUpload  — upload or update a local file preserving the relative path
// structure on Drive (subfolders are created on the fly).
// Returns the Drive file ID; fills outMd5 from the Drive response.
// ---------------------------------------------------------------------------
std::string GoogleDrive::syncUpload(const std::string &rootFolderId,
                                    const std::string &relPath,
                                    const std::string &localPath,
                                    const std::string &existingId,
                                    std::string &outMd5)
{
    // Resolve the parent Drive folder, creating subfolders as needed
    std::string parentFolderId = rootFolderId;
    std::string fileName;

    // Split relPath (e.g. "/saves/TitleA/001.sav") into dir + filename
    std::string trimmed = relPath;
    while (!trimmed.empty() && trimmed[0] == '/') trimmed.erase(0, 1);

    size_t lastSlash = trimmed.rfind('/');
    if (lastSlash != std::string::npos)
    {
        std::string dirPart  = trimmed.substr(0, lastSlash);
        fileName             = trimmed.substr(lastSlash + 1);

        // Create intermediate folders
        size_t start = 0;
        while (start < dirPart.size())
        {
            size_t end = dirPart.find('/', start);
            if (end == std::string::npos) end = dirPart.size();
            std::string seg = dirPart.substr(start, end - start);
            if (!seg.empty())
            {
                std::string id = _findOrCreateFolder(seg, parentFolderId);
                if (id.empty())
                {
                    printf("syncUpload: cannot create folder '%s'\n", seg.c_str());
                    return "";
                }
                parentFolderId = id;
            }
            start = end + 1;
        }
    }
    else
    {
        fileName = trimmed;
    }

    FILE *file = fopen(localPath.c_str(), "rb");
    if (!file)
    {
        printf("syncUpload: cannot open %s: %s\n", localPath.c_str(), strerror(errno));
        return "";
    }
    std::string fileContents = _readFile(file);
    fclose(file);

    std::string boundary = "3DSyncBoundary" + std::to_string(_uploadCount++);
    while (fileContents.find(boundary) != std::string::npos)
        boundary += "x";

    // Metadata part
    std::string metadata = "{\"name\":\"" + _jsonEscape(fileName) + "\"";
    if (existingId.empty())
        metadata += ",\"parents\":[\"" + _jsonEscape(parentFolderId) + "\"]";
    metadata += "}";

    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
    body += metadata + "\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Type: application/octet-stream\r\n\r\n";
    body += fileContents;
    body += "\r\n--" + boundary + "--\r\n";

    std::string auth        = "Authorization: Bearer " + _token;
    std::string contentType = "Content-Type: multipart/related; boundary=" + boundary;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, contentType.c_str());
    headers = curl_slist_append(headers, "Expect:");

    std::string url = "https://www.googleapis.com/upload/drive/v3/files";
    if (existingId.empty())
    {
        url += "?uploadType=multipart&fields=id,md5Checksum";
    }
    else
    {
        url += "/" + existingId + "?uploadType=multipart&fields=id,md5Checksum";
    }

    _curl.setURL(url);
    _curl.setHeaders(headers);
    _curl.setPostData(body);
    if (!existingId.empty()) _curl.setPatch();
    int res = _performWithRetry();
    if (!existingId.empty()) _curl.clearCustomRequest();
    curl_slist_free_all(headers);

    if (res != 0)
    {
        printf("syncUpload failed for %s\n", localPath.c_str());
        return "";
    }

    std::string response = _curl.getResponse();
    std::string fileId   = _extractJsonString(response, "id");
    outMd5               = _extractJsonString(response, "md5Checksum");

    // If Drive omitted md5Checksum (shouldn't happen for binary files), fall back to ""
    return fileId;
}

// ---------------------------------------------------------------------------
// downloadFile  — stream Drive file content to a temp file then atomically
// rename to localPath to avoid partial writes on failure.
// ---------------------------------------------------------------------------
bool GoogleDrive::downloadFile(const DriveFileInfo &file, const std::string &localPath)
{
    std::string tmpPath = localPath + ".3dstmp";
    FILE *fp = fopen(tmpPath.c_str(), "wb");
    if (!fp)
    {
        printf("downloadFile: cannot create temp file %s: %s\n", tmpPath.c_str(), strerror(errno));
        return false;
    }

    std::string url  = "https://www.googleapis.com/drive/v3/files/" + file.id + "?alt=media";
    std::string auth = "Authorization: Bearer " + _token;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());

    _curl.setURL(url);
    _curl.setHeaders(headers);
    _curl.setDownloadFile(fp);
    _curl.resetToGet();
    int res = _performWithRetry();
    _curl.clearDownloadFile();
    curl_slist_free_all(headers);
    fclose(fp);

    if (res != 0)
    {
        printf("downloadFile failed for %s\n", localPath.c_str());
        remove(tmpPath.c_str());
        return false;
    }

    if (rename(tmpPath.c_str(), localPath.c_str()) != 0)
    {
        printf("downloadFile: rename failed: %s\n", strerror(errno));
        remove(tmpPath.c_str());
        return false;
    }

    printf("Downloaded %s\n", localPath.c_str());
    return true;
}

std::string GoogleDrive::getServerTime() const
{
    return _lastServerTime;
}
