#include "remoteparse.h"

#include <stdio.h>
#include <string.h>

#include "../utils/json.h"
#include "../utils/pathutil.h"
#include "../utils/urlutil.h"
#include "../utils/xmlutil.h"

std::string sizeTimeTag(long long size, long long mtime)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld:%lld", size, mtime);
    return buf;
}

void parseDavResponses(const std::string &xml, const std::string &rootHref,
                       std::vector<DavEntry> &out)
{
    std::string clean = stripXmlNamespaces(xml);
    size_t pos = 0;

    while (true)
    {
        size_t start = clean.find("<response", pos);
        if (start == std::string::npos)
            break;
        size_t end = clean.find("</response", start);
        if (end == std::string::npos)
            break;
        pos = end + 1;

        std::string href = urlDecode(xmlTagText(clean, "href", start, end));
        if (href.empty())
            continue;

        // Strip the scheme and host if the server returned an absolute URL.
        size_t schemeEnd = href.find("://");
        if (schemeEnd != std::string::npos)
        {
            size_t slash = href.find('/', schemeEnd + 3);
            href = (slash == std::string::npos) ? "/" : href.substr(slash);
        }

        if (href.compare(0, rootHref.size(), rootHref) != 0)
            continue; // outside the listing root

        std::string relPath = href.substr(rootHref.size());
        while (!relPath.empty() && relPath[relPath.size() - 1] == '/')
            relPath.erase(relPath.size() - 1);
        if (relPath.empty())
            continue; // the root itself

        DavEntry entry;
        entry.relPath = "/" + relPath;

        size_t collection = clean.find("<collection", start);
        entry.isCollection = (collection != std::string::npos && collection < end);

        entry.tag = normalizeEtag(xmlTagText(clean, "getetag", start, end));
        if (entry.tag.empty())
            entry.tag = xmlTagText(clean, "getcontentlength", start, end) + ":" +
                        xmlTagText(clean, "getlastmodified", start, end);
        out.push_back(entry);
    }
}

void parseDropboxEntries(const std::string &response, const std::string &root,
                         std::map<std::string, RemoteFileInfo> &out)
{
    std::vector<std::string> entries;
    jsonSplitArray(response, "entries", entries);

    for (auto &entry : entries)
    {
        if (jsonString(entry, ".tag") != "file")
            continue;

        std::string path = jsonString(entry, "path_display");
        if (path.empty())
            path = jsonString(entry, "path_lower");
        if (path.size() <= root.size())
            continue;

        // Dropbox paths are case-insensitive, so compare the root that way and
        // keep the server's spelling for the remainder.
        if (strncasecmp(path.c_str(), root.c_str(), root.size()) != 0)
            continue;

        std::string relPath = path.substr(root.size());
        if (relPath.empty() || relPath[0] != '/')
            continue;

        // Skip our own interrupted transfers.
        if (isTempTransferName(relPath))
            continue;

        RemoteFileInfo info;
        // "rev:<rev>" pins the download to the revision listed here.
        std::string rev = jsonString(entry, "rev");
        info.id = rev.empty() ? path : ("rev:" + rev);
        info.relPath = relPath;
        info.tag = jsonString(entry, "content_hash");
        out[relPath] = info;
    }
}

std::string buildFtpUrl(bool implicitTls, const std::string &host, int port,
                        const std::string &path)
{
    char portBuf[32];
    snprintf(portBuf, sizeof(portBuf), "%d", port);
    std::string url = (implicitTls ? "ftps://" : "ftp://") + host + ":" + portBuf + "/";
    return url + urlEncodePath(path);
}
