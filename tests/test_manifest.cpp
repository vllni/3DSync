// Manifest persistence.  Every sync decision is made against this file, so the
// round trip and the migration of older formats matter more than most things
// here: a manifest that fails to load looks exactly like a first sync, which
// raises a conflict on every file.

#include <stdio.h>
#include <stdlib.h>

#include "framework.h"

#include "../source/modules/manifest.h"

namespace
{

// A manifest path in a scratch directory, removed on destruction.
class ManifestFile
{
public:
    ManifestFile()
    {
        char templatePath[] = "/tmp/3dsync-manifest-XXXXXX";
        const char *dir = mkdtemp(templatePath);
        _dir = (dir != NULL) ? dir : "/tmp";
        _path = _dir + "/manifest.json";
    }

    ~ManifestFile()
    {
        std::string command = "rm -rf '" + _dir + "'";
        if (system(command.c_str()) != 0)
            return;
    }

    const std::string &path() const { return _path; }

    void writeRaw(const std::string &contents)
    {
        FILE *fp = fopen(_path.c_str(), "w");
        if (!fp)
            return;
        fwrite(contents.data(), 1, contents.size(), fp);
        fclose(fp);
    }

    std::string readRaw() const
    {
        FILE *fp = fopen(_path.c_str(), "rb");
        if (!fp)
            return "";
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
            out.append(buf, n);
        fclose(fp);
        return out;
    }

private:
    std::string _dir;
    std::string _path;
};

ManifestEntry makeEntry(long long mtime, long long size, const std::string &tag,
                        const std::string &id)
{
    ManifestEntry entry;
    entry.localMtime = (time_t)mtime;
    entry.localSize = size;
    entry.remoteTag = tag;
    entry.remoteId = id;
    return entry;
}

} // namespace

TEST(manifest, missing_file_loads_as_empty)
{
    ManifestFile file;
    Manifest manifest(file.path());
    // A first run has no manifest, which is not an error.
    CHECK(manifest.load());
    CHECK(!manifest.has("drive|/3ds/x.sav"));
}

TEST(manifest, round_trips_an_entry)
{
    ManifestFile file;
    {
        Manifest manifest(file.path());
        manifest.set("drive|/3ds/saves/001.sav", makeEntry(1716905520, 32768, "abc123", "fileId"));
        CHECK(manifest.save());
    }

    Manifest reloaded(file.path());
    CHECK(reloaded.load());
    CHECK(reloaded.has("drive|/3ds/saves/001.sav"));

    ManifestEntry entry = reloaded.get("drive|/3ds/saves/001.sav");
    CHECK_EQ((long long)entry.localMtime, (long long)1716905520);
    CHECK_EQ(entry.localSize, (long long)32768);
    CHECK_STR_EQ(entry.remoteTag, "abc123");
    CHECK_STR_EQ(entry.remoteId, "fileId");
}

TEST(manifest, round_trips_several_remotes_for_one_path)
{
    ManifestFile file;
    {
        Manifest manifest(file.path());
        manifest.set("drive|/3ds/001.sav", makeEntry(1, 10, "md5", "driveId"));
        manifest.set("smb://nas/3ds|/3ds/001.sav", makeEntry(2, 20, "20:2", "/3ds/001.sav"));
        manifest.set("dropbox|/3ds/001.sav", makeEntry(3, 30, "hash", "rev:abc"));
        CHECK(manifest.save());
    }

    Manifest reloaded(file.path());
    CHECK(reloaded.load());
    CHECK_STR_EQ(reloaded.get("drive|/3ds/001.sav").remoteTag, "md5");
    CHECK_STR_EQ(reloaded.get("smb://nas/3ds|/3ds/001.sav").remoteTag, "20:2");
    CHECK_STR_EQ(reloaded.get("dropbox|/3ds/001.sav").remoteId, "rev:abc");
}

TEST(manifest, migrates_a_pre_provider_manifest)
{
    ManifestFile file;
    // Written before multi-provider support: a bare local path as the key and
    // the tag called "md5".  Both must still be read, and the key must gain the
    // drive prefix, or an upgrade would look like a first sync everywhere.
    file.writeRaw("{\n"
                  "  \"/3ds/Checkpoint/saves/001.sav\": "
                  "{\"mtime\": 1716905520, \"md5\": \"abc123\", \"id\": \"driveFileId\"}\n"
                  "}\n");

    Manifest manifest(file.path());
    CHECK(manifest.load());
    CHECK(manifest.has("drive|/3ds/Checkpoint/saves/001.sav"));

    ManifestEntry entry = manifest.get("drive|/3ds/Checkpoint/saves/001.sav");
    CHECK_STR_EQ(entry.remoteTag, "abc123");
    CHECK_STR_EQ(entry.remoteId, "driveFileId");
    CHECK_EQ((long long)entry.localMtime, (long long)1716905520);
    // The old format had no size; 0 means unknown.
    CHECK_EQ(entry.localSize, (long long)0);
}

TEST(manifest, prefers_tag_over_a_legacy_md5_field)
{
    ManifestFile file;
    file.writeRaw("{\"drive|/x\": {\"mtime\": 1, \"tag\": \"new\", \"md5\": \"old\"}}");
    Manifest manifest(file.path());
    CHECK(manifest.load());
    CHECK_STR_EQ(manifest.get("drive|/x").remoteTag, "new");
}

TEST(manifest, tolerates_compact_and_spaced_json)
{
    ManifestFile file;
    file.writeRaw("{\"drive|/a\":{\"mtime\":5,\"size\":6,\"tag\":\"t\",\"id\":\"i\"},"
                  "\"drive|/b\": {\"mtime\": 7, \"size\": 8, \"tag\": \"u\", \"id\": \"j\"}}");
    Manifest manifest(file.path());
    CHECK(manifest.load());
    CHECK_EQ((long long)manifest.get("drive|/a").localMtime, (long long)5);
    CHECK_EQ(manifest.get("drive|/b").localSize, (long long)8);
    CHECK_STR_EQ(manifest.get("drive|/b").remoteTag, "u");
}

TEST(manifest, escapes_and_restores_awkward_keys)
{
    ManifestFile file;
    // Backslashes and quotes cannot occur on FAT, but a corrupted or
    // hand-edited manifest can contain them and must not break parsing.
    std::string key = "drive|/3ds/od\"d\\path/001.sav";
    {
        Manifest manifest(file.path());
        manifest.set(key, makeEntry(1, 2, "t", "i"));
        CHECK(manifest.save());
    }
    Manifest reloaded(file.path());
    CHECK(reloaded.load());
    CHECK(reloaded.has(key));
}

TEST(manifest, keeps_unicode_paths_intact)
{
    ManifestFile file;
    std::string key = "drive|/3ds/Pok\xc3\xa9mon X/001.sav";
    {
        Manifest manifest(file.path());
        manifest.set(key, makeEntry(11, 22, "t", "i"));
        CHECK(manifest.save());
    }
    Manifest reloaded(file.path());
    CHECK(reloaded.load());
    CHECK(reloaded.has(key));
    CHECK_EQ(reloaded.get(key).localSize, (long long)22);
}

TEST(manifest, remove_drops_the_entry)
{
    ManifestFile file;
    Manifest manifest(file.path());
    manifest.set("drive|/x", makeEntry(1, 2, "t", "i"));
    CHECK(manifest.has("drive|/x"));
    manifest.remove("drive|/x");
    CHECK(!manifest.has("drive|/x"));
    // Removing something absent is not an error.
    manifest.remove("drive|/nothing");
}

TEST(manifest, missing_entry_reads_as_zeroes)
{
    ManifestFile file;
    Manifest manifest(file.path());
    ManifestEntry entry = manifest.get("drive|/absent");
    CHECK_EQ((long long)entry.localMtime, (long long)0);
    CHECK_EQ(entry.localSize, (long long)0);
    CHECK_STR_EQ(entry.remoteTag, "");
}

TEST(manifest, saved_file_is_valid_json_shape)
{
    ManifestFile file;
    {
        Manifest manifest(file.path());
        manifest.set("drive|/a", makeEntry(1, 2, "t", "i"));
        manifest.set("drive|/b", makeEntry(3, 4, "u", "j"));
        CHECK(manifest.save());
    }
    std::string raw = file.readRaw();
    CHECK(raw.size() > 2);
    CHECK_EQ(raw[0], '{');
    CHECK(raw.find("\"tag\": \"t\"") != std::string::npos);
    // Entries are comma separated, with no trailing comma before the brace.
    CHECK(raw.find(",\n") != std::string::npos);
    CHECK(raw.find(",\n}") == std::string::npos);
}

TEST(manifest, an_empty_manifest_saves_and_reloads)
{
    ManifestFile file;
    {
        Manifest manifest(file.path());
        CHECK(manifest.save());
    }
    Manifest reloaded(file.path());
    CHECK(reloaded.load());
    CHECK(!reloaded.has("anything"));
}

TEST(manifest, garbage_does_not_crash_the_parser)
{
    ManifestFile file;
    const char *bodies[] = {"", "{", "}", "not json at all", "{\"unterminated\": {",
                            "{\"a\": }", "[1,2,3]"};
    for (const char *body : bodies)
    {
        file.writeRaw(body);
        Manifest manifest(file.path());
        // Whatever it makes of the contents, it must return and stay usable.
        manifest.load();
        manifest.set("drive|/x", makeEntry(1, 1, "t", "i"));
        CHECK(manifest.has("drive|/x"));
    }
}
