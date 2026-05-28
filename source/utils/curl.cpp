#include "curl.h"

Curl::Curl() : _downloadFile(nullptr)
{
    curl_global_init(CURL_GLOBAL_ALL);
    _curl = curl_easy_init();
    if (!_curl)
        printf("Failed to init libcurl.\n");
    curl_easy_setopt(_curl, CURLOPT_USERAGENT, "3DSync/" VERSION_STRING);
    curl_easy_setopt(_curl, CURLOPT_CONNECTTIMEOUT, 50L);
    curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(_curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(_curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(_curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(_curl, CURLOPT_PIPEWAIT, 1L);
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, _write_callback);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(_curl, CURLOPT_HEADERFUNCTION, _header_callback);
    curl_easy_setopt(_curl, CURLOPT_HEADERDATA, this);
#ifdef DEBUG
    curl_easy_setopt(_curl, CURLOPT_VERBOSE, 1L);
#endif
}

Curl::~Curl()
{
    curl_easy_cleanup(_curl);
}

void Curl::setURL(std::string URL)
{
    curl_easy_setopt(_curl, CURLOPT_URL, URL.c_str());
}

void Curl::setHeaders(curl_slist *headers)
{
    curl_easy_setopt(_curl, CURLOPT_HTTPHEADER, headers);
}

void Curl::setReadData(void *pointer)
{
    curl_easy_setopt(_curl, CURLOPT_READFUNCTION, _read_callback);
    curl_easy_setopt(_curl, CURLOPT_READDATA, pointer);
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(_curl, CURLOPT_POST, 1L);
}

void Curl::setPostData(const std::string &data)
{
    _postData = data;
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDS, _postData.data());
    curl_easy_setopt(_curl, CURLOPT_POSTFIELDSIZE, _postData.size());
    curl_easy_setopt(_curl, CURLOPT_POST, 1L);
}

void Curl::resetToGet()
{
    _postData.clear();
    curl_easy_setopt(_curl, CURLOPT_HTTPGET, 1L);
}

int Curl::perform()
{
    _responseData.clear();
    _rawHeaders.clear();
    CURLcode rescode = curl_easy_perform(_curl);
    const char *res = curl_easy_strerror(rescode);
    printf("Curl result: %s\n", res);
    return rescode;
}

std::string Curl::getResponse() const
{
    return _responseData;
}

long Curl::getStatusCode() const
{
    long code = 0;
    curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

std::string Curl::getResponseHeader(const std::string &name) const
{
    // Build a lowercase copy for case-insensitive matching
    std::string lowerHeaders = _rawHeaders;
    std::string lowerName = name;
    for (char &c : lowerHeaders) c = (char)tolower((unsigned char)c);
    for (char &c : lowerName)    c = (char)tolower((unsigned char)c);
    std::string search = lowerName + ":";
    size_t pos = lowerHeaders.find(search);
    if (pos == std::string::npos)
        return "";
    pos += search.size();
    while (pos < _rawHeaders.size() && _rawHeaders[pos] == ' ') pos++;
    size_t end = _rawHeaders.find("\r\n", pos);
    if (end == std::string::npos) end = _rawHeaders.find('\n', pos);
    if (end == std::string::npos) end = _rawHeaders.size();
    return _rawHeaders.substr(pos, end - pos);
}

void Curl::setDownloadFile(FILE *fp)
{
    _downloadFile = fp;
}

void Curl::clearDownloadFile()
{
    _downloadFile = nullptr;
}

void Curl::setPatch()
{
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, "PATCH");
}

void Curl::clearCustomRequest()
{
    curl_easy_setopt(_curl, CURLOPT_CUSTOMREQUEST, NULL);
}

size_t Curl::_read_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *readhere = (FILE *)userdata;
    curl_off_t nread;
    size_t retcode = fread(ptr, size, nmemb, readhere);
    nread = (curl_off_t)retcode;
    if (nread > 0)
    {
        printf("Sent %" CURL_FORMAT_CURL_OFF_T " bytes from file\n", nread);
    }
    return retcode;
}

size_t Curl::_write_callback(void *data, size_t size, size_t nmemb, void *userdata)
{
    Curl *self = static_cast<Curl *>(userdata);
    size_t totalSize = size * nmemb;
    if (self->_downloadFile != nullptr)
    {
        return fwrite(data, size, nmemb, self->_downloadFile);
    }
    self->_responseData.append(static_cast<char *>(data), totalSize);
#ifdef DEBUG
    fwrite(data, size, nmemb, stdout);
#endif
    return totalSize;
}

size_t Curl::_header_callback(void *data, size_t size, size_t nmemb, void *userdata)
{
    Curl *self = static_cast<Curl *>(userdata);
    size_t totalSize = size * nmemb;
    self->_rawHeaders.append(static_cast<char *>(data), totalSize);
    return totalSize;
}
