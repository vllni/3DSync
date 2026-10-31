#include <stdio.h>
#include <malloc.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <set>
#include <iostream>
#include <string.h>

#include <3ds.h>

#include <curl/curl.h>

#include "libs/inih/INIReader/INIReader.h"
#include "modules/dropbox.h"
#include "modules/ftpremote.h"
#include "modules/googledrive.h"
#include "modules/manifest.h"
#include "modules/smbremote.h"
#include "modules/syncprovider.h"
#include "modules/webdavremote.h"
#include "sync/remoteconfig.h"
#include "sync/syncengine.h"
#include "ui/frontend.h"
#include "utils/debug.h"
#include "utils/configseal.h"
#include "utils/fsutil.h"
#include "utils/secrets.h"
#include "utils/timeutil.h"
#include "version.h"

// ---------------------------------------------------------------------------
// getConfiguredSyncPaths
// ---------------------------------------------------------------------------
// INIReader lowercases both section and key and stores them as "section=key";
// whether such a key is a sync path, and of which kind.
static bool parsePathKey(const std::string &key, SyncDirection &dir, bool &recursive,
                         std::string &prefix)
{
    static const struct
    {
        const char *prefix;
        SyncDirection dir;
        bool recursive;
    } kinds[] = {{"paths=", SYNC_BOTH, true},
                 {"shallowpaths=", SYNC_BOTH, false},
                 {"uploadpaths=", SYNC_UPLOAD_ONLY, true},
                 {"uploadshallowpaths=", SYNC_UPLOAD_ONLY, false}};
    for (auto &k : kinds)
        if (key.rfind(k.prefix, 0) == 0)
        {
            dir = k.dir;
            recursive = k.recursive;
            prefix = k.prefix;
            return true;
        }
    return false;
}

// For the idle screen, which must not walk the SD card to say how many.
static int countSyncPaths(const INIReader &reader)
{
    int n = 0;
    SyncDirection dir;
    bool recursive;
    std::string prefix;
    for (auto &kv : reader.GetValues())
        if (parsePathKey(kv.first, dir, recursive, prefix))
            n++;
    return n;
}

// The names of the configured sync paths ("saves"), lowercased as INIReader
// keys them, for checking Only= without walking the SD card.
static std::vector<std::string> configuredEntryNames(const INIReader &reader)
{
    std::vector<std::string> names;
    SyncDirection dir;
    bool recursive;
    std::string prefix;
    for (auto &kv : reader.GetValues())
        if (parsePathKey(kv.first, dir, recursive, prefix))
            names.push_back(kv.first.substr(prefix.size()));
    return names;
}

// [Filter]: Extensions= for every entry, "<name>=" for one.  Missing or
// empty: all files.
static PathFilters configuredFilters(const INIReader &reader)
{
    return parsePathFilters(reader.GetValues(), configuredEntryNames(reader));
}

static std::vector<SyncEntry> getConfiguredSyncPaths(const INIReader &reader)
{
    std::vector<SyncEntry> entries;
    std::map<std::string, std::string> values = reader.GetValues();
    PathFilters filters = configuredFilters(reader);

    for (auto &kv : values)
    {
        SyncDirection dir;
        bool recursive;
        std::string prefix;
        if (!parsePathKey(kv.first, dir, recursive, prefix))
            continue;

        SyncEntry entry;
        entry.localBase = kv.second;
        entry.remoteName = kv.first.substr(prefix.size());
        entry.direction = dir;
        entry.filter = filters.forEntry(entry.remoteName);
        const ExtensionFilter &filter = entry.filter;
        // Filtered here as well as in performSync(): a provider's
        // legacyUpload() is handed this list and nothing else.
        std::vector<std::string> found = listLocalFiles(kv.second, recursive);
        for (auto &f : found)
            if (filter.matches(f))
                entry.localFiles.push_back(f);
        // An entry that found nothing locally is silently normal — the remote
        // may hold everything — but it is also what a mistyped path looks like,
        // or a filter that matches none of the files there.
        debugf("entry %s -> %s: %d local file(s), %d left out by the filter, %s%s\n",
               entry.localBase.c_str(), entry.remoteName.c_str(),
               (int)entry.localFiles.size(), (int)(found.size() - entry.localFiles.size()),
               recursive ? "recursive" : "shallow",
               dir == SYNC_UPLOAD_ONLY ? ", upload only" : "");
        entries.push_back(entry);
    }
    return entries;
}

// ---------------------------------------------------------------------------
// offerDebugLogSave  — end of a debug run: keep the transcript or drop it
// ---------------------------------------------------------------------------
// The whole point of debug mode is to produce something that can be attached
// to a bug report, and a screen that has already scrolled is not that.  The
// save is offered rather than done, because the transcript is a plaintext file
// on a shared SD card and the run it came from listed every save path on the
// console — asking is what makes writing it the user's decision.
// ---------------------------------------------------------------------------
static std::string debugLogPath()
{
    time_t now = time(NULL);
    struct tm *lt = (now > 0) ? localtime(&now) : NULL;

    char stamp[32];
    // A clock that has not been set yet would name every run the same; the
    // fixed name is honest about that, and the previous one is overwritten.
    if (lt == NULL || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", lt) == 0)
        snprintf(stamp, sizeof(stamp), "unknown-time");

    return std::string("/3ds/3DSync/logs/debug-") + stamp + ".log";
}

static void offerDebugLogSave()
{
    if (!debugEnabled() || debugLogSize() == 0)
        return;

    // The question itself is on the bottom screen; the log keeps a line so
    // the saved file and the screen agree about what was asked.
    unsigned kb = (unsigned)((debugLogSize() + 1023) / 1024);
    printf("\n--- Debug log ---\n");
    printf("%u KB captured%s.\n", kb,
           debugLogDropped() > 0 ? ", middle of the run trimmed" : "");

    if (!frontend::askSaveDebugLog(kb, debugLogDropped() > 0))
    {
        printf("Debug log discarded.\n");
        return;
    }
    std::string path = debugLogPath();
    // The write itself prints, and the stdout hook would fold that into the
    // very text being written; it is already snapshotted by then, so the
    // file just ends a line or two before the screen does.
    if (debugLogSave(path))
        printf(CONSOLE_GREEN "Saved to\n  %s\n" CONSOLE_RESET, path.c_str());
    else
        printf(CONSOLE_RED "Could not write\n  %s\n" CONSOLE_RESET, path.c_str());
}

// ---------------------------------------------------------------------------
// printExperimentalNotice  — shown once per experimental backend, per run
// ---------------------------------------------------------------------------
// Kept to the console's 50 columns.  The URL is the issue tracker rather than
// the repository root: someone reading this is being asked to report something.
// ---------------------------------------------------------------------------
static const char *ISSUES_URL = "github.com/vllni/3DSync/issues";

static void printExperimentalNotice(const char *backendName)
{
    printf(CONSOLE_YELLOW "  %s support is EXPERIMENTAL." CONSOLE_RESET "\n",
           backendName);
    printf("  It has seen far less testing than Drive, so\n");
    printf("  please keep a backup of your saves.\n");
    printf("  Bugs and feedback are very welcome at\n");
    printf(CONSOLE_CYAN "  %s" CONSOLE_RESET "\n\n", ISSUES_URL);
}

// ---------------------------------------------------------------------------
// ActiveRemote  — a remote built from its section, for the length of a run
// ---------------------------------------------------------------------------
struct ActiveRemote
{
    SyncProvider *provider;
    RemoteConfig config;
    std::string name; // config.displayName(): "SMB" or "SMB (backup)"
};

// ---------------------------------------------------------------------------
// connectRemote  — open one remote before any entry is synced
// ---------------------------------------------------------------------------
// Every remote takes part in each entry at once (see syncEntry()), so all of
// them are connected first.  One that cannot be reached is left out of the run.
// ---------------------------------------------------------------------------
static bool connectRemote(const ActiveRemote &remote, SyncSummary &summary)
{
    SyncProvider &provider = *remote.provider;
    const char *name = remote.name.c_str();
    printf("\n=== %s ===\n", name);
    frontend::setActivity(name, "Connecting\xE2\x80\xA6");
    if (provider.isExperimental())
        printExperimentalNotice(provider.name());

    if (!provider.connect())
    {
        printf(CONSOLE_RED "%s: cannot connect — skipping.\n" CONSOLE_RESET, name);
        debugf("%s: connect() failed, fatal=%d\n", name, provider.hasFatalError() ? 1 : 0);
        summary.problems.push_back(remote.name + " (not reached)");
        return false;
    }
    debugf("%s: connected, manifest prefix \"%s\"%s\n", name,
           manifestScope(remote.config, provider.manifestPrefix()).c_str(),
           remote.config.role == ROLE_BACKUP   ? ", backup only"
           : remote.config.role == ROLE_SOURCE ? ", read only"
                                               : "");

    // A wrong 3DS clock makes every mtime comparison suspect, so say so once.
    std::string serverTimeStr = provider.serverTime();
    if (serverTimeStr.empty())
        debugf("%s: no server time reported, skew unchecked\n", name);
    else
    {
        time_t serverTime = parseServerTime(serverTimeStr);
        time_t localTime = time(NULL);
        if (serverTime == 0)
            debugf("%s: cannot parse server time \"%s\"\n", name,
                   serverTimeStr.c_str());
        else
        {
            long skew = serverTime > localTime ? (long)(serverTime - localTime)
                                               : (long)(localTime - serverTime);
            debugf("%s: server %s, skew %ld s\n", name,
                   serverTimeStr.c_str(), skew);
            if (skew > 60)
            {
                printf("WARNING: 3DS clock skew detected (%ld s).\n", skew);
                printf("Timestamps may be unreliable. Set the 3DS clock.\n\n");
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// syncAllEntries  — run every configured entry across the connected remotes
// ---------------------------------------------------------------------------
// A remote that hits a fatal error is reported once and left out of the
// entries after it; the others carry on.
// ---------------------------------------------------------------------------
static void syncAllEntries(std::vector<ActiveRemote> remotes,
                           const std::vector<SyncEntry> &entries, Manifest &manifest,
                           SyncSummary &summary, SyncUi &ui)
{
    // Drops the remotes that failed, saying so once for each.
    auto dropFailed = [&]() {
        for (size_t i = 0; i < remotes.size();)
        {
            if (!remotes[i].provider->hasFatalError())
            {
                i++;
                continue;
            }
            printf(CONSOLE_RED "\n%s: aborted, remaining entries skipped.\n" CONSOLE_RESET,
                   remotes[i].name.c_str());
            summary.problems.push_back(remotes[i].name + " (stopped by an error)");
            remotes.erase(remotes.begin() + i);
        }
    };

    for (auto &entry : entries)
    {
        if (ui.cancelRequested() || remotes.empty())
            break;
        bool uploadOnly = (entry.direction == SYNC_UPLOAD_ONLY);

        std::vector<const ActiveRemote *> taking;
        for (auto &r : remotes)
        {
            if (r.config.syncs(entry.remoteName))
                taking.push_back(&r);
            else
                debugf("%s: not in its Only=, leaves out %s\n", r.name.c_str(),
                       entry.remoteName.c_str());
        }
        if (taking.empty())
            continue; // Only= routes this entry nowhere

        std::string names;
        for (const ActiveRemote *r : taking)
            names += (names.empty() ? "" : ", ") + r->name;
        frontend::setActivity(names, entry.localBase);
        printf("\n%s [%s]\n", uploadOnly ? "Uploading" : "Syncing", entry.localBase.c_str());
        // A filter leaves files out without a word, and the summary would
        // otherwise not explain why a file never moved.
        if (!entry.filter.allowsAll())
            printf("  Only files ending in\n    %s\n", entry.filter.describe().c_str());

        std::vector<Participant> participants;
        for (const ActiveRemote *r : taking)
        {
            // Providers that shipped their own one-way upload keep using it so
            // the remote layout their users already have does not change.
            bool readOnly = r->config.role == ROLE_SOURCE;
            if (uploadOnly && !readOnly && r->provider->legacyUpload(entry.localBase, entry.remoteName,
                                                        entry.localFiles, ui))
                continue;
            // A backup only ever receives and a source is never written to,
            // whatever the entry.
            participants.push_back(
                Participant(*r->provider, r->name,
                            manifestScope(r->config, r->provider->manifestPrefix()),
                            r->config.role == ROLE_BACKUP, readOnly));
        }

        if (!participants.empty() && !syncEntry(participants, manifest, entry, summary, ui))
            debugf("%s: stopped at the user's request\n", entry.remoteName.c_str());
        dropFailed();
        // Kept per entry, so a crash or a flat battery later in the run loses
        // this entry's records at most.  A failure is reported by the last
        // save in runSync(), which tries again.
        manifest.saveIfChanged();

        if (ui.cancelRequested())
        {
            printf(CONSOLE_RED "\nSync cancelled by user.\n" CONSOLE_RESET);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// componentsInit / componentsExit
// ---------------------------------------------------------------------------
// The startup services all return a Result that the app used to discard.  Most
// failures are survivable — romfsInit() fails on a build without a romfs, and
// nothing here reads one — so a bad one is not made fatal, but it is recorded:
// startup happens before the menu, so a debug run replays this afterwards
// instead of printing to a screen nobody has chosen debug mode on yet.
static std::vector<std::string> g_initLog;
// Credentials loadConfig() opened from their sealed form, as "section=name".
static std::set<std::string> g_unsealedFields;
// Sealed values loadConfig() could not open, for configIssues().
static std::vector<ConfigIssue> g_sealIssues;

static void initStep(const char *what, Result res)
{
    char line[96];
    if (R_FAILED(res))
        snprintf(line, sizeof(line), "%s failed: 0x%08lX", what, (unsigned long)res);
    else
        snprintf(line, sizeof(line), "%s ok", what);
    g_initLog.push_back(line);
}

static void dumpInitLog()
{
    if (!debugEnabled())
        return;
    for (auto &line : g_initLog)
        debugf("%s\n", line.c_str());
}

bool componentsInit()
{
    bool result = true;
    // Screens and stdout first, so a failure below is printed somewhere.
    frontend::init();
    printf("3DSync v" APP_VERSION " \xE2\x80\x94 modified by michvllni,\n"
           "original by Kyraminol\n\n");
    printf("Initializing components...\n");

    initStep("APT_SetAppCpuTimeLimit", APT_SetAppCpuTimeLimit(30));
    initStep("cfguInit", cfguInit());
    initStep("romfsInit", romfsInit());
    initStep("pxiDevInit", pxiDevInit());
    initStep("amInit", amInit());
    initStep("acInit", acInit());

    u32 *socketBuffer = (u32 *)memalign(0x1000, 0x100000);
    if (socketBuffer == NULL)
    {
        printf("Failed to create socket buffer.\n");
        g_initLog.push_back("memalign for the socket buffer failed");
        result = false;
    }
    Result socRes = socInit(socketBuffer, 0x100000);
    initStep("socInit", socRes);
    if (socRes)
    {
        printf("socInit failed.\n");
        result = false;
    }

    initStep("httpcInit", httpcInit(0));
    initStep("sslcInit", sslcInit(0));
    return result;
}

void componentsExit()
{
    sslcExit();
    httpcExit();
    socExit();
    acExit();
    pxiDevExit();
    romfsExit();
    cfguExit();
    frontend::exit();
}

// ---------------------------------------------------------------------------
// parseTlsMode  — INI "TLS=" value for the FTP remote
// ---------------------------------------------------------------------------
static FtpTlsMode parseTlsMode(std::string value)
{
    for (char &c : value)
        c = (char)tolower((unsigned char)c);

    if (value == "none")
        return FTP_TLS_NONE;
    if (value == "require" || value == "required" || value == "explicit")
        return FTP_TLS_REQUIRE;
    if (value == "implicit")
        return FTP_TLS_IMPLICIT;
    return FTP_TLS_TRY; // RemoteConfig::ftpTls is checked, so this is "try"
}

// ---------------------------------------------------------------------------
// debugConfigSummary  — what the INI actually parsed to
// ---------------------------------------------------------------------------
// A mistyped section name or a stray character is invisible otherwise: inih
// skips the bad line and the app reports only "no remote configured".
// Credentials sit in 3DSync.ini in plaintext and a debug screen is what ends up
// photographed into a bug report, so anything that looks like a secret is
// reported as a length alone — enough to spot the trailing space that causes
// half of all "login denied" reports, without printing the password.
// ---------------------------------------------------------------------------
static bool looksSecret(const std::string &sectionAndKey)
{
    // In the path sections the key is a remote name the user chose, which is
    // not a secret and is the most useful line here — and "Monkey=" would
    // otherwise match on "key".
    static const char *pathSections[] = {"paths=", "shallowpaths=",
                                         "uploadpaths=", "uploadshallowpaths="};
    for (const char *section : pathSections)
        if (sectionAndKey.rfind(section, 0) == 0)
            return false;

    // Matched as substrings rather than by exact field name so a field added
    // later is hidden by default.
    static const char *needles[] = {"password", "secret", "token", "key"};
    for (const char *needle : needles)
        if (sectionAndKey.find(needle) != std::string::npos)
            return true;
    return false;
}

static void debugConfigSummary(const INIReader &reader)
{
    if (!debugEnabled())
        return;

    int parseError = reader.ParseError();
    if (parseError > 0)
        debugf("3DSync.ini: syntax error on line %d, that line was ignored\n",
               parseError);
    else if (parseError < 0)
        debugf("3DSync.ini: cannot be opened\n");

    // INIReader stores "section=key", both lowercased.
    for (auto &kv : reader.GetValues())
    {
        if (looksSecret(kv.first))
            debugf("ini %s = set, %d chars%s\n", kv.first.c_str(), (int)kv.second.size(),
                   g_unsealedFields.count(kv.first) ? ", sealed" : "");
        else
            debugf("ini %s = %s\n", kv.first.c_str(), kv.second.c_str());
    }
}

// ---------------------------------------------------------------------------
// configuredRemotes / makeProvider  — the remotes 3DSync.ini describes
// ---------------------------------------------------------------------------
// configuredRemotes() is shared by runSync() and the idle screen, so the screen
// never lists a remote the sync would skip or leaves out one it would run.
// ---------------------------------------------------------------------------
static RemoteConfigs configuredRemotes(const INIReader &reader)
{
    return parseRemotes(reader.GetValues(), configuredEntryNames(reader));
}

// The backend for one remote section, read from that section alone, so a
// labelled one ("[SMB:Backup]") takes the same keys as the unnamed one.
static SyncProvider *makeProvider(const INIReader &reader, const RemoteConfig &remote)
{
    const std::string &s = remote.section;
    switch (remote.type)
    {
    case REMOTE_DROPBOX:
        return new Dropbox(reader.Get(s, "appkey", ""), reader.Get(s, "appsecret", ""),
                           reader.Get(s, "refreshtoken", ""), reader.Get(s, "path", ""),
                           reader.Get(s, "token", ""));
    case REMOTE_DRIVE:
        return new GoogleDrive(reader.Get(s, "clientid", ""), reader.Get(s, "clientsecret", ""),
                               reader.Get(s, "refreshtoken", ""),
                               reader.Get(s, "folderid", ""), reader.Get(s, "token", ""));
    case REMOTE_SMB:
        return new SmbRemote(reader.Get(s, "server", ""), reader.Get(s, "share", ""),
                             reader.Get(s, "user", ""), reader.Get(s, "password", ""),
                             reader.Get(s, "domain", ""), reader.Get(s, "path", ""));
    case REMOTE_FTP:
        return new FtpRemote(reader.Get(s, "host", ""), (int)reader.GetInteger(s, "port", 0),
                             reader.Get(s, "user", "anonymous"),
                             reader.Get(s, "password", ""), reader.Get(s, "path", ""),
                             parseTlsMode(remote.ftpTls),
                             reader.Get(s, "mode", "passive") == "active", remote.verifyTls);
    case REMOTE_WEBDAV:
        return new WebDavRemote(reader.Get(s, "url", ""), reader.Get(s, "user", ""),
                                reader.Get(s, "password", ""), remote.verifyTls);
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// configIssues  — everything in 3DSync.ini the user has to fix
// ---------------------------------------------------------------------------
// The INI is read once, at startup, so this is too: main() shows it before the
// menu, and every run repeats it in the log, where a saved debug log keeps it.
// ---------------------------------------------------------------------------
static std::vector<ConfigIssue> configIssues(const INIReader &reader,
                                             const RemoteConfigs &remotes)
{
    std::vector<ConfigIssue> issues;
    if (reader.ParseError() > 0)
    {
        // inih skips the line and reads on, so whatever was on it is missing.
        char line[48];
        snprintf(line, sizeof(line), "  Line %d was skipped; check its", reader.ParseError());
        ConfigIssue issue;
        issue.brief = "3DSync.ini has a syntax error";
        issue.lines = {"3DSync.ini has a syntax error:", line,
                       "  [Section] or Key=value form."};
        issue.leftOut = false;
        issues.push_back(issue);
    }
    issues.insert(issues.end(), g_sealIssues.begin(), g_sealIssues.end());
    issues.insert(issues.end(), remotes.issues.begin(), remotes.issues.end());
    for (auto &name : configuredFilters(reader).unknown)
    {
        // Most likely a typo in a path's name: that path then syncs with the
        // default, which is not what the line asked for.
        ConfigIssue issue;
        issue.section = "filter";
        issue.brief = "[Filter] names no path";
        issue.lines = {"[Filter] has a line for a path that is not", "configured:", "  " + name,
                       "  Use a name from [Paths], or Extensions=", "  for every path."};
        issue.leftOut = false;
        issues.push_back(issue);
    }
    return issues;
}

static void printConfigIssues(const std::vector<ConfigIssue> &issues)
{
    for (auto &issue : issues)
    {
        printf(CONSOLE_YELLOW "%s" CONSOLE_RESET "\n", issue.lines[0].c_str());
        for (size_t i = 1; i < issue.lines.size(); i++)
            printf("%s\n", issue.lines[i].c_str());
    }
    if (!issues.empty())
        printf("\n");
}

// ---------------------------------------------------------------------------
// runSync  — one full sync pass, on the frontend's worker thread
// ---------------------------------------------------------------------------
static void runSync(const INIReader &reader)
{
    frontend::WorkerSyncUi ui;

    if (debugEnabled())
    {
        printf(CONSOLE_MAGENTA "--- debug mode: verbose output ---"
               CONSOLE_RESET "\n");
        debugf("3DSync v%s\n", APP_VERSION);
        dumpInitLog();
        debugConfigSummary(reader);
    }

    RemoteConfigs remotes = configuredRemotes(reader);
    std::vector<ConfigIssue> issues = configIssues(reader, remotes);
    printConfigIssues(issues);
    debugf("remotes configured: %s\n", remoteNames(remotes.remotes).c_str());

    if (remotes.remotes.empty())
    {
        printf("No remote configured in 3DSync.ini\n");
        printf("Add a [Dropbox], [GoogleDrive], [SMB], [FTP]\n");
        printf("or [WebDAV] section.\n");
        frontend::setLastResult("no remote configured", false);
        return;
    }

    std::vector<SyncEntry> syncEntries = getConfiguredSyncPaths(reader);
    if (syncEntries.empty())
    {
        printf("No sync paths configured in 3DSync.ini\n");
        frontend::setLastResult("no sync paths configured", false);
        return;
    }

    Manifest manifest("/3ds/3DSync/manifest.json");
    manifest.load();
    SyncSummary summary;

    // A remote the user meant to have that is left out is a remote not synced.
    for (auto &issue : issues)
        if (issue.leftOut)
            summary.problems.push_back(issue.brief);

    // In the order the menu card names them.
    std::vector<std::unique_ptr<SyncProvider> > providers;
    std::vector<ActiveRemote> connected;
    for (auto &config : remotes.remotes)
    {
        if (ui.cancelRequested())
            break;
        providers.push_back(std::unique_ptr<SyncProvider>(makeProvider(reader, config)));
        if (providers.back())
            providers.back()->setDisplayName(config.displayName());
        ActiveRemote remote = {providers.back().get(), config, config.displayName()};
        if (remote.provider && connectRemote(remote, summary))
            connected.push_back(remote);
    }
    syncAllEntries(connected, syncEntries, manifest, summary, ui);

    if (!manifest.saveIfChanged())
        printf(CONSOLE_RED "Sync state could not be saved — the next run will\n"
               "re-examine every file.\n" CONSOLE_RESET);

    printf("\n--- Sync Summary ---\n");
    for (auto &line : summaryLines(summary))
        printf("%s\n", line.c_str());

    char result[64];
    snprintf(result, sizeof(result), "%d up, %d down", summary.uploaded, summary.downloaded);
    if (ui.cancelRequested())
    {
        printf(CONSOLE_RED "\nSync cancelled. Progress has been saved.\n" CONSOLE_RESET);
        frontend::setLastResult(std::string("cancelled, ") + result, false);
    }
    else if (summary.hasErrors())
    {
        printf(CONSOLE_RED "\nSync finished with errors." CONSOLE_RESET "\n");
        frontend::setLastResult(summaryResult(summary), false);
    }
    else
    {
        printf("\nSync complete.\n");
        frontend::setLastResult(summaryResult(summary), true);
    }

    offerDebugLogSave();
}

// ---------------------------------------------------------------------------
// loadConfig  — read 3DSync.ini, sealing its credentials to this console
// ---------------------------------------------------------------------------
// See utils/secrets.h for what sealing protects and what it does not.  The
// configurator cannot know this console's key, so the file arrives in plain
// text and is sealed the first time it is read here; a credential edited in
// by hand later is sealed the same way on the next start.  The rest of the app
// never sees a sealed value: they are opened into the reader below.
// ---------------------------------------------------------------------------
static const char *CONFIG_PATH = "/3ds/3DSync/3DSync.ini";

// Mixed into the console-unique hash.  Never change it: every 3DSync.ini that
// is already sealed would stop opening.  (It is the app's UNIQUE_ID.)
static const u32 SEAL_SALT = 0xF5555;


static bool consoleRandom(unsigned char *buf, size_t len)
{
    return R_SUCCEEDED(sslcGenerateRandomData(buf, (u32)len));
}

static bool consoleSealKey(SealKey &key)
{
    u64 hash = 0;
    Result res = CFGU_GenHashConsoleUnique(SEAL_SALT, &hash);
    initStep("CFGU_GenHashConsoleUnique", res);
    if (R_FAILED(res))
        return false;
    key = deriveSealKey(hash);
    return true;
}

static INIReader loadConfig()
{
    SealKey key;
    bool haveKey = consoleSealKey(key);
    recoverConfigSeal(CONFIG_PATH, haveKey ? &key : NULL);

    {
        INIReader plain(CONFIG_PATH);
        bool wanted = plain.GetBoolean("Security", "SealCredentials", true);
        if (plain.ParseError() >= 0 && wanted && haveKey)
        {
            std::vector<std::string> skipped;
            int sealed = sealConfigFile(CONFIG_PATH, key, consoleRandom, &skipped);
            // A secret inih would read from several lines cannot be sealed
            // without changing what it reads: it stays plain, and the user
            // has to know, or "Sealed 2 credentials" reads as "all of them".
            for (auto &field : skipped)
            {
                size_t eq = field.find('=');
                std::string section = field.substr(0, eq), name = field.substr(eq + 1);
                ConfigIssue issue;
                issue.section = section;
                issue.brief = "[" + section + "] " + name + "= not sealed";
                issue.lines = {"A credential stays readable on the card:",
                               "  [" + section + "] " + name + "=",
                               "  It repeats, continues on the next line",
                               "  or is too long. Write it once, on one",
                               "  line, and start 3DSync again."};
                issue.leftOut = false;
                g_sealIssues.push_back(issue);
            }
            if (sealed > 0)
                printf(CONSOLE_GREEN "Sealed %d credential%s in 3DSync.ini to this\n"
                       "console." CONSOLE_RESET " The file no longer works on its own.\n\n",
                       sealed, sealed == 1 ? "" : "s");
            else if (sealed < 0)
                printf(CONSOLE_RED "Could not seal the credentials in 3DSync.ini;\n"
                       "they stay readable on the SD card.\n\n" CONSOLE_RESET);
        }
        else if (plain.ParseError() >= 0 && wanted)
            g_initLog.push_back("no console key: credentials left unsealed");
        else if (plain.ParseError() >= 0)
            g_initLog.push_back("SealCredentials=false: credentials left unsealed");
    }

    INIReader reader(CONFIG_PATH);
    for (auto &kv : reader.GetValues())
    {
        if (!isSealedValue(kv.second))
            continue;
        size_t eq = kv.first.find('=');
        std::string section = kv.first.substr(0, eq), name = kv.first.substr(eq + 1);
        std::string plain;
        if (haveKey && openSealedValue(key, section, name, kv.second, plain))
        {
            reader.Set(section, name, plain);
            g_unsealedFields.insert(kv.first);
            continue;
        }
        // Never hand the provider the sealed text as if it were the secret:
        // clear it, so the remote reads as unconfigured, and say why — in
        // the startup check, with everything else wrong in the file.
        reader.Set(section, name, "");
        ConfigIssue issue;
        issue.section = section;
        issue.brief = "[" + section + "] " + name + "= won't open";
        if (haveKey)
            issue.lines = {"A sealed value cannot be opened here:", "  [" + section + "] " + name,
                           "  It was sealed on another console, or",
                           "  this one was reset. Paste the plain",
                           "  value back, or make a new file with",
                           "  the configurator."};
        else
            // Not the file's fault: pasting the value back would only have it
            // fail the same way next time.
            issue.lines = {"A sealed value cannot be opened:", "  [" + section + "] " + name,
                           "  This console's key could not be read",
                           "  (CFG service). Restart the console; if",
                           "  it persists, run a debug sync (Y) and",
                           "  report the log."};
        issue.leftOut = false;
        g_sealIssues.push_back(issue);
    }
    return reader;
}

// ---------------------------------------------------------------------------
// syncWorker  — what a menu choice runs, on the worker thread
// ---------------------------------------------------------------------------
static void syncWorker(void *arg)
{
    const INIReader &reader = *(const INIReader *)arg;
    if (reader.ParseError() >= 0)
        runSync(reader);
    else
    {
        printf("Can't load configuration\n");
        debugConfigSummary(reader);
        frontend::setLastResult("configuration not loaded", false);
        offerDebugLogSave();
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (!componentsInit())
    {
        componentsExit();
        return 1;
    }

    INIReader reader = loadConfig();
    if (reader.ParseError() < 0)
        printf("Can't load configuration\n");
    RemoteConfigs remotes = configuredRemotes(reader);
    std::vector<ConfigIssue> issues =
        reader.ParseError() >= 0 ? configIssues(reader, remotes) : std::vector<ConfigIssue>();
    frontend::setMenuInfo(reader.ParseError() >= 0, remoteNames(remotes.remotes),
                          countSyncPaths(reader), configuredFilters(reader).describe(),
                          (int)issues.size());

    // Said once, before the menu: a remote that is set up wrongly is left out
    // of every sync, which nothing else on screen would explain.
    if (!issues.empty())
    {
        printConfigIssues(issues);
        std::vector<std::string> lines;
        for (auto &issue : issues)
            lines.insert(lines.end(), issue.lines.begin(), issue.lines.end());
        if (!frontend::showConfigIssues(lines, (int)issues.size()))
        {
            componentsExit();
            return 0;
        }
    }

    while (true)
    {
        frontend::MenuChoice choice = frontend::waitForMenu();
        if (choice == frontend::MENU_EXIT)
            break;

        // Set before anything else runs: the providers read it when they build
        // their libcurl handle, and the engine when it logs a decision.
        setDebugEnabled(choice == frontend::MENU_SYNC_DEBUG);
        // Per run, not per session: syncing twice should not offer the first
        // run's transcript along with the second's.
        debugLogReset();

        frontend::runWorker(syncWorker, &reader);
        // after sync: fall back to the menu (sync again or exit)
    }

    componentsExit();
    return 0;
}
