#include "googledrive.h"
#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

GoogleDrive::GoogleDrive(const std::string &clientId, const std::string &clientSecret,
                         const std::string &refreshToken, const std::string &folderId,
                         const std::string &directToken)
    : _token(directToken), _clientId(clientId), _clientSecret(clientSecret),
      _refreshToken(refreshToken), _folderId(folderId), _uploadCount(0)
{
}

void GoogleDrive::upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths)
{
    if (_token.empty() && !_refreshToken.empty())
    {
        if (!_refreshAccessToken())
        {
            printf("Cannot upload: failed to obtain Google Drive access token\n");
            return;
        }
    }

    for (auto item : paths)
    {
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
            std::string fileName = _driveFileName(item.first.second + path);
            std::string metadata = "{\"name\":\"" + _jsonEscape(fileName) + "\"";
            if (_folderId != "")
            {
                metadata += ",\"parents\":[\"" + _jsonEscape(_folderId) + "\"]";
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
            if (_curl.perform() != 0)
            {
                printf("Google Drive upload failed for %s\n", localPath.c_str());
            }
            curl_slist_free_all(headers);
            fclose(file);
            printf("\n");
        }
    }
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
    int result = _curl.perform();
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
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos)
        return "";
    pos += search.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos)
        return "";
    return json.substr(pos, end - pos);
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
