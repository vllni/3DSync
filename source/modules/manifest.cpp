#include "manifest.h"

#include "../utils/debug.h"
#include "../utils/json.h"

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
    {
        // Normal on a first run, and indistinguishable from a card that cannot
        // be read — in which case every file will look like a first sync.
        debugErrno("opening the manifest", _path);
        return true;
    }

    std::string json;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        json.append(buf, n);
    if (ferror(fp))
        debugErrno("reading the manifest", _path);
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
        {
            // A half-written manifest (an interrupted save, a full card) stops
            // the parse here, and every file after this point then looks
            // unseen — which means a conflict prompt on each of them.
            debugf("manifest truncated after %d entries, %d bytes in\n",
                   (int)_entries.size(), (int)objStart);
            break;
        }

        std::string valueStr = json.substr(objStart, objEnd - objStart);

        ManifestEntry entry = {};

        long long number = 0;
        if (jsonInt(valueStr, "mtime", number))
            entry.localMtime = (time_t)number;
        if (jsonInt(valueStr, "size", number))
            entry.localSize = number;

        entry.remoteTag = jsonString(valueStr, "tag");
        if (entry.remoteTag.empty())
            entry.remoteTag = jsonString(valueStr, "md5"); // pre-provider format
        entry.remoteId = jsonString(valueStr, "id");

        // Keys without a provider prefix predate multi-provider support and can
        // only have come from Google Drive.
        std::string key = jsonUnescape(rawKey);
        if (key.find('|') == std::string::npos)
            key = "drive|" + key;

        _entries[key] = entry;
        pos = objEnd + 1;
    }
    debugf("manifest: %d entries from %s\n", (int)_entries.size(), _path.c_str());
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
                jsonEscape(kv.first).c_str(),
                (long long)kv.second.localMtime,
                kv.second.localSize,
                jsonEscape(kv.second.remoteTag).c_str(),
                jsonEscape(kv.second.remoteId).c_str());
    }
    fputs("\n}\n", fp);

    // The manifest is written at the very end of a run, so a card that fills up
    // during the sync fails here — and the file that is left is the truncated
    // one load() then gives up on.
    bool ok = (ferror(fp) == 0);
    if (fclose(fp) != 0)
        ok = false;
    if (!ok)
    {
        printf("Manifest: writing %s failed: %s\n", _path.c_str(), strerror(errno));
        debugf("%d entries were not saved\n", (int)_entries.size());
    }
    return ok;
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
