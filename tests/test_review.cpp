// A review-first sync: planEntry() lists and decides every file, asking every
// conflict, without transferring anything or writing the manifest; then
// applyEntryPlan() carries the plans out.  What it ends with must be what a
// normal sync of the same state ends with.

#include "framework.h"
#include "mocks.h"

#include "../source/utils/pathutil.h"

namespace
{

const char *UP = "/up.sav";             // only on the 3DS: uploaded
const char *DOWN = "/down.sav";         // only on the remote: downloaded
const char *BOTH = "/both.sav";         // on both, no record: a conflict
const char *MANIFEST_UNUSED = "/tmp/3dsync-tests-manifest-unused.json";

struct Fixture
{
    TempDir local;
    MockRemote remote;
    MockUi ui;
    Manifest manifest;
    SyncSummary summary;
    SyncEntry entry;
    std::vector<Participant> parts;

    Fixture() : manifest(std::string(MANIFEST_UNUSED))
    {
        entry.localBase = local.path();
        entry.remoteName = "saves";
        entry.direction = SYNC_BOTH;
        parts.push_back(Participant(remote, "R", remote.prefix));

        local.write(UP, "up", 1700000000);
        remote.setRemote(DOWN, "down");
        local.write(BOTH, "3ds copy", 1700000000);
        remote.setRemote(BOTH, "remote copy");
        entry.localFiles = {UP, BOTH};
        ui.answers = {"R"};
    }

    std::string key(const char *relPath) const
    {
        return remote.manifestPrefix() + "|" + local.path() + relPath;
    }

    bool plan(EntryPlan &p) { return planEntry(parts, manifest, entry, p, summary, ui); }
};

} // namespace

TEST(review, planning_transfers_nothing_and_writes_no_manifest)
{
    Fixture f;
    EntryPlan plan;
    CHECK(f.plan(plan));

    // Every question first...
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    // ...and nothing done yet.
    CHECK(f.remote.uploaded.empty());
    CHECK(f.remote.downloaded.empty());
    CHECK(!f.local.exists(DOWN));
    CHECK(!f.manifest.has(f.key(UP)));
    CHECK(!f.manifest.has(f.key(DOWN)));
    CHECK(!f.manifest.has(f.key(BOTH)));

    PlanCounts counts = countPlan(plan);
    CHECK_EQ(counts.downloads, 2); // DOWN, and BOTH from the remote
    CHECK_EQ(counts.uploads, 1);   // UP

    applyEntryPlan(plan, f.manifest, f.summary, f.ui);
    CHECK_EQ(f.remote.uploaded.size(), (size_t)1);
    CHECK_EQ(f.remote.downloaded.size(), (size_t)2);
    CHECK_STR_EQ(f.local.read(DOWN), "down");
    CHECK_STR_EQ(f.local.read(BOTH), "remote copy");
    CHECK_STR_EQ(f.remote.files[UP].content, "up");
    CHECK(f.manifest.has(f.key(UP)));
    CHECK(f.manifest.has(f.key(DOWN)));
    CHECK(f.manifest.has(f.key(BOTH)));
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1); // nothing asked while applying
}

TEST(review, it_ends_where_a_normal_sync_ends)
{
    Fixture reviewed, normal;
    EntryPlan plan;
    CHECK(reviewed.plan(plan));
    applyEntryPlan(plan, reviewed.manifest, reviewed.summary, reviewed.ui);
    CHECK(syncEntry(normal.parts, normal.manifest, normal.entry, normal.summary, normal.ui));

    const char *files[] = {UP, DOWN, BOTH};
    for (const char *file : files)
    {
        CHECK_STR_EQ(reviewed.local.read(file), normal.local.read(file));
        CHECK_STR_EQ(reviewed.remote.files[file].content, normal.remote.files[file].content);
        CHECK_EQ(reviewed.manifest.has(reviewed.key(file)), normal.manifest.has(normal.key(file)));
        CHECK_EQ(reviewed.manifest.get(reviewed.key(file)).generation,
                 normal.manifest.get(normal.key(file)).generation);
    }
    CHECK_EQ(reviewed.summary.uploaded, normal.summary.uploaded);
    CHECK_EQ(reviewed.summary.downloaded, normal.summary.downloaded);
    CHECK_EQ(reviewed.summary.changes.size(), normal.summary.changes.size());

    // And the next normal sync has nothing left to do.
    reviewed.remote.uploaded.clear();
    reviewed.remote.downloaded.clear();
    reviewed.entry.localFiles = {UP, DOWN, BOTH};
    reviewed.summary = SyncSummary();
    CHECK(syncEntry(reviewed.parts, reviewed.manifest, reviewed.entry, reviewed.summary,
                    reviewed.ui));
    CHECK(reviewed.remote.uploaded.empty());
    CHECK(reviewed.remote.downloaded.empty());
    CHECK_EQ(reviewed.ui.conflictsRaised.size(), (size_t)1);
}

TEST(review, a_skipped_conflict_is_left_alone)
{
    Fixture f;
    f.ui.answers = {"skip"};
    EntryPlan plan;
    CHECK(f.plan(plan));
    applyEntryPlan(plan, f.manifest, f.summary, f.ui);

    CHECK_STR_EQ(f.local.read(BOTH), "3ds copy");
    CHECK_STR_EQ(f.remote.files[BOTH].content, "remote copy");
    CHECK(!f.manifest.has(f.key(BOTH)));
    CHECK(f.manifest.has(f.key(UP))); // the rest still happened
}

TEST(review, cancelling_at_a_conflict_changes_nothing)
{
    Fixture f;
    f.ui.answers = {"cancel"};
    EntryPlan plan;
    CHECK(!f.plan(plan));
    CHECK(f.remote.uploaded.empty());
    CHECK(f.remote.downloaded.empty());
    CHECK(!f.manifest.has(f.key(UP)));
}

TEST(review, a_save_changed_after_the_review_is_left_alone)
{
    Fixture f;
    EntryPlan plan;
    CHECK(f.plan(plan));
    // Played on the 3DS between the review and the transfers.
    f.local.write(UP, "played since", 1700000900);
    applyEntryPlan(plan, f.manifest, f.summary, f.ui);

    CHECK(f.remote.files.find(UP) == f.remote.files.end()); // not uploaded
    CHECK(!f.manifest.has(f.key(UP)));
    CHECK_EQ(f.summary.problems.size(), (size_t)1);
    CHECK_STR_EQ(f.local.read(DOWN), "down"); // the rest still happened
}

TEST(review, a_copy_compared_while_planning_is_not_downloaded_again)
{
    Fixture f;
    f.entry.compareUpTo = 1024;
    f.remote.setRemote(BOTH, "3ds kopy"); // the size of "3ds copy", so it is compared
    EntryPlan plan;
    CHECK(f.plan(plan));
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1); // the comparison
    CHECK(f.local.exists(std::string(BOTH) + ".compare0" + TEMP_SUFFIX));

    applyEntryPlan(plan, f.manifest, f.summary, f.ui);
    CHECK_STR_EQ(f.local.read(BOTH), "3ds kopy");
    CHECK_EQ(f.remote.downloaded.size(), (size_t)2); // plus DOWN, not BOTH again
    CHECK(!f.local.exists(std::string(BOTH) + ".compare0" + TEMP_SUFFIX));
}

TEST(review, an_unused_compared_copy_goes_with_the_plan)
{
    Fixture f;
    f.entry.compareUpTo = 1024;
    f.remote.setRemote(BOTH, "3ds kopy");
    {
        EntryPlan plan;
        CHECK(f.plan(plan));
        CHECK(f.local.exists(std::string(BOTH) + ".compare0" + TEMP_SUFFIX));
        // Cancelled at the review: never applied.
    }
    CHECK(!f.local.exists(std::string(BOTH) + ".compare0" + TEMP_SUFFIX));
}

TEST(review, every_entrys_questions_come_before_any_transfer)
{
    TempDir saves, shots;
    MockRemote remote;
    MockUi ui;
    ui.answers = {"3DS"};
    Manifest manifest{std::string(MANIFEST_UNUSED)};
    SyncSummary summary;

    SyncEntry first, second;
    first.localBase = saves.path();
    first.remoteName = "saves";
    first.direction = SYNC_BOTH;
    second.localBase = shots.path();
    second.remoteName = "shots";
    second.direction = SYNC_BOTH;
    saves.write("/a.sav", "3ds a", 1700000000);
    shots.write("/b.png", "3ds b", 1700000000);
    first.localFiles = {"/a.sav"};
    second.localFiles = {"/b.png"};
    remote.setRemote("/a.sav", "remote a"); // the mock keys by relPath
    remote.setRemote("/b.png", "remote b");

    std::vector<Participant> firstParts, secondParts;
    firstParts.push_back(Participant(remote, "R", remote.prefix));
    secondParts.push_back(Participant(remote, "R", remote.prefix));
    EntryPlan firstPlan, secondPlan;
    CHECK(planEntry(firstParts, manifest, first, firstPlan, summary, ui));
    CHECK(planEntry(secondParts, manifest, second, secondPlan, summary, ui));
    CHECK_EQ(ui.conflictsRaised.size(), (size_t)2);
    CHECK(remote.uploaded.empty());

    applyEntryPlan(firstPlan, manifest, summary, ui);
    applyEntryPlan(secondPlan, manifest, summary, ui);
    CHECK_EQ(remote.uploaded.size(), (size_t)2);
    CHECK_EQ(ui.conflictsRaised.size(), (size_t)2);
}

TEST(review, the_list_names_each_transfer)
{
    Fixture f;
    EntryPlan plan;
    CHECK(f.plan(plan));
    std::vector<std::string> lines = planLines(plan);

    std::string all;
    for (auto &line : lines)
        all += line + "\n";
    CHECK_STR_EQ(all, "  /both.sav\n    from R\n  /down.sav\n    from R\n  /up.sav\n    to R\n");
    for (auto &line : lines)
        if (line.compare(0, 4, "    ") == 0) // the path lines may be any length
            CHECK(line.size() < 50);
}

TEST(review, nothing_to_do_lists_nothing)
{
    TempDir local;
    MockRemote remote;
    MockUi ui;
    Manifest manifest{std::string(MANIFEST_UNUSED)};
    SyncSummary summary;
    SyncEntry entry;
    entry.localBase = local.path();
    entry.remoteName = "saves";
    entry.direction = SYNC_BOTH;
    std::vector<Participant> parts;
    parts.push_back(Participant(remote, "R", remote.prefix));

    EntryPlan plan;
    CHECK(planEntry(parts, manifest, entry, plan, summary, ui));
    CHECK(planLines(plan).empty());
    CHECK_EQ(countPlan(plan).downloads, 0);
    CHECK_EQ(countPlan(plan).uploads, 0);
}
