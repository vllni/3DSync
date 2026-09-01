#include "debug.h"

#include "console.h"
#include "fsutil.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <errno.h>

// One bad response should not scroll the rest of the run off a 30-line screen,
// so a dumped body is cut here.  The real length is printed when it is.
static const size_t MAX_BODY_DUMP = 512;

static const char *REDACTED = "<redacted>";

static bool g_debugEnabled = false;

void setDebugEnabled(bool enabled) { g_debugEnabled = enabled; }
bool debugEnabled() { return g_debugEnabled; }

void debugf(const char *format, ...)
{
    if (!g_debugEnabled)
        return;

    printf(CONSOLE_MAGENTA "dbg: ");
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf(CONSOLE_RESET);
}

// ---------------------------------------------------------------------------
// Redaction
// ---------------------------------------------------------------------------

static size_t findNoCase(const std::string &haystack, const std::string &needle,
                         size_t from)
{
    if (needle.empty() || haystack.size() < needle.size())
        return std::string::npos;

    for (size_t i = from; i + needle.size() <= haystack.size(); i++)
    {
        size_t j = 0;
        while (j < needle.size() &&
               tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == needle.size())
            return i;
    }
    return std::string::npos;
}

// "access_token": "ya29...."  ->  "access_token": "<redacted>"
static void redactJsonValue(std::string &text, const char *key)
{
    std::string marker = std::string("\"") + key + "\"";
    size_t pos = 0;

    while ((pos = findNoCase(text, marker, pos)) != std::string::npos)
    {
        size_t at = pos + marker.size();
        pos = at;

        while (at < text.size() && isspace((unsigned char)text[at]))
            at++;
        if (at >= text.size() || text[at] != ':')
            continue;
        at++;
        while (at < text.size() && isspace((unsigned char)text[at]))
            at++;
        if (at >= text.size() || text[at] != '"')
            continue;

        size_t valueStart = at + 1;
        size_t valueEnd = text.find('"', valueStart);
        if (valueEnd == std::string::npos)
            valueEnd = text.size();
        if (valueEnd == valueStart)
            continue; // already empty, nothing to hide

        text.replace(valueStart, valueEnd - valueStart, REDACTED);
        pos = valueStart + strlen(REDACTED);
    }
}

// refresh_token=1//abc&client_id=...  ->  refresh_token=<redacted>&client_id=...
static void redactFormValue(std::string &text, const char *key)
{
    std::string marker = std::string(key) + "=";
    size_t pos = 0;

    while ((pos = findNoCase(text, marker, pos)) != std::string::npos)
    {
        // Only at the start of a parameter, so "client_id=" is not matched by
        // a search for "id=".
        if (pos != 0)
        {
            char before = text[pos - 1];
            if (before != '&' && before != '?' && before != '\n' && before != ' ')
            {
                pos += marker.size();
                continue;
            }
        }

        size_t valueStart = pos + marker.size();
        size_t valueEnd = valueStart;
        while (valueEnd < text.size() && strchr("&\r\n \"", text[valueEnd]) == NULL)
            valueEnd++;

        if (valueEnd == valueStart)
        {
            pos = valueStart;
            continue;
        }
        text.replace(valueStart, valueEnd - valueStart, REDACTED);
        pos = valueStart + strlen(REDACTED);
    }
}

// Authorization: Bearer ya29...  ->  Authorization: <redacted>
static void redactHeaderValue(std::string &text, const char *name)
{
    std::string marker = std::string(name) + ":";
    size_t pos = 0;

    while ((pos = findNoCase(text, marker, pos)) != std::string::npos)
    {
        size_t valueStart = pos + marker.size();
        while (valueStart < text.size() && text[valueStart] == ' ')
            valueStart++;

        size_t valueEnd = valueStart;
        while (valueEnd < text.size() && text[valueEnd] != '\r' && text[valueEnd] != '\n')
            valueEnd++;

        if (valueEnd == valueStart)
        {
            pos = valueStart;
            continue;
        }
        text.replace(valueStart, valueEnd - valueStart, REDACTED);
        pos = valueStart + strlen(REDACTED);
    }
}

std::string debugRedact(const std::string &text)
{
    std::string out = text;

    // OAuth responses (Drive and Dropbox both return these).
    redactJsonValue(out, "access_token");
    redactJsonValue(out, "refresh_token");
    redactJsonValue(out, "id_token");
    redactJsonValue(out, "client_secret");
    redactJsonValue(out, "password");

    // Token request bodies, and anything echoed back as a query string.
    redactFormValue(out, "access_token");
    redactFormValue(out, "refresh_token");
    redactFormValue(out, "client_secret");
    redactFormValue(out, "code_verifier");
    redactFormValue(out, "password");

    // Request headers, which curl's trace prints verbatim.
    redactHeaderValue(out, "authorization");
    redactHeaderValue(out, "proxy-authorization");
    redactHeaderValue(out, "cookie");
    redactHeaderValue(out, "set-cookie");

    return out;
}

// ---------------------------------------------------------------------------
// Body dumps
// ---------------------------------------------------------------------------

void debugBody(const char *label, const std::string &body)
{
    if (!g_debugEnabled)
        return;

    if (body.empty())
    {
        debugf("%s: <empty>\n", label);
        return;
    }

    std::string safe = debugRedact(body);
    if (safe.size() <= MAX_BODY_DUMP)
    {
        debugf("%s (%d bytes):\n%s\n", label, (int)body.size(), safe.c_str());
        return;
    }
    debugf("%s (%d bytes, first %d shown):\n%s...\n", label, (int)body.size(),
           (int)MAX_BODY_DUMP, safe.substr(0, MAX_BODY_DUMP).c_str());
}

void debugFileBody(const char *label, const std::string &path)
{
    if (!g_debugEnabled)
        return;

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        debugf("%s: cannot reopen %s: %s\n", label, path.c_str(), strerror(errno));
        return;
    }

    // Only the head is wanted, and the file may be a half-downloaded save.
    char buf[MAX_BODY_DUMP + 1];
    size_t n = fread(buf, 1, MAX_BODY_DUMP, fp);

    long total = -1;
    if (fseek(fp, 0, SEEK_END) == 0)
        total = ftell(fp);
    fclose(fp);

    std::string body(buf, n);
    // Binary bytes would leave the console in an odd state, so they are shown
    // as dots — enough to tell "this is a save file" from "this is JSON".
    for (size_t i = 0; i < body.size(); i++)
    {
        unsigned char c = (unsigned char)body[i];
        if (c < 0x20 && c != '\n' && c != '\t')
            body[i] = '.';
    }

    if (total > (long)n)
        debugf("%s (%ld bytes on disk, first %d shown):\n%s\n", label, total, (int)n,
               debugRedact(body).c_str());
    else
        debugf("%s (%d bytes):\n%s\n", label, (int)n, debugRedact(body).c_str());
}

void debugErrno(const char *what, const std::string &path)
{
    if (!g_debugEnabled)
        return;

    // Some callers arrive here from ferror(), which reports a failure without
    // setting errno — better to say so than to attribute a stale one.
    if (errno == 0)
        debugf("%s failed for %s (no errno reported)\n", what, path.c_str());
    else
        debugf("%s failed for %s: %s (errno %d)\n", what, path.c_str(),
               strerror(errno), errno);
}

// ---------------------------------------------------------------------------
// Session log
// ---------------------------------------------------------------------------
// A long sync can print far more than is worth holding in memory on a console
// with a few megabytes to spare, and the two parts that matter are opposite
// ends of the run: the head has the version, the init log and the config
// summary, the tail has whatever went wrong last.  So the head is kept up to
// LOG_HEAD_MAX and everything after that goes into a tail window of at most
// LOG_TAIL_MAX, with the bytes squeezed out in between counted rather than
// quietly lost.
static const size_t LOG_HEAD_MAX = 96 * 1024;
static const size_t LOG_TAIL_MAX = 48 * 1024;

static std::string g_logHead;
static std::string g_logTail;
static size_t g_logDropped = 0;

void debugLogReset()
{
    g_logHead.clear();
    g_logTail.clear();
    g_logDropped = 0;
}

void debugLogAppend(const char *data, size_t length)
{
    if (!g_debugEnabled || data == NULL || length == 0)
        return;

    if (g_logHead.size() < LOG_HEAD_MAX)
    {
        size_t take = LOG_HEAD_MAX - g_logHead.size();
        if (take > length)
            take = length;
        g_logHead.append(data, take);
        data += take;
        length -= take;
        if (length == 0)
            return;
    }

    g_logTail.append(data, length);
    // Trimming one write's worth off the front on every append would memmove
    // the whole window each time, so the tail is allowed to run to twice its
    // size and then halved in one go.
    if (g_logTail.size() > LOG_TAIL_MAX * 2)
    {
        size_t drop = g_logTail.size() - LOG_TAIL_MAX;
        g_logTail.erase(0, drop);
        g_logDropped += drop;
    }
}

size_t debugLogSize() { return g_logHead.size() + g_logTail.size(); }
size_t debugLogDropped() { return g_logDropped; }

// Console colours are written as CSI sequences, which are noise in a text file
// and hide the text they wrap from debugRedact().  Both are stripped here:
// ESC [ ... final-byte, and a bare ESC that got cut off at a buffer edge.
static std::string stripAnsi(const std::string &text)
{
    std::string out;
    out.reserve(text.size());

    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] != '\x1b')
        {
            out += text[i];
            continue;
        }
        if (i + 1 >= text.size() || text[i + 1] != '[')
            continue; // lone ESC, drop it
        size_t j = i + 2;
        while (j < text.size() && (unsigned char)text[j] >= 0x20 &&
               (unsigned char)text[j] <= 0x3f)
            j++;
        if (j < text.size())
            j++; // the final byte of the sequence
        i = j - 1;
    }
    return out;
}

bool debugLogSave(const std::string &path)
{
    // Redaction runs over the finished text rather than per write: a token can
    // straddle two console writes, and by here the sequences that would have
    // split a match are gone.
    std::string text = debugRedact(stripAnsi(g_logHead));
    if (g_logDropped > 0)
    {
        char note[96];
        snprintf(note, sizeof(note),
                 "\n... %u bytes dropped from the middle of the run ...\n\n",
                 (unsigned)g_logDropped);
        text += note;
    }
    text += debugRedact(stripAnsi(g_logTail));

    mkparents(path);

    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        debugErrno("fopen", path);
        return false;
    }

    size_t written = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), fp);
    bool ok = (written == text.size());
    if (!ok)
        debugErrno("fwrite", path);
    if (fclose(fp) != 0)
    {
        debugErrno("fclose", path);
        ok = false;
    }
    return ok;
}
