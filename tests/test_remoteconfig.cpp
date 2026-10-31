// Which remotes 3DSync.ini configures: section names with and without a label,
// Role=, Only=, and the warnings for what the user most likely got wrong.

#include "framework.h"

#include "../source/sync/remoteconfig.h"

namespace
{

// INIReader's GetValues(): "section=key", both lowercased.
typedef std::map<std::string, std::string> Values;

const std::vector<std::string> ENTRIES = {"saves", "gba"};

std::string joined(const std::vector<std::string> &lines)
{
    std::string out;
    for (auto &l : lines)
        out += l + "\n";
    return out;
}

} // namespace

TEST(remoteconfig, an_unnamed_section_is_the_remote_it_always_was)
{
    Values v = {{"smb=server", "nas"}, {"smb=share", "3ds"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes.size(), (size_t)1);
    CHECK_EQ(r.remotes[0].type, REMOTE_SMB);
    CHECK_STR_EQ(r.remotes[0].section, "smb");
    CHECK_STR_EQ(r.remotes[0].label, "");
    CHECK_STR_EQ(r.remotes[0].displayName(), "SMB");
    CHECK_EQ(r.remotes[0].role, ROLE_SYNC);
    // Its manifest records are the ones it already has.
    CHECK_STR_EQ(manifestScope(r.remotes[0], "smb://nas/3ds"), "smb://nas/3ds");
    CHECK(r.warnings.empty());
}

TEST(remoteconfig, a_label_makes_a_second_remote_of_the_same_type)
{
    Values v = {{"smb=server", "nas"},        {"smb=share", "3ds"},
                {"smb:enc=server", "nas"},    {"smb:enc=share", "3ds-encrypted"},
                {"smb:backup=server", "nas2"}, {"smb:backup=share", "b"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes.size(), (size_t)3);
    // The unnamed one first, then the labels in order.
    CHECK_STR_EQ(r.remotes[0].displayName(), "SMB");
    CHECK_STR_EQ(r.remotes[1].displayName(), "SMB (backup)");
    CHECK_STR_EQ(r.remotes[2].displayName(), "SMB (enc)");
    // Same server, separate state.
    CHECK_STR_EQ(manifestScope(r.remotes[2], "smb://nas/3ds-encrypted"),
                 "smb://nas/3ds-encrypted#enc");
    CHECK_STR_EQ(remoteNames(r.remotes), "SMB, SMB (backup), SMB (enc)");
}

TEST(remoteconfig, every_type_is_recognised_in_connect_order)
{
    Values v = {{"webdav=url", "http://h/dav/"},
                {"ftp:x=host", "h"},
                {"smb=server", "nas"},
                {"smb=share", "s"},
                {"googledrive=refreshtoken", "t"},
                {"dropbox:work=refreshtoken", "t"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_STR_EQ(remoteNames(r.remotes), "Dropbox (work), Drive, SMB, FTP (x), WebDAV");
}

TEST(remoteconfig, an_incomplete_section_is_left_out_quietly)
{
    Values v = {{"smb:enc=server", "nas"}, {"ftp=port", "21"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes.empty());
    CHECK(r.warnings.empty());
    CHECK_EQ(r.incomplete.size(), (size_t)2);
}

TEST(remoteconfig, other_sections_are_not_remotes)
{
    Values v = {{"paths=saves", "/roms/saves"},
                {"filter=extensions", "sav"},
                {"security=sealcredentials", "false"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes.empty());
    CHECK(r.warnings.empty());
}

TEST(remoteconfig, role_backup_makes_a_sink)
{
    Values v = {{"ftp:backup=host", "h"}, {"ftp:backup=role", "Backup"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes[0].role, ROLE_BACKUP);
    CHECK(r.warnings.empty());
}

TEST(remoteconfig, an_unknown_role_syncs_both_ways_and_says_so)
{
    Values v = {{"ftp=host", "h"}, {"ftp=role", "mirror"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes[0].role, ROLE_SYNC);
    CHECK_EQ(r.warnings.size(), (size_t)1);
    CHECK(joined(r.warnings[0]).find("[ftp]") != std::string::npos);
}

TEST(remoteconfig, only_limits_the_entries)
{
    Values v = {{"webdav=url", "u"}, {"webdav=only", "Saves"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes[0].syncs("saves"));
    CHECK(r.remotes[0].syncs("SAVES"));
    CHECK(!r.remotes[0].syncs("gba"));
    CHECK(r.warnings.empty());
}

TEST(remoteconfig, only_splits_on_commas_and_whitespace)
{
    Values v = {{"webdav=url", "u"}, {"webdav=only", "saves,gba  saves"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes[0].only.size(), (size_t)2);
    CHECK(r.remotes[0].syncs("gba"));
}

TEST(remoteconfig, an_empty_only_means_every_entry)
{
    Values v = {{"webdav=url", "u"}, {"webdav=only", ""}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes[0].syncs("saves"));
    CHECK(r.remotes[0].syncs("anything"));
}

TEST(remoteconfig, only_naming_a_missing_path_is_a_warning)
{
    Values v = {{"webdav=url", "u"}, {"webdav=only", "saves, savs"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.warnings.size(), (size_t)1);
    CHECK(joined(r.warnings[0]).find("savs") != std::string::npos);
    // The remote still syncs what it names correctly.
    CHECK(r.remotes[0].syncs("saves"));
}

TEST(remoteconfig, unknown_and_empty_labelled_sections_are_warnings)
{
    Values v = {{"smbx:nas=server", "nas"}, {"smb:=server", "nas"}, {"smb:=share", "s"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes.empty());
    CHECK_EQ(r.warnings.size(), (size_t)2);
    // Listed for the summary too: a remote the user meant is not syncing.
    CHECK_EQ(r.ignored.size(), (size_t)2);
}

TEST(remoteconfig, warnings_fit_the_log)
{
    Values v = {{"smbx:nas=server", "nas"},
                {"smb:=server", "nas"},
                {"ftp=host", "h"},
                {"ftp=role", "mirror"},
                {"ftp=only", "nothere"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.warnings.size(), (size_t)4);
    for (auto &w : r.warnings)
        for (size_t i = 0; i < w.size(); i++)
        {
            CHECK(w[i].size() < 50);
            // The first line starts the message, the rest are indented.
            CHECK_EQ(w[i].compare(0, 2, "  ") == 0, i > 0);
        }
}
