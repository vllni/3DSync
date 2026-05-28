#ifndef MODULES_MANIFEST_H
#define MODULES_MANIFEST_H

#include <ctime>
#include <map>
#include <string>

// Record of one file's last-known sync state.
struct ManifestEntry
{
    time_t localMtime; // st_mtime at the time of the last successful sync
    std::string driveMd5; // md5Checksum returned by Drive after the last sync
    std::string driveId;  // Drive file ID (for update/download without a name search)
};

// Persists sync state to /3ds/3DSync/manifest.json.
// Keys are the full local filesystem paths (e.g. /3ds/Checkpoint/saves/Game/001.sav).
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
    static std::string _escape(const std::string &s);
    static std::string _unescape(const std::string &s);
};

#endif
