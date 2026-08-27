#include "framework.h"

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
