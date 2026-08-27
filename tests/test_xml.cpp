// WebDAV multistatus reading.  Servers disagree about namespace prefixes, so
// these cases are taken from what Nextcloud and Apache mod_dav actually send.

#include "framework.h"

#include "../source/utils/xmlutil.h"

TEST(xml, strips_namespace_prefixes)
{
    CHECK_STR_EQ(stripXmlNamespaces("<d:href>/x</d:href>"), "<href>/x</href>");
    CHECK_STR_EQ(stripXmlNamespaces("<D:href>/x</D:href>"), "<href>/x</href>");
    CHECK_STR_EQ(stripXmlNamespaces("<lp1:getetag>\"a\"</lp1:getetag>"),
                 "<getetag>\"a\"</getetag>");
    CHECK_STR_EQ(stripXmlNamespaces("<href>/x</href>"), "<href>/x</href>");
}

TEST(xml, strips_prefixes_on_self_closing_tags)
{
    CHECK_STR_EQ(stripXmlNamespaces("<d:collection/>"), "<collection/>");
    CHECK_STR_EQ(stripXmlNamespaces("<d:resourcetype><d:collection/></d:resourcetype>"),
                 "<resourcetype><collection/></resourcetype>");
}

TEST(xml, leaves_attribute_values_alone)
{
    // The xmlns declaration carries a colon in its value, which must survive.
    std::string stripped = stripXmlNamespaces("<d:multistatus xmlns:d=\"DAV:\">");
    CHECK(stripped.find("xmlns:d=\"DAV:\"") != std::string::npos);
    CHECK(stripped.find("<multistatus") == 0);
}

TEST(xml, reads_tag_text_within_a_range)
{
    std::string doc = "<response><href>/a</href><getetag>\"tag\"</getetag></response>";
    CHECK_STR_EQ(xmlTagText(doc, "href", 0, doc.size()), "/a");
    CHECK_STR_EQ(xmlTagText(doc, "getetag", 0, doc.size()), "\"tag\"");
    CHECK_STR_EQ(xmlTagText(doc, "missing", 0, doc.size()), "");
}

TEST(xml, self_closing_tag_has_no_text)
{
    std::string doc = "<response><getetag/></response>";
    CHECK_STR_EQ(xmlTagText(doc, "getetag", 0, doc.size()), "");
}

TEST(xml, does_not_read_past_the_range)
{
    // Two responses: reading the first must not pick up the second one's href,
    // which is how entries would get attributed to the wrong file.
    std::string doc = "<response><x>1</x></response><response><href>/second</href></response>";
    size_t firstEnd = doc.find("</response") + 1;
    CHECK_STR_EQ(xmlTagText(doc, "href", 0, firstEnd), "");
    CHECK_STR_EQ(xmlTagText(doc, "x", 0, firstEnd), "1");
}

TEST(xml, normalises_etags)
{
    CHECK_STR_EQ(normalizeEtag("\"abc123\""), "abc123");
    CHECK_STR_EQ(normalizeEtag("W/\"abc123\""), "abc123");
    CHECK_STR_EQ(normalizeEtag("abc123"), "abc123");
    CHECK_STR_EQ(normalizeEtag(""), "");
    // A quoted empty ETag collapses to empty, which callers treat as "absent".
    CHECK_STR_EQ(normalizeEtag("\"\""), "");
}
