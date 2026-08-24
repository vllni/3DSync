#include "dropbox.h"
#include <3ds.h>

Dropbox::Dropbox(std::string token) : _token(token)
{
}

bool Dropbox::upload(std::map<std::pair<std::string, std::string>, std::vector<std::string>> paths)
{
    for (auto item : paths)
    {
        for (auto path : item.second)
        {
            hidScanInput();
            if (hidKeysDown() & KEY_START)
            {
                printf("Upload cancelled by user\n");
                return false;
            }

            std::string localPath = item.first.first + path;

            printf("Uploading %s\n", localPath.c_str());

            FILE *file = fopen(localPath.c_str(), "rb");
            if (file == NULL)
            {
                printf("Failed to open file for upload\n");
                return false;
            }

            std::string args(
                "Dropbox-API-Arg: {\"path\":\"/" +
                item.first.second + path +
                "\",\"mode\":\"overwrite\",\"mute\":false}"
            );

            std::string auth("Authorization: Bearer " + _token);

            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, auth.c_str());
            headers = curl_slist_append(headers, args.c_str());
            headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
            headers = curl_slist_append(headers, "Expect:");

            _curl.setURL("https://content.dropboxapi.com/2/files/upload");
            _curl.setHeaders(headers);
            _curl.setReadData((void *)file);

            int result = _curl.perform();
            long status = _curl.getStatusCode();

            fclose(file);
            curl_slist_free_all(headers);

            if (result != CURLE_OK)
            {
                printf("Upload failed: %s\n",
                       curl_easy_strerror((CURLcode)result));
                return false;
            }

            if (status == 200)
            {
                printf("Successfully uploaded %s\n\n", localPath.c_str());
                continue;
            }

            if (status == 401)
            {
                printf("Upload failed: Dropbox rejected the access token.\n");
                printf("Check that the Dropbox token is valid and has not expired.\n");
                return false;
            }

            if (status == 403)
            {
                printf("Upload failed: Dropbox denied access.\n");
                printf("Ask the app author to check the Dropbox OAuth permissions are correct.\n");
                return false;
            }

            if (status == 409)
            {
                printf("Upload failed: Dropbox rejected the requested path or upload.\n");
                printf("Response: %s\n", _curl.getResponse().c_str());
                return false;
            }

            if (status == 429)
            {
                printf("Upload failed: Dropbox rate limit reached.\n");
                printf("Please wait before trying again.\n");
                return false;
            }

            if (status >= 500 && status <= 599)
            {
                printf("Upload failed: Dropbox server error (HTTP %ld).\n", status);
                printf("Please try again later.\n");
                return false;
            }

            printf("Upload failed: Dropbox returned HTTP %ld.\n", status);
            printf("Response: %s\n", _curl.getResponse().c_str());
            return false;
        }
    }

    return true;
}

bool Dropbox::validateToken()
{
    curl_slist *headers = NULL;

    std::string auth("Authorization: Bearer " + _token);
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    _curl.setURL("https://api.dropboxapi.com/2/check/user");
    _curl.setHeaders(headers);
    _curl.setPostData("{\"query\":\"3DSync token validation\"}");

    int result = _curl.perform();
    long status = _curl.getStatusCode();


    // printf("Dropbox token validation: CURL=%d HTTP=%ld\n", result, status);

    if (result != CURLE_OK || status != 200)
    {
        printf("Dropbox token validation failed.\n");
        printf("Response: %s\n", _curl.getResponse().c_str());

        curl_slist_free_all(headers);
        return false;
    }

    printf("Dropbox token is valid.\n");
    // printf("Response: %s\n", _curl.getResponse().c_str());

    curl_slist_free_all(headers);
    return true;
}