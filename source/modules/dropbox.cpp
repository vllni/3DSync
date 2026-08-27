#include "dropbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "../utils/fsutil.h"

// Dropbox rejects a single-request upload above 150 MB; larger files need the
// upload_session endpoints, which nothing on a 3DS SD card should require.
static const long long MAX_UPLOAD_BYTES = 150LL * 1024 * 1024;

Dropbox::Dropbox(const std::string &appKey, const std::string &appSecret,
                 const std::string &refreshToken, const std::string &basePath,
                 const std::string &directToken)
    : _appKey(appKey), _appSecret(appSecret), _refreshToken(refreshToken),
      _token(directToken), _fatalError(false)
{
    // Normalise to "" or "/a/b" — Dropbox wants the root as an empty string.
    _basePath = basePath;
    while (!_basePath.empty() && _basePath[0] == '/')
        _basePath.erase(0, 1);
    while (!_basePath.empty() && _basePath[_basePath.size() - 1] == '/')
        _basePath.erase(_basePath.size() - 1);
    if (!_basePath.empty())
        _basePath = "/" + _basePath;
}

bool Dropbox::hasFatalError() const { return _fatalError; }

std::string Dropbox::_pathFor(const std::string &path) const
{
    std::string clean = path;
    while (!clean.empty() && clean[clean.size() - 1] == '/')
        clean.erase(clean.size() - 1);
    if (!clean.empty() && clean[0] != '/')
        clean = "/" + clean;
    return _basePath + clean;
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

bool Dropbox::_refreshAccessToken()
{
    printf("Refreshing Dropbox access token...\n");

    std::string body = "grant_type=refresh_token&refresh_token=" + _refreshToken;
    // A PKCE app authenticates with its key alone; a confidential app also
    // sends the secret.
    body += "&client_id=" + _appKey;
    if (!_appSecret.empty())
        body += "&client_secret=" + _appSecret;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    _curl.reset();
    _curl.setURL(std::string("https://api.dropbox.com/oauth2/token"));
    _curl.setHeaders(headers);
    _curl.setPostData(body);
    int res = _performWithRetry();
    curl_slist_free_all(headers);

    if (res != 0)
    {
        printf(CONSOLE_RED "  FATAL: Dropbox token refresh failed." CONSOLE_RESET "\n");
        printf("  The refresh token may have been revoked.\n");
        printf("  Re-run the configurator to obtain a new one.\n");
        _fatalError = true;
        return false;
    }

    std::string accessToken = _jsonString(_curl.getResponse(), "access_token");
    if (accessToken.empty())
    {
        printf("Failed to parse access_token from refresh response\n");
        _fatalError = true;
        return false;
    }

    _token = accessToken;
    return true;
}

bool Dropbox::connect()
{
    if (!_refreshToken.empty())
    {
        if (_appKey.empty())
        {
            printf(CONSOLE_RED "Dropbox: RefreshToken needs AppKey." CONSOLE_RESET "\n");
            _fatalError = true;
            return false;
        }
        if (!_refreshAccessToken())
            return false;
    }

    if (_token.empty())
    {
        printf(CONSOLE_RED "Dropbox: no token configured." CONSOLE_RESET "\n");
        _fatalError = true;
        return false;
    }

    return validateToken();
}

bool Dropbox::validateToken()
{
    if (_rpc("https://api.dropboxapi.com/2/check/user", "{\"query\":\"3DSync\"}") != 0)
    {
        printf(CONSOLE_RED "Dropbox token rejected." CONSOLE_RESET "\n");
        if (_refreshToken.empty())
            printf("  Static tokens expire after ~4 hours. Re-run the\n"
                   "  configurator to get a refresh token instead.\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

int Dropbox::_rpc(const std::string &endpoint, const std::string &jsonBody)
{
    std::string auth("Authorization: Bearer " + _token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    _curl.reset();
    _curl.setURL(endpoint);
    _curl.setHeaders(headers);
    _curl.setPostData(jsonBody);
    int res = _performWithRetry();
    curl_slist_free_all(headers);
    return res;
}

int Dropbox::_performWithRetry(FILE *uploadFile)
{
    if (_fatalError)
        return -1;

    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (attempt > 0 && uploadFile != NULL)
            rewind(uploadFile); // the previous attempt consumed part of the file

        int curlRes = _curl.perform();
        long status = _curl.getStatusCode();

        std::string date = _curl.getResponseHeader("Date");
        if (!date.empty())
            _lastServerTime = date;

        // Network-level failure (timeout, DNS, TLS, …)
        if (curlRes != 0)
        {
            printf("  Network error (attempt %d/3)\n", attempt + 1);
            if (attempt < 2)
            {
                _curl.rewindDownloadFile(); // discard any partial body
                svcSleepThread(2000000000LL);
                continue;
            }
            return curlRes;
        }

        if (status >= 200 && status < 300)
            return 0;

        printf(CONSOLE_RED "  Dropbox API error: HTTP %ld" CONSOLE_RESET "\n", status);

        // Rate limited — Dropbox states the wait in Retry-After.
        if (status == 429 && attempt < 2)
        {
            long wait = strtol(_curl.getResponseHeader("Retry-After").c_str(), NULL, 10);
            if (wait < 1)
                wait = 10;
            if (wait > 30)
                wait = 30;
            printf("  Rate limited, waiting %ld seconds...\n", wait);
            _curl.rewindDownloadFile();
            svcSleepThread((s64)wait * 1000000000LL);
            continue;
        }

        if (status >= 500 && status < 600 && attempt < 2)
        {
            printf("  Server error, waiting 5 seconds...\n");
            _curl.rewindDownloadFile();
            svcSleepThread(5000000000LL);
            continue;
        }

        // Fatal: the token is invalid, expired or revoked.
        if (status == 401)
        {
            printf(CONSOLE_RED "  FATAL: Authentication failed." CONSOLE_RESET "\n");
            printf("  The Dropbox token is invalid or has expired.\n");
            _fatalError = true;
            return -401;
        }

        // Fatal: the app lacks the permission for this call.
        if (status == 403)
        {
            printf(CONSOLE_RED "  FATAL: Access denied (HTTP 403)." CONSOLE_RESET "\n");
            printf("  The Dropbox app is missing the required permissions.\n");
            _fatalError = true;
            return -403;
        }

        // 400 (malformed request) and 409 (path conflict, insufficient space,
        // …) carry the reason in the body and affect only this call.
        std::string body = _curl.getResponse();
        if (!body.empty())
            printf(CONSOLE_RED "  %s" CONSOLE_RESET "\n", body.substr(0, 200).c_str());

        return (int)status;
    }
    return -1; // exhausted retries
}

// ---------------------------------------------------------------------------
// Sync operations
// ---------------------------------------------------------------------------

std::string Dropbox::ensureRoot(const std::string &remoteName)
{
    std::string root = _pathFor(remoteName);
    if (root.empty())
        return root; // the Dropbox root always exists

    // create_folder_v2 reports 409 path/conflict when it is already there,
    // which is the outcome we wanted anyway.
    std::string body = "{\"path\":\"" + _jsonEscape(root) + "\",\"autorename\":false}";
    int res = _rpc("https://api.dropboxapi.com/2/files/create_folder_v2", body);
    if (res != 0 && res != 409)
    {
        if (_fatalError)
            return "";
        printf("Dropbox: cannot create %s\n", root.c_str());
        return "";
    }
    return root;
}

bool Dropbox::list(const std::string &root,
                   std::map<std::string, RemoteFileInfo> &out)
{
    std::string body = "{\"path\":\"" + _jsonEscape(root) +
                       "\",\"recursive\":true,\"include_deleted\":false}";
    std::string endpoint = "https://api.dropboxapi.com/2/files/list_folder";

    while (true)
    {
        int res = _rpc(endpoint, body);
        if (res != 0)
        {
            if (_fatalError)
                return false;
            // 409 path/not_found: nothing has been uploaded here yet.
            if (res == 409)
                return true;
            return false;
        }

        std::string response = _curl.getResponse();
        std::vector<std::string> entries;
        _splitEntries(response, entries);

        for (auto &entry : entries)
        {
            if (_jsonString(entry, ".tag") != "file")
                continue;

            std::string path = _jsonString(entry, "path_display");
            if (path.empty())
                path = _jsonString(entry, "path_lower");
            if (path.size() <= root.size())
                continue;

            // Dropbox paths are case-insensitive, so compare the root that way
            // and keep the server's spelling for the remainder.
            if (strncasecmp(path.c_str(), root.c_str(), root.size()) != 0)
                continue;
            std::string relPath = path.substr(root.size());
            if (relPath.empty() || relPath[0] != '/')
                continue;

            // Skip our own interrupted transfers.
            if (relPath.size() > 7 &&
                relPath.compare(relPath.size() - 7, 7, ".3dstmp") == 0)
                continue;

            RemoteFileInfo info;
            // "rev:<rev>" pins the download to the revision listed here.
            std::string rev = _jsonString(entry, "rev");
            info.id = rev.empty() ? path : ("rev:" + rev);
            info.relPath = relPath;
            info.tag = _jsonString(entry, "content_hash");
            out[relPath] = info;
        }

        if (!_jsonTrue(response, "has_more"))
            break;

        std::string cursor = _jsonString(response, "cursor");
        if (cursor.empty())
        {
            printf("Dropbox: has_more set but no cursor returned\n");
            break;
        }
        body = "{\"cursor\":\"" + _jsonEscape(cursor) + "\"}";
        endpoint = "https://api.dropboxapi.com/2/files/list_folder/continue";
    }
    return true;
}

bool Dropbox::download(const RemoteFileInfo &file, const std::string &localPath)
{
    std::string tmpPath;
    FILE *fp = openTempFor(localPath, tmpPath);
    if (!fp)
        return false;

    std::string args("Dropbox-API-Arg: {\"path\":\"" + _headerJsonEscape(file.id) + "\"}");
    std::string auth("Authorization: Bearer " + _token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, args.c_str());
    // The argument travels in the header, so the body must be empty.
    headers = curl_slist_append(headers, "Content-Type:");

    _curl.reset();
    _curl.setURL(std::string("https://content.dropboxapi.com/2/files/download"));
    _curl.setHeaders(headers);
    _curl.setPostData("");
    _curl.setDownloadFile(fp);

    int res = _performWithRetry();
    _curl.clearDownloadFile();
    curl_slist_free_all(headers);
    fclose(fp);

    if (res != 0)
    {
        // On an error the body is the JSON error, which went into the temp file.
        printf("Dropbox: download failed for %s\n", file.relPath.c_str());
        remove(tmpPath.c_str());
        return false;
    }
    return replaceLocalFile(tmpPath, localPath);
}

bool Dropbox::upload(const std::string &root, const std::string &relPath,
                     const std::string &localPath, const RemoteFileInfo *existing,
                     std::string &outTag, std::string &outId)
{
    (void)existing;

    std::string remotePath = root + relPath;

    FILE *fp = fopen(localPath.c_str(), "rb");
    if (!fp)
    {
        printf("Dropbox: cannot read %s\n", localPath.c_str());
        return false;
    }

    curl_off_t size = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
    {
        size = (curl_off_t)ftell(fp);
        rewind(fp);
    }
    if (size > MAX_UPLOAD_BYTES)
    {
        printf("Dropbox: %s is larger than 150 MB — skipping\n", relPath.c_str());
        fclose(fp);
        return false;
    }

    // "overwrite" replaces the remote file in place; the API default "add"
    // silently renames the upload to "name (1).ext" instead.
    std::string args("Dropbox-API-Arg: {\"path\":\"" + _headerJsonEscape(remotePath) +
                     "\",\"mode\":\"overwrite\",\"mute\":true}");
    std::string auth("Authorization: Bearer " + _token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, args.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    headers = curl_slist_append(headers, "Expect:");

    _curl.reset();
    _curl.setURL(std::string("https://content.dropboxapi.com/2/files/upload"));
    _curl.setHeaders(headers);
    // Dropbox's content endpoints reject a chunked body, so the size is
    // announced up front.
    _curl.setReadData((void *)fp, size);
    int res = _performWithRetry(fp);

    fclose(fp);
    curl_slist_free_all(headers);

    if (res != 0)
    {
        printf("Dropbox: upload failed for %s\n", relPath.c_str());
        return false;
    }

    // The upload response is the new file's metadata, so the tag needs no
    // extra round trip.
    std::string response = _curl.getResponse();
    outTag = _jsonString(response, "content_hash");
    std::string rev = _jsonString(response, "rev");
    outId = rev.empty() ? remotePath : ("rev:" + rev);

    if (outTag.empty())
    {
        // Fall back to hashing what we just sent rather than leaving the
        // manifest without a tag, which would re-upload on every run.
        outTag = computeDropboxHash(localPath);
    }
    return true;
}

std::string Dropbox::localTag(const std::string &localPath)
{
    return computeDropboxHash(localPath);
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
// Hand-rolled, like the rest of the project: no JSON library is available.
// ---------------------------------------------------------------------------

std::string Dropbox::_jsonString(const std::string &json, const std::string &key)
{
    // Handles both compact ("key":"value") and spaced ("key": "value") forms.
    for (const char *sep : {"\":\"", "\": \""})
    {
        std::string search = "\"" + key + sep;
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            continue;
        pos += search.size();

        std::string out;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                char esc = json[pos + 1];
                if (esc == 'n') out += '\n';
                else if (esc == 'r') out += '\r';
                else if (esc == 't') out += '\t';
                else if (esc == 'u' && pos + 5 < json.size())
                {
                    // Dropbox only escapes non-ASCII this way in paths; keep
                    // the BMP characters we can represent in one UTF-8 run.
                    char hex[5] = {json[pos + 2], json[pos + 3], json[pos + 4], json[pos + 5], 0};
                    unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                    if (cp < 0x80)
                    {
                        out += (char)cp;
                    }
                    else if (cp < 0x800)
                    {
                        out += (char)(0xc0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3f));
                    }
                    else
                    {
                        out += (char)(0xe0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3f));
                        out += (char)(0x80 | (cp & 0x3f));
                    }
                    pos += 6;
                    continue;
                }
                else out += esc;
                pos += 2;
                continue;
            }
            out += json[pos];
            pos++;
        }
        return out;
    }
    return "";
}

bool Dropbox::_jsonTrue(const std::string &json, const std::string &key)
{
    for (const char *sep : {"\":", "\": "})
    {
        std::string search = "\"" + key + sep;
        size_t pos = json.find(search);
        if (pos != std::string::npos)
            return json.compare(pos + search.size(), 4, "true") == 0;
    }
    return false;
}

void Dropbox::_splitEntries(const std::string &json, std::vector<std::string> &out)
{
    size_t arrayStart = json.find("\"entries\"");
    if (arrayStart == std::string::npos)
        return;
    arrayStart = json.find('[', arrayStart);
    if (arrayStart == std::string::npos)
        return;

    int depth = 0;
    bool inString = false;
    size_t objStart = 0;

    for (size_t i = arrayStart + 1; i < json.size(); i++)
    {
        char c = json[i];

        if (inString)
        {
            if (c == '\\')
                i++; // skip the escaped character, braces included
            else if (c == '"')
                inString = false;
            continue;
        }

        if (c == '"')
            inString = true;
        else if (c == '{')
        {
            if (depth == 0)
                objStart = i;
            depth++;
        }
        else if (c == '}')
        {
            depth--;
            if (depth == 0)
                out.push_back(json.substr(objStart, i - objStart + 1));
        }
        else if (c == ']' && depth == 0)
        {
            break; // end of the entries array
        }
    }
}

std::string Dropbox::_jsonEscape(const std::string &value)
{
    std::string out;
    char buf[8];
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += (char)c;
        }
        else if (c < 0x20)
        {
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        }
        else
        {
            out += (char)c;
        }
    }
    return out;
}

std::string Dropbox::_headerJsonEscape(const std::string &value)
{
    std::string out;
    char buf[16];
    size_t i = 0;

    while (i < value.size())
    {
        unsigned char c = (unsigned char)value[i];

        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += (char)c;
            i++;
        }
        else if (c < 0x20 || c == 0x7f)
        {
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
            i++;
        }
        else if (c < 0x80)
        {
            out += (char)c;
            i++;
        }
        else
        {
            // Decode one UTF-8 sequence into a code point.
            unsigned int cp = 0;
            int extra = 0;
            if ((c & 0xe0) == 0xc0)
            {
                cp = c & 0x1f;
                extra = 1;
            }
            else if ((c & 0xf0) == 0xe0)
            {
                cp = c & 0x0f;
                extra = 2;
            }
            else if ((c & 0xf8) == 0xf0)
            {
                cp = c & 0x07;
                extra = 3;
            }
            else
            {
                i++; // invalid lead byte — drop it
                continue;
            }

            if (i + (size_t)extra >= value.size())
                break; // truncated sequence at end of string

            bool valid = true;
            for (int k = 1; k <= extra; k++)
            {
                unsigned char cont = (unsigned char)value[i + k];
                if ((cont & 0xc0) != 0x80)
                {
                    valid = false;
                    break;
                }
                cp = (cp << 6) | (cont & 0x3f);
            }
            if (!valid)
            {
                i++;
                continue;
            }
            i += extra + 1;

            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                snprintf(buf, sizeof(buf), "\\u%04x\\u%04x",
                         0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
            }
            else
                snprintf(buf, sizeof(buf), "\\u%04x", cp);
            out += buf;
        }
    }

    return out;
}
