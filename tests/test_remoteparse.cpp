// Each remote's own parsing, fed the responses a server really sends.  The
// payloads below are trimmed from the API documentation and from real server
// output, so a change in how a response is read shows up here rather than on
// hardware.

#include "framework.h"

#include "../source/modules/remoteparse.h"

// ---------------------------------------------------------------------------
// Dropbox list_folder
// ---------------------------------------------------------------------------

namespace
{
const char *DROPBOX_PAGE =
    "{\"entries\":["
    "{\".tag\":\"folder\",\"name\":\"saves\",\"path_lower\":\"/3ds/saves\","
    "\"path_display\":\"/3DS/saves\",\"id\":\"id:folder1\"},"
    "{\".tag\":\"file\",\"name\":\"001.sav\",\"path_lower\":\"/3ds/saves/game/001.sav\","
    "\"path_display\":\"/3DS/saves/Game/001.sav\",\"id\":\"id:file1\","
    "\"client_modified\":\"2024-05-28T14:32:00Z\",\"server_modified\":\"2024-05-28T14:32:01Z\","
    "\"rev\":\"5e1f2a3b4c\",\"size\":32768,\"is_downloadable\":true,"
    "\"content_hash\":\"deadbeef\"},"
    "{\".tag\":\"deleted\",\"name\":\"gone.sav\",\"path_lower\":\"/3ds/saves/gone.sav\","
    "\"path_display\":\"/3DS/saves/gone.sav\"}"
    "],\"cursor\":\"AAE\",\"has_more\":false}";
} // namespace

TEST(dropbox_parse, reads_a_file_entry)
{
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(DROPBOX_PAGE, "/3DS/saves", out);

    CHECK_EQ(out.size(), (size_t)1); // the folder and the deleted entry are skipped
    CHECK_EQ(out.count("/Game/001.sav"), (size_t)1);
    CHECK_STR_EQ(out["/Game/001.sav"].tag, "deadbeef");
    // Addressing the revision pins the download to what was listed.
    CHECK_STR_EQ(out["/Game/001.sav"].id, "rev:5e1f2a3b4c");
    CHECK_STR_EQ(out["/Game/001.sav"].relPath, "/Game/001.sav");
}

TEST(dropbox_parse, matches_the_root_case_insensitively)
{
    // Dropbox paths are case-insensitive, so a folder stored as "/3DS" must
    // still match a root configured as "/3ds".
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(DROPBOX_PAGE, "/3ds/saves", out);
    CHECK_EQ(out.count("/Game/001.sav"), (size_t)1);
}

TEST(dropbox_parse, ignores_entries_outside_the_root)
{
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(DROPBOX_PAGE, "/Elsewhere", out);
    CHECK_EQ(out.size(), (size_t)0);
}

TEST(dropbox_parse, skips_in_flight_transfers)
{
    const char *page =
        "{\"entries\":["
        "{\".tag\":\"file\",\"path_display\":\"/3DS/001.sav.3dstmp\",\"rev\":\"a\","
        "\"content_hash\":\"h1\"},"
        "{\".tag\":\"file\",\"path_display\":\"/3DS/001.sav\",\"rev\":\"b\","
        "\"content_hash\":\"h2\"}]}";
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(page, "/3DS", out);
    CHECK_EQ(out.size(), (size_t)1);
    CHECK_EQ(out.count("/001.sav"), (size_t)1);
}

TEST(dropbox_parse, falls_back_to_path_lower)
{
    const char *page = "{\"entries\":[{\".tag\":\"file\","
                       "\"path_lower\":\"/3ds/001.sav\",\"rev\":\"a\",\"content_hash\":\"h\"}]}";
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(page, "/3ds", out);
    CHECK_EQ(out.count("/001.sav"), (size_t)1);
}

TEST(dropbox_parse, handles_unicode_and_braces_in_names)
{
    const char *page =
        "{\"entries\":[{\".tag\":\"file\","
        "\"path_display\":\"/3DS/Pok\\u00e9mon {X}/001.sav\",\"rev\":\"a\","
        "\"content_hash\":\"h\"}]}";
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(page, "/3DS", out);
    CHECK_EQ(out.size(), (size_t)1);
    CHECK_EQ(out.count("/Pok\xc3\xa9mon {X}/001.sav"), (size_t)1);
}

TEST(dropbox_parse, an_entry_equal_to_the_root_is_not_a_child)
{
    const char *page = "{\"entries\":[{\".tag\":\"file\",\"path_display\":\"/3DS\","
                       "\"rev\":\"a\",\"content_hash\":\"h\"}]}";
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(page, "/3DS", out);
    CHECK_EQ(out.size(), (size_t)0);
}

TEST(dropbox_parse, a_sibling_sharing_the_root_prefix_is_excluded)
{
    // "/3DSync" starts with "/3DS" but is not inside it.
    const char *page = "{\"entries\":[{\".tag\":\"file\","
                       "\"path_display\":\"/3DSync/001.sav\",\"rev\":\"a\",\"content_hash\":\"h\"}]}";
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries(page, "/3DS", out);
    CHECK_EQ(out.size(), (size_t)0);
}

TEST(dropbox_parse, pages_accumulate)
{
    // list_folder/continue returns the same shape; the caller merges into one map.
    std::map<std::string, RemoteFileInfo> out;
    parseDropboxEntries("{\"entries\":[{\".tag\":\"file\",\"path_display\":\"/r/a.sav\","
                        "\"rev\":\"1\",\"content_hash\":\"h1\"}],\"has_more\":true,"
                        "\"cursor\":\"C1\"}", "/r", out);
    parseDropboxEntries("{\"entries\":[{\".tag\":\"file\",\"path_display\":\"/r/b.sav\","
                        "\"rev\":\"2\",\"content_hash\":\"h2\"}],\"has_more\":false}", "/r", out);
    CHECK_EQ(out.size(), (size_t)2);
    CHECK_STR_EQ(out["/a.sav"].tag, "h1");
    CHECK_STR_EQ(out["/b.sav"].tag, "h2");
}

// ---------------------------------------------------------------------------
// WebDAV PROPFIND
// ---------------------------------------------------------------------------

namespace
{
const char *DAV_BODY =
    "<?xml version=\"1.0\"?>"
    "<d:multistatus xmlns:d=\"DAV:\">"
    "  <d:response>"
    "    <d:href>/dav/3DS/</d:href>"
    "    <d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
    "      <d:getetag>\"root\"</d:getetag></d:prop>"
    "      <d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
    "  </d:response>"
    "  <d:response>"
    "    <d:href>/dav/3DS/Game/</d:href>"
    "    <d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
    "      <d:getetag>\"dir\"</d:getetag></d:prop></d:propstat>"
    "  </d:response>"
    "  <d:response>"
    "    <d:href>/dav/3DS/Game/001.sav</d:href>"
    "    <d:propstat><d:prop><d:resourcetype/>"
    "      <d:getetag>W/\"abc123\"</d:getetag>"
    "      <d:getcontentlength>32768</d:getcontentlength>"
    "      <d:getlastmodified>Tue, 28 May 2024 14:32:00 GMT</d:getlastmodified>"
    "    </d:prop></d:propstat>"
    "  </d:response>"
    "</d:multistatus>";
} // namespace

TEST(webdav_parse, reads_files_and_collections)
{
    std::vector<DavEntry> out;
    parseDavResponses(DAV_BODY, "/dav/3DS/", out);

    // The root's own entry is dropped; the folder and the file remain.
    CHECK_EQ(out.size(), (size_t)2);
    CHECK_STR_EQ(out[0].relPath, "/Game");
    CHECK(out[0].isCollection);
    CHECK_STR_EQ(out[1].relPath, "/Game/001.sav");
    CHECK(!out[1].isCollection);
}

TEST(webdav_parse, prefers_the_etag_and_strips_its_quoting)
{
    std::vector<DavEntry> out;
    parseDavResponses(DAV_BODY, "/dav/3DS/", out);
    CHECK_STR_EQ(out[1].tag, "abc123");
}

TEST(webdav_parse, falls_back_to_size_and_last_modified)
{
    const char *body =
        "<multistatus><response><href>/dav/3DS/001.sav</href><propstat><prop>"
        "<resourcetype/>"
        "<getcontentlength>512</getcontentlength>"
        "<getlastmodified>Tue, 28 May 2024 14:32:00 GMT</getlastmodified>"
        "</prop></propstat></response></multistatus>";
    std::vector<DavEntry> out;
    parseDavResponses(body, "/dav/3DS/", out);
    CHECK_EQ(out.size(), (size_t)1);
    CHECK_STR_EQ(out[0].tag, "512:Tue, 28 May 2024 14:32:00 GMT");
}

TEST(webdav_parse, accepts_an_absolute_href)
{
    // Some servers answer with a full URL instead of a path.
    const char *body =
        "<multistatus><response>"
        "<href>https://nas.example/dav/3DS/001.sav</href>"
        "<propstat><prop><resourcetype/><getetag>\"e\"</getetag></prop></propstat>"
        "</response></multistatus>";
    std::vector<DavEntry> out;
    parseDavResponses(body, "/dav/3DS/", out);
    CHECK_EQ(out.size(), (size_t)1);
    CHECK_STR_EQ(out[0].relPath, "/001.sav");
}

TEST(webdav_parse, decodes_percent_escapes_in_hrefs)
{
    const char *body =
        "<multistatus><response>"
        "<href>/dav/3DS/Pok%C3%A9mon%20X/001.sav</href>"
        "<propstat><prop><resourcetype/><getetag>\"e\"</getetag></prop></propstat>"
        "</response></multistatus>";
    std::vector<DavEntry> out;
    parseDavResponses(body, "/dav/3DS/", out);
    CHECK_EQ(out.size(), (size_t)1);
    CHECK_STR_EQ(out[0].relPath, "/Pok\xc3\xa9mon X/001.sav");
}

TEST(webdav_parse, ignores_responses_outside_the_root)
{
    const char *body =
        "<multistatus><response>"
        "<href>/dav/other/001.sav</href>"
        "<propstat><prop><resourcetype/><getetag>\"e\"</getetag></prop></propstat>"
        "</response></multistatus>";
    std::vector<DavEntry> out;
    parseDavResponses(body, "/dav/3DS/", out);
    CHECK_EQ(out.size(), (size_t)0);
}

TEST(webdav_parse, a_collection_marker_belongs_to_its_own_response)
{
    // The file is listed first and must not inherit the folder's <collection/>.
    const char *body =
        "<multistatus>"
        "<response><href>/r/001.sav</href><propstat><prop><resourcetype/>"
        "<getetag>\"f\"</getetag></prop></propstat></response>"
        "<response><href>/r/dir/</href><propstat><prop>"
        "<resourcetype><collection/></resourcetype><getetag>\"d\"</getetag>"
        "</prop></propstat></response>"
        "</multistatus>";
    std::vector<DavEntry> out;
    parseDavResponses(body, "/r/", out);
    CHECK_EQ(out.size(), (size_t)2);
    CHECK_STR_EQ(out[0].relPath, "/001.sav");
    CHECK(!out[0].isCollection);
    CHECK(out[1].isCollection);
}

TEST(webdav_parse, empty_body_yields_nothing)
{
    std::vector<DavEntry> out;
    parseDavResponses("", "/r/", out);
    parseDavResponses("<multistatus></multistatus>", "/r/", out);
    CHECK_EQ(out.size(), (size_t)0);
}

// ---------------------------------------------------------------------------
// FTP URLs and size:time tags
// ---------------------------------------------------------------------------

TEST(ftp_parse, builds_urls)
{
    CHECK_STR_EQ(buildFtpUrl(false, "192.168.1.10", 21, "saves/001.sav"),
                 "ftp://192.168.1.10:21/saves/001.sav");
    CHECK_STR_EQ(buildFtpUrl(false, "nas.local", 2121, ""), "ftp://nas.local:2121/");
    // Implicit TLS is the scheme's job, not an option's.
    CHECK_STR_EQ(buildFtpUrl(true, "nas.local", 990, "x"), "ftps://nas.local:990/x");
}

TEST(ftp_parse, encodes_the_path_in_urls)
{
    CHECK_STR_EQ(buildFtpUrl(false, "h", 21, "My Games/Pok\xc3\xa9mon/001.sav"),
                 "ftp://h:21/My%20Games/Pok%C3%A9mon/001.sav");
    // '*' is encoded like any other reserved character, which is why the
    // listing wildcard is appended after the path is built rather than passed
    // through here — libcurl would match a literal "%2A" otherwise.
    CHECK_STR_EQ(buildFtpUrl(false, "h", 21, "dir/*"), "ftp://h:21/dir/%2A");
    CHECK_STR_EQ(buildFtpUrl(false, "h", 21, "dir") + "/*", "ftp://h:21/dir/*");
}

TEST(remote_tags, size_and_time_format)
{
    CHECK_STR_EQ(sizeTimeTag(32768, 1716905520), "32768:1716905520");
    CHECK_STR_EQ(sizeTimeTag(0, 0), "0:0");
    // An unknown size is recorded as -1 rather than silently as 0.
    CHECK_STR_EQ(sizeTimeTag(-1, 1716905520), "-1:1716905520");
}
