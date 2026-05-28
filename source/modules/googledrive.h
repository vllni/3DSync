#ifndef MODULES_GOOGLEDRIVE_H
#define MODULES_GOOGLEDRIVE_H

#include <string>
#include <vector>
#include <map>

#include "../utils/curl.h"

class GoogleDrive{
    public:
        GoogleDrive(const std::string &token, const std::string &folderId=std::string());
        ~GoogleDrive(){};
        void upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths);
    private:
        std::string _token;
        std::string _folderId;
        Curl _curl;
        std::string _jsonEscape(std::string value);
        std::string _driveFileName(std::string path);
        std::string _readFile(FILE *file);
};


#endif
