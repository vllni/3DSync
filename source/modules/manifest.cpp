#include "manifest.h"

#include "../utils/debug.h"
#include "../utils/fsutil.h"
#include "../utils/json.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Manifest::Manifest(const std::string &path) : _path(path), _dirty(false) {}

// The '}' closing the object whose contents start at pos, or npos.  Braces and
// quotes inside a string value do not count: a tag or a path holding a '}'
// used to end the entry there, and every entry after it was lost.
static size_t objectEnd(const std::string &json, size_t pos)
{
    bool inString = false;
    for (; pos < json.size(); pos++)
    {
        char c = json[pos];
        if (inString)
        {
            if (c == '\\')
                pos++;
            else if (c == '"')
                inString = false;
        }
        else if (c == '"')
            inString = true;
        else if (c == '}')
            return pos;
    }
    return std::string::npos;
}

// ---------------------------------------------------------------------------
// load()
// ---------------------------------------------------------------------------
// Parses the JSON file produced by save().  The format is:
//   {
//     "drive|/full/path/file.sav":
//         {"mtime": 1234567890, "size": 512, "tag": "abc", "id": "xyz", "gen": 3},
//     ...
//   }
// "gen" is left out while it is 0, so a manifest that never met the n-way
// engine is written back byte for byte.  An older 3DSync reading a newer file
// ignores "gen" and keeps the "local|" entries as keys it never asks for.
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
    _dirty = false;

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
        size_t objEnd   = objectEnd(json, objStart);
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
        if (jsonInt(valueStr, "gen", number))
            entry.generation = number;

        entry.remoteTag = jsonString(valueStr, "tag");
        if (entry.remoteTag.empty())
            entry.remoteTag = jsonString(valueStr, "md5"); // pre-provider format
        entry.remoteId = jsonString(valueStr, "id");

        // Keys without a provider prefix predate multi-provider support and can
        // only have come from Google Drive.
        std::string key = jsonUnescape(rawKey);
        if (key.find('|') == std::string::npos)
        {
            key = "drive|" + key;
            _dirty = true; // migrated: worth writing back
        }

        _entries[key] = entry;
        pos = objEnd + 1;
    }
    debugf("manifest: %d entries from %s\n", (int)_entries.size(), _path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// save()
// ---------------------------------------------------------------------------
// Written to a temp file and swapped in, so a crash, a dead battery or a full
// card leaves the previous manifest whole rather than a truncated one.
bool Manifest::save()
{
    std::string tmpPath;
    FILE *fp = openTempFor(_path, tmpPath);
    if (!fp)
        return false;

    fputs("{\n", fp);
    bool first = true;
    for (const auto &kv : _entries)
    {
        if (!first) fputs(",\n", fp);
        first = false;

        fprintf(fp, "  \"%s\": {\"mtime\": %lld, \"size\": %lld, \"tag\": \"%s\", \"id\": \"%s\"",
                jsonEscape(kv.first).c_str(),
                (long long)kv.second.localMtime,
                kv.second.localSize,
                jsonEscape(kv.second.remoteTag).c_str(),
                jsonEscape(kv.second.remoteId).c_str());
        if (kv.second.generation != 0)
            fprintf(fp, ", \"gen\": %lld", kv.second.generation);
        fputc('}', fp);
    }
    fputs("\n}\n", fp);

    // A card that filled up during the sync fails here; the previous manifest
    // is then still in place.
    bool ok = (fflush(fp) == 0 && ferror(fp) == 0);
    if (fclose(fp) != 0)
        ok = false;
    if (!ok)
    {
        printf("Manifest: writing %s failed: %s\n", _path.c_str(), strerror(errno));
        debugf("%d entries were not saved\n", (int)_entries.size());
        remove(tmpPath.c_str());
        return false;
    }
    if (!replaceLocalFile(tmpPath, _path))
        return false;
    _dirty = false;
    return true;
}

bool Manifest::saveIfChanged()
{
    return !_dirty || save();
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
    // The engine rewrites records it only confirmed, and a run that changed
    // nothing should not write the file per entry.
    auto it = _entries.find(key);
    if (it != _entries.end() && it->second.localMtime == entry.localMtime &&
        it->second.localSize == entry.localSize &&
        it->second.remoteTag == entry.remoteTag && it->second.remoteId == entry.remoteId &&
        it->second.generation == entry.generation)
        return;
    _entries[key] = entry;
    _dirty = true;
}

void Manifest::remove(const std::string &key)
{
    if (_entries.erase(key) > 0)
        _dirty = true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
