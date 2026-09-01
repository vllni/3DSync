#include "framework.h"

#include "mocks.h"

#include "../source/utils/debug.h"

// Debug mode prints response bodies and libcurl's header trace, and both carry
// credentials: the whole point of the mode is to produce output someone pastes
// into a bug report, so what it must never print is worth pinning down.

TEST(debug, hides_oauth_tokens_in_json)
{
    std::string body = "{\"access_token\":\"ya29.a0AfB_secret\",\"expires_in\":3599}";
    CHECK_STR_EQ(debugRedact(body),
                 "{\"access_token\":\"<redacted>\",\"expires_in\":3599}");
}

TEST(debug, hides_tokens_with_spaced_separators)
{
    std::string body = "{ \"refresh_token\" : \"1//03xyz\" }";
    CHECK_STR_EQ(debugRedact(body), "{ \"refresh_token\" : \"<redacted>\" }");
}

TEST(debug, hides_form_encoded_secrets_but_keeps_the_client_id)
{
    std::string body = "grant_type=refresh_token&refresh_token=1//03abc"
                       "&client_id=123.apps&client_secret=GOCSPX-zzz";
    CHECK_STR_EQ(debugRedact(body),
                 "grant_type=refresh_token&refresh_token=<redacted>"
                 "&client_id=123.apps&client_secret=<redacted>");
}

TEST(debug, hides_the_bearer_header_curl_traces)
{
    std::string headers = "GET /drive/v3/files HTTP/1.1\r\n"
                          "Authorization: Bearer ya29.secret\r\n"
                          "Accept: */*\r\n";
    CHECK_STR_EQ(debugRedact(headers),
                 "GET /drive/v3/files HTTP/1.1\r\n"
                 "Authorization: <redacted>\r\n"
                 "Accept: */*\r\n");
}

TEST(debug, leaves_ordinary_error_bodies_alone)
{
    // The bodies that matter for debugging must survive untouched.
    std::string body = "{\"error_summary\":\"path/not_found/\","
                       "\"error\":{\".tag\":\"path\"}}";
    CHECK_STR_EQ(debugRedact(body), body);
}

TEST(debug, is_off_until_asked_for)
{
    // The flag is global, so leave it as it was found.
    bool was = debugEnabled();
    setDebugEnabled(false);
    CHECK(!debugEnabled());
    setDebugEnabled(true);
    CHECK(debugEnabled());
    setDebugEnabled(was);
}

// ---------------------------------------------------------------------------
// Session log
// ---------------------------------------------------------------------------
// The capture is what a bug report is actually made of, so the two ways it can
// betray that are worth pinning: keeping bytes when nobody asked for debug
// mode, and writing a token into a file on a shared SD card.

TEST(debuglog, captures_nothing_outside_debug_mode)
{
    bool was = debugEnabled();
    setDebugEnabled(false);
    debugLogReset();
    debugLogAppend("Syncing [/test] <-> Dropbox:test\n", 33);
    CHECK_EQ(debugLogSize(), (size_t)0);
    setDebugEnabled(was);
    debugLogReset();
}

TEST(debuglog, keeps_what_is_appended_in_debug_mode)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    debugLogReset();
    debugLogAppend("abc", 3);
    debugLogAppend("de", 2);
    CHECK_EQ(debugLogSize(), (size_t)5);
    CHECK_EQ(debugLogDropped(), (size_t)0);
    setDebugEnabled(was);
    debugLogReset();
}

TEST(debuglog, drops_the_middle_of_an_oversized_run_and_says_so)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    debugLogReset();

    // Comfortably past head + twice the tail window.
    std::string chunk(4096, 'x');
    for (int i = 0; i < 80; i++)
        debugLogAppend(chunk.data(), chunk.size());

    CHECK(debugLogDropped() > 0);
    // Whatever was dropped, the buffer stays bounded rather than growing with
    // the run.
    CHECK(debugLogSize() < 320u * 1024u);

    setDebugEnabled(was);
    debugLogReset();
}

TEST(debuglog, saves_without_colour_codes_or_secrets)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    debugLogReset();

    std::string line = "\x1b[31;1m  Dropbox API error: HTTP 409\x1b[0m\n"
                       "dbg: {\"access_token\":\"ya29.secret\"}\n";
    debugLogAppend(line.data(), line.size());

    TempDir dir;
    std::string path = dir.path() + "/logs/debug.log";
    CHECK(debugLogSave(path));

    std::string saved = dir.read("/logs/debug.log");
    CHECK_STR_EQ(saved, "  Dropbox API error: HTTP 409\n"
                        "dbg: {\"access_token\":\"<redacted>\"}\n");

    setDebugEnabled(was);
    debugLogReset();
}

TEST(debuglog, a_new_run_does_not_carry_the_previous_one)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    debugLogReset();
    debugLogAppend("first run\n", 10);
    debugLogReset();
    CHECK_EQ(debugLogSize(), (size_t)0);
    CHECK_EQ(debugLogDropped(), (size_t)0);
    setDebugEnabled(was);
}
