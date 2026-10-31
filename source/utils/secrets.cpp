#include "secrets.h"

#include <ctype.h>
#include <string.h>
#include <map>
#include <vector>

#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include "debug.h"

#ifndef INI_MAX_LINE
#define INI_MAX_LINE 2048
#endif

static const char kPrefix[] = "enc:v1:";
static const size_t kPrefixLen = sizeof(kPrefix) - 1;
static const size_t kNonceLen = 12, kTagLen = 16;

static std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = (char)tolower((unsigned char)c);
    return out;
}

SealKey deriveSealKey(uint64_t consoleHash)
{
    unsigned char ikm[8];
    for (int i = 0; i < 8; i++)
        ikm[i] = (unsigned char)(consoleHash >> (8 * i));
    static const unsigned char salt[] = "3DSync";
    static const unsigned char info[] = "3DSync.ini credential seal v1";

    SealKey key;
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), salt, sizeof(salt) - 1, ikm,
                 sizeof(ikm), info, sizeof(info) - 1, key.bytes, sizeof(key.bytes));
    return key;
}

bool isSealableSecret(const std::string &section, const std::string &name)
{
    static const char *const fields[] = {
        "dropbox=token",     "dropbox=refreshtoken",     "dropbox=appsecret",
        "googledrive=token", "googledrive=clientsecret", "googledrive=refreshtoken",
        "smb=password",      "ftp=password",             "webdav=password",
    };
    // A labelled remote ("[SMB:Backup]") has the fields of its type.  The
    // associated data stays the full section, so a value moved between two
    // remotes of one type still does not open.
    std::string type = lower(section.substr(0, section.find(':')));
    std::string key = type + "=" + lower(name);
    for (const char *f : fields)
        if (key == f)
            return true;
    return false;
}

bool isSealedValue(const std::string &value)
{
    return value.compare(0, kPrefixLen, kPrefix) == 0;
}

// Associated data: the field as INIReader names it, so a sealed password
// pasted into another field fails to open instead of being used there.
static std::string aadFor(const std::string &section, const std::string &name)
{
    return lower(section) + "=" + lower(name);
}

bool sealValue(const SealKey &key, const std::string &section, const std::string &name,
               const std::string &plaintext, SealRandom random, std::string &out)
{
    std::vector<unsigned char> blob(kNonceLen + plaintext.size() + kTagLen);
    if (!random(&blob[0], kNonceLen))
        return false;

    std::string aad = aadFor(section, name);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.bytes, 256);
    if (rc == 0)
        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plaintext.size(), &blob[0],
                                       kNonceLen, (const unsigned char *)aad.data(), aad.size(),
                                       (const unsigned char *)plaintext.data(),
                                       &blob[kNonceLen], kTagLen,
                                       &blob[kNonceLen + plaintext.size()]);
    mbedtls_gcm_free(&gcm);
    if (rc != 0)
    {
        debugf("seal %s: AES-GCM failed (-0x%04X)\n", aad.c_str(), (unsigned)-rc);
        return false;
    }

    size_t encLen = 0;
    mbedtls_base64_encode(NULL, 0, &encLen, &blob[0], blob.size());
    std::vector<unsigned char> enc(encLen);
    if (mbedtls_base64_encode(&enc[0], enc.size(), &encLen, &blob[0], blob.size()) != 0)
        return false;
    out = std::string(kPrefix) + std::string((const char *)&enc[0], encLen);
    return true;
}

bool openSealedValue(const SealKey &key, const std::string &section, const std::string &name,
                     const std::string &sealed, std::string &out)
{
    if (!isSealedValue(sealed))
        return false;
    const unsigned char *b64 = (const unsigned char *)sealed.data() + kPrefixLen;
    size_t b64Len = sealed.size() - kPrefixLen;

    size_t blobLen = 0;
    mbedtls_base64_decode(NULL, 0, &blobLen, b64, b64Len);
    if (blobLen < kNonceLen + kTagLen)
        return false;
    std::vector<unsigned char> blob(blobLen);
    if (mbedtls_base64_decode(&blob[0], blob.size(), &blobLen, b64, b64Len) != 0 ||
        blobLen < kNonceLen + kTagLen)
        return false;

    size_t textLen = blobLen - kNonceLen - kTagLen;
    std::vector<unsigned char> text(textLen + 1);
    std::string aad = aadFor(section, name);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key.bytes, 256);
    if (rc == 0)
        rc = mbedtls_gcm_auth_decrypt(&gcm, textLen, &blob[0], kNonceLen,
                                      (const unsigned char *)aad.data(), aad.size(),
                                      &blob[kNonceLen + textLen], kTagLen, &blob[kNonceLen],
                                      &text[0]);
    mbedtls_gcm_free(&gcm);
    if (rc != 0)
        return false;
    out.assign((const char *)&text[0], textLen);
    return true;
}

// ---------------------------------------------------------------------------
// sealIniText
// ---------------------------------------------------------------------------
// Walks the file the way ini_parse_stream() does (see libs/inih/ini.c) and
// records, for every name=value line, where the value starts and ends — the
// same span inih would hand INIReader — so it can be replaced in place.
// ---------------------------------------------------------------------------
namespace
{
struct ValueSpan
{
    size_t begin, end; // offsets into the whole text
    std::string field; // "section=name", lowercased
    std::string section, name;
};

struct Line
{
    size_t begin, end; // content, without the line terminator
};
} // namespace

static bool isSpace(char c) { return isspace((unsigned char)c) != 0; }

// ini_find_chars_or_comment(): the first of `chars`, or a ';' that follows
// whitespace, or the end.
static size_t findCharsOrComment(const std::string &t, size_t pos, size_t end, const char *chars)
{
    bool wasSpace = false;
    while (pos < end && !(chars && strchr(chars, t[pos])) && !(wasSpace && t[pos] == ';'))
    {
        wasSpace = isSpace(t[pos]);
        pos++;
    }
    return pos;
}

static std::vector<ValueSpan> scanValues(const std::string &t, std::map<std::string, int> &uses)
{
    std::vector<ValueSpan> spans;
    std::vector<Line> lines;
    for (size_t pos = 0; pos <= t.size();)
    {
        size_t nl = t.find('\n', pos);
        size_t end = (nl == std::string::npos) ? t.size() : nl;
        Line l = {pos, end};
        lines.push_back(l);
        if (nl == std::string::npos)
            break;
        pos = nl + 1;
    }

    std::string section, prevName;
    for (size_t i = 0; i < lines.size(); i++)
    {
        size_t lineStart = lines[i].begin, end = lines[i].end;
        size_t start = lineStart;
        if (i == 0 && t.compare(0, 3, "\xEF\xBB\xBF") == 0)
            start += 3;
        while (start < end && isSpace(t[start]))
            start++;
        while (end > start && isSpace(t[end - 1]))
            end--;

        if (start == end)
            continue;
        if (t[start] == ';' || t[start] == '#')
            continue;
        if (!prevName.empty() && start > lineStart)
        {
            // Continuation: inih appends this line to the previous value, so
            // that value is no longer a single span.
            uses[lower(section) + "=" + lower(prevName)] += 2;
            continue;
        }
        if (t[start] == '[')
        {
            size_t close = findCharsOrComment(t, start + 1, end, "]");
            if (close < end && t[close] == ']')
            {
                section = t.substr(start + 1, close - start - 1);
                prevName.clear();
            }
            continue;
        }

        size_t sep = findCharsOrComment(t, start, end, "=:");
        if (sep >= end || (t[sep] != '=' && t[sep] != ':'))
            continue;
        size_t nameEnd = sep;
        while (nameEnd > start && isSpace(t[nameEnd - 1]))
            nameEnd--;
        std::string name = t.substr(start, nameEnd - start);

        size_t vEnd = findCharsOrComment(t, sep + 1, end, NULL);
        size_t vBegin = sep + 1;
        while (vBegin < vEnd && isSpace(t[vBegin]))
            vBegin++;
        while (vEnd > vBegin && isSpace(t[vEnd - 1]))
            vEnd--;

        prevName = name;
        ValueSpan span = {vBegin, vEnd, lower(section) + "=" + lower(name), section, name};
        uses[span.field]++;
        spans.push_back(span);
    }
    return spans;
}

SealResult sealIniText(const std::string &text, const SealKey &key, SealRandom random)
{
    SealResult result;
    result.sealed = 0;
    result.skipped = 0;

    std::map<std::string, int> uses;
    std::vector<ValueSpan> spans = scanValues(text, uses);

    std::string out;
    size_t copied = 0;
    for (auto &s : spans)
    {
        if (!isSealableSecret(s.section, s.name))
            continue;
        std::string value = text.substr(s.begin, s.end - s.begin);
        if (value.empty() || isSealedValue(value))
            continue;
        if (uses[s.field] != 1)
        {
            debugf("3DSync.ini: %s repeats or continues on the next line, left as it is\n",
                   s.field.c_str());
            result.skipped++;
            result.skippedFields.push_back(s.field);
            continue;
        }

        std::string sealed;
        if (!sealValue(key, s.section, s.name, value, random, sealed))
        {
            result.skipped++;
            result.skippedFields.push_back(s.field);
            continue;
        }
        size_t lineBegin = text.rfind('\n', s.begin);
        lineBegin = (lineBegin == std::string::npos) ? 0 : lineBegin + 1;
        size_t lineEnd = text.find('\n', s.end);
        if (lineEnd == std::string::npos)
            lineEnd = text.size();
        // inih reads at most INI_MAX_LINE - 1 bytes per line, newline included.
        if ((lineEnd - lineBegin) - value.size() + sealed.size() + 1 >= (size_t)INI_MAX_LINE - 1)
        {
            debugf("3DSync.ini: %s would be too long once sealed, left as it is\n",
                   s.field.c_str());
            result.skipped++;
            result.skippedFields.push_back(s.field);
            continue;
        }

        out.append(text, copied, s.begin - copied);
        out += sealed;
        copied = s.end;
        result.sealed++;
    }
    out.append(text, copied, std::string::npos);
    result.text = out;
    return result;
}

bool openIniText(const std::string &text, const SealKey &key, std::string &out)
{
    std::map<std::string, int> uses;
    std::vector<ValueSpan> spans = scanValues(text, uses);
    out.clear();
    size_t copied = 0;
    for (auto &s : spans)
    {
        std::string value = text.substr(s.begin, s.end - s.begin);
        if (!isSealableSecret(s.section, s.name) || !isSealedValue(value))
            continue;
        std::string plain;
        if (!openSealedValue(key, s.section, s.name, value, plain))
            return false;
        out.append(text, copied, s.begin - copied);
        out += plain;
        copied = s.end;
    }
    out.append(text, copied, std::string::npos);
    return true;
}
