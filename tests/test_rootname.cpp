// The remote folder of a [Paths] entry, spelled as 3DSync.ini has it (#71).
// Earlier versions lowercased it, so a remote that tells names apart by case
// may hold the folder under that spelling: the engine falls back to it only
// when the folder as written is missing, and creates nothing while looking.

#include "framework.h"
#include "mocks.h"

#include "../source/libs/inih/INIReader/INIReader.h"
#include "../source/utils/pathutil.h"

namespace
{

struct Fixture
{
    TempDir local;
    MockRemote remote;
    MockUi ui;
    Manifest manifest;
    SyncSummary summary;
    SyncEntry entry;

    explicit Fixture(const std::string &remoteName)
        : manifest(std::string("/tmp/3dsync-tests-manifest-unused.json"))
    {
        entry.localBase = local.path();
        entry.remoteName = remoteName;
        entry.name = asciiLower(remoteName);
        entry.direction = SYNC_BOTH;
    }

    bool run()
    {
        std::vector<Participant> parts;
        parts.push_back(Participant(remote, "R", remote.prefix));
        return syncEntry(parts, manifest, entry, summary, ui);
    }
};

} // namespace

TEST(rootname, as_written_folder_is_used)
{
    Fixture f("GBA");
    f.remote.folders.insert("GBA");
    f.remote.folders.insert("gba");
    CHECK(f.run());
    CHECK_EQ(f.remote.probedRoots.size(), (size_t)1);
    CHECK(f.remote.ensuredRoots.empty());
    CHECK_EQ(f.remote.listedRoots.size(), (size_t)1);
    CHECK_STR_EQ(f.remote.listedRoots[0], "/root/GBA");
}

TEST(rootname, lowercase_folder_of_an_old_setup_is_kept)
{
    Fixture f("GBA");
    f.remote.folders.insert("gba");
    CHECK(f.run());
    CHECK_EQ(f.remote.probedRoots.size(), (size_t)2);
    CHECK_STR_EQ(f.remote.probedRoots[1], "gba");
    // Nothing created: neither GBA nor gba.
    CHECK(f.remote.ensuredRoots.empty());
    CHECK_STR_EQ(f.remote.listedRoots[0], "/root/gba");
    CHECK(f.summary.problems.empty());
}

TEST(rootname, neither_folder_creates_the_one_as_written)
{
    Fixture f("Checkpoint/Saves");
    CHECK(f.run());
    CHECK_EQ(f.remote.probedRoots.size(), (size_t)2);
    CHECK_EQ(f.remote.ensuredRoots.size(), (size_t)1);
    CHECK_STR_EQ(f.remote.ensuredRoots[0], "Checkpoint/Saves");
    CHECK_STR_EQ(f.remote.listedRoots[0], "/root/Checkpoint/Saves");
}

TEST(rootname, lowercase_name_is_not_probed)
{
    Fixture f("saves");
    CHECK(f.run());
    CHECK(f.remote.probedRoots.empty());
    CHECK_EQ(f.remote.ensuredRoots.size(), (size_t)1);
    CHECK_STR_EQ(f.remote.ensuredRoots[0], "saves");
}

TEST(rootname, fatal_error_while_probing_creates_nothing)
{
    Fixture f("GBA");
    f.remote.fatalOnFindRoot = true;
    f.run();
    CHECK_EQ(f.remote.probedRoots.size(), (size_t)1);
    CHECK(f.remote.ensuredRoots.empty());
    CHECK(f.remote.listedRoots.empty());
}

TEST(rootname, ini_keeps_the_spelling_of_a_name)
{
    const char ini[] = "[Paths]\nGBA = /roms/gba\nsaves=/saves\n[Paths]\ngba=/again\n";
    INIReader reader(ini, sizeof(ini) - 1);
    CHECK_EQ(reader.ParseError(), 0);
    // Lookups stay case-insensitive...
    CHECK_STR_EQ(reader.Get("paths", "gba", ""), "/roms/gba\n/again");
    // ...and the first spelling is kept for the folder.
    CHECK_STR_EQ(reader.GetSpelling("paths=gba"), "GBA");
    CHECK_STR_EQ(reader.GetSpelling("paths=saves"), "saves");
    // A key the file never had reads lowercased.
    CHECK_STR_EQ(reader.GetSpelling("paths=other"), "other");
}

TEST(rootname, ascii_lower)
{
    CHECK_STR_EQ(asciiLower("Roms/GBA"), "roms/gba");
    // Non-ASCII bytes are left as they are, as INIReader leaves them.
    CHECK_STR_EQ(asciiLower("Pok\xc3\xa9mon"), "pok\xc3\xa9mon");
}
