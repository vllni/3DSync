#include <stdio.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <map>
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
#include "sync/syncengine.h"
#include "ui/frontend.h"
#include "utils/debug.h"
#include "utils/configseal.h"
#include "utils/fsutil.h"
#include "utils/secrets.h"
#include "utils/timeutil.h"
#include "version.h"

// ---------------------------------------------------------------------------
// recurse_dir
// ---------------------------------------------------------------------------
std::vector<std::string> recurse_dir(std::string basepath, std::string additionalpath = "", bool recursive = true)
{
    std::vector<std::string> paths;
    DIR *dir;
    struct dirent *ent;
    std::string path(basepath + additionalpath);
    if ((dir = opendir(path.c_str())) != NULL)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            std::string childAdditional = additionalpath + "/" + ent->d_name;
            std::string childFull = basepath + childAdditional;
            DIR *childDir = opendir(childFull.c_str());
            if (childDir != NULL)
            {
                closedir(childDir);
                if (recursive)
                {
                    std::vector<std::string> sub = recurse_dir(basepath, childAdditional, true);
                    paths.insert(paths.end(), sub.begin(), sub.end());
                }
            }
            else
            {
                paths.push_back(childAdditional);
            }
        }
        closedir(dir);
    }
    else
    {
        // opendir also fails on a plain file, which is how a "Paths=" entry
        // pointing at a single save is handled — hence no error in that case.
        if (additionalpath != "")
            paths.push_back(additionalpath);
        else
        {
            printf("Folder %s not found\n", basepath.c_str());
            debugErrno("opendir", path);
        }
    }
    return paths;
}

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

static std::vector<SyncEntry> getConfiguredSyncPaths(const INIReader &reader)
{
    std::vector<SyncEntry> entries;
    std::map<std::string, std::string> values = reader.GetValues();

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
        entry.localFiles = recurse_dir(kv.second, "", recursive);
        entry.direction = dir;
        // An entry that found nothing locally is silently normal — the remote
        // may hold everything — but it is also what a mistyped path looks like.
        debugf("entry %s -> %s: %d local file(s), %s%s\n", entry.localBase.c_str(),
               entry.remoteName.c_str(), (int)entry.localFiles.size(),
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
// syncProvider  — run every configured entry against one remote
// ---------------------------------------------------------------------------
static void syncProvider(SyncProvider &provider, const std::vector<SyncEntry> &entries,
                         Manifest &manifest, SyncSummary &summary, SyncUi &ui)
{
    printf("\n=== %s ===\n", provider.name());
    frontend::setActivity(provider.name(), "Connecting\xE2\x80\xA6");
    if (provider.isExperimental())
        printExperimentalNotice(provider.name());

    if (!provider.connect())
    {
        printf(CONSOLE_RED "%s: cannot connect — skipping.\n" CONSOLE_RESET, provider.name());
        debugf("%s: connect() failed, fatal=%d\n", provider.name(),
               provider.hasFatalError() ? 1 : 0);
        return;
    }
    debugf("%s: connected, manifest prefix \"%s\"\n", provider.name(),
           provider.manifestPrefix().c_str());

    // A wrong 3DS clock makes every mtime comparison suspect, so say so once.
    std::string serverTimeStr = provider.serverTime();
    if (serverTimeStr.empty())
        debugf("%s: no server time reported, skew unchecked\n", provider.name());
    else
    {
        time_t serverTime = parseServerTime(serverTimeStr);
        time_t localTime = time(NULL);
        if (serverTime == 0)
            debugf("%s: cannot parse server time \"%s\"\n", provider.name(),
                   serverTimeStr.c_str());
        else
        {
            long skew = serverTime > localTime ? (long)(serverTime - localTime)
                                               : (long)(localTime - serverTime);
            debugf("%s: server %s, skew %ld s\n", provider.name(),
                   serverTimeStr.c_str(), skew);
            if (skew > 60)
            {
                printf("WARNING: 3DS clock skew detected (%ld s).\n", skew);
                printf("Timestamps may be unreliable. Set the 3DS clock.\n\n");
            }
        }
    }

    for (auto &entry : entries)
    {
        bool uploadOnly = (entry.direction == SYNC_UPLOAD_ONLY);
        frontend::setActivity(provider.name(), entry.localBase);

        if (uploadOnly)
        {
            printf("\nUploading [%s] -> %s:%s\n", entry.localBase.c_str(),
                   provider.name(), entry.remoteName.c_str());
            // Providers that shipped their own one-way upload keep using it so
            // the remote layout their users already have does not change.
            if (provider.legacyUpload(entry.localBase, entry.remoteName, entry.localFiles, ui))
            {
                if (provider.hasFatalError())
                {
                    printf(CONSOLE_RED "\n%s: aborted, remaining entries skipped.\n" CONSOLE_RESET,
                           provider.name());
                    break;
                }
                continue;
            }
        }
        else
        {
            printf("\nSyncing [%s] <-> %s:%s\n", entry.localBase.c_str(),
                   provider.name(), entry.remoteName.c_str());
        }

        if (!performSync(provider, manifest, entry, summary, uploadOnly, ui) &&
            !provider.hasFatalError())
        {
            debugf("%s: %s stopped short without a fatal error — treating as "
                   "cancellation\n", provider.name(), entry.remoteName.c_str());
            ui.requestCancel();
        }

        if (ui.cancelRequested())
        {
            printf(CONSOLE_RED "\nSync cancelled by user.\n" CONSOLE_RESET);
            break;
        }
        if (provider.hasFatalError())
        {
            printf(CONSOLE_RED "\n%s: aborted, remaining entries skipped.\n" CONSOLE_RESET,
                   provider.name());
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
    return FTP_TLS_TRY; // default: use TLS when the server offers it
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
// configuredRemotes  — which [sections] are filled in enough to try
// ---------------------------------------------------------------------------
// Shared by runSync() and the idle screen, so the screen never lists a remote
// the sync would skip or leaves out one it would run.
struct ConfiguredRemotes
{
    bool dropbox, drive, smb, ftp, webdav;

    bool any() const { return dropbox || drive || smb || ftp || webdav; }

    // In the order runSync() visits them, by SyncProvider::name().
    std::string names() const
    {
        std::string out;
        const struct
        {
            bool on;
            const char *name;
        } all[] = {{dropbox, "Dropbox"}, {drive, "Drive"}, {smb, "SMB"},
                   {ftp, "FTP"}, {webdav, "WebDAV"}};
        for (auto &r : all)
            if (r.on)
                out += (out.empty() ? "" : ", ") + std::string(r.name);
        return out;
    }
};

static ConfiguredRemotes configuredRemotes(const INIReader &reader)
{
    ConfiguredRemotes r;
    r.dropbox = !reader.Get("Dropbox", "token", "").empty() ||
                !reader.Get("Dropbox", "refreshtoken", "").empty();
    r.drive = !reader.Get("GoogleDrive", "token", "").empty() ||
              !reader.Get("GoogleDrive", "refreshtoken", "").empty();
    r.smb = !reader.Get("SMB", "server", "").empty() && !reader.Get("SMB", "share", "").empty();
    r.ftp = !reader.Get("FTP", "host", "").empty();
    r.webdav = !reader.Get("WebDAV", "url", "").empty();
    return r;
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

    std::string dropboxToken = reader.Get("Dropbox", "token", "");
    std::string dropboxAppKey = reader.Get("Dropbox", "appkey", "");
    std::string dropboxAppSecret = reader.Get("Dropbox", "appsecret", "");
    std::string dropboxRefreshToken = reader.Get("Dropbox", "refreshtoken", "");
    std::string dropboxPath = reader.Get("Dropbox", "path", "");

    std::string googleDriveToken = reader.Get("GoogleDrive", "token", "");
    std::string googleDriveClientId = reader.Get("GoogleDrive", "clientid", "");
    std::string googleDriveClientSecret = reader.Get("GoogleDrive", "clientsecret", "");
    std::string googleDriveRefreshToken = reader.Get("GoogleDrive", "refreshtoken", "");
    std::string googleDriveFolderId = reader.Get("GoogleDrive", "folderid", "");

    std::string smbServer = reader.Get("SMB", "server", "");
    std::string smbShare = reader.Get("SMB", "share", "");
    std::string ftpHost = reader.Get("FTP", "host", "");
    std::string webdavUrl = reader.Get("WebDAV", "url", "");

    ConfiguredRemotes remotes = configuredRemotes(reader);
    bool hasDropbox = remotes.dropbox, hasGoogleDrive = remotes.drive, hasSmb = remotes.smb,
         hasFtp = remotes.ftp, hasWebdav = remotes.webdav;

    debugf("remotes configured: dropbox=%d drive=%d smb=%d ftp=%d webdav=%d\n",
           hasDropbox, hasGoogleDrive, hasSmb, hasFtp, hasWebdav);

    if (!remotes.any())
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

    // --- Dropbox ---
    if (hasDropbox && !ui.cancelRequested())
    {
        Dropbox dropbox(dropboxAppKey, dropboxAppSecret, dropboxRefreshToken,
                        dropboxPath, dropboxToken);
        syncProvider(dropbox, syncEntries, manifest, summary, ui);
    }

    // --- Google Drive ---
    if (hasGoogleDrive && !ui.cancelRequested())
    {
        GoogleDrive drive(googleDriveClientId, googleDriveClientSecret,
                          googleDriveRefreshToken, googleDriveFolderId,
                          googleDriveToken);
        syncProvider(drive, syncEntries, manifest, summary, ui);
    }

    // --- SMB2/3 file server (NAS, Windows share) ---
    if (hasSmb && !ui.cancelRequested())
    {
        SmbRemote smb(smbServer, smbShare,
                      reader.Get("SMB", "user", ""),
                      reader.Get("SMB", "password", ""),
                      reader.Get("SMB", "domain", ""),
                      reader.Get("SMB", "path", ""));
        syncProvider(smb, syncEntries, manifest, summary, ui);
    }

    // --- FTP / FTPS ---
    if (hasFtp && !ui.cancelRequested())
    {
        std::string mode = reader.Get("FTP", "mode", "passive");
        FtpRemote ftp(ftpHost,
                      (int)reader.GetInteger("FTP", "port", 0),
                      reader.Get("FTP", "user", "anonymous"),
                      reader.Get("FTP", "password", ""),
                      reader.Get("FTP", "path", ""),
                      parseTlsMode(reader.Get("FTP", "tls", "try")),
                      mode == "active");
        syncProvider(ftp, syncEntries, manifest, summary, ui);
    }

    // --- WebDAV ---
    if (hasWebdav && !ui.cancelRequested())
    {
        WebDavRemote webdav(webdavUrl,
                            reader.Get("WebDAV", "user", ""),
                            reader.Get("WebDAV", "password", ""));
        syncProvider(webdav, syncEntries, manifest, summary, ui);
    }

    if (!manifest.save())
        printf(CONSOLE_RED "Sync state could not be saved — the next run will\n"
               "re-examine every file.\n" CONSOLE_RESET);

    printf("\n--- Sync Summary ---\n");
    if (summary.changes.empty())
    {
        printf("All %d files up to date.\n", (int)summary.checkedPaths.size());
    }
    else
    {
        printf("Uploaded: %d  Downloaded: %d  Unchanged: %d\n",
               summary.uploaded, summary.downloaded,
               (int)summary.checkedPaths.size());
        printf("\nChanged files:\n");
        for (auto &c : summary.changes)
            printf("  %s: %s\n", c.action.c_str(), c.path.c_str());
    }

    char result[64];
    snprintf(result, sizeof(result), "%d up, %d down", summary.uploaded, summary.downloaded);
    if (ui.cancelRequested())
    {
        printf(CONSOLE_RED "\nSync cancelled. Progress has been saved.\n" CONSOLE_RESET);
        frontend::setLastResult(std::string("cancelled, ") + result, false);
    }
    else
    {
        printf("\nSync complete.\n");
        frontend::setLastResult(summary.changes.empty() ? std::string("everything up to date")
                                                        : std::string(result),
                                true);
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
    recoverConfigSeal(CONFIG_PATH);
    SealKey key;
    bool haveKey = consoleSealKey(key);

    {
        INIReader plain(CONFIG_PATH);
        bool wanted = plain.GetBoolean("Security", "SealCredentials", true);
        if (plain.ParseError() >= 0 && wanted && haveKey)
        {
            int sealed = sealConfigFile(CONFIG_PATH, key, consoleRandom);
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
        // clear it, so the remote reads as unconfigured, and say why.
        reader.Set(section, name, "");
        printf(CONSOLE_RED "3DSync.ini: [%s] %s cannot be opened\n" CONSOLE_RESET
               "here. It was sealed on another console (or this\n"
               "one was reset). Paste the plain value back into\n"
               "the file, or make a new one with the configurator.\n\n",
               section.c_str(), name.c_str());
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
    frontend::setMenuInfo(reader.ParseError() >= 0, configuredRemotes(reader).names(),
                          countSyncPaths(reader));

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
