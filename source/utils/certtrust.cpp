#include "certtrust.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace
{

const size_t DIGEST_SIZE = 32;

int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

// Standard base64 with padding.  Whitespace is the caller's to remove.
bool base64Decode(const std::string &in, std::string &out)
{
    out.clear();
    if (in.empty() || in.size() % 4 != 0)
        return false;
    for (size_t i = 0; i < in.size(); i += 4)
    {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++)
        {
            char c = in[i + k];
            if (c == '=' && i + 4 == in.size() && k >= 2)
            {
                pad++;
                v[k] = 0;
                continue;
            }
            if (pad > 0) // data after '='
                return false;
            v[k] = base64Value(c);
            if (v[k] < 0)
                return false;
        }
        unsigned bits = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        out += (char)(bits >> 16);
        if (pad < 2)
            out += (char)((bits >> 8) & 0xff);
        if (pad < 1)
            out += (char)(bits & 0xff);
    }
    return true;
}

std::string withoutSpaces(const std::string &s, bool colonsToo)
{
    std::string out;
    for (char c : s)
        if (!isspace((unsigned char)c) && !(colonsToo && c == ':'))
            out += c;
    return out;
}

std::vector<std::string> splitCommas(const std::string &value)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true)
    {
        size_t comma = value.find(',', start);
        out.push_back(value.substr(start, comma == std::string::npos ? std::string::npos
                                                                      : comma - start));
        if (comma == std::string::npos)
            return out;
        start = comma + 1;
    }
}

bool parseDigest(const std::string &item, std::string &digest)
{
    std::string s = withoutSpaces(item, true);
    if (s.size() > 8 && strncasecmp(s.c_str(), "sha256//", 8) == 0)
        return base64Decode(s.substr(8), digest) && digest.size() == DIGEST_SIZE;
    if (s.size() != DIGEST_SIZE * 2)
        return false;
    digest.clear();
    for (size_t i = 0; i < s.size(); i += 2)
    {
        if (!isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1]))
            return false;
        digest += (char)strtol(s.substr(i, 2).c_str(), NULL, 16);
    }
    return true;
}

// One DER element filling the whole buffer, a SEQUENCE as every certificate
// is.  Not a certificate parser: mbedTLS does that when it connects.  This
// catches a value cut short or one that is not a certificate at all.
bool isOneDerSequence(const std::string &der)
{
    if (der.size() < 2 || (unsigned char)der[0] != 0x30)
        return false;
    size_t len = (unsigned char)der[1];
    size_t header = 2;
    if (len & 0x80)
    {
        size_t bytes = len & 0x7f;
        if (bytes == 0 || bytes > 4 || der.size() < 2 + bytes)
            return false;
        len = 0;
        for (size_t i = 0; i < bytes; i++)
            len = (len << 8) | (unsigned char)der[2 + i];
        header += bytes;
    }
    return header + len == der.size();
}

// One DER element at der[at]: its tag, and where its contents start and end.
// False if it runs past end.
bool readTlv(const std::string &der, size_t at, size_t end, unsigned char &tag, size_t &body,
             size_t &next)
{
    if (at + 2 > end)
        return false;
    tag = (unsigned char)der[at];
    size_t len = (unsigned char)der[at + 1];
    body = at + 2;
    if (len & 0x80)
    {
        size_t bytes = len & 0x7f;
        if (bytes == 0 || bytes > 4 || body + bytes > end)
            return false;
        len = 0;
        for (size_t i = 0; i < bytes; i++)
            len = (len << 8) | (unsigned char)der[body + i];
        body += bytes;
    }
    if (len > end - body)
        return false;
    next = body + len;
    return true;
}

bool parseIpv4(const std::string &host, unsigned char ip[4])
{
    int part = 0;
    size_t i = 0;
    for (; part < 4; part++)
    {
        size_t start = i;
        unsigned value = 0;
        while (i < host.size() && isdigit((unsigned char)host[i]) && i - start < 3)
            value = value * 10 + (host[i++] - '0');
        if (i == start || value > 255)
            return false;
        ip[part] = (unsigned char)value;
        if (part < 3 && (i >= host.size() || host[i++] != '.'))
            return false;
    }
    return i == host.size();
}

void replaceAll(std::string &s, const char *what, const char *with)
{
    size_t n = strlen(what);
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at))
        s.replace(at, n, with);
}

} // namespace

bool parsePinnedKeys(const std::string &value, std::vector<std::string> &out)
{
    out.clear();
    for (auto &item : splitCommas(value))
    {
        if (withoutSpaces(item, false).empty())
            continue; // "a, b," or a lone comma
        std::string digest;
        if (!parseDigest(item, digest))
        {
            out.clear();
            return false;
        }
        out.push_back(digest);
    }
    return !out.empty();
}

bool parseCaCerts(const std::string &value, std::vector<std::string> &out)
{
    out.clear();
    std::string s = value;
    replaceAll(s, "-----BEGIN CERTIFICATE-----", ",");
    replaceAll(s, "-----END CERTIFICATE-----", ",");
    for (auto &item : splitCommas(s))
    {
        std::string b64 = withoutSpaces(item, false);
        if (b64.empty())
            continue;
        std::string der;
        if (!base64Decode(b64, der) || !isOneDerSequence(der))
        {
            out.clear();
            return false;
        }
        out.push_back(der);
    }
    return !out.empty();
}

std::string urlHost(const std::string &url)
{
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos
                                                                       : end - start);
    size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[') // IPv6, which the 3DS lacks
        return authority.substr(1, authority.find(']') - 1);
    return authority.substr(0, authority.find(':'));
}

bool certificateListsIp(const std::string &der, const std::string &host)
{
    unsigned char ip[4];
    if (!parseIpv4(host, ip))
        return false;
    // Certificate ::= SEQUENCE { tbsCertificate SEQUENCE { ... [3] extensions } ... }
    unsigned char tag;
    size_t body, next, tbsBody, tbsEnd;
    if (!readTlv(der, 0, der.size(), tag, body, next) || tag != 0x30 ||
        !readTlv(der, body, next, tag, tbsBody, tbsEnd) || tag != 0x30)
        return false;
    // The extensions are the [3] element; every field before it is skipped.
    size_t at = tbsBody, extBody = 0, extEnd = 0;
    while (at < tbsEnd)
    {
        if (!readTlv(der, at, tbsEnd, tag, body, next))
            return false;
        if (tag == 0xa3)
        {
            extBody = body;
            extEnd = next;
        }
        at = next;
    }
    // [3] { SEQUENCE OF Extension }
    if (extEnd == 0 || !readTlv(der, extBody, extEnd, tag, body, next) || tag != 0x30)
        return false;
    static const char SAN_OID[] = "\x06\x03\x55\x1d\x11"; // 2.5.29.17
    for (at = body; at < next;)
    {
        // Extension ::= SEQUENCE { OID, critical BOOLEAN OPTIONAL, OCTET STRING }
        size_t eBody, eEnd;
        if (!readTlv(der, at, next, tag, eBody, eEnd) || tag != 0x30)
            return false;
        at = eEnd;
        if (der.compare(eBody, 5, SAN_OID, 5) != 0)
            continue;
        size_t field = eBody, fBody, fEnd = eBody;
        while (field < eEnd)
        {
            if (!readTlv(der, field, eEnd, tag, fBody, fEnd))
                return false;
            if (tag == 0x04) // the value: GeneralNames ::= SEQUENCE OF GeneralName
                break;
            field = fEnd;
        }
        size_t gBody, gEnd;
        if (field >= eEnd || !readTlv(der, fBody, fEnd, tag, gBody, gEnd) || tag != 0x30)
            return false;
        for (size_t name = gBody; name < gEnd;)
        {
            size_t nBody, nEnd;
            if (!readTlv(der, name, gEnd, tag, nBody, nEnd))
                return false;
            // iPAddress [7] OCTET STRING, 4 bytes for IPv4.
            if (tag == 0x87 && nEnd - nBody == 4 && memcmp(der.data() + nBody, ip, 4) == 0)
                return true;
            name = nEnd;
        }
        return false;
    }
    return false;
}

std::vector<std::string> fingerprintLines(const std::string &digest)
{
    static const char HEX[] = "0123456789ABCDEF";
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < digest.size(); i++)
    {
        unsigned char b = (unsigned char)digest[i];
        if (!line.empty())
            line += ':';
        line += HEX[b >> 4];
        line += HEX[b & 0x0f];
        if (i % 16 == 15)
        {
            lines.push_back(line);
            line.clear();
        }
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}
