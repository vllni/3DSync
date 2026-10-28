// [Filter] Extensions=: how the list is read and which names it lets through.

#include "framework.h"

#include "../source/sync/extfilter.h"

TEST(extfilter, empty_allows_everything)
{
    ExtensionFilter f = ExtensionFilter::parse("");
    CHECK(f.allowsAll());
    CHECK(f.matches("/Game/001.sav"));
    CHECK(f.matches("/Game/noextension"));
    CHECK_STR_EQ(f.describe(), "all files");

    CHECK(ExtensionFilter::parse("  , ,").allowsAll());
    CHECK(ExtensionFilter().allowsAll());
}

TEST(extfilter, star_means_everything_whatever_else_is_listed)
{
    CHECK(ExtensionFilter::parse("*").allowsAll());
    CHECK(ExtensionFilter::parse("*.*").allowsAll());
    CHECK(ExtensionFilter::parse("sav, *").allowsAll());
}

TEST(extfilter, dot_and_star_prefixes_are_the_same_request)
{
    ExtensionFilter f = ExtensionFilter::parse("sav .bin *.dat");
    CHECK_EQ(f.extensions().size(), (size_t)3);
    CHECK_STR_EQ(f.describe(), ".sav .bin .dat");
    CHECK(f.matches("/a.sav"));
    CHECK(f.matches("/a.bin"));
    CHECK(f.matches("/a.dat"));
}

TEST(extfilter, commas_and_whitespace_separate_and_repeats_collapse)
{
    ExtensionFilter f = ExtensionFilter::parse(" sav,bin ,\tSAV,  .sav ");
    CHECK_EQ(f.extensions().size(), (size_t)2);
    CHECK_STR_EQ(f.describe(), ".sav .bin");
}

TEST(extfilter, matching_ignores_case)
{
    ExtensionFilter f = ExtensionFilter::parse("SaV");
    CHECK(f.matches("/Game/001.sav"));
    CHECK(f.matches("/Game/001.SAV"));
    CHECK_STR_EQ(f.describe(), ".sav");
}

TEST(extfilter, other_extensions_and_bare_names_are_left_out)
{
    ExtensionFilter f = ExtensionFilter::parse("sav");
    CHECK(!f.matches("/Game/001.bin"));
    CHECK(!f.matches("/Game/sav"));
    CHECK(!f.matches("/Game/001.save"));
    CHECK(!f.matches("/Game/001.sav.bak"));
    CHECK(!f.matches("/Game/001xsav"));
}

TEST(extfilter, only_the_file_name_is_looked_at)
{
    ExtensionFilter f = ExtensionFilter::parse("sav");
    // A folder named like a file does not let its contents through.
    CHECK(!f.matches("/backup.sav/notes.txt"));
    CHECK(f.matches("/backup.sav/001.sav"));
    CHECK(f.matches("001.sav"));
}

TEST(extfilter, a_hidden_file_is_not_its_own_extension)
{
    ExtensionFilter f = ExtensionFilter::parse("sav");
    CHECK(!f.matches("/Game/.sav"));
}

TEST(extfilter, multi_part_extensions_match_the_whole_ending)
{
    ExtensionFilter f = ExtensionFilter::parse("tar.gz");
    CHECK(f.matches("/backup.tar.gz"));
    CHECK(!f.matches("/backup.gz"));
    CHECK(!f.matches("/backuptar.gz"));
}
