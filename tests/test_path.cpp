// Remote path shapes.  Four backends used to normalise paths by hand, each
// slightly differently; these pin down the one shared helper.

#include "framework.h"

#include "../source/utils/pathutil.h"

TEST(path, strips_and_collapses_separators)
{
    CHECK_STR_EQ(normalizeRemotePath("Checkpoint/saves"), "Checkpoint/saves");
    CHECK_STR_EQ(normalizeRemotePath("/Checkpoint/saves"), "Checkpoint/saves");
    CHECK_STR_EQ(normalizeRemotePath("Checkpoint/saves/"), "Checkpoint/saves");
    CHECK_STR_EQ(normalizeRemotePath("///Checkpoint//saves///"), "Checkpoint/saves");
}

TEST(path, converts_windows_separators)
{
    // SMB shares are often configured with backslashes.
    CHECK_STR_EQ(normalizeRemotePath("\\Checkpoint\\saves"), "Checkpoint/saves");
    CHECK_STR_EQ(normalizeRemotePath("Checkpoint\\\\saves"), "Checkpoint/saves");
}

TEST(path, empty_and_root_stay_empty)
{
    // Every remote spells its own root as the empty string.
    CHECK_STR_EQ(normalizeRemotePath(""), "");
    CHECK_STR_EQ(normalizeRemotePath("/"), "");
    CHECK_STR_EQ(normalizeRemotePath("///"), "");
    CHECK_STR_EQ(normalizeRemotePath("", true), "");
    CHECK_STR_EQ(normalizeRemotePath("/", true), "");
}

TEST(path, leading_slash_is_opt_in)
{
    // Dropbox and WebDAV want "/a/b"; SMB and FTP want "a/b".
    CHECK_STR_EQ(normalizeRemotePath("a/b", true), "/a/b");
    CHECK_STR_EQ(normalizeRemotePath("/a/b/", true), "/a/b");
    CHECK_STR_EQ(normalizeRemotePath("a/b", false), "a/b");
}

TEST(path, keeps_inner_dots_and_spaces)
{
    CHECK_STR_EQ(normalizeRemotePath("/My Games/Pokémon X/001.sav", true),
                 "/My Games/Pokémon X/001.sav");
}

TEST(path, recognises_in_flight_transfers)
{
    CHECK(isTempTransferName("/Game/001.sav.3dstmp"));
    CHECK(isTempTransferName("x.3dstmp"));
    CHECK(!isTempTransferName("/Game/001.sav"));
    CHECK(!isTempTransferName("/Game/3dstmp"));
    // Exactly the suffix and nothing else is a real file called ".3dstmp".
    CHECK(!isTempTransferName(".3dstmp"));
    CHECK(!isTempTransferName(""));
}
