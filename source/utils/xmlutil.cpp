#include "xmlutil.h"

std::string stripXmlNamespaces(const std::string &xml)
{
    std::string out;
    out.reserve(xml.size());

    for (size_t i = 0; i < xml.size(); i++)
    {
        out += xml[i];
        if (xml[i] != '<')
            continue;

        size_t nameStart = i + 1;
        if (nameStart < xml.size() && xml[nameStart] == '/')
        {
            out += '/';
            nameStart++;
        }

        // A ':' before the end of the tag name means the token is prefixed.
        size_t scan = nameStart;
        size_t colon = std::string::npos;
        while (scan < xml.size() && xml[scan] != '>' && xml[scan] != ' ' &&
               xml[scan] != '\t' && xml[scan] != '\r' && xml[scan] != '\n' &&
               xml[scan] != '/')
        {
            if (xml[scan] == ':')
            {
                colon = scan;
                break;
            }
            scan++;
        }

        if (colon != std::string::npos)
            i = colon; // skip the prefix and the ':' itself
        else
            i = nameStart - 1;
    }
    return out;
}

std::string xmlTagText(const std::string &xml, const std::string &tag,
                                      size_t from, size_t to)
{
    std::string open = "<" + tag;
    size_t start = xml.find(open, from);
    if (start == std::string::npos || start >= to)
        return "";

    size_t gt = xml.find('>', start);
    if (gt == std::string::npos || gt >= to)
        return "";
    if (gt > start && xml[gt - 1] == '/')
        return ""; // self-closing, no text content

    size_t close = xml.find("</" + tag, gt);
    if (close == std::string::npos || close > to)
        return "";
    return xml.substr(gt + 1, close - gt - 1);
}

std::string normalizeEtag(std::string etag)
{
    if (etag.compare(0, 2, "W/") == 0)
        etag.erase(0, 2);
    if (etag.size() >= 2 && etag[0] == '"' && etag[etag.size() - 1] == '"')
        etag = etag.substr(1, etag.size() - 2);
    return etag;
}
