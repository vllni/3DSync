#include "pathutil.h"

const char *TEMP_SUFFIX = ".3dstmp";

std::string normalizeRemotePath(const std::string &path, bool leadingSlash)
{
    std::string out;
    for (size_t i = 0; i < path.size(); i++)
    {
        char c = (path[i] == '\\') ? '/' : path[i];
        // Drop a leading separator and collapse runs of them.
        if (c == '/' && (out.empty() || out[out.size() - 1] == '/'))
            continue;
        out += c;
    }
    while (!out.empty() && out[out.size() - 1] == '/')
        out.erase(out.size() - 1);

    if (leadingSlash && !out.empty())
        out = "/" + out;
    return out;
}

bool isTempTransferName(const std::string &path)
{
    size_t suffixLen = 0;
    while (TEMP_SUFFIX[suffixLen] != '\0')
        suffixLen++;
    return path.size() > suffixLen &&
           path.compare(path.size() - suffixLen, suffixLen, TEMP_SUFFIX) == 0;
}
