#ifndef MODULES_GOOGLEDRIVE_H
#define MODULES_GOOGLEDRIVE_H

#include <string>
#include <vector>
#include <map>

#include "../utils/curl.h"

class GoogleDrive
{
public:
    // Refresh-token based construction (preferred): exchanges refresh token for access token on first upload.
    // clientId, clientSecret, and refreshToken come from [GoogleDrive] section of the INI.
    // For backwards compatibility, a pre-existing access token can be supplied via directToken.
    GoogleDrive(const std::string &clientId, const std::string &clientSecret,
                const std::string &refreshToken, const std::string &folderId = std::string(),
                const std::string &directToken = std::string());
    ~GoogleDrive() {};
    void upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths);

private:
    std::string _token;
    std::string _clientId;
    std::string _clientSecret;
    std::string _refreshToken;
    std::string _folderId;
    int _uploadCount;
    Curl _curl;
    bool _refreshAccessToken();
    std::string _extractJsonString(const std::string &json, const std::string &key);
    std::string _urlEncode(const std::string &value);
    std::string _jsonEscape(std::string value);
    std::string _driveFileName(std::string path);
    std::string _readFile(FILE *file);
};

#endif
