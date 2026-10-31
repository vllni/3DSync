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
    CHECK(r.issues.empty());
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

TEST(remoteconfig, an_incomplete_section_is_an_issue_naming_the_key)
{
    Values v = {{"smb:enc=server", "nas"}, {"ftp=port", "21"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes.empty());
    CHECK_EQ(r.issues.size(), (size_t)2);
    // Section order: "ftp" before "smb:enc".
    CHECK_STR_EQ(r.issues[0].brief, "[ftp] needs Host=");
    CHECK_STR_EQ(r.issues[1].brief, "[smb:enc] needs Share=");
    CHECK(r.issues[0].leftOut && r.issues[1].leftOut);
    CHECK(joined(r.issues[1].lines).find("Add Share=") != std::string::npos);
}

TEST(remoteconfig, each_type_names_what_it_lacks)
{
    Values v = {{"smb=user", "u"},
                {"dropbox=appkey", "k"},
                {"googledrive:b=clientid", "c"},
                {"webdav=user", "u"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.issues.size(), (size_t)4);
    std::string all;
    for (auto &i : r.issues)
        all += i.brief + "|";
    CHECK_STR_EQ(all, "[dropbox] needs a login|[googledrive:b] needs a login|"
                      "[smb] needs Server=, Share=|[webdav] needs Url=|");
}

TEST(remoteconfig, other_sections_are_not_remotes)
{
    Values v = {{"paths=saves", "/roms/saves"},
                {"filter=extensions", "sav"},
                {"security=sealcredentials", "false"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes.empty());
    CHECK(r.issues.empty());
}

TEST(remoteconfig, role_backup_makes_a_sink)
{
    Values v = {{"ftp:backup=host", "h"}, {"ftp:backup=role", "Backup"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes[0].role, ROLE_BACKUP);
    CHECK(r.issues.empty());
}

TEST(remoteconfig, role_source_is_read_only)
{
    Values v = {{"webdav:pc=url", "https://h/"}, {"webdav:pc=role", "Source"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes[0].role, ROLE_SOURCE);
    CHECK(r.issues.empty());
}

TEST(remoteconfig, an_unknown_role_leaves_the_remote_out)
{
    // A misspelt "backup" must not become a remote that syncs both ways.
    Values v = {{"ftp=host", "h"}, {"ftp=role", "bakup"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes.empty());
    CHECK_EQ(r.issues.size(), (size_t)1);
    CHECK(joined(r.issues[0].lines).find("[ftp]") != std::string::npos);
    CHECK(r.issues[0].leftOut);
}

TEST(remoteconfig, certificates_are_checked_unless_webdav_or_ftp_opt_out)
{
    Values v = {{"webdav=url", "https://nas/dav/"}, {"webdav=verifytls", "False"},
                {"ftp=host", "h"},
                {"dropbox=refreshtoken", "t"}, {"dropbox=verifytls", "no"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes.size(), (size_t)3);
    for (auto &remote : r.remotes)
        CHECK_EQ(remote.verifyTls, remote.type != REMOTE_WEBDAV);
    // Dropbox cannot opt out, and is told so.
    CHECK_EQ(r.issues.size(), (size_t)1);
    CHECK_STR_EQ(r.issues[0].section, "dropbox");
    CHECK(!r.issues[0].leftOut);
}

TEST(remoteconfig, an_unknown_verifytls_keeps_checking)
{
    Values v = {{"webdav=url", "https://nas/dav/"}, {"webdav=verifytls", "nope"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes[0].verifyTls);
    CHECK_EQ(r.issues.size(), (size_t)1);
}

TEST(remoteconfig, ftp_tls_is_checked_and_a_typo_requires_tls)
{
    Values v = {{"ftp=host", "h"}, {"ftp:b=host", "h"}, {"ftp:b=tls", "Explicit"},
                {"ftp:c=host", "h"}, {"ftp:c=tls", "requier"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.remotes.size(), (size_t)3);
    CHECK_STR_EQ(r.remotes[0].ftpTls, "try");
    CHECK_STR_EQ(r.remotes[1].ftpTls, "require");
    CHECK_STR_EQ(r.remotes[2].ftpTls, "require");
    CHECK_EQ(r.issues.size(), (size_t)1);
    CHECK_STR_EQ(r.issues[0].section, "ftp:c");
}

TEST(remoteconfig, only_limits_the_entries)
{
    Values v = {{"webdav=url", "u"}, {"webdav=only", "Saves"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes[0].syncs("saves"));
    CHECK(r.remotes[0].syncs("SAVES"));
    CHECK(!r.remotes[0].syncs("gba"));
    CHECK(r.issues.empty());
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
    CHECK_EQ(r.issues.size(), (size_t)1);
    CHECK(joined(r.issues[0].lines).find("savs") != std::string::npos);
    CHECK(!r.issues[0].leftOut);
    // The remote still syncs what it names correctly.
    CHECK(r.remotes[0].syncs("saves"));
}

TEST(remoteconfig, unknown_and_empty_labelled_sections_are_issues)
{
    Values v = {{"smbx:nas=server", "nas"}, {"smb:=server", "nas"}, {"smb:=share", "s"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK(r.remotes.empty());
    CHECK_EQ(r.issues.size(), (size_t)2);
    // A remote the user meant is not syncing.
    CHECK(r.issues[0].leftOut && r.issues[1].leftOut);
}

TEST(remoteconfig, issues_fit_the_screens)
{
    Values v = {{"smbx:nas=server", "nas"},
                {"smb:=server", "nas"},
                {"smb:b=user", "u"},
                {"dropbox=appkey", "k"},
                {"webdav=user", "u"},
                {"ftp=host", "h"},
                {"ftp=only", "nothere"},
                {"ftp=tls", "maybe"},
                {"ftp=verifytls", "perhaps"},
                {"ftp:b=host", "h"},
                {"ftp:b=role", "mirror"},
                {"googledrive:b=refreshtoken", "t"},
                {"googledrive:b=verifytls", "false"}};
    RemoteConfigs r = parseRemotes(v, ENTRIES);
    CHECK_EQ(r.issues.size(), (size_t)10);
    for (auto &issue : r.issues)
    {
        // Short labels stay on one line of the bottom screen's list.
        CHECK(issue.brief.size() <= 40);
        for (size_t i = 0; i < issue.lines.size(); i++)
        {
            CHECK(issue.lines[i].size() < 50);
            // The first line starts the message, the rest are indented.
            CHECK_EQ(issue.lines[i].compare(0, 2, "  ") == 0, i > 0);
        }
    }
}
