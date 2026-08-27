// The sync decision table, driven through MockRemote so every case is
// reachable: what performSync() does for each combination of local state,
// remote state and manifest state, and what it records afterwards.

#include "framework.h"
#include "mocks.h"

namespace
{

// A sync ready to run: one entry, one local folder, one mock remote.
struct Fixture
{
    TempDir local;
    MockRemote remote;
    MockUi ui;
    Manifest manifest;
    SyncSummary summary;
    SyncEntry entry;

    Fixture() : manifest(std::string("/tmp/3dsync-tests-manifest-unused.json"))
    {
        entry.localBase = local.path();
        entry.remoteName = "Checkpoint/saves";
        entry.direction = SYNC_BOTH;
    }

    // Declare which relative paths exist locally, as recurse_dir would.
    void localFiles(const std::vector<std::string> &paths) { entry.localFiles = paths; }

    std::string key(const std::string &relPath) const
    {
        return remote.manifestPrefix() + "|" + local.path() + relPath;
    }

    // Record the state a previous successful sync would have left behind.
    void seedManifest(const std::string &relPath, const std::string &remoteTag)
    {
        ManifestEntry record;
        record.localMtime = (time_t)local.mtimeOf(relPath);
        record.localSize = (long long)local.read(relPath).size();
        record.remoteTag = remoteTag;
        record.remoteId = "id" + relPath;
        manifest.set(key(relPath), record);
    }

    bool run(bool uploadOnly = false)
    {
        return performSync(remote, manifest, entry, summary, uploadOnly, ui);
    }
};

const char *SAVE = "/Game/001.sav";

} // namespace

TEST(engine, unchanged_file_is_left_alone)
{
    Fixture f;
    f.local.write(SAVE, "same", 1700000000);
    f.remote.setRemote(SAVE, "same");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
    CHECK_EQ(f.remote.downloaded.size(), (size_t)0);
    CHECK_EQ(f.summary.changes.size(), (size_t)0);
    CHECK_EQ(f.summary.checkedPaths.size(), (size_t)1);
}

TEST(engine, local_change_uploads)
{
    Fixture f;
    f.local.write(SAVE, "old", 1700000000);
    f.remote.setRemote(SAVE, "old");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    // A newer timestamp is what the engine notices first.
    f.local.write(SAVE, "new", 1700000900);

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
    CHECK_EQ(f.remote.files[SAVE].content, std::string("new"));
    CHECK_EQ(f.summary.uploaded, 1);
    // The manifest now describes what was just sent.
    CHECK_EQ(f.manifest.get(f.key(SAVE)).remoteTag, f.remote.tagOf(SAVE));
    CHECK_EQ(f.manifest.get(f.key(SAVE)).localSize, (long long)3);
}

TEST(engine, size_change_alone_uploads)
{
    Fixture f;
    // Same mtime, different length: FAT's 2-second granularity means a quick
    // save can look untouched, so size is checked as well.
    f.local.write(SAVE, "short", 1700000000);
    f.remote.setRemote(SAVE, "short");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.local.write(SAVE, "much longer content", 1700000000);

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
}

TEST(engine, content_change_under_frozen_mtime_uploads)
{
    Fixture f;
    f.remote.contentTags = true; // a remote with a content hash, like Drive
    f.local.write(SAVE, "aaaa", 1700000000);
    f.remote.setRemote(SAVE, "aaaa");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    // Same mtime and same length — only localTag() can see this.
    f.local.write(SAVE, "bbbb", 1700000000);

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
}

TEST(engine, content_change_under_frozen_mtime_is_missed_without_content_tags)
{
    Fixture f;
    f.remote.contentTags = false; // SMB, FTP, WebDAV: size + time only
    f.local.write(SAVE, "aaaa", 1700000000);
    f.remote.setRemote(SAVE, "aaaa");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.local.write(SAVE, "bbbb", 1700000000);

    CHECK(f.run());
    // Documents the known limit of a size+mtime tag rather than pretending
    // otherwise: same mtime, same size, so nothing is detected.
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
}

TEST(engine, remote_change_downloads)
{
    Fixture f;
    f.local.write(SAVE, "old", 1700000000);
    f.remote.setRemote(SAVE, "old");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.remote.setRemote(SAVE, "fresh from the server");

    CHECK(f.run());
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1);
    CHECK_EQ(f.local.read(SAVE), std::string("fresh from the server"));
    CHECK_EQ(f.summary.downloaded, 1);
    CHECK_EQ(f.manifest.get(f.key(SAVE)).remoteTag, f.remote.tagOf(SAVE));
}

TEST(engine, local_only_file_uploads)
{
    Fixture f;
    f.local.write(SAVE, "brand new", 1700000000);
    f.localFiles({SAVE});

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
    CHECK_EQ(f.remote.downloaded.size(), (size_t)0);
}

TEST(engine, remote_only_file_downloads)
{
    Fixture f;
    f.remote.setRemote(SAVE, "from the server");
    f.localFiles({});

    CHECK(f.run());
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1);
    CHECK(f.local.exists(SAVE));
    CHECK_EQ(f.local.read(SAVE), std::string("from the server"));
}

TEST(engine, both_sides_gone_clears_the_manifest_entry)
{
    Fixture f;
    ManifestEntry stale;
    stale.localMtime = 1700000000;
    stale.localSize = 4;
    stale.remoteTag = "whatever";
    f.manifest.set(f.key(SAVE), stale);
    // Neither side has the file any more, but the entry names it.
    f.entry.localFiles.push_back(SAVE);

    CHECK(f.run());
    CHECK(!f.manifest.has(f.key(SAVE)));
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
    CHECK_EQ(f.remote.downloaded.size(), (size_t)0);
}

TEST(engine, first_sync_with_both_sides_present_is_a_conflict)
{
    Fixture f;
    // No manifest entry, so there is no baseline to diff against and the engine
    // must ask rather than guess which side is newer.
    f.local.write(SAVE, "local", 1700000000);
    f.remote.setRemote(SAVE, "remote");
    f.localFiles({SAVE});
    f.ui.answers = {CONFLICT_KEEP_LOCAL};

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK_EQ(f.remote.files[SAVE].content, std::string("local"));
}

TEST(engine, conflict_keep_local_uploads)
{
    Fixture f;
    f.local.write(SAVE, "start", 1700000000);
    f.remote.setRemote(SAVE, "start");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.local.write(SAVE, "local wins", 1700000900);
    f.remote.setRemote(SAVE, "remote version");
    f.ui.answers = {CONFLICT_KEEP_LOCAL};

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK_EQ(f.remote.files[SAVE].content, std::string("local wins"));
    CHECK_EQ(f.summary.uploaded, 1);
}

TEST(engine, conflict_keep_remote_downloads)
{
    Fixture f;
    f.local.write(SAVE, "start", 1700000000);
    f.remote.setRemote(SAVE, "start");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.local.write(SAVE, "local version", 1700000900);
    f.remote.setRemote(SAVE, "remote wins");
    f.ui.answers = {CONFLICT_KEEP_REMOTE};

    CHECK(f.run());
    CHECK_EQ(f.local.read(SAVE), std::string("remote wins"));
    CHECK_EQ(f.summary.downloaded, 1);
}

TEST(engine, conflict_skip_touches_nothing)
{
    Fixture f;
    f.local.write(SAVE, "start", 1700000000);
    f.remote.setRemote(SAVE, "start");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.local.write(SAVE, "local version", 1700000900);
    f.remote.setRemote(SAVE, "remote version");
    f.ui.answers = {CONFLICT_SKIP};

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
    CHECK_EQ(f.remote.downloaded.size(), (size_t)0);
    CHECK_EQ(f.local.read(SAVE), std::string("local version"));
    CHECK_EQ(f.summary.changes.size(), (size_t)1);
    CHECK_STR_EQ(f.summary.changes[0].action, "skipped");
}

TEST(engine, conflict_cancel_stops_the_sync)
{
    Fixture f;
    f.local.write(SAVE, "start", 1700000000);
    f.remote.setRemote(SAVE, "start");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.local.write(SAVE, "local version", 1700000900);
    f.remote.setRemote(SAVE, "remote version");
    f.ui.answers = {CONFLICT_CANCEL};

    CHECK(!f.run());
    CHECK(f.ui.cancelled); // the caller must see the cancellation too
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
}

TEST(engine, upload_only_never_downloads)
{
    Fixture f;
    f.remote.setRemote("/only-remote.sav", "server side");
    f.local.write(SAVE, "local", 1700000000);
    f.localFiles({SAVE});

    CHECK(f.run(true));
    CHECK_EQ(f.remote.downloaded.size(), (size_t)0);
    CHECK(!f.local.exists("/only-remote.sav"));
    // The remote-only file is left in place rather than deleted.
    CHECK_EQ(f.remote.files.count("/only-remote.sav"), (size_t)1);
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
}

TEST(engine, upload_only_resolves_conflicts_as_local_wins)
{
    Fixture f;
    f.local.write(SAVE, "start", 1700000000);
    f.remote.setRemote(SAVE, "start");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.local.write(SAVE, "local version", 1700000900);
    f.remote.setRemote(SAVE, "remote version");

    CHECK(f.run(true));
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0); // never prompts
    CHECK_EQ(f.remote.files[SAVE].content, std::string("local version"));
}

TEST(engine, upload_only_leaves_a_remote_only_change_alone)
{
    Fixture f;
    f.local.write(SAVE, "same", 1700000000);
    f.remote.setRemote(SAVE, "same");
    f.localFiles({SAVE});
    f.seedManifest(SAVE, f.remote.tagOf(SAVE));

    f.remote.setRemote(SAVE, "changed on the server");

    CHECK(f.run(true));
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
    CHECK_EQ(f.local.read(SAVE), std::string("same"));
}

TEST(engine, failed_upload_leaves_no_manifest_entry)
{
    Fixture f;
    f.remote.failUploads = true;
    f.local.write(SAVE, "local", 1700000000);
    f.localFiles({SAVE});

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
    // Without an entry the next run retries, instead of believing it succeeded.
    CHECK(!f.manifest.has(f.key(SAVE)));
    CHECK_EQ(f.summary.uploaded, 0);
    CHECK_EQ(f.summary.changes.size(), (size_t)0);
}

TEST(engine, failed_download_leaves_no_manifest_entry)
{
    Fixture f;
    f.remote.failDownloads = true;
    f.remote.setRemote(SAVE, "server side");
    f.localFiles({});

    CHECK(f.run());
    CHECK(!f.manifest.has(f.key(SAVE)));
    CHECK_EQ(f.summary.downloaded, 0);
}

TEST(engine, fatal_provider_error_stops_the_entry)
{
    Fixture f;
    f.remote.fatalOnList = true;
    f.local.write(SAVE, "local", 1700000000);
    f.localFiles({SAVE});

    CHECK(!f.run());
    CHECK(f.remote.hasFatalError());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
}

TEST(engine, listing_failure_without_fatal_error_skips_the_entry)
{
    Fixture f;
    f.remote.failList = true;
    f.local.write(SAVE, "local", 1700000000);
    f.localFiles({SAVE});

    // A folder that cannot be listed is skipped, not treated as empty — which
    // would upload every local file over whatever is really there.
    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
}

TEST(engine, cancellation_stops_before_the_next_file)
{
    Fixture f;
    f.local.write("/a.sav", "a", 1700000000);
    f.local.write("/b.sav", "b", 1700000000);
    f.local.write("/c.sav", "c", 1700000000);
    f.localFiles({"/a.sav", "/b.sav", "/c.sav"});
    f.ui.cancelAfterPolls = 1; // stop once the first file is done

    CHECK(!f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
    // What did happen is recorded, so a resumed sync does not redo it.
    CHECK_EQ(f.manifest.has(f.key("/a.sav")), true);
    CHECK(!f.manifest.has(f.key("/b.sav")));
}

TEST(engine, manifest_keys_are_scoped_per_remote)
{
    Fixture f;
    f.local.write(SAVE, "local", 1700000000);
    f.localFiles({SAVE});

    CHECK(f.run());
    CHECK(f.manifest.has("mock|" + f.local.path() + SAVE));

    // A second remote sees no state for the same local file, so it uploads too
    // instead of trusting the first remote's tag.
    MockRemote second;
    second.prefix = "other";
    SyncSummary secondSummary;
    MockUi secondUi;
    CHECK(performSync(second, f.manifest, f.entry, secondSummary, false, secondUi));
    CHECK_EQ(second.uploaded.size(), (size_t)1);
    CHECK(f.manifest.has("other|" + f.local.path() + SAVE));
}

TEST(engine, legacy_manifest_entry_without_a_size_does_not_reupload)
{
    Fixture f;
    f.local.write(SAVE, "unchanged", 1700000000);
    f.remote.setRemote(SAVE, "unchanged");
    f.localFiles({SAVE});

    // Manifests written before the size field carried mtime only; a zero must
    // mean "unknown", not "zero bytes", or every file would look changed.
    ManifestEntry legacy;
    legacy.localMtime = (time_t)f.local.mtimeOf(SAVE);
    legacy.localSize = 0;
    legacy.remoteTag = f.remote.tagOf(SAVE);
    legacy.remoteId = "id" + std::string(SAVE);
    f.manifest.set(f.key(SAVE), legacy);

    CHECK(f.run());
    CHECK_EQ(f.remote.uploaded.size(), (size_t)0);
}

TEST(engine, nested_paths_round_trip)
{
    Fixture f;
    f.remote.setRemote("/TitleA/deep/nested/001.sav", "nested content");
    f.localFiles({});

    CHECK(f.run());
    CHECK(f.local.exists("/TitleA/deep/nested/001.sav"));
    CHECK_EQ(f.local.read("/TitleA/deep/nested/001.sav"), std::string("nested content"));
}
