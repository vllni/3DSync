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

TEST(debuglog, captures_nothing_without_a_run)
{
    debugLogReset();
    debugLogAppend("Syncing [/test] <-> Dropbox:test\n", 33);
    CHECK_EQ(debugLogSize(), (size_t)0);
}

TEST(debuglog, a_normal_run_is_captured_too)
{
    bool was = debugEnabled();
    setDebugEnabled(false);
    TempDir dir;
    CHECK(debugLogStart(dir.path() + "/sync.log.part"));
    debugLogAppend("Syncing [/test]\n", 16);
    CHECK(debugLogKeep(dir.path() + "/sync.log"));
    CHECK_STR_EQ(dir.read("/sync.log"), "Syncing [/test]\n");
    setDebugEnabled(was);
}

TEST(debuglog, keeps_every_line_of_a_long_run)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    TempDir dir;
    CHECK(debugLogStart(dir.path() + "/logs/debug.log.part"));

    // Far past what the old in-memory head and tail kept (144 KB).
    std::string line(99, 'x');
    line += "\n";
    for (int i = 0; i < 4000; i++)
        debugLogAppend(line.data(), line.size());
    debugLogAppend("the last line\n", 14);
    CHECK_EQ(debugLogSize(), (size_t)(4000 * 100 + 14));

    CHECK(debugLogKeep(dir.path() + "/logs/debug.log"));
    std::string saved = dir.read("/logs/debug.log");
    CHECK_EQ(saved.size(), (size_t)(4000 * 100 + 14));
    CHECK(saved.compare(saved.size() - 14, 14, "the last line\n") == 0);
    CHECK(!dir.exists("/logs/debug.log.part"));
    setDebugEnabled(was);
}

TEST(debuglog, the_card_never_holds_colour_codes_or_secrets)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    TempDir dir;
    CHECK(debugLogStart(dir.path() + "/debug.log.part"));

    // A token split over two console writes, as a body dump can arrive.
    std::string first = "\x1b[31;1m  Dropbox API error: HTTP 409\x1b[0m\n"
                        "dbg: {\"access_token\":\"ya29.se";
    std::string second = "cret\"}\n";
    debugLogAppend(first.data(), first.size());
    debugLogAppend(second.data(), second.size());

    // Already on the card while the run goes on, and already clean.
    std::string spooled = dir.read("/debug.log.part");
    CHECK(spooled.find("ya29") == std::string::npos);

    CHECK(debugLogKeep(dir.path() + "/debug.log"));
    CHECK_STR_EQ(dir.read("/debug.log"), "  Dropbox API error: HTTP 409\n"
                                        "dbg: {\"access_token\":\"<redacted>\"}\n");
    setDebugEnabled(was);
}

TEST(debuglog, a_declined_log_is_deleted)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    TempDir dir;
    CHECK(debugLogStart(dir.path() + "/debug.log.part"));
    debugLogAppend("first run\n", 10);
    debugLogReset();
    CHECK(!dir.exists("/debug.log.part"));
    CHECK_EQ(debugLogSize(), (size_t)0);
    setDebugEnabled(was);
}

TEST(debuglog, a_line_still_open_at_the_end_is_kept)
{
    bool was = debugEnabled();
    setDebugEnabled(true);
    TempDir dir;
    CHECK(debugLogStart(dir.path() + "/debug.log.part"));
    debugLogAppend("no line break", 13);
    CHECK(debugLogKeep(dir.path() + "/debug.log"));
    CHECK_STR_EQ(dir.read("/debug.log"), "no line break");
    setDebugEnabled(was);
}

TEST(debug, hides_the_ftp_password_command)
{
    // As libcurl hands an outgoing FTP command to the trace...
    CHECK_STR_EQ(debugRedact("USER bob\r\nPASS hunter2\r\n"), "USER bob\r\nPASS <redacted>\r\n");
    CHECK_STR_EQ(debugRedact("ACCT secret\r\n"), "ACCT <redacted>\r\n");
    // ...and as it sits in the session log, after the trace's own prefixes.
    CHECK_STR_EQ(debugRedact("dbg: > PASS hunter2\n"), "dbg: > PASS <redacted>\n");
    CHECK_STR_EQ(debugRedact("\x1b[35mdbg: > pass hunter2\n\x1b[0m"),
                 "\x1b[35mdbg: > pass <redacted>\n\x1b[0m");
    // A word that merely contains it, or the word in a sentence, is left alone.
    CHECK_STR_EQ(debugRedact("bypass the cache\nThe PASS command failed\n"),
                 "bypass the cache\nThe PASS command failed\n");
}

TEST(debug, hides_a_password_written_into_a_url)
{
    CHECK_STR_EQ(debugRedact("status 200 <- https://bob:hunter2@dav.example/remote.php/"),
                 "status 200 <- https://bob:<redacted>@dav.example/remote.php/");
    CHECK_STR_EQ(debugRedact("ftp://anonymous@host/pub ftp://a:b@h"),
                 "ftp://anonymous@host/pub ftp://a:<redacted>@h");
    // A port is not a password.
    CHECK_STR_EQ(debugRedact("https://nas.local:5006/dav/"), "https://nas.local:5006/dav/");
}
