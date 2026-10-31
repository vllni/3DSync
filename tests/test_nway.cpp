// The n-way engine: several remotes on one entry, every file decided once with
// all of them in view.  test_engine.cpp covers the decision table with one
// remote; this covers what only exists with more than one — propagation in a
// single run, a remote that missed a change, conflicts between remotes, and
// the upgrade from manifests the pairwise engine wrote.

#include "framework.h"
#include "mocks.h"

#include <unistd.h>

namespace
{

const char *SAVE = "/Game/001.sav";

// One entry, one local folder, three remotes A, B and C.
struct Fixture
{
    TempDir local;
    MockRemote a, b, c;
    MockUi ui;
    Manifest manifest;
    SyncSummary summary;
    SyncEntry entry;
    // Leave a remote out of the next run, as if it could not be reached.
    bool useA = true, useB = true, useC = true;
    bool sinkC = false;
    bool readOnlyC = false;
    // Name the file to the engine even when the 3DS no longer has it.
    bool listSave = false;

    Fixture() : manifest(std::string("/tmp/3dsync-tests-manifest-unused.json"))
    {
        a.prefix = "a";
        b.prefix = "b";
        c.prefix = "c";
        entry.localBase = local.path();
        entry.remoteName = "saves";
        entry.direction = SYNC_BOTH;
    }

    std::string key(const MockRemote &r, const std::string &relPath) const
    {
        return r.manifestPrefix() + "|" + local.path() + relPath;
    }

    std::string localKey(const std::string &relPath) const
    {
        return "local|" + local.path() + relPath;
    }

    bool run()
    {
        // What recurse_dir would find right now.
        entry.localFiles.clear();
        if (local.exists(SAVE) || listSave)
            entry.localFiles.push_back(SAVE);

        std::vector<Participant> parts;
        if (useA)
            parts.push_back(Participant(a, "A", a.prefix));
        if (useB)
            parts.push_back(Participant(b, "B", b.prefix));
        if (useC)
            parts.push_back(Participant(c, "C", c.prefix, sinkC, readOnlyC));
        summary = SyncSummary();
        return syncEntry(parts, manifest, entry, summary, ui);
    }

    // A file the 3DS made, synced once to every remote.
    void syncedEverywhere(const std::string &content)
    {
        local.write(SAVE, content, 1700000000);
        CHECK(run());
        CHECK_EQ(a.files[SAVE].content, content);
        CHECK_EQ(b.files[SAVE].content, content);
        CHECK_EQ(c.files[SAVE].content, content);
        clearCalls();
    }

    void clearCalls()
    {
        MockRemote *all[] = {&a, &b, &c};
        for (MockRemote *r : all)
        {
            r->uploaded.clear();
            r->downloaded.clear();
        }
        ui.conflictsRaised.clear();
        ui.sidesOffered.clear();
    }
};

bool hasChange(const SyncSummary &s, const std::string &action, const std::string &remote)
{
    for (auto &c : s.changes)
        if (c.action == action && c.remote == remote)
            return true;
    return false;
}

} // namespace

TEST(nway, a_new_local_file_reaches_every_remote)
{
    Fixture f;
    f.syncedEverywhere("v1");
    CHECK(hasChange(f.summary, "uploaded", "A, B, C"));
    CHECK_EQ(f.summary.uploaded, 3);
    CHECK_EQ(f.manifest.get(f.localKey(SAVE)).generation, (long long)1);
    CHECK_EQ(f.manifest.get(f.key(f.b, SAVE)).generation, (long long)1);
}

TEST(nway, a_change_on_any_remote_reaches_every_side_in_one_run)
{
    // Whatever the remote's place in the order: the pairwise engine only got
    // this right for the first one.
    for (int changed = 0; changed < 3; changed++)
    {
        Fixture f;
        f.syncedEverywhere("v1");
        MockRemote *remotes[] = {&f.a, &f.b, &f.c};
        remotes[changed]->setRemote(SAVE, "v2");

        CHECK(f.run());
        CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
        CHECK_EQ(f.local.read(SAVE), std::string("v2"));
        CHECK_EQ(f.a.files[SAVE].content, std::string("v2"));
        CHECK_EQ(f.b.files[SAVE].content, std::string("v2"));
        CHECK_EQ(f.c.files[SAVE].content, std::string("v2"));
        CHECK_EQ(remotes[changed]->downloaded.size(), (size_t)1);
        CHECK_EQ(remotes[changed]->uploaded.size(), (size_t)0);
        CHECK_EQ(f.summary.downloaded, 1);
        CHECK_EQ(f.summary.uploaded, 2);
        CHECK_EQ(f.manifest.get(f.localKey(SAVE)).generation, (long long)2);
    }
}

TEST(nway, the_summary_names_the_remotes)
{
    Fixture f;
    f.syncedEverywhere("v1");
    f.b.setRemote(SAVE, "v2");

    CHECK(f.run());
    CHECK(hasChange(f.summary, "downloaded", "B"));
    CHECK(hasChange(f.summary, "uploaded", "A, C"));
    bool named = false;
    for (auto &line : summaryLines(f.summary))
        named = named || line.find("uploaded (A, C): ") != std::string::npos;
    CHECK(named);
}

TEST(nway, a_remote_that_missed_a_change_is_brought_up_to_date)
{
    Fixture f;
    f.syncedEverywhere("v1");

    // B is offline while the 3DS changes the file.
    f.useB = false;
    f.local.write(SAVE, "v2 from the 3DS", 1700000900);
    CHECK(f.run());
    CHECK_EQ(f.a.files[SAVE].content, std::string("v2 from the 3DS"));
    CHECK_EQ(f.b.files[SAVE].content, std::string("v1"));
    f.clearCalls();

    // Back again: behind, not changed — it is sent the new version, not asked.
    f.useB = true;
    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.b.files[SAVE].content, std::string("v2 from the 3DS"));
    CHECK_EQ(f.b.downloaded.size(), (size_t)0);
    CHECK_EQ(f.a.uploaded.size(), (size_t)0);
}

TEST(nway, a_remote_that_missed_a_change_and_made_its_own_is_a_conflict)
{
    Fixture f;
    f.syncedEverywhere("v1");

    f.useB = false;
    f.local.write(SAVE, "v2 from the 3DS", 1700000900);
    CHECK(f.run());
    f.clearCalls();

    // Meanwhile B was edited from the old version: neither is newer.
    f.b.setRemote(SAVE, "v2 from elsewhere");
    f.useB = true;
    f.ui.answers = {"3DS"};
    CHECK(f.run());
    CHECK_EQ(f.ui.sidesOffered.size(), (size_t)1);
    CHECK(f.ui.sidesOffered[0] == std::vector<std::string>({"3DS", "B"}));
    CHECK_EQ(f.b.files[SAVE].content, std::string("v2 from the 3DS"));
}

TEST(nway, two_remotes_changed_is_one_prompt_between_them)
{
    Fixture f;
    f.syncedEverywhere("v1");
    f.a.setRemote(SAVE, "edited on A");
    f.c.setRemote(SAVE, "edited on C");
    f.ui.answers = {"C"};

    CHECK(f.run());
    CHECK_EQ(f.ui.sidesOffered.size(), (size_t)1);
    // The 3DS did not change it, so it is not one of the choices.
    CHECK(f.ui.sidesOffered[0] == std::vector<std::string>({"A", "C"}));
    CHECK_EQ(f.local.read(SAVE), std::string("edited on C"));
    CHECK_EQ(f.a.files[SAVE].content, std::string("edited on C"));
    CHECK_EQ(f.b.files[SAVE].content, std::string("edited on C"));
}

TEST(nway, a_skipped_conflict_changes_nothing)
{
    Fixture f;
    f.syncedEverywhere("v1");
    f.a.setRemote(SAVE, "edited on A");
    f.b.setRemote(SAVE, "edited on B");
    f.ui.answers = {"skip"};

    CHECK(f.run());
    CHECK_EQ(f.local.read(SAVE), std::string("v1"));
    CHECK_EQ(f.a.files[SAVE].content, std::string("edited on A"));
    CHECK_EQ(f.b.files[SAVE].content, std::string("edited on B"));
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);

    // ...and asks again next time, rather than forgetting either edit.
    f.clearCalls();
    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
}

TEST(nway, a_failed_download_of_the_winner_copies_nothing)
{
    Fixture f;
    f.syncedEverywhere("v1");
    f.a.setRemote(SAVE, "v2");
    f.a.failDownloads = true;

    CHECK(f.run());
    CHECK_EQ(f.local.read(SAVE), std::string("v1"));
    CHECK_EQ(f.b.uploaded.size(), (size_t)0);
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);
    CHECK_EQ(f.summary.failures.size(), (size_t)1);
    CHECK_STR_EQ(f.summary.failures[0].remote, "A");

    // Retried next run.
    f.a.failDownloads = false;
    CHECK(f.run());
    CHECK_EQ(f.c.files[SAVE].content, std::string("v2"));
}

TEST(nway, a_failed_upload_is_retried_without_a_conflict)
{
    Fixture f;
    f.syncedEverywhere("v1");
    f.b.failUploads = true;
    f.local.write(SAVE, "v2", 1700000900);
    CHECK(f.run());
    CHECK_EQ(f.summary.failures.size(), (size_t)1);
    CHECK_EQ(f.summary.checkedPaths.size(), (size_t)0);

    f.b.failUploads = false;
    f.clearCalls();
    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.b.files[SAVE].content, std::string("v2"));
    CHECK_EQ(f.a.uploaded.size(), (size_t)0);
}

TEST(nway, the_same_bytes_on_two_sides_are_not_a_conflict)
{
    // Another device put the same save on A that the 3DS has just made: with
    // a content tag that is provable, and there is nothing to choose.
    Fixture f;
    f.a.contentTags = true;
    f.syncedEverywhere("v1");
    f.local.write(SAVE, "v2", 1700000900);
    f.a.setRemote(SAVE, "v2");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.a.uploaded.size(), (size_t)0);
    CHECK_EQ(f.b.files[SAVE].content, std::string("v2"));
    CHECK_EQ(f.manifest.get(f.key(f.a, SAVE)).generation, (long long)2);
}

TEST(nway, adding_a_remote_that_already_holds_the_file)
{
    Fixture f;
    f.useB = false;
    f.useC = false;
    f.local.write(SAVE, "v1", 1700000000);
    CHECK(f.run());

    // B arrives with the same bytes and a content hash: adopted silently.
    f.b.contentTags = true;
    f.b.setRemote(SAVE, "v1");
    // C arrives with something else: the user decides.
    f.c.setRemote(SAVE, "other");
    f.useB = true;
    f.useC = true;
    f.ui.answers = {"3DS"};
    f.clearCalls();

    CHECK(f.run());
    CHECK_EQ(f.ui.sidesOffered.size(), (size_t)1);
    CHECK(f.ui.sidesOffered[0] == std::vector<std::string>({"3DS", "C"}));
    CHECK_EQ(f.b.uploaded.size(), (size_t)0);
    CHECK_EQ(f.c.files[SAVE].content, std::string("v1"));
    CHECK(f.manifest.has(f.key(f.b, SAVE)));
}

TEST(nway, a_sink_never_wins_and_is_never_asked)
{
    Fixture f;
    f.sinkC = true;
    f.syncedEverywhere("v1");
    f.c.setRemote(SAVE, "edited on the backup");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.local.read(SAVE), std::string("v1"));
    CHECK_EQ(f.c.downloaded.size(), (size_t)0);
    // Left alone until there is something new to send it.
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);

    f.a.setRemote(SAVE, "v2");
    CHECK(f.run());
    CHECK_EQ(f.c.files[SAVE].content, std::string("v2"));
}

TEST(nway, a_file_only_on_a_sink_is_not_fetched)
{
    Fixture f;
    f.sinkC = true;
    f.c.setRemote("/only-on-backup.sav", "backup");

    CHECK(f.run());
    CHECK(!f.local.exists("/only-on-backup.sav"));
    CHECK_EQ(f.a.uploaded.size(), (size_t)0);
}

// C as a source: it holds v1, and the first run brings it to the 3DS, A and B.
void syncedFromSource(Fixture &f)
{
    f.readOnlyC = true;
    f.c.setRemote(SAVE, "v1");
    CHECK(f.run());
    CHECK_EQ(f.local.read(SAVE), std::string("v1"));
    CHECK_EQ(f.a.files[SAVE].content, std::string("v1"));
    CHECK_EQ(f.b.files[SAVE].content, std::string("v1"));
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);
    f.clearCalls();
}

TEST(nway, a_change_on_a_source_reaches_every_side)
{
    Fixture f;
    syncedFromSource(f);
    f.c.setRemote(SAVE, "v2");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.local.read(SAVE), std::string("v2"));
    CHECK_EQ(f.a.files[SAVE].content, std::string("v2"));
    CHECK_EQ(f.b.files[SAVE].content, std::string("v2"));
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);
}

TEST(nway, a_source_is_never_written_to)
{
    Fixture f;
    syncedFromSource(f);
    f.local.write(SAVE, "edited on the 3DS", 1700000100);

    CHECK(f.run());
    CHECK_EQ(f.a.files[SAVE].content, std::string("edited on the 3DS"));
    CHECK_EQ(f.b.files[SAVE].content, std::string("edited on the 3DS"));
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);
    CHECK_EQ(f.c.files[SAVE].content, std::string("v1"));

    // Being behind is its normal state, not something to raise or retry.
    f.clearCalls();
    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.a.uploaded.size() + f.b.uploaded.size() + f.c.uploaded.size(), (size_t)0);
    CHECK_EQ(f.local.read(SAVE), std::string("edited on the 3DS"));
}

TEST(nway, a_source_changed_after_a_3ds_edit_is_a_conflict)
{
    Fixture f;
    syncedFromSource(f);
    f.local.write(SAVE, "edited on the 3DS", 1700000100);
    CHECK(f.run());
    f.clearCalls();

    // Its edit started from v1, not from the 3DS's version.
    f.c.setRemote(SAVE, "edited on the source");
    f.ui.answers = {"3DS"};
    CHECK(f.run());
    CHECK_EQ(f.ui.sidesOffered.size(), (size_t)1);
    CHECK(f.ui.sidesOffered[0] == std::vector<std::string>({"3DS", "C"}));
    CHECK_EQ(f.local.read(SAVE), std::string("edited on the 3DS"));
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);

    // Answered once: the lost edit is not raised again.
    f.clearCalls();
    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.local.read(SAVE), std::string("edited on the 3DS"));
}

TEST(nway, a_source_takes_no_part_in_an_upload_only_entry)
{
    Fixture f;
    f.readOnlyC = true;
    f.entry.direction = SYNC_UPLOAD_ONLY;
    f.local.write(SAVE, "v1", 1700000000);
    f.c.setRemote("/only-on-source.sav", "src");

    CHECK(f.run());
    CHECK_EQ(f.a.files[SAVE].content, std::string("v1"));
    CHECK_EQ(f.c.listCalls, 0);
    CHECK_EQ(f.c.uploaded.size(), (size_t)0);
    CHECK(!f.local.exists("/only-on-source.sav"));
}

TEST(nway, restoring_a_deleted_file_does_not_resend_it)
{
    Fixture f;
    f.syncedEverywhere("v1");
    std::string path = f.local.path() + SAVE;
    CHECK_EQ(remove(path.c_str()), 0);

    CHECK(f.run());
    CHECK_EQ(f.local.read(SAVE), std::string("v1"));
    CHECK_EQ(f.a.downloaded.size() + f.b.downloaded.size() + f.c.downloaded.size(), (size_t)1);
    CHECK_EQ(f.a.uploaded.size() + f.b.uploaded.size() + f.c.uploaded.size(), (size_t)0);
}

TEST(nway, a_fatal_error_on_one_remote_leaves_the_others_syncing)
{
    Fixture f;
    f.b.fatalOnList = true;
    f.local.write(SAVE, "v1", 1700000000);

    CHECK(f.run());
    CHECK(f.b.hasFatalError());
    CHECK_EQ(f.a.files[SAVE].content, std::string("v1"));
    CHECK_EQ(f.c.files[SAVE].content, std::string("v1"));
}

TEST(nway, gone_everywhere_clears_every_record)
{
    Fixture f;
    f.syncedEverywhere("v1");
    std::string path = f.local.path() + SAVE;
    CHECK_EQ(remove(path.c_str()), 0);
    f.a.files.clear();
    f.b.files.clear();
    f.c.files.clear();
    f.listSave = true;

    CHECK(f.run());
    CHECK(!f.manifest.has(f.localKey(SAVE)));
    CHECK(!f.manifest.has(f.key(f.a, SAVE)));
    CHECK(!f.manifest.has(f.key(f.c, SAVE)));
}

// ---------------------------------------------------------------------------
// Upgrading from a manifest the pairwise engine wrote
// ---------------------------------------------------------------------------
namespace
{

// The record the pairwise engine left after syncing the current local file.
void seedPairwise(Fixture &f, MockRemote &r)
{
    ManifestEntry rec = ManifestEntry();
    rec.localMtime = (time_t)f.local.mtimeOf(SAVE);
    rec.localSize = (long long)f.local.read(SAVE).size();
    rec.remoteTag = r.tagOf(SAVE);
    rec.remoteId = "id" + std::string(SAVE);
    f.manifest.set(f.key(r, SAVE), rec);
}

} // namespace

TEST(nway, upgrading_an_in_step_manifest_changes_nothing)
{
    Fixture f;
    f.local.write(SAVE, "v1", 1700000000);
    f.a.setRemote(SAVE, "v1");
    f.b.setRemote(SAVE, "v1");
    f.c.setRemote(SAVE, "v1");
    seedPairwise(f, f.a);
    seedPairwise(f, f.b);
    seedPairwise(f, f.c);

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.summary.uploaded + f.summary.downloaded, 0);
    CHECK_EQ(f.summary.checkedPaths.size(), (size_t)1);
    // ...and numbers what it found, so the fallback is needed only once.
    CHECK_EQ(f.manifest.get(f.localKey(SAVE)).generation, (long long)1);
    CHECK_EQ(f.manifest.get(f.key(f.b, SAVE)).generation, (long long)1);
}

TEST(nway, upgrading_with_a_local_change_uploads_it_everywhere)
{
    Fixture f;
    f.local.write(SAVE, "v1", 1700000000);
    f.a.setRemote(SAVE, "v1");
    f.b.setRemote(SAVE, "v1");
    seedPairwise(f, f.a);
    seedPairwise(f, f.b);
    f.useC = false;
    f.local.write(SAVE, "v2", 1700000900);

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)0);
    CHECK_EQ(f.a.files[SAVE].content, std::string("v2"));
    CHECK_EQ(f.b.files[SAVE].content, std::string("v2"));
}

TEST(nway, upgrading_keeps_a_pairwise_conflict_a_conflict)
{
    // B last synced an older local version (its record is stale) and has
    // changed since: the pairwise engine would have asked, and so does this.
    Fixture f;
    f.local.write(SAVE, "v1", 1700000000);
    f.b.setRemote(SAVE, "v1");
    seedPairwise(f, f.b);
    f.local.write(SAVE, "v2", 1700000900);
    f.a.setRemote(SAVE, "v2");
    seedPairwise(f, f.a);
    f.b.setRemote(SAVE, "edited on B");
    f.useC = false;
    f.ui.answers = {"B"};

    CHECK(f.run());
    CHECK_EQ(f.ui.sidesOffered.size(), (size_t)1);
    CHECK(f.ui.sidesOffered[0] == std::vector<std::string>({"3DS", "B"}));
    CHECK_EQ(f.local.read(SAVE), std::string("edited on B"));
    CHECK_EQ(f.a.files[SAVE].content, std::string("edited on B"));
}

TEST(nway, a_remote_name_that_leaves_the_folder_is_never_written)
{
    Fixture f;
    f.a.setRemote("/../escaped.sav", "evil");
    f.a.setRemote("/Game/../../boot.firm", "evil");
    f.a.setRemote("/ok.sav", "fine");

    CHECK(f.run());
    CHECK(f.local.exists("/ok.sav"));
    // Nothing one or two levels above the sync folder.
    std::string up = f.local.path() + "/../escaped.sav";
    CHECK(access(up.c_str(), F_OK) != 0);
    CHECK_EQ(f.a.downloaded.size(), (size_t)1);
    CHECK_EQ(f.summary.problems.size(), (size_t)1);
    CHECK(f.summary.problems[0].find("unsafe") != std::string::npos);
    // The other remotes are not sent it either.
    CHECK(f.b.files.find("/../escaped.sav") == f.b.files.end());
}
