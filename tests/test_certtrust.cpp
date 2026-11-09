// PinnedKey= and CACert=: the forms a user may paste, and what is refused.

#include "framework.h"

#include "../source/utils/certtrust.h"

namespace
{

// A self-signed EC certificate for CN=nas.local, and the SHA-256 of its
// public key in the forms people find it: openssl's and curl's base64, hex,
// and a browser's colon-separated pairs.
const char CERT_B64[] =
    "MIIBfTCCASOgAwIBAgIUDuaYMN3Z2l7FIDsXuwma49fNmYQwCgYIKoZIzj0EAwIwFDESMBAGA1UEAwwJ"
    "bmFzLmxvY2FsMB4XDTI2MTEwOTE5MDU0MVoXDTM2MTEwNjE5MDU0MVowFDESMBAGA1UEAwwJbmFzLmxv"
    "Y2FsMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEgwVJmXz+c1oXyG3XyLcTJO6Ki3VU/sarBVVNTPYH"
    "HKjIMN7SzEpNlCorf64VhMBdMdlrJG4JxnrvbMrnHXZDxqNTMFEwHQYDVR0OBBYEFDW072mtvcv9p4us"
    "UqQP8JARYdUCMB8GA1UdIwQYMBaAFDW072mtvcv9p4usUqQP8JARYdUCMA8GA1UdEwEB/wQFMAMBAf8w"
    "CgYIKoZIzj0EAwIDSAAwRQIgIakhQamB3hx+eo+t1lnGs4vrb9A1vLp5+mc79bB0NzgCIQC9FWsyCKzk"
    "RqRKz5DUnw8bDGij2A41yvkh2MG66K7Ghg==";
const char KEY_B64[] = "sha256//tETJ80bJx1pplYT+TsxpThIceVsFYhqqCYT1fF6q/gc=";
const char KEY_HEX[] = "b444c9f346c9c75a699584fe4ecc694e121c795b05621aaa0984f57c5eaafe07";
const char KEY_COLONS[] = "B4:44:C9:F3:46:C9:C7:5A:69:95:84:FE:4E:CC:69:4E:"
                          "12:1C:79:5B:05:62:1A:AA:09:84:F5:7C:5E:AA:FE:07";

} // namespace

TEST(certtrust, a_pin_reads_alike_in_every_form)
{
    std::vector<std::string> b64, hex, colons, spaced;
    CHECK(parsePinnedKeys(KEY_B64, b64));
    CHECK(parsePinnedKeys(KEY_HEX, hex));
    CHECK(parsePinnedKeys(KEY_COLONS, colons));
    // Chrome shows the pairs with spaces between.
    std::string withSpaces = KEY_COLONS;
    for (auto &c : withSpaces)
        if (c == ':')
            c = ' ';
    CHECK(parsePinnedKeys(withSpaces, spaced));
    CHECK_EQ(b64.size(), (size_t)1);
    CHECK_EQ(b64[0].size(), (size_t)32);
    CHECK(b64 == hex);
    CHECK(hex == colons);
    CHECK(colons == spaced);
    CHECK_EQ((unsigned char)hex[0][0], 0xb4u);
    CHECK_EQ((unsigned char)hex[0][31], 0x07u);
}

TEST(certtrust, several_pins_are_separated_by_commas)
{
    // The old key and the new one, while a renewal is under way.
    std::vector<std::string> pins;
    CHECK(parsePinnedKeys(std::string(KEY_HEX) + ", " + KEY_B64 + ",", pins));
    CHECK_EQ(pins.size(), (size_t)2);
    CHECK(pins[0] == pins[1]);
}

TEST(certtrust, a_pin_that_is_no_sha256_is_refused)
{
    std::vector<std::string> pins;
    // One digit short, a digit too many, not hex, SHA-1 length, base64 of
    // the wrong length, base64 without its sha256// prefix.
    const char *bad[] = {"b444c9f346c9c75a699584fe4ecc694e121c795b05621aaa0984f57c5eaafe0",
                         "b444c9f346c9c75a699584fe4ecc694e121c795b05621aaa0984f57c5eaafe070",
                         "x444c9f346c9c75a699584fe4ecc694e121c795b05621aaa0984f57c5eaafe07",
                         "da39a3ee5e6b4b0d3255bfef95601890afd80709",
                         "sha256//tETJ80bJx1pplYT+TsxpThIceVsFYhqqCYT1fF6q",
                         "tETJ80bJx1pplYT+TsxpThIceVsFYhqqCYT1fF6q/gc=",
                         ",",
                         ""};
    for (const char *value : bad)
    {
        pins.push_back("left over");
        CHECK(!parsePinnedKeys(value, pins));
        CHECK(pins.empty());
    }
    // One bad pin spoils the list: a typo must not leave fewer pins than
    // the line names without anyone being told.
    CHECK(!parsePinnedKeys(std::string(KEY_HEX) + ",abc", pins));
    CHECK(pins.empty());
}

TEST(certtrust, the_printed_fingerprint_reads_back_however_it_is_joined)
{
    std::vector<std::string> pins;
    CHECK(parsePinnedKeys(KEY_HEX, pins));
    std::vector<std::string> lines = fingerprintLines(pins[0]);
    CHECK_EQ(lines.size(), (size_t)2);
    // Under the log's 50 columns with the two-space indent.
    CHECK_EQ(lines[0].size(), (size_t)47);
    CHECK_STR_EQ(lines[0], "B4:44:C9:F3:46:C9:C7:5A:69:95:84:FE:4E:CC:69:4E");
    for (const char *join : {"", ":", " "})
    {
        std::vector<std::string> back;
        CHECK(parsePinnedKeys(lines[0] + join + lines[1], back));
        CHECK(back == pins);
    }
}

TEST(certtrust, a_ca_certificate_is_base64_der)
{
    std::vector<std::string> certs;
    CHECK(parseCaCerts(CERT_B64, certs));
    CHECK_EQ(certs.size(), (size_t)1);
    CHECK_EQ(certs[0].size(), (size_t)385);
    CHECK_EQ((unsigned char)certs[0][0], 0x30u);
}

TEST(certtrust, pem_lines_pasted_onto_one_line_are_allowed)
{
    std::string pem = std::string("-----BEGIN CERTIFICATE----- ") + CERT_B64 +
                      " -----END CERTIFICATE-----";
    std::vector<std::string> one, two;
    CHECK(parseCaCerts(pem, one));
    CHECK_EQ(one.size(), (size_t)1);
    // Two PEM blocks back to back need no comma; two base64 values do.
    CHECK(parseCaCerts(pem + pem, two));
    CHECK_EQ(two.size(), (size_t)2);
    CHECK(parseCaCerts(std::string(CERT_B64) + "," + CERT_B64, two));
    CHECK_EQ(two.size(), (size_t)2);
}

TEST(certtrust, a_ca_value_cut_short_or_not_a_certificate_is_refused)
{
    std::vector<std::string> certs;
    std::string cert = CERT_B64;
    // Cut at a multiple of four, so it is still valid base64: only the DER
    // length says it is short.
    CHECK(!parseCaCerts(cert.substr(0, 400), certs));
    CHECK(certs.empty());
    CHECK(!parseCaCerts(cert.substr(0, cert.size() - 1), certs));
    CHECK(!parseCaCerts("aGVsbG8gd29ybGQ=", certs)); // "hello world"
    CHECK(!parseCaCerts("not base64 at all", certs));
    CHECK(!parseCaCerts(" , ", certs));
    CHECK(!parseCaCerts(cert + ",AAAA", certs));
    CHECK(certs.empty());
}

namespace
{
// Self-signed, CN=3dsync-test, subjectAltName=DNS:localhost,IP:192.168.1.10:
// what testinfra's servers make for themselves.
const char IP_CERT_B64[] =
    "MIIBnDCCAUOgAwIBAgIUJVHJmcrSCJicQguqHTWRZQ9UblwwCgYIKoZIzj0EAwIwFjEUMBIGA1UEAwwL"
    "M2RzeW5jLXRlc3QwHhcNMjYxMTA5MTkxNjMxWhcNMzYxMTA2MTkxNjMxWjAWMRQwEgYDVQQDDAszZHN5"
    "bmMtdGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABLtkeyt9CAc8mnchSxKOOaxZelvRTGL3/F2T"
    "FOtkJEF0z7wbhQ2MBjyBMBAjyIfggujlAgvfSu5ah40frlDeDACjbzBtMB0GA1UdDgQWBBRZb7vbZu+l"
    "vjQcIVNuK5jKc9HPbzAfBgNVHSMEGDAWgBRZb7vbZu+lvjQcIVNuK5jKc9HPbzAPBgNVHRMBAf8EBTAD"
    "AQH/MBoGA1UdEQQTMBGCCWxvY2FsaG9zdIcEwKgBCjAKBggqhkjOPQQDAgNHADBEAiBtYSIAE4Tim1jl"
    "Mn56E+h9HcnGNMjWFBAo4yrNwoMhowIgAo4hzni98exKNd1og7wMyB0DaKcodjG2fF4KOoWVRFM=";

std::string der(const char *b64)
{
    std::vector<std::string> certs;
    parseCaCerts(b64, certs);
    return certs.empty() ? std::string() : certs[0];
}
} // namespace

TEST(certtrust, the_host_is_what_libcurl_connects_to)
{
    CHECK_STR_EQ(urlHost("https://192.168.1.5/dav/"), "192.168.1.5");
    CHECK_STR_EQ(urlHost("https://u:p@w@nas.local:8443/dav/x"), "nas.local");
    CHECK_STR_EQ(urlHost("ftp://192.168.1.5:2121/saves/a.sav"), "192.168.1.5");
    CHECK_STR_EQ(urlHost("ftps://host"), "host");
    CHECK_STR_EQ(urlHost("https://host?x=1"), "host");
}

TEST(certtrust, an_ip_listed_in_the_certificate_is_found)
{
    std::string cert = der(IP_CERT_B64);
    CHECK(!cert.empty());
    CHECK(certificateListsIp(cert, "192.168.1.10"));
    // Another address, a prefix of it, a name, a malformed address.
    CHECK(!certificateListsIp(cert, "192.168.1.1"));
    CHECK(!certificateListsIp(cert, "192.168.1.100"));
    CHECK(!certificateListsIp(cert, "localhost"));
    CHECK(!certificateListsIp(cert, "192.168.1"));
    CHECK(!certificateListsIp(cert, "192.168.1.10.5"));
    CHECK(!certificateListsIp(cert, "192.168.001.010x"));
    // A certificate without any subjectAltName, and one cut short.
    CHECK(!certificateListsIp(der(CERT_B64), "192.168.1.10"));
    CHECK(!certificateListsIp(cert.substr(0, 300), "192.168.1.10"));
}
