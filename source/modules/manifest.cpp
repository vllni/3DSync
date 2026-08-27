#include "manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Manifest::Manifest(const std::string &path) : _path(path) {}

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------
// Parses the JSON file produced by save().  The format is:
//   {
//     "drive|/full/path/file.sav":
//         {"mtime": 1234567890, "size": 512, "tag": "abc", "id": "xyz"},
//     ...
//   }
// Manifests written before multi-provider support used a bare local path as the
// key and called the tag "md5"; both are still read, and such keys are migrated
// to the "drive|" prefix so the first sync after an upgrade is not treated as a
// conflict on every file.
// We hand-roll the parser because no external JSON library is available.
// The parser tolerates both compact ("key":"val") and spaced ("key": "val")
// separators, matching the convention used by _extractString / GoogleDrive.
// ---------------------------------------------------------------------------
bool Manifest::load()
{
    FILE *fp = fopen(_path.c_str(), "r");
    if (!fp)
        return true; // Normal on first run — empty manifest

    std::string json;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        json.append(buf, n);
    fclose(fp);

    _entries.clear();

    size_t pos = 0;
    size_t len = json.size();

    while (pos < len)
    {
        // Find the opening quote of the next top-level key
        size_t keyStart = json.find('"', pos);
        if (keyStart == std::string::npos)
            break;

        // Find the closing quote of the key
        size_t keyEnd = keyStart + 1;
        while (keyEnd < len)
        {
            if (json[keyEnd] == '\\') { keyEnd += 2; continue; }
            if (json[keyEnd] == '"')  { break; }
            keyEnd++;
        }
        if (keyEnd >= len)
            break;

        std::string rawKey = json.substr(keyStart + 1, keyEnd - keyStart - 1);

        // Skip whitespace and the colon
        size_t after = keyEnd + 1;
        while (after < len && (json[after] == ' ' || json[after] == '\t' ||
               json[after] == '\r' || json[after] == '\n'))
            after++;
        if (after >= len || json[after] != ':')
        {
            pos = keyEnd + 1;
            continue;
        }
        after++; // skip ':'

        // Skip whitespace again
        while (after < len && (json[after] == ' ' || json[after] == '\t' ||
               json[after] == '\r' || json[after] == '\n'))
            after++;

        if (after >= len || json[after] != '{')
        {
            // Not a value object — skip (could be the outer brace or a comma)
            pos = keyEnd + 1;
            continue;
        }

        // Find the matching closing brace
        size_t objStart = after + 1;
        size_t objEnd   = json.find('}', objStart);
        if (objEnd == std::string::npos)
            break;

        std::string valueStr = json.substr(objStart, objEnd - objStart);

        ManifestEntry entry = {};

        long long number = 0;
        if (_extractInt(valueStr, "mtime", number))
            entry.localMtime = (time_t)number;
        if (_extractInt(valueStr, "size", number))
            entry.localSize = number;

        entry.remoteTag = _extractString(valueStr, "tag");
        if (entry.remoteTag.empty())
            entry.remoteTag = _extractString(valueStr, "md5"); // pre-provider format
        entry.remoteId = _extractString(valueStr, "id");

        // Keys without a provider prefix predate multi-provider support and can
        // only have come from Google Drive.
        std::string key = _unescape(rawKey);
        if (key.find('|') == std::string::npos)
            key = "drive|" + key;

        _entries[key] = entry;
        pos = objEnd + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// save()
// ---------------------------------------------------------------------------
bool Manifest::save() const
{
    FILE *fp = fopen(_path.c_str(), "w");
    if (!fp)
    {
        printf("Manifest: cannot write %s: %s\n", _path.c_str(), strerror(errno));
        return false;
    }

    fputs("{\n", fp);
    bool first = true;
    for (const auto &kv : _entries)
    {
        if (!first) fputs(",\n", fp);
        first = false;

        fprintf(fp, "  \"%s\": {\"mtime\": %lld, \"size\": %lld, \"tag\": \"%s\", \"id\": \"%s\"}",
                _escape(kv.first).c_str(),
                (long long)kv.second.localMtime,
                kv.second.localSize,
                _escape(kv.second.remoteTag).c_str(),
                _escape(kv.second.remoteId).c_str());
    }
    fputs("\n}\n", fp);
    fclose(fp);
    return true;
}

bool Manifest::has(const std::string &key) const
{
    return _entries.count(key) > 0;
}

ManifestEntry Manifest::get(const std::string &key) const
{
    auto it = _entries.find(key);
    if (it != _entries.end()) return it->second;
    return ManifestEntry{};
}

void Manifest::set(const std::string &key, const ManifestEntry &entry)
{
    _entries[key] = entry;
}

void Manifest::remove(const std::string &key)
{
    _entries.erase(key);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string Manifest::_extractString(const std::string &json, const std::string &key)
{
    // Handles both compact ("key":"val") and spaced ("key": "val") separators
    for (const char *sep : {"\":\"", "\": \""})
    {
        std::string search = "\"" + key + sep;
        size_t pos = json.find(search);
        if (pos != std::string::npos)
        {
            pos += search.size();
            size_t end = json.find('"', pos);
            if (end != std::string::npos)
                return json.substr(pos, end - pos);
        }
    }
    return "";
}

bool Manifest::_extractInt(const std::string &json, const std::string &key, long long &out)
{
    // Handles both compact ("key":123) and spaced ("key": 123) separators
    for (const char *sep : {"\":", "\": "})
    {
        std::string search = "\"" + key + sep;
        size_t pos = json.find(search);
        if (pos != std::string::npos)
        {
            out = strtoll(json.c_str() + pos + search.size(), nullptr, 10);
            return true;
        }
    }
    return false;
}

std::string Manifest::_escape(const std::string &s)
{
    std::string result;
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':  result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n";  break;
        case '\r': result += "\\r";  break;
        case '\t': result += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                result += buf;
            }
            else
            {
                result += (char)c;
            }
            break;
        }
    }
    return result;
}

std::string Manifest::_unescape(const std::string &s)
{
    std::string result;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            switch (s[i + 1])
            {
            case '"':  result += '"';  i++; break;
            case '\\': result += '\\'; i++; break;
            case 'n':  result += '\n'; i++; break;
            case 'r':  result += '\r'; i++; break;
            case 't':  result += '\t'; i++; break;
            default:   result += s[i]; break;
            }
        }
        else
        {
            result += s[i];
        }
    }
    return result;
}
