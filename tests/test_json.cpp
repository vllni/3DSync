// The JSON lookups every provider relies on.  These are not a parser, so the
// tests pin down exactly what they do handle — including the escape forms the
// APIs actually emit for non-ASCII save folder names.

#include "framework.h"

#include "../source/utils/json.h"

TEST(json, reads_compact_and_spaced_members)
{
    CHECK_STR_EQ(jsonString("{\"rev\":\"abc\"}", "rev"), "abc");
    CHECK_STR_EQ(jsonString("{\"rev\": \"abc\"}", "rev"), "abc");
    CHECK_STR_EQ(jsonString("{\"other\":\"x\"}", "rev"), "");
}

TEST(json, decodes_escapes)
{
    CHECK_STR_EQ(jsonString("{\"a\":\"line\\nbreak\"}", "a"), "line\nbreak");
    CHECK_STR_EQ(jsonString("{\"a\":\"tab\\there\"}", "a"), "tab\there");
    CHECK_STR_EQ(jsonString("{\"a\":\"quote\\\"inside\"}", "a"), "quote\"inside");
    CHECK_STR_EQ(jsonString("{\"a\":\"back\\\\slash\"}", "a"), "back\\slash");
    CHECK_STR_EQ(jsonString("{\"a\":\"slash\\/path\"}", "a"), "slash/path");
}

TEST(json, decodes_unicode_escapes)
{
    // Dropbox escapes non-ASCII in paths this way: "Pokémon".
    CHECK_STR_EQ(jsonString("{\"p\":\"Pok\\u00e9mon\"}", "p"), "Pok\xc3\xa9mon");
    // Three-byte range.
    CHECK_STR_EQ(jsonString("{\"p\":\"\\u65e5\"}", "p"), "\xe6\x97\xa5");
    // Surrogate pair for an astral code point (U+1F600).
    CHECK_STR_EQ(jsonString("{\"p\":\"\\ud83d\\ude00\"}", "p"), "\xf0\x9f\x98\x80");
}

TEST(json, reads_booleans_and_integers)
{
    CHECK(jsonTrue("{\"has_more\":true}", "has_more"));
    CHECK(jsonTrue("{\"has_more\": true}", "has_more"));
    CHECK(!jsonTrue("{\"has_more\":false}", "has_more"));
    CHECK(!jsonTrue("{\"other\":true}", "has_more"));

    long long value = -1;
    CHECK(jsonInt("{\"size\": 4096}", "size", value));
    CHECK_EQ(value, (long long)4096);
    CHECK(jsonInt("{\"size\":-7}", "size", value));
    CHECK_EQ(value, (long long)-7);
    CHECK(!jsonInt("{\"other\":1}", "size", value));
}

TEST(json, splits_an_array_of_objects)
{
    std::string doc = "{\"entries\":[{\"a\":1},{\"b\":2}],\"cursor\":\"x\"}";
    std::vector<std::string> objects;
    jsonSplitArray(doc, "entries", objects);
    CHECK_EQ(objects.size(), (size_t)2);
    CHECK_STR_EQ(objects[0], "{\"a\":1}");
    CHECK_STR_EQ(objects[1], "{\"b\":2}");
}

TEST(json, split_keeps_nested_objects_whole)
{
    std::string doc = "{\"entries\":[{\"a\":{\"deep\":1},\"b\":2}]}";
    std::vector<std::string> objects;
    jsonSplitArray(doc, "entries", objects);
    CHECK_EQ(objects.size(), (size_t)1);
    CHECK_STR_EQ(objects[0], "{\"a\":{\"deep\":1},\"b\":2}");
}

TEST(json, split_is_not_confused_by_braces_in_strings)
{
    // A save folder really can be named like this.
    std::string doc = "{\"entries\":[{\"name\":\"weird{name}\"},{\"name\":\"ok\"}]}";
    std::vector<std::string> objects;
    jsonSplitArray(doc, "entries", objects);
    CHECK_EQ(objects.size(), (size_t)2);
    CHECK_STR_EQ(jsonString(objects[0], "name"), "weird{name}");
    CHECK_STR_EQ(jsonString(objects[1], "name"), "ok");
}

TEST(json, split_handles_escaped_quotes_in_strings)
{
    std::string doc = "{\"entries\":[{\"name\":\"say \\\"hi\\\" {x}\"},{\"name\":\"second\"}]}";
    std::vector<std::string> objects;
    jsonSplitArray(doc, "entries", objects);
    CHECK_EQ(objects.size(), (size_t)2);
    CHECK_STR_EQ(jsonString(objects[1], "name"), "second");
}

TEST(json, split_of_missing_or_empty_array_yields_nothing)
{
    std::vector<std::string> objects;
    jsonSplitArray("{\"other\":[{\"a\":1}]}", "entries", objects);
    CHECK_EQ(objects.size(), (size_t)0);
    jsonSplitArray("{\"entries\":[]}", "entries", objects);
    CHECK_EQ(objects.size(), (size_t)0);
}

TEST(json, escapes_a_body_value)
{
    CHECK_STR_EQ(jsonEscape("plain"), "plain");
    CHECK_STR_EQ(jsonEscape("quote\"here"), "quote\\\"here");
    CHECK_STR_EQ(jsonEscape("back\\slash"), "back\\\\slash");
    CHECK_STR_EQ(jsonEscape("line\nbreak"), "line\\nbreak");
    CHECK_STR_EQ(jsonEscape("bell\x07"), "bell\\u0007");
    // Non-ASCII is legal in a body and stays as UTF-8.
    CHECK_STR_EQ(jsonEscape("Pok\xc3\xa9mon"), "Pok\xc3\xa9mon");
}

TEST(json, header_escaping_is_pure_ascii)
{
    // Dropbox-API-Arg travels in an HTTP header, so nothing above 0x7f may
    // survive: a save folder named after a game title hits this immediately.
    CHECK_STR_EQ(jsonEscapeAscii("/Pok\xc3\xa9mon/001.sav"), "/Pok\\u00e9mon/001.sav");
    CHECK_STR_EQ(jsonEscapeAscii("\xe6\x97\xa5"), "\\u65e5");
    // Above the BMP becomes a surrogate pair.
    CHECK_STR_EQ(jsonEscapeAscii("\xf0\x9f\x98\x80"), "\\ud83d\\ude00");
    CHECK_STR_EQ(jsonEscapeAscii("quote\"x"), "quote\\\"x");
    CHECK_STR_EQ(jsonEscapeAscii("del\x7f"), "del\\u007f");
}

TEST(json, header_escaping_drops_invalid_utf8)
{
    // A lone continuation byte cannot be encoded; dropping it keeps the header
    // well-formed rather than sending something the API will reject.
    CHECK_STR_EQ(jsonEscapeAscii("a\x80z"), "az");
    // A truncated sequence at the end is cut rather than half-encoded.
    CHECK_STR_EQ(jsonEscapeAscii("a\xe6\x97"), "a");
}

TEST(json, unescape_reverses_escape)
{
    const char *values[] = {"plain", "quote\"here", "back\\slash", "line\nbreak",
                            "tab\there", "bell\x07", "Pok\xc3\xa9mon"};
    for (const char *value : values)
        CHECK_STR_EQ(jsonUnescape(jsonEscape(value)), value);
}

TEST(json, unescape_reads_unicode_escapes)
{
    CHECK_STR_EQ(jsonUnescape("Pok\\u00e9mon"), "Pok\xc3\xa9mon");
    CHECK_STR_EQ(jsonUnescape("\\ud83d\\ude00"), "\xf0\x9f\x98\x80");
    // A malformed escape is passed through rather than swallowing the text.
    CHECK_STR_EQ(jsonUnescape("\\uZZZZ"), "uZZZZ");
}
