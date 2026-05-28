#ifndef UTILS_CURL_H
#define UTILS_CURL_H

#include <cstdio>
#include <string>

#include <curl/curl.h>

class Curl
{
public:
    Curl();
    ~Curl();
    void setURL(std::string URL);
    void setHeaders(curl_slist *headers);
    void setReadData(void *pointer);
    void setPostData(const std::string &data);
    void resetToGet();
    // Direct response body to a FILE* instead of the internal string buffer.
    // Call clearDownloadFile() after perform() to restore buffering mode.
    void setDownloadFile(FILE *fp);
    void clearDownloadFile();
    // Override the HTTP method to PATCH (body still set via setPostData).
    // Call clearCustomRequest() after perform() to reset.
    void setPatch();
    void clearCustomRequest();
    int perform();
    std::string getResponse() const;
    long getStatusCode() const;
    // Case-insensitive search in the last response's headers (e.g. "Date").
    std::string getResponseHeader(const std::string &name) const;

private:
    CURL *_curl;
    std::string _postData;
    std::string _responseData;
    std::string _rawHeaders;
    FILE *_downloadFile;
    static size_t _read_callback(void *ptr, size_t size, size_t nmemb, void *userdata);
    static size_t _write_callback(void *data, size_t size, size_t nmemb, void *userdata);
    static size_t _header_callback(void *data, size_t size, size_t nmemb, void *userdata);
};

#endif
