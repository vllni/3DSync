#include "urlutil.h"

#include <stdlib.h>

std::string urlEncodePath(const std::string &path)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/')
        {
            out += (char)c;
        }
        else
        {
            out += '%';
            out += hex[(c >> 4) & 0xf];
            out += hex[c & 0xf];
        }
    }
    return out;
}

std::string urlDecode(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); i++)
    {
        if (value[i] == '%' && i + 2 < value.size())
        {
            char hex[3] = {value[i + 1], value[i + 2], 0};
            char *end = NULL;
            long byte = strtol(hex, &end, 16);
            if (end && *end == 0)
            {
                out += (char)byte;
                i += 2;
                continue;
            }
        }
        out += value[i];
    }
    return out;
}
