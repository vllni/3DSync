// A conflict with a remote that has no content hash (SMB, FTP, WebDAV) is
// checked against the bytes before anyone is asked: within [Sync]
// CompareUpTo= the remote copy is downloaded and compared with the 3DS's.

#include "framework.h"
#include "mocks.h"

#include "../source/sync/remoteconfig.h"
#include "../source/utils/fsutil.h"
#include "../source/utils/pathutil.h"

namespace
{

const char *SAVE = "/Game/001.sav";

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
        entry.remoteName = "saves";
        entry.direction = SYNC_BOTH;
        entry.compareUpTo = 1024;
        entry.localFiles.push_back(SAVE);
        ui.answers = {"skip"};
    }

    bool run()
    {
        summary = SyncSummary();
        return performSync(remote, manifest, entry, summary, false, ui);
    }

    // First sync with a copy on both sides: a conflict unless they match.
    void bothHave(const std::string &onThe3ds, const std::string &onTheRemote)
    {
        local.write(SAVE, onThe3ds, 1700000000);
        remote.setRemote(SAVE, onTheRemote);
    }

    bool leftATempFile() const { return local.exists(std::string(SAVE) + ".compare0" + TEMP_SUFFIX); }
};

} // namespace

TEST(compare, the_same_bytes_are_no_conflict)
{
    Fixture f;
    f.bothHave("same save", "same save");

    CHECK(f.run());
    CHECK(f.ui.conflictsRaised.empty());
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1); // the comparison, and nothing else
    CHECK(f.remote.uploaded.empty());
    CHECK(!f.leftATempFile());
    CHECK_STR_EQ(f.local.read(SAVE), "same save");

    // Recorded as in step: the next run has nothing to compare or copy.
    f.remote.downloaded.clear();
    CHECK(f.run());
    CHECK(f.ui.conflictsRaised.empty());
    CHECK(f.remote.downloaded.empty());
    CHECK(f.remote.uploaded.empty());
}

TEST(compare, different_bytes_of_the_same_size_are_asked_about)
{
    Fixture f;
    f.bothHave("save A", "save B");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1);
    CHECK(!f.leftATempFile());
    CHECK_STR_EQ(f.local.read(SAVE), "save A"); // skipped: nothing replaced
}

TEST(compare, the_compared_copy_is_used_when_the_remote_wins)
{
    Fixture f;
    f.bothHave("save A", "save B");
    f.ui.answers = {"Mock"};

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1); // fetched once, to compare
    CHECK_STR_EQ(f.local.read(SAVE), "save B");
    CHECK_EQ(f.summary.downloaded, 1);
    CHECK(!f.leftATempFile());

    // In step afterwards.
    f.remote.downloaded.clear();
    f.ui.conflictsRaised.clear();
    CHECK(f.run());
    CHECK(f.ui.conflictsRaised.empty());
    CHECK(f.remote.downloaded.empty());
}

TEST(compare, the_compared_copy_is_dropped_when_the_3ds_wins)
{
    Fixture f;
    f.bothHave("save A", "save B");
    f.ui.answers = {"3DS"};

    CHECK(f.run());
    CHECK_STR_EQ(f.local.read(SAVE), "save A");
    CHECK_STR_EQ(f.remote.files[SAVE].content, "save A");
    CHECK(!f.leftATempFile());
}

TEST(compare, a_different_size_needs_no_download)
{
    Fixture f;
    f.bothHave("short", "a longer save");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK(f.remote.downloaded.empty());
}

TEST(compare, without_a_size_the_bytes_decide)
{
    Fixture f;
    f.remote.reportSizes = false;
    f.bothHave("same save", "same save");

    CHECK(f.run());
    CHECK(f.ui.conflictsRaised.empty());
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1);
}

TEST(compare, a_file_over_the_limit_is_asked_about)
{
    Fixture f;
    f.entry.compareUpTo = 4;
    f.bothHave("same save", "same save");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK(f.remote.downloaded.empty());
}

TEST(compare, no_limit_compares_any_size)
{
    Fixture f;
    f.entry.compareUpTo = COMPARE_UP_TO_UNLIMITED;
    std::string big(100000, 's');
    f.bothHave(big, big);

    CHECK(f.run());
    CHECK(f.ui.conflictsRaised.empty());
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1);
}

TEST(compare, an_entry_without_a_limit_set_never_compares)
{
    Fixture f;
    f.entry.compareUpTo = 0;
    f.bothHave("same save", "same save");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK(f.remote.downloaded.empty());
}

TEST(compare, a_content_hash_is_trusted_without_a_download)
{
    Fixture f;
    f.remote.contentTags = true;
    f.bothHave("save A", "save B");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK(f.remote.downloaded.empty());
}

TEST(compare, a_failed_download_is_asked_about)
{
    Fixture f;
    f.remote.failDownloads = true;
    f.bothHave("same save", "same save");

    CHECK(f.run());
    CHECK_EQ(f.ui.conflictsRaised.size(), (size_t)1);
    CHECK(!f.leftATempFile());
}

TEST(compare, both_changed_to_the_same_bytes_after_a_sync)
{
    Fixture f;
    f.bothHave("old", "old");
    f.entry.compareUpTo = 0;
    f.ui.answers = {"3DS"};
    CHECK(f.run()); // in step from here on
    f.entry.compareUpTo = 1024;
    f.ui.conflictsRaised.clear();
    f.remote.downloaded.clear();
    f.remote.uploaded.clear();

    // The same edit reached both sides separately, e.g. copied by hand.
    f.local.write(SAVE, "new save", 1700000900);
    f.remote.setRemote(SAVE, "new save");

    CHECK(f.run());
    CHECK(f.ui.conflictsRaised.empty());
    CHECK(f.remote.uploaded.empty());
    CHECK_EQ(f.remote.downloaded.size(), (size_t)1);
}

TEST(compare, compare_files)
{
    TempDir dir;
    dir.write("/a", "abc", 1700000000);
    dir.write("/b", "abc", 1700000000);
    dir.write("/c", "abd", 1700000000);
    dir.write("/d", "abcd", 1700000000);
    dir.write("/e", "", 1700000000);
    dir.write("/f", "", 1700000000);
    std::string big(40000, 'x'), big2 = big;
    big2[39999] = 'y';
    dir.write("/g", big, 1700000000);
    dir.write("/h", big, 1700000000);
    dir.write("/i", big2, 1700000000);
    std::string p = dir.path();
    CHECK_EQ(compareFiles(p + "/a", p + "/b"), 1);
    CHECK_EQ(compareFiles(p + "/a", p + "/c"), 0);
    CHECK_EQ(compareFiles(p + "/a", p + "/d"), 0);
    CHECK_EQ(compareFiles(p + "/d", p + "/a"), 0);
    CHECK_EQ(compareFiles(p + "/e", p + "/f"), 1);
    CHECK_EQ(compareFiles(p + "/g", p + "/h"), 1); // past one read chunk
    CHECK_EQ(compareFiles(p + "/g", p + "/i"), 0);
    CHECK_EQ(compareFiles(p + "/a", p + "/missing"), -1);
}

TEST(compare, compare_conflicts_setting)
{
    bool on = false;
    CHECK(parseCompareConflicts("", on));
    CHECK(on);
    CHECK(parseCompareConflicts("false", on));
    CHECK(!on);
    CHECK(parseCompareConflicts(" Off ", on));
    CHECK(!on);
    CHECK(parseCompareConflicts("0", on));
    CHECK(!on);
    CHECK(parseCompareConflicts("YES", on));
    CHECK(on);
    CHECK(!parseCompareConflicts("never", on));
    CHECK(on); // the default until it is fixed
}

TEST(compare, compare_up_to_setting)
{
    long long bytes = 0;
    CHECK(parseCompareUpTo("", bytes));
    CHECK_EQ(bytes, 4096LL * 1024);
    CHECK(parseCompareUpTo(" 512 ", bytes));
    CHECK_EQ(bytes, 512LL * 1024);
    CHECK(parseCompareUpTo("0", bytes));
    CHECK_EQ(bytes, COMPARE_UP_TO_UNLIMITED);
    CHECK(!parseCompareUpTo("4MB", bytes));
    CHECK_EQ(bytes, 4096LL * 1024); // the default until it is fixed
    CHECK(!parseCompareUpTo("-1", bytes));
    CHECK(!parseCompareUpTo("1.5", bytes));
    CHECK(!parseCompareUpTo("99999999999999", bytes));
}

TEST(compare, with_several_remotes_only_the_winners_copy_is_kept)
{
    TempDir local;
    MockRemote a, b;
    a.prefix = "a";
    b.prefix = "b";
    MockUi ui;
    ui.answers = {"B"};
    Manifest manifest(std::string("/tmp/3dsync-tests-manifest-unused.json"));
    SyncSummary summary;
    SyncEntry entry;
    entry.localBase = local.path();
    entry.remoteName = "saves";
    entry.direction = SYNC_BOTH;
    entry.compareUpTo = 1024;
    entry.localFiles.push_back(SAVE);
    local.write(SAVE, "3ds", 1700000000);
    a.setRemote(SAVE, "aaa");
    b.setRemote(SAVE, "bbb");

    std::vector<Participant> parts;
    parts.push_back(Participant(a, "A", a.prefix));
    parts.push_back(Participant(b, "B", b.prefix));
    CHECK(syncEntry(parts, manifest, entry, summary, ui));

    CHECK_EQ(ui.conflictsRaised.size(), (size_t)1);
    CHECK_EQ(a.downloaded.size(), (size_t)1); // compared
    CHECK_EQ(b.downloaded.size(), (size_t)1); // compared, then used
    CHECK_STR_EQ(local.read(SAVE), "bbb");
    CHECK_STR_EQ(a.files[SAVE].content, "bbb");
    for (int i = 0; i < 3; i++)
    {
        char name[48];
        snprintf(name, sizeof(name), "%s.compare%d%s", SAVE, i, TEMP_SUFFIX);
        CHECK(!local.exists(name));
    }
}
