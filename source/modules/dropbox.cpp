#include "dropbox.h"
#include <3ds.h>
#include <cstdlib>

Dropbox::Dropbox(std::string token) : _token(token), _fatalError(false)
{
}

bool Dropbox::hasFatalError() const { return _fatalError; }

bool Dropbox::validateToken()
{
    std::string auth("Authorization: Bearer " + _token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    _curl.setURL(std::string("https://api.dropboxapi.com/2/check/user"));
    _curl.setHeaders(headers);
    _curl.setPostData("{\"query\":\"3DSync\"}");
    int res = _performWithRetry();
    curl_slist_free_all(headers);

    if (res != 0)
    {
        printf(CONSOLE_RED "Dropbox token rejected." CONSOLE_RESET "\n");
        printf("Re-run the configurator to obtain a new token.\n");
        return false;
    }
    return true;
}

bool Dropbox::upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths)
{
    for (auto &item : paths)
    {
        for (auto &path : item.second)
        {
            hidScanInput();
            if (hidKeysDown() & KEY_START)
            {
                printf("Upload cancelled by user\n");
                return false;
            }

            std::string localPath = item.first.first + path;
            std::string remotePath = "/" + item.first.second + path;

            FILE *file = fopen(localPath.c_str(), "rb");
            if (file == NULL)
            {
                printf("Cannot open %s — skipping\n", localPath.c_str());
                continue;
            }

            // Dropbox's content endpoints reject a chunked body, so the size
            // has to be known up front.
            curl_off_t size = -1;
            if (fseek(file, 0, SEEK_END) == 0)
            {
                size = (curl_off_t)ftell(file);
                rewind(file);
            }

            // "overwrite" replaces the remote file in place; the default "add"
            // silently renames the upload to "name (1).ext" instead.
            std::string args("Dropbox-API-Arg: {\"path\":\"" + _headerJsonEscape(remotePath) +
                             "\",\"mode\":\"overwrite\",\"mute\":false}");
            std::string auth("Authorization: Bearer " + _token);
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, auth.c_str());
            headers = curl_slist_append(headers, args.c_str());
            headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
            headers = curl_slist_append(headers, "Expect:");

            _curl.setURL(std::string("https://content.dropboxapi.com/2/files/upload"));
            _curl.setHeaders(headers);
            _curl.setReadData((void *)file, size);
            int res = _performWithRetry(file);

            fclose(file);
            curl_slist_free_all(headers);

            if (_fatalError)
                return false;
            if (res != 0)
                printf("Upload failed for %s\n", localPath.c_str());
        }
    }

    return true;
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

        // Network-level failure (timeout, DNS, TLS, …)
        if (curlRes != 0)
        {
            printf("  Network error (attempt %d/3)\n", attempt + 1);
            if (attempt < 2)
            {
                svcSleepThread(2000000000LL); // 2 s back-off
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
            svcSleepThread((s64)wait * 1000000000LL);
            continue;
        }

        if (status >= 500 && status < 600 && attempt < 2)
        {
            printf("  Server error, waiting 5 seconds...\n");
            svcSleepThread(5000000000LL);
            continue;
        }

        // Fatal: the token is invalid, expired or revoked.
        if (status == 401)
        {
            printf(CONSOLE_RED "  FATAL: Authentication failed." CONSOLE_RESET "\n");
            printf("  The Dropbox token is invalid or has expired.\n");
            printf("  Re-run the configurator to obtain a new token.\n");
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
        // …) carry the reason in the body and affect only this file.
        std::string body = _curl.getResponse();
        if (!body.empty())
            printf(CONSOLE_RED "  %s" CONSOLE_RESET "\n", body.substr(0, 200).c_str());

        return (int)status;
    }
    return -1; // exhausted retries
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
