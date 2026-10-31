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

TEST(extfilter, space_after_a_comma_is_optional)
{
    const char *specs[] = {"sav,bin,srm", "sav, bin, srm", "sav, bin,srm", "sav ,bin , srm",
                           "sav bin srm", "sav,,bin,srm,"};
    for (const char *spec : specs)
    {
        ExtensionFilter f = ExtensionFilter::parse(spec);
        CHECK_STR_EQ(f.describe(), ".sav .bin .srm");
        CHECK(f.matches("/Game/main.srm"));
    }
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

// [Filter] as a whole: a default and lines naming one path each.
namespace
{
typedef std::map<std::string, std::string> Values;
const std::vector<std::string> NAMES = {"saves", "roms/nds/saves", "screenshots"};
} // namespace

TEST(pathfilters, without_a_filter_section_every_path_takes_all_files)
{
    PathFilters f = parsePathFilters(Values{{"paths=saves", "/s"}}, NAMES);
    CHECK(f.forEntry("saves").allowsAll());
    CHECK(f.unknown.empty());
    CHECK_STR_EQ(f.describe(), "all files");
}

TEST(pathfilters, extensions_is_the_default_for_every_path)
{
    PathFilters f = parsePathFilters(Values{{"filter=extensions", "sav, bin"}}, NAMES);
    CHECK(f.forEntry("screenshots").matches("/a.sav"));
    CHECK(!f.forEntry("screenshots").matches("/a.png"));
    CHECK_STR_EQ(f.describe(), ".sav .bin");
}

TEST(pathfilters, a_path_line_replaces_the_default_for_that_path)
{
    PathFilters f = parsePathFilters(Values{{"filter=extensions", "sav"},
                                            {"filter=roms/nds/saves", "dsv"},
                                            {"filter=screenshots", "*"}},
                                     NAMES);
    CHECK(f.forEntry("roms/nds/saves").matches("/game.dsv"));
    CHECK(!f.forEntry("roms/nds/saves").matches("/game.sav"));
    CHECK(f.forEntry("screenshots").allowsAll());
    CHECK(f.forEntry("saves").matches("/game.sav"));
    CHECK(!f.forEntry("saves").matches("/game.dsv"));
    CHECK_STR_EQ(f.describe(), "per path");
}

TEST(pathfilters, path_names_are_compared_without_case)
{
    PathFilters f = parsePathFilters(Values{{"filter=saves", "sav"}}, {"Saves"});
    CHECK(f.unknown.empty());
    CHECK(!f.forEntry("SAVES").matches("/a.bin"));
}

TEST(pathfilters, a_line_naming_no_path_is_reported_and_ignored)
{
    PathFilters f = parsePathFilters(Values{{"filter=save", "sav"}}, NAMES);
    CHECK_EQ(f.unknown.size(), (size_t)1);
    CHECK_STR_EQ(f.unknown[0], "save");
    CHECK(f.forEntry("saves").allowsAll());
}
