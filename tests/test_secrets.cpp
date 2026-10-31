// Credentials sealed to the console.  What has to hold: a sealed value opens
// on the console that sealed it and nowhere else, not in another field, not
// after a byte changes; sealing the file touches the secrets and nothing else,
// in a form inih still reads; and an interrupted write never loses the file.

#include <stdio.h>

#include "framework.h"
#include "mocks.h"

#include "../source/libs/inih/INIReader/INIReader.h"
#include "../source/utils/configseal.h"
#include "../source/utils/secrets.h"

// Deterministic, but different on every call, so two seals of the same value
// differ the way they do on the console.
static unsigned g_counter = 0;
static bool countingRandom(unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)(g_counter * 31 + i * 7);
    g_counter++;
    return true;
}

static bool failingRandom(unsigned char *, size_t) { return false; }

static const SealKey kKey = deriveSealKey(0x0123456789ABCDEFull);
static const SealKey kOtherConsole = deriveSealKey(0x0123456789ABCDEEull);

TEST(secrets, round_trip)
{
    std::string sealed, opened;
    CHECK(sealValue(kKey, "SMB", "Password", "hunter2 ;x", countingRandom, sealed));
    CHECK(isSealedValue(sealed));
    CHECK(sealed.find("hunter2") == std::string::npos);
    CHECK(openSealedValue(kKey, "SMB", "Password", sealed, opened));
    CHECK_STR_EQ(opened, "hunter2 ;x");
}

TEST(secrets, field_names_are_case_insensitive)
{
    std::string sealed, opened;
    CHECK(sealValue(kKey, "Dropbox", "RefreshToken", "rt", countingRandom, sealed));
    CHECK(openSealedValue(kKey, "dropbox", "refreshtoken", sealed, opened));
    CHECK_STR_EQ(opened, "rt");
}

TEST(secrets, another_console_cannot_open_it)
{
    std::string sealed, opened;
    sealValue(kKey, "FTP", "Password", "pw", countingRandom, sealed);
    CHECK(!openSealedValue(kOtherConsole, "FTP", "Password", sealed, opened));
}

TEST(secrets, a_value_moved_to_another_field_does_not_open)
{
    std::string sealed, opened;
    sealValue(kKey, "FTP", "Password", "pw", countingRandom, sealed);
    CHECK(!openSealedValue(kKey, "SMB", "Password", sealed, opened));
}

TEST(secrets, tampering_or_garbage_does_not_open)
{
    std::string sealed, opened;
    sealValue(kKey, "WebDAV", "Password", "secret", countingRandom, sealed);
    // A character in the middle: the last one or two may only carry padding
    // bits the decoder ignores.
    std::string flipped = sealed;
    size_t mid = 7 + (sealed.size() - 7) / 2;
    flipped[mid] = flipped[mid] == 'A' ? 'B' : 'A';
    CHECK(!openSealedValue(kKey, "WebDAV", "Password", flipped, opened));
    CHECK(!openSealedValue(kKey, "WebDAV", "Password", "enc:v1:", opened));
    CHECK(!openSealedValue(kKey, "WebDAV", "Password", "enc:v1:!!!not base64", opened));
    CHECK(!openSealedValue(kKey, "WebDAV", "Password", "enc:v1:QUJD", opened));
    CHECK(!openSealedValue(kKey, "WebDAV", "Password", "plain", opened));
}

TEST(secrets, same_value_seals_differently_each_time)
{
    std::string a, b;
    sealValue(kKey, "SMB", "Password", "same", countingRandom, a);
    sealValue(kKey, "SMB", "Password", "same", countingRandom, b);
    CHECK(a != b);
}

TEST(secrets, no_randomness_no_seal)
{
    std::string sealed;
    CHECK(!sealValue(kKey, "SMB", "Password", "pw", failingRandom, sealed));
}

TEST(secrets, only_credentials_are_sealable)
{
    CHECK(isSealableSecret("SMB", "Password"));
    CHECK(isSealableSecret("googledrive", "CLIENTSECRET"));
    CHECK(isSealableSecret("Dropbox", "RefreshToken"));
    CHECK(!isSealableSecret("Dropbox", "AppKey")); // public, and the configurator shows it
    CHECK(!isSealableSecret("SMB", "User"));
    CHECK(!isSealableSecret("Paths", "Password")); // a remote folder someone named "Password"
}

TEST(secrets, a_labelled_remote_has_the_secrets_of_its_type)
{
    CHECK(isSealableSecret("SMB:Backup", "Password"));
    CHECK(isSealableSecret("dropbox:work", "RefreshToken"));
    CHECK(!isSealableSecret("SMB:Backup", "User"));
    CHECK(!isSealableSecret("Paths:x", "Password")); // not a remote type
}

TEST(secrets, a_value_moved_between_two_remotes_of_a_type_does_not_open)
{
    std::string sealed, opened;
    sealValue(kKey, "SMB:enc", "Password", "pw", countingRandom, sealed);
    CHECK(!openSealedValue(kKey, "SMB", "Password", sealed, opened));
    CHECK(!openSealedValue(kKey, "SMB:backup", "Password", sealed, opened));
    CHECK(openSealedValue(kKey, "smb:ENC", "password", sealed, opened));
    CHECK_STR_EQ(opened, "pw");
}

// ---------------------------------------------------------------------------
// sealIniText
// ---------------------------------------------------------------------------
static const char *kIni =
    "\xEF\xBB\xBF; 3DSync config\r\n"
    "[Dropbox]\r\n"
    "AppKey=y88zxdudhuoo41q\r\n"
    "RefreshToken = abc.def ; from the configurator\r\n"
    "\r\n"
    "[SMB]\r\n"
    "Server=192.168.1.10\r\n"
    "Password: p;w d\r\n"
    "\r\n"
    "[Paths]\r\n"
    "Password=/3ds/Password\r\n";

static std::string openedValue(const INIReader &r, const char *section, const char *name)
{
    std::string v = r.Get(section, name, ""), out;
    return openSealedValue(kKey, section, name, v, out) ? out : "<not sealed: " + v + ">";
}

TEST(sealini, seals_the_secrets_and_keeps_everything_else)
{
    SealResult r = sealIniText(kIni, kKey, countingRandom);
    CHECK_EQ(r.sealed, 2);
    CHECK_EQ(r.skipped, 0);
    CHECK(r.text.find("abc.def") == std::string::npos);
    CHECK(r.text.find("p;w d") == std::string::npos);

    // Byte for byte elsewhere: the BOM, the comments, CRLF, the separator
    // style, and the inline comment after the sealed value.
    const std::string head = "\xEF\xBB\xBF; 3DSync config\r\n";
    CHECK(r.text.compare(0, head.size(), head) == 0);
    CHECK(r.text.find("AppKey=y88zxdudhuoo41q\r\n") != std::string::npos);
    CHECK(r.text.find("RefreshToken = enc:v1:") != std::string::npos);
    CHECK(r.text.find(" ; from the configurator\r\n") != std::string::npos);
    CHECK(r.text.find("Password: enc:v1:") != std::string::npos);
    CHECK(r.text.find("Password=/3ds/Password\r\n") != std::string::npos);

    // And inih reads it back to the same values.
    INIReader reader(r.text.data(), r.text.size());
    CHECK_EQ(reader.ParseError(), 0);
    CHECK_STR_EQ(openedValue(reader, "Dropbox", "RefreshToken"), "abc.def");
    CHECK_STR_EQ(openedValue(reader, "SMB", "Password"), "p;w d");
    CHECK_STR_EQ(reader.Get("Dropbox", "AppKey", ""), "y88zxdudhuoo41q");
    CHECK_STR_EQ(reader.Get("Paths", "Password", ""), "/3ds/Password");
}

TEST(sealini, a_second_pass_changes_nothing)
{
    SealResult first = sealIniText(kIni, kKey, countingRandom);
    SealResult second = sealIniText(first.text, kKey, countingRandom);
    CHECK_EQ(second.sealed, 0);
    CHECK(second.text == first.text);
}

TEST(sealini, a_new_plain_value_next_to_sealed_ones_is_sealed)
{
    // How a password is changed: type the new one in, start the app.
    SealResult first = sealIniText(kIni, kKey, countingRandom);
    std::string edited = first.text + "[FTP]\nHost=h\nPassword=new\n";
    SealResult second = sealIniText(edited, kKey, countingRandom);
    CHECK_EQ(second.sealed, 1);
    INIReader reader(second.text.data(), second.text.size());
    CHECK_STR_EQ(openedValue(reader, "FTP", "Password"), "new");
    CHECK_STR_EQ(openedValue(reader, "SMB", "Password"), "p;w d");
}

TEST(sealini, leaves_values_inih_would_join)
{
    // An indented line continues the previous value, and a repeated key is
    // appended to it; sealing one line of either would change what is read.
    const char *ini = "[SMB]\nPassword=first\n  second\n[FTP]\nPassword=a\nPassword=b\n"
                      "[WebDAV]\nPassword=\n";
    SealResult r = sealIniText(ini, kKey, countingRandom);
    CHECK_EQ(r.sealed, 0);
    CHECK_EQ(r.skipped, 3);
    CHECK(r.text == ini);
}

TEST(sealini, leaves_a_value_that_would_overflow_the_line)
{
    std::string ini = "[Dropbox]\nToken=" + std::string(1900, 'x') + "\n";
    SealResult r = sealIniText(ini, kKey, countingRandom);
    CHECK_EQ(r.sealed, 0);
    CHECK_EQ(r.skipped, 1);
}

TEST(sealini, no_trailing_newline)
{
    SealResult r = sealIniText("[SMB]\nPassword=x", kKey, countingRandom);
    CHECK_EQ(r.sealed, 1);
    INIReader reader(r.text.data(), r.text.size());
    CHECK_STR_EQ(openedValue(reader, "SMB", "Password"), "x");
}

// ---------------------------------------------------------------------------
// The file on the card
// ---------------------------------------------------------------------------
TEST(configseal, seals_the_file_in_place)
{
    TempDir dir;
    dir.write("/3DSync.ini", kIni);
    std::string path = dir.path() + "/3DSync.ini";
    CHECK_EQ(sealConfigFile(path, kKey, countingRandom), 2);
    std::string text = dir.read("/3DSync.ini");
    CHECK(text.find("abc.def") == std::string::npos);
    CHECK(text.find("enc:v1:") != std::string::npos);
    CHECK(!dir.exists("/3DSync.ini.sealed"));
    CHECK(!dir.exists("/3DSync.ini.3dstmp"));
    CHECK_EQ(sealConfigFile(path, kKey, countingRandom), 0); // already sealed
}

TEST(configseal, nothing_to_seal_leaves_the_file_alone)
{
    TempDir dir;
    dir.write("/3DSync.ini", "[FTP]\nHost=h\n");
    CHECK_EQ(sealConfigFile(dir.path() + "/3DSync.ini", kKey, countingRandom), 0);
    CHECK_STR_EQ(dir.read("/3DSync.ini"), "[FTP]\nHost=h\n");
    CHECK_EQ(sealConfigFile(dir.path() + "/missing.ini", kKey, countingRandom), 0);
}

// A seal interrupted at each point, and a file the user replaced meanwhile.
// The original and its sealed copy are the real thing, so the nonce check in
// sealedCopyBelongsTo() sees what it would on the card.
static const std::string kPlain = "[SMB]\nServer=nas\nPassword=plain-password\n[Paths]\nsaves=/s\n";

static std::string sealedOf(const std::string &text)
{
    return sealIniText(text, kKey, countingRandom).text;
}

TEST(configseal, a_copy_left_before_the_overwrite_is_applied)
{
    TempDir dir;
    std::string sealed = sealedOf(kPlain);
    dir.write("/3DSync.ini", kPlain);
    dir.write("/3DSync.ini.sealed", sealed);
    CHECK(recoverConfigSeal(dir.path() + "/3DSync.ini", &kKey));
    CHECK_STR_EQ(dir.read("/3DSync.ini"), sealed);
    CHECK(!dir.exists("/3DSync.ini.sealed"));
}

TEST(configseal, a_copy_left_during_the_overwrite_is_applied)
{
    std::string sealed = sealedOf(kPlain);
    size_t first = sealed.find("enc:v1:");
    // Power lost partway: the start is the copy's, the rest still the old file.
    for (size_t k : {first + 30, sealed.size() - 3})
    {
        std::string partial = sealed.substr(0, k) + (k < kPlain.size() ? kPlain.substr(k) : "");
        CHECK(sealedCopyBelongsTo(partial, sealed, &kKey));
        CHECK(sealedCopyBelongsTo(partial, sealed, NULL)); // needs no key
    }
}

TEST(configseal, a_copy_left_after_the_overwrite_is_only_removed)
{
    TempDir dir;
    std::string sealed = sealedOf(kPlain);
    dir.write("/3DSync.ini", sealed);
    dir.write("/3DSync.ini.sealed", sealed);
    CHECK(recoverConfigSeal(dir.path() + "/3DSync.ini", NULL));
    CHECK_STR_EQ(dir.read("/3DSync.ini"), sealed);
    CHECK(!dir.exists("/3DSync.ini.sealed"));
}

TEST(configseal, a_file_replaced_since_keeps_and_drops_the_copy)
{
    // The user copied a new 3DSync.ini onto the card after the interruption:
    // same layout, other password, other server.  It must not be overwritten
    // with the old configuration.
    TempDir dir;
    std::string sealed = sealedOf(kPlain);
    std::string mine = "[SMB]\nServer=nas\nPassword=new-password\n[Paths]\nsaves=/s\n";
    dir.write("/3DSync.ini", mine);
    dir.write("/3DSync.ini.sealed", sealed);
    CHECK(!recoverConfigSeal(dir.path() + "/3DSync.ini", &kKey));
    CHECK_STR_EQ(dir.read("/3DSync.ini"), mine);
    CHECK(!dir.exists("/3DSync.ini.sealed"));

    CHECK(!sealedCopyBelongsTo("[SMB]\nServer=other\n", sealed, &kKey));
    // Not even an earlier sealed value carried into the new file at the same
    // place counts, unless the copy's own new nonce is there too.
    CHECK(!sealedCopyBelongsTo(sealed.substr(0, sealed.find("enc:v1:") + 10) + "xx", sealed, NULL));
}

TEST(configseal, reports_the_secrets_it_leaves_plain)
{
    TempDir dir;
    dir.write("/3DSync.ini", "[SMB]\nPassword=a\nPassword=b\n[FTP]\nHost=h\nPassword=c\n");
    std::vector<std::string> skipped;
    CHECK_EQ(sealConfigFile(dir.path() + "/3DSync.ini", kKey, countingRandom, &skipped), 1);
    CHECK_EQ(skipped.size(), (size_t)2); // both lines of the repeated one
    CHECK_STR_EQ(skipped[0], "smb=password");
}

TEST(sealini, opening_a_sealed_text_gives_back_the_original)
{
    std::string sealed = sealedOf(kPlain);
    std::string opened;
    CHECK(openIniText(sealed, kKey, opened));
    CHECK_STR_EQ(opened, kPlain);
    CHECK(!openIniText(sealed, kOtherConsole, opened));
}

TEST(configseal, an_unfinished_copy_left_behind_is_dropped)
{
    // Power lost while ".3dstmp" was still being written: the original is
    // the only complete version.
    TempDir dir;
    dir.write("/3DSync.ini", "[SMB]\nPassword=plain\n");
    dir.write("/3DSync.ini.3dstmp", "[SMB]\nPass");
    recoverConfigSeal(dir.path() + "/3DSync.ini", &kKey);
    CHECK_STR_EQ(dir.read("/3DSync.ini"), "[SMB]\nPassword=plain\n");
    CHECK(!dir.exists("/3DSync.ini.3dstmp"));
}

TEST(sealini, seals_a_labelled_remote)
{
    std::string ini = "[SMB:Enc]\nServer=nas\nPassword=3dsync\n";
    SealResult r = sealIniText(ini, kKey, countingRandom);
    CHECK_EQ(r.sealed, 1);
    CHECK(r.text.find("3dsync") == std::string::npos);
    INIReader reader(r.text.data(), r.text.size());
    CHECK_STR_EQ(openedValue(reader, "SMB:Enc", "Password"), "3dsync");
}
