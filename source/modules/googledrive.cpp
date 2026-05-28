#include "googledrive.h"
#include <3ds.h>
#include <stdio.h>

GoogleDrive::GoogleDrive(const std::string &token, const std::string &folderId) : _token(token), _folderId(folderId){
}

void GoogleDrive::upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths){
    for(auto item : paths){
        for(auto path : item.second){
            printf("Uploading %s to Google Drive\n", (item.first.first + path).c_str());
            FILE *file = fopen((item.first.first + path).c_str(), "rb");
            if(file == NULL){
                printf("Failed to open file\n");
                continue;
            }

            static int uploadCount = 0;
            std::string fileContents = _readFile(file);
            std::string boundary = "3DSyncGoogleDriveBoundary" + std::to_string(uploadCount++);
            while(fileContents.find(boundary) != std::string::npos){
                boundary += "x";
            }
            std::string fileName = _driveFileName(item.first.second + path);
            std::string metadata = "{\"name\":\"" + _jsonEscape(fileName) + "\"";
            if(_folderId != ""){
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
            _curl.perform();
            curl_slist_free_all(headers);
            fclose(file);
            printf("\n");
        }
    }
}

std::string GoogleDrive::_jsonEscape(std::string value){
    std::string escaped;
    for(auto character : value){
        switch(character){
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
                if((unsigned char)character < 0x20){
                    char buffer[7];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned char)character);
                    escaped += buffer;
                } else {
                    escaped += character;
                }
                break;
        }
    }
    return escaped;
}

std::string GoogleDrive::_driveFileName(std::string path){
    while(path.size() > 0 && path[0] == '/'){
        path.erase(0, 1);
    }
    for(auto &character : path){
        if(character == '/'){
            character = '_';
        }
    }
    return path;
}

std::string GoogleDrive::_readFile(FILE *file){
    std::string contents;
    if(fseek(file, 0, SEEK_END) == 0){
        long size = ftell(file);
        if(size > 0){
            contents.reserve(size);
        }
        if(fseek(file, 0, SEEK_SET) != 0){
            return contents;
        }
    }

    char buffer[4096];
    size_t read;
    while((read = fread(buffer, 1, sizeof(buffer), file)) > 0){
        contents.append(buffer, read);
    }
    return contents;
}
