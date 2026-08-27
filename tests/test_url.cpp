// Percent-encoding for the FTP and WebDAV URLs.  Save folders are named after
// game titles, so spaces and non-ASCII are the normal case.

#include "framework.h"

#include "../source/utils/urlutil.h"

TEST(url, keeps_unreserved_characters_and_separators)
{
    CHECK_STR_EQ(urlEncodePath("Checkpoint/saves/001.sav"), "Checkpoint/saves/001.sav");
    CHECK_STR_EQ(urlEncodePath("a-b_c.d~e"), "a-b_c.d~e");
}

TEST(url, encodes_spaces_and_reserved_characters)
{
    CHECK_STR_EQ(urlEncodePath("My Games/save 1.sav"), "My%20Games/save%201.sav");
    CHECK_STR_EQ(urlEncodePath("100%/x"), "100%25/x");
    CHECK_STR_EQ(urlEncodePath("a?b#c"), "a%3Fb%23c");
    CHECK_STR_EQ(urlEncodePath("a+b"), "a%2Bb");
}

TEST(url, encodes_non_ascii_bytewise)
{
    CHECK_STR_EQ(urlEncodePath("Pok\xc3\xa9mon"), "Pok%C3%A9mon");
}

TEST(url, decode_reverses_encode)
{
    const char *values[] = {"Checkpoint/saves/001.sav", "My Games/save 1.sav",
                            "100%/x", "a?b#c", "Pok\xc3\xa9mon", ""};
    for (const char *value : values)
        CHECK_STR_EQ(urlDecode(urlEncodePath(value)), value);
}

TEST(url, decode_leaves_malformed_escapes_alone)
{
    CHECK_STR_EQ(urlDecode("a%"), "a%");
    CHECK_STR_EQ(urlDecode("a%2"), "a%2");
    CHECK_STR_EQ(urlDecode("a%zz"), "a%zz");
    // '+' is a form-encoding convention, not a path one: it must stay a plus.
    CHECK_STR_EQ(urlDecode("a+b"), "a+b");
}

TEST(url, decodes_a_webdav_href)
{
    CHECK_STR_EQ(urlDecode("/remote.php/dav/files/user/3DS/Pok%C3%A9mon%20X/001.sav"),
                 "/remote.php/dav/files/user/3DS/Pok\xc3\xa9mon X/001.sav");
}
