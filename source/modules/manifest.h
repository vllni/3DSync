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
};

// Persists sync state to /3ds/3DSync/manifest.json.
// Keys are "<provider prefix>|<full local path>", e.g.
// "drive|/3ds/Checkpoint/saves/Game/001.sav" — see SyncProvider::manifestPrefix().
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
    static std::string _extractString(const std::string &json, const std::string &key);
    static bool _extractInt(const std::string &json, const std::string &key, long long &out);
    static std::string _escape(const std::string &s);
    static std::string _unescape(const std::string &s);
};

#endif
