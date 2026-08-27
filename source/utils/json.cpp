#include "json.h"

#include <stdio.h>
#include <stdlib.h>

// Append one code point to out as UTF-8.
static void appendUtf8(std::string &out, unsigned int cp)
{
    if (cp < 0x80)
    {
        out += (char)cp;
    }
    else if (cp < 0x800)
    {
        out += (char)(0xc0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3f));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xe0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3f));
        out += (char)(0x80 | (cp & 0x3f));
    }
    else
    {
        out += (char)(0xf0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3f));
        out += (char)(0x80 | ((cp >> 6) & 0x3f));
        out += (char)(0x80 | (cp & 0x3f));
    }
}

// Read a \u escape at json[pos] (pointing at the 'u'), advancing pos past it.
// Combines a surrogate pair when one follows.
static bool readUnicodeEscape(const std::string &json, size_t &pos, unsigned int &cp)
{
    if (pos + 4 >= json.size())
        return false;

    char hex[5] = {json[pos + 1], json[pos + 2], json[pos + 3], json[pos + 4], 0};
    char *end = NULL;
    unsigned int value = (unsigned int)strtoul(hex, &end, 16);
    if (end == NULL || *end != '\0')
        return false;
    pos += 5;

    // High surrogate: the low half follows as a second escape.
    if (value >= 0xd800 && value <= 0xdbff && pos + 5 < json.size() &&
        json[pos] == '\\' && json[pos + 1] == 'u')
    {
        char lowHex[5] = {json[pos + 2], json[pos + 3], json[pos + 4], json[pos + 5], 0};
        unsigned int low = (unsigned int)strtoul(lowHex, &end, 16);
        if (end != NULL && *end == '\0' && low >= 0xdc00 && low <= 0xdfff)
        {
            value = 0x10000 + ((value - 0xd800) << 10) + (low - 0xdc00);
            pos += 6;
        }
    }

    cp = value;
    return true;
}

// Position of a member's value, past the colon and any spacing, or npos.
// One lookup per member rather than trying a list of literal separators: that
// approach returned as soon as any of them matched, so "k": true was read as
// the four characters after the colon and never as a value at all.
static size_t findValue(const std::string &json, const std::string &key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos)
        return std::string::npos;
    pos += search.size();

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size() || json[pos] != ':')
        return std::string::npos;
    pos++;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n'))
        pos++;
    return pos < json.size() ? pos : std::string::npos;
}

std::string jsonString(const std::string &json, const std::string &key)
{
    {
        size_t pos = findValue(json, key);
        if (pos == std::string::npos || json[pos] != '"')
            return "";
        pos++;

        std::string out;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] != '\\' || pos + 1 >= json.size())
            {
                out += json[pos];
                pos++;
                continue;
            }

            char esc = json[pos + 1];
            if (esc == 'u')
            {
                size_t after = pos + 1;
                unsigned int cp = 0;
                if (readUnicodeEscape(json, after, cp))
                {
                    appendUtf8(out, cp);
                    pos = after;
                    continue;
                }
                out += esc;
                pos += 2;
                continue;
            }

            switch (esc)
            {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            default:  out += esc;  break; // covers \" \\ \/
            }
            pos += 2;
        }
        return out;
    }
}

bool jsonTrue(const std::string &json, const std::string &key)
{
    size_t pos = findValue(json, key);
    return pos != std::string::npos && json.compare(pos, 4, "true") == 0;
}

bool jsonInt(const std::string &json, const std::string &key, long long &out)
{
    size_t pos = findValue(json, key);
    if (pos == std::string::npos)
        return false;
    out = strtoll(json.c_str() + pos, NULL, 10);
    return true;
}

void jsonSplitArray(const std::string &json, const std::string &key,
                    std::vector<std::string> &out)
{
    size_t arrayStart = json.find("\"" + key + "\"");
    if (arrayStart == std::string::npos)
        return;
    arrayStart = json.find('[', arrayStart);
    if (arrayStart == std::string::npos)
        return;

    int depth = 0;
    bool inString = false;
    size_t objStart = 0;

    for (size_t i = arrayStart + 1; i < json.size(); i++)
    {
        char c = json[i];

        if (inString)
        {
            if (c == '\\')
                i++; // skip the escaped character, braces included
            else if (c == '"')
                inString = false;
            continue;
        }

        if (c == '"')
            inString = true;
        else if (c == '{')
        {
            if (depth == 0)
                objStart = i;
            depth++;
        }
        else if (c == '}')
        {
            depth--;
            if (depth == 0)
                out.push_back(json.substr(objStart, i - objStart + 1));
        }
        else if (c == ']' && depth == 0)
        {
            break; // end of this array
        }
    }
}

std::string jsonEscape(const std::string &value)
{
    std::string out;
    char buf[8];
    for (unsigned char c : value)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += (char)c;
            }
            break;
        }
    }
    return out;
}

std::string jsonEscapeAscii(const std::string &value)
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
            {
                snprintf(buf, sizeof(buf), "\\u%04x", cp);
            }
            out += buf;
        }
    }
    return out;
}

std::string jsonUnescape(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); i++)
    {
        if (value[i] != '\\' || i + 1 >= value.size())
        {
            out += value[i];
            continue;
        }

        char esc = value[i + 1];
        if (esc == 'u')
        {
            size_t after = i + 1;
            unsigned int cp = 0;
            if (readUnicodeEscape(value, after, cp))
            {
                appendUtf8(out, cp);
                i = after - 1;
                continue;
            }
        }

        switch (esc)
        {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        default:  out += esc;  break;
        }
        i++;
    }
    return out;
}
