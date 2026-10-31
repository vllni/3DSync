#ifndef MODULES_MANIFEST_H
#define MODULES_MANIFEST_H

#include <ctime>
#include <map>
#include <string>

// Record of one file's last-known sync state.
struct ManifestEntry
{
    time_t localMtime;     // st_mtime after the last successful sync
    long long localSize;   // st_size after the last successful sync (0 = unknown)
    std::string remoteTag; // provider change token: MD5, ETag or "<size>:<mtime>"
    std::string remoteId;  // provider handle (Drive file ID, or the remote path)
    // Which version of the file this side holds, counted up each time the
    // 3DS's copy changes.  0: unknown, as in every entry written before the
    // n-way engine; syncengine.cpp then falls back to comparing mtime and size.
    long long generation;
};

// Persists sync state to /3ds/3DSync/manifest.json.
// Keys are "<provider prefix>|<full local path>", e.g.
// "drive|/3ds/Checkpoint/saves/Game/001.sav" — see SyncProvider::manifestPrefix().
// "local|<full local path>" is the 3DS's own copy as of the last sync: its
// mtime, size and generation, and no tag.
class Manifest
{
public:
    explicit Manifest(const std::string &path);
    // Returns true on success or if the file does not yet exist (first run).
    bool load();
    bool save() const;
    bool has(const std::string &key) const;
    ManifestEntry get(const std::string &key) const;
    void set(const std::string &key, const ManifestEntry &entry);
    void remove(const std::string &key);

private:
    std::string _path;
    std::map<std::string, ManifestEntry> _entries;
};

#endif
