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

bool isSafeRelativePath(const std::string &relPath)
{
    if (relPath.size() < 2 || relPath[0] != '/')
        return false;
    size_t start = 1;
    while (start <= relPath.size())
    {
        size_t end = relPath.find('/', start);
        if (end == std::string::npos)
            end = relPath.size();
        std::string segment = relPath.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        start = end + 1;
    }
    for (char c : relPath)
        if (c == '\\' || (unsigned char)c < 0x20 || c == 0x7f)
            return false;
    return true;
}
