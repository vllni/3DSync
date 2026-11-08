#include "debug.h"

#include "console.h"
#include "fsutil.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
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

// FTP sends the password as a command of its own, "PASS hunter2", which
// curl's trace prints like any other outgoing line (ACCT likewise).  Found at
// the start of a line, after what the trace or the log put in front of it
// ("dbg: ", "> "), so "bypass" or "PASS" inside a sentence is left alone.
static void redactFtpCommand(std::string &text, const char *command)
{
    size_t len = strlen(command);
    size_t lineStart = 0;
    while (lineStart < text.size())
    {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = text.size();
        size_t p = lineStart;
        // The log keeps colour codes until it is saved: skip "\x1b[..m" too.
        while (p < lineEnd)
        {
            if (text[p] == ' ' || text[p] == '>')
                p++;
            else if (text.compare(p, 4, "dbg:") == 0)
                p += 4;
            else if (text[p] == '\x1b')
            {
                size_t m = text.find('m', p);
                p = (m == std::string::npos || m >= lineEnd) ? lineEnd : m + 1;
            }
            else
                break;
        }
        if (lineEnd - p > len && strncasecmp(text.c_str() + p, command, len) == 0 &&
            text[p + len] == ' ')
        {
            size_t valueStart = p + len + 1;
            size_t valueEnd = lineEnd;
            while (valueEnd > valueStart && text[valueEnd - 1] == '\r')
                valueEnd--;
            if (valueEnd > valueStart)
            {
                text.replace(valueStart, valueEnd - valueStart, REDACTED);
                lineEnd = valueStart + strlen(REDACTED) + (lineEnd - valueEnd);
            }
        }
        lineStart = lineEnd + 1;
    }
}

// ftp://bob:hunter2@host/  ->  ftp://bob:<redacted>@host/ — a password
// written into a WebDAV Url= or an FTP URL is printed with every request.
static void redactUrlPassword(std::string &text)
{
    size_t pos = 0;
    while ((pos = text.find("://", pos)) != std::string::npos)
    {
        size_t authStart = pos + 3;
        size_t authEnd = authStart;
        while (authEnd < text.size() && text[authEnd] != '/' && text[authEnd] != '@' &&
               text[authEnd] != ' ' && text[authEnd] != '\n' && text[authEnd] != '\r' &&
               text[authEnd] != '"' && text[authEnd] != '?' && text[authEnd] != '#')
            authEnd++;
        size_t colon = text.find(':', authStart);
        if (authEnd < text.size() && text[authEnd] == '@' && colon != std::string::npos &&
            colon < authEnd)
        {
            text.replace(colon + 1, authEnd - colon - 1, REDACTED);
            authEnd = colon + 1 + strlen(REDACTED);
        }
        pos = authEnd;
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

    // FTP's own login commands, and credentials written into a URL.
    redactFtpCommand(out, "PASS");
    redactFtpCommand(out, "ACCT");
    redactUrlPassword(out);

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
// Written to the SD card as the run goes, not held in memory: a long sync
// prints far more than the console can spare, and trimming the middle away
// lost exactly the part a report needed.  Each complete line is stripped of
// colour codes and passed through debugRedact() on its way out, so the file
// never holds a token, even while the run is still going.  Lines, because a
// token may arrive in two console writes but never spans a line break.
// ---------------------------------------------------------------------------
static FILE *g_logFile = NULL;
static std::string g_logSpool;   // the file being written
static std::string g_logPending; // the start of a line not yet ended
static size_t g_logWritten = 0;
static bool g_logFailed = false;
// A dump without line breaks is written out once it gets this long rather
// than held: a body is already redacted when it is dumped.
static const size_t LOG_PENDING_MAX = 64 * 1024;

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

static void logWriteOut(const std::string &text)
{
    if (g_logFile == NULL || g_logFailed || text.empty())
        return;
    std::string clean = debugRedact(stripAnsi(text));
    // No printf here: stdout is what feeds this.
    if (fwrite(clean.data(), 1, clean.size(), g_logFile) != clean.size())
        g_logFailed = true;
    else
        g_logWritten += clean.size();
}

void debugLogReset()
{
    if (g_logFile != NULL)
    {
        fclose(g_logFile);
        remove(g_logSpool.c_str());
    }
    g_logFile = NULL;
    g_logSpool.clear();
    g_logPending.clear();
    g_logWritten = 0;
    g_logFailed = false;
}

bool debugLogStart(const std::string &spoolPath)
{
    debugLogReset();
    mkparents(spoolPath);
    g_logFile = fopen(spoolPath.c_str(), "wb");
    if (g_logFile == NULL)
        return false;
    // Whole blocks to the card, not a write per console line.
    setvbuf(g_logFile, NULL, _IOFBF, 16 * 1024);
    g_logSpool = spoolPath;
    return true;
}

void debugLogAppend(const char *data, size_t length)
{
    if (g_logFile == NULL || data == NULL || length == 0)
        return;
    g_logPending.append(data, length);
    size_t end = g_logPending.rfind('\n');
    if (end != std::string::npos)
    {
        logWriteOut(g_logPending.substr(0, end + 1));
        g_logPending.erase(0, end + 1);
    }
    if (g_logPending.size() > LOG_PENDING_MAX)
    {
        logWriteOut(g_logPending);
        g_logPending.clear();
    }
}

size_t debugLogSize() { return g_logWritten + g_logPending.size(); }
bool debugLogFailed() { return g_logFailed; }

bool debugLogKeep(const std::string &path)
{
    if (g_logFile == NULL)
        return false;
    logWriteOut(g_logPending);
    g_logPending.clear();
    bool ok = !g_logFailed;
    if (fclose(g_logFile) != 0)
        ok = false;
    g_logFile = NULL;
    // FAT will not rename over an existing file.
    remove(path.c_str());
    if (rename(g_logSpool.c_str(), path.c_str()) != 0)
    {
        debugErrno("rename", path);
        ok = false;
    }
    g_logSpool.clear();
    g_logWritten = 0;
    return ok;
}
