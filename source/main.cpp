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
#include "utils/fsutil.h"

// ---------------------------------------------------------------------------
// Sync direction per configured path
// ---------------------------------------------------------------------------
enum SyncDirection
{
    SYNC_BOTH,       // bidirectional (download + upload)
    SYNC_UPLOAD_ONLY // legacy / one-way upload
};

struct SyncEntry
{
    std::string localBase;               // absolute local path, e.g. /3ds/Checkpoint/saves
    std::string remoteName;              // Drive folder path, e.g. Checkpoint/saves
    std::vector<std::string> localFiles; // relative paths discovered locally
    SyncDirection direction;
};

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
        if (additionalpath != "")
            paths.push_back(additionalpath);
        else
            printf("Folder %s not found\n", basepath.c_str());
    }
    return paths;
}

// ---------------------------------------------------------------------------
// getConfiguredSyncPaths
// ---------------------------------------------------------------------------
static std::vector<SyncEntry> getConfiguredSyncPaths(const INIReader &reader)
{
    std::vector<SyncEntry> entries;
    std::map<std::string, std::string> values = reader.GetValues();

    for (auto &kv : values)
    {
        // INIReader lowercases both section and key, format: "section=key"
        SyncDirection dir;
        bool recursive;
        std::string prefix;

        if (kv.first.rfind("paths=", 0) == 0)
        {
            dir = SYNC_BOTH;
            recursive = true;
            prefix = "paths=";
        }
        else if (kv.first.rfind("shallowpaths=", 0) == 0)
        {
            dir = SYNC_BOTH;
            recursive = false;
            prefix = "shallowpaths=";
        }
        else if (kv.first.rfind("uploadpaths=", 0) == 0)
        {
            dir = SYNC_UPLOAD_ONLY;
            recursive = true;
            prefix = "uploadpaths=";
        }
        else if (kv.first.rfind("uploadshallowpaths=", 0) == 0)
        {
            dir = SYNC_UPLOAD_ONLY;
            recursive = false;
            prefix = "uploadshallowpaths=";
        }
        else
            continue;

        SyncEntry entry;
        entry.localBase = kv.second;
        entry.remoteName = kv.first.substr(prefix.size());
        entry.localFiles = recurse_dir(kv.second, "", recursive);
        entry.direction = dir;
        entries.push_back(entry);
    }
    return entries;
}

// ---------------------------------------------------------------------------
// parseServerTime  — convert a server timestamp to time_t (UTC), 0 on failure
// ---------------------------------------------------------------------------
// Two formats turn up: RFC 3339 ("2024-05-28T14:32:00.000Z") from Drive's JSON,
// and RFC 7231 ("Tue, 28 May 2024 14:32:00 GMT") from the HTTP Date header that
// every provider returns.  mktime() interprets the fields as local time; the
// 3DS clock is kept as UTC, so the two line up.
// ---------------------------------------------------------------------------
static time_t parseServerTime(const std::string &s)
{
    static const char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm t = {};
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    char monthName[8] = {};

    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        // RFC 3339
    }
    else if (sscanf(s.c_str(), "%*3s, %d %3s %d %d:%d:%d",
                    &day, monthName, &year, &hour, &min, &sec) == 6)
    {
        month = 0;
        for (int i = 0; i < 12; i++)
            if (strncmp(monthName, months[i], 3) == 0)
                month = i + 1;
        if (month == 0)
            return 0;
    }
    else
    {
        return 0;
    }

    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = 0;
    return mktime(&t);
}

// ---------------------------------------------------------------------------
// waitForConflictKey  — block until A / B / X / START
// ---------------------------------------------------------------------------
enum ConflictChoice
{
    CONFLICT_KEEP_LOCAL,
    CONFLICT_KEEP_REMOTE,
    CONFLICT_SKIP,
    CONFLICT_CANCEL
};

// g_applyAllChoice: -1 = not set; otherwise a ConflictChoice value applied to all conflicts.
static int g_applyAllChoice = -1;

static ConflictChoice waitForConflictKey()
{
    if (g_applyAllChoice >= 0)
        return (ConflictChoice)g_applyAllChoice;

    while (aptMainLoop())
    {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_A)
            return CONFLICT_KEEP_LOCAL;
        if (k & KEY_B)
            return CONFLICT_KEEP_REMOTE;
        if (k & KEY_X)
            return CONFLICT_SKIP;
        if (k & KEY_START)
            return CONFLICT_CANCEL;
        if (k & KEY_L)
        {
            printf("  Apply resolution to ALL remaining conflicts:\n");
            printf("  A: 3DS wins all  B: remote wins all\n  X: skip all  L: cancel\n\n");
            bool inSubmenu = true;
            while (aptMainLoop() && inSubmenu)
            {
                hidScanInput();
                u32 k2 = hidKeysDown();
                if (k2 & KEY_A)
                {
                    g_applyAllChoice = CONFLICT_KEEP_LOCAL;
                    return CONFLICT_KEEP_LOCAL;
                }
                if (k2 & KEY_B)
                {
                    g_applyAllChoice = CONFLICT_KEEP_REMOTE;
                    return CONFLICT_KEEP_REMOTE;
                }
                if (k2 & KEY_X)
                {
                    g_applyAllChoice = CONFLICT_SKIP;
                    return CONFLICT_SKIP;
                }
                if (k2 & KEY_L)
                {
                    inSubmenu = false;
                }
                gfxFlushBuffers();
                gfxSwapBuffers();
                gspWaitForVBlank();
            }
            // L pressed again — cancel apply-all, reprint conflict prompt
            printf("  A: keep 3DS version  B: keep remote version\n  X: skip  START: cancel  L: apply all\n\n");
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return CONFLICT_CANCEL;
}

// waitForMainMenuKey — returns true to start sync, false to exit.
static bool waitForMainMenuKey()
{
    printf("\n      A: Run sync\n");
    printf("  START: Exit\n\n");
    while (aptMainLoop())
    {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_A)
            return true;
        if (k & KEY_START)
            return false;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return false;
}

// ---------------------------------------------------------------------------
// g_cancelRequested — set when START is pressed during sync; causes the sync
// loop to stop after the current file operation completes.
// ---------------------------------------------------------------------------
static bool g_cancelRequested = false;

// ---------------------------------------------------------------------------
// SyncSummary  — accumulates per-file outcomes for end-of-sync report
// ---------------------------------------------------------------------------
struct SyncSummary
{
    int uploaded;
    int downloaded;
    std::set<std::string> checkedPaths; // full localPath, deduplicates across entries
    struct FileAction
    {
        std::string path;
        std::string action;
    };
    std::vector<FileAction> changes;
    SyncSummary() : uploaded(0), downloaded(0) {}
};

// ---------------------------------------------------------------------------
// recordUpload / recordDownload — transfer one file and update the manifest
// ---------------------------------------------------------------------------
static bool recordUpload(SyncProvider &provider, Manifest &manifest,
                         const std::string &key, const std::string &root,
                         const std::string &relPath, const std::string &localPath,
                         const RemoteFileInfo *existing, SyncSummary &summary)
{
    std::string tag, id;
    if (!provider.upload(root, relPath, localPath, existing, tag, id))
        return false;

    struct stat st = {};
    stat(localPath.c_str(), &st);
    manifest.set(key, {st.st_mtime, (long long)st.st_size, tag, id});
    summary.uploaded++;
    summary.changes.push_back({localPath, "uploaded"});
    return true;
}

static bool recordDownload(SyncProvider &provider, Manifest &manifest,
                           const std::string &key, const RemoteFileInfo &file,
                           const std::string &localPath, SyncSummary &summary)
{
    if (!provider.download(file, localPath))
        return false;

    struct stat st = {};
    stat(localPath.c_str(), &st);
    manifest.set(key, {st.st_mtime, (long long)st.st_size, file.tag, file.id});
    summary.downloaded++;
    summary.changes.push_back({localPath, "downloaded"});
    return true;
}

// ---------------------------------------------------------------------------
// performSync  — sync one SyncEntry against one remote
// ---------------------------------------------------------------------------
// uploadOnly mirrors local to remote: nothing is ever downloaded, no conflict
// is ever raised (the 3DS wins), and files that exist only on the remote are
// left untouched.
//
// Returns false if a fatal provider error occurred or cancellation was
// requested.
// ---------------------------------------------------------------------------
static bool performSync(SyncProvider &provider, Manifest &manifest,
                        const SyncEntry &entry, SyncSummary &summary,
                        bool uploadOnly)
{
    if (provider.hasFatalError())
        return false;

    // Resolve (and create if missing) the remote folder for this entry
    std::string root = provider.ensureRoot(entry.remoteName);
    if (root.empty() && !entry.remoteName.empty())
    {
        if (provider.hasFatalError())
            return false;
        printf("Cannot resolve %s folder for %s — skipping\n",
               provider.name(), entry.remoteName.c_str());
        return true;
    }

    printf("Listing %s:%s\n", provider.name(), entry.remoteName.c_str());
    std::map<std::string, RemoteFileInfo> remoteFiles;
    if (!provider.list(root, remoteFiles))
    {
        if (provider.hasFatalError())
            return false;
        printf("Cannot list %s:%s — skipping\n",
               provider.name(), entry.remoteName.c_str());
        return true;
    }

    // Build the full set of relative paths to consider: everything local, plus
    // everything on the remote unless we are only pushing.
    std::set<std::string> allRelPaths;
    for (auto &f : entry.localFiles)
        allRelPaths.insert(f);
    if (!uploadOnly)
        for (auto &rf : remoteFiles)
            allRelPaths.insert(rf.first);

    // Manifest keys are scoped per remote so two providers syncing the same
    // local folder keep separate state.
    const std::string keyPrefix = provider.manifestPrefix() + "|";

    for (auto &relPath : allRelPaths)
    {
        // Poll for START between file operations for graceful cancellation.
        // hidScanInput is called here so we don't interfere with conflict prompts.
        hidScanInput();
        if (hidKeysDown() & KEY_START)
        {
            printf("  -> Cancellation requested\n");
            g_cancelRequested = true;
        }
        if (g_cancelRequested)
            break;

        std::string localPath = entry.localBase + relPath;
        std::string key = keyPrefix + localPath;

        // --- Local file info ---
        struct stat localSt = {};
        bool localExists = (stat(localPath.c_str(), &localSt) == 0);
        time_t localMtime = localExists ? localSt.st_mtime : 0;
        long long localSize = localExists ? (long long)localSt.st_size : 0;

        // --- Remote file info ---
        auto remoteIt = remoteFiles.find(relPath);
        bool remoteExists = (remoteIt != remoteFiles.end());
        const RemoteFileInfo *rfi = remoteExists ? &remoteIt->second : NULL;

        // --- Manifest entry ---
        bool inManifest = manifest.has(key);
        ManifestEntry mEntry = inManifest ? manifest.get(key) : ManifestEntry{};

        // No manifest entry + both sides exist = no baseline to diff against,
        // treat as conflict.
        bool firstSync = !inManifest && localExists && remoteExists;
        bool localChanged = firstSync ||
                            (inManifest && (localMtime != mEntry.localMtime ||
                                            (mEntry.localSize != 0 &&
                                             localSize != mEntry.localSize)));
        bool remoteChanged = firstSync ||
                             (inManifest && remoteExists && rfi->tag != mEntry.remoteTag);

        // FAT32 mtime has 2-second granularity and some emulators never update
        // the timestamp at all.  Where the provider's tag is derivable from file
        // contents (Drive returns an MD5), compare contents as a fallback.
        if (!localChanged && inManifest && localExists && !mEntry.remoteTag.empty())
        {
            std::string tag = provider.localTag(localPath);
            if (!tag.empty() && tag != mEntry.remoteTag)
                localChanged = true;
        }

        if (provider.hasFatalError())
            break;

        // ----------------------------------------------------------------
        // Decision table
        // ----------------------------------------------------------------

        if (!localExists && !remoteExists)
        {
            // Both gone — clean up manifest
            if (inManifest)
                manifest.remove(key);
            continue;
        }

        if (!localExists && remoteExists)
        {
            // File only on the remote (new there, or deleted locally)
            if (uploadOnly)
                continue;
            recordDownload(provider, manifest, key, *rfi, localPath, summary);
            continue;
        }

        if (localExists && !remoteExists)
        {
            // File only local (new, or deleted on the remote) — upload
            recordUpload(provider, manifest, key, root, relPath, localPath, NULL, summary);
            continue;
        }

        // Both exist
        if (!localChanged && !remoteChanged)
        {
            summary.checkedPaths.insert(localPath);
            continue;
        }

        if (localChanged && !remoteChanged)
        {
            recordUpload(provider, manifest, key, root, relPath, localPath, rfi, summary);
            continue;
        }

        if (!localChanged && remoteChanged)
        {
            if (uploadOnly)
                continue;
            recordDownload(provider, manifest, key, *rfi, localPath, summary);
            continue;
        }

        // Both changed
        if (uploadOnly)
        {
            recordUpload(provider, manifest, key, root, relPath, localPath, rfi, summary);
            continue;
        }

        printf("\n  *** CONFLICT: %s\n\n", localPath.c_str());
        printf("  A: keep 3DS version  B: keep remote version\n  X: skip  START: cancel  L: apply all\n\n");
        ConflictChoice choice = waitForConflictKey();

        if (choice == CONFLICT_CANCEL)
        {
            printf("  -> Sync cancelled\n");
            return false;
        }
        if (choice == CONFLICT_SKIP)
        {
            printf("  -> Skipped\n");
            summary.changes.push_back({localPath, "skipped"});
            continue;
        }
        if (choice == CONFLICT_KEEP_LOCAL)
        {
            printf("  -> Keeping 3DS version, uploading\n");
            recordUpload(provider, manifest, key, root, relPath, localPath, rfi, summary);
        }
        else
        {
            printf("  -> Keeping remote version, downloading\n");
            recordDownload(provider, manifest, key, *rfi, localPath, summary);
        }

        if (provider.hasFatalError())
            break;
    }

    return !provider.hasFatalError() && !g_cancelRequested;
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
                         Manifest &manifest, SyncSummary &summary)
{
    printf("\n=== %s ===\n", provider.name());
    if (provider.isExperimental())
        printExperimentalNotice(provider.name());

    if (!provider.connect())
    {
        printf(CONSOLE_RED "%s: cannot connect — skipping.\n" CONSOLE_RESET, provider.name());
        return;
    }

    // A wrong 3DS clock makes every mtime comparison suspect, so say so once.
    std::string serverTimeStr = provider.serverTime();
    if (!serverTimeStr.empty())
    {
        time_t serverTime = parseServerTime(serverTimeStr);
        time_t localTime = time(NULL);
        if (serverTime != 0)
        {
            long skew = serverTime > localTime ? (long)(serverTime - localTime)
                                               : (long)(localTime - serverTime);
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

        if (uploadOnly)
        {
            printf("\nUploading [%s] -> %s:%s\n", entry.localBase.c_str(),
                   provider.name(), entry.remoteName.c_str());
            // Providers that shipped their own one-way upload keep using it so
            // the remote layout their users already have does not change.
            if (provider.legacyUpload(entry.localBase, entry.remoteName, entry.localFiles))
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

        if (!performSync(provider, manifest, entry, summary, uploadOnly) &&
            !provider.hasFatalError())
            g_cancelRequested = true;

        if (g_cancelRequested)
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
bool componentsInit()
{
    bool result = true;
    gfxInitDefault();

    consoleInit(GFX_BOTTOM, NULL);
    printf(CONSOLE_RED "\n 3DSync " VERSION_STRING " modified by michvllni, original by Kyraminol" CONSOLE_RESET);
    printf("\n\n\n\n\n\n  Sync your saves with another 3DS,\n   a PC or even a cloud.");
    printf("\n\n\n\n\n\n Commit: " CONSOLE_BLUE REVISION_STRING CONSOLE_RESET);

    consoleInit(GFX_TOP, NULL);
    printf("Initializing components...\n\n");

    APT_SetAppCpuTimeLimit(30);
    cfguInit();
    romfsInit();
    pxiDevInit();
    amInit();
    acInit();

    u32 *socketBuffer = (u32 *)memalign(0x1000, 0x100000);
    if (socketBuffer == NULL)
    {
        printf("Failed to create socket buffer.\n");
        result = false;
    }
    if (socInit(socketBuffer, 0x100000))
    {
        printf("socInit failed.\n");
        result = false;
    }

    httpcInit(0);
    sslcInit(0);
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
    gfxExit();
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
// runSync  — one full sync pass; called from main loop
// ---------------------------------------------------------------------------
static void runSync(const INIReader &reader)
{
    g_cancelRequested = false;
    g_applyAllChoice = -1;

    std::string dropboxToken = reader.Get("Dropbox", "token", "");
    std::string dropboxAppKey = reader.Get("Dropbox", "appkey", "");
    std::string dropboxAppSecret = reader.Get("Dropbox", "appsecret", "");
    std::string dropboxRefreshToken = reader.Get("Dropbox", "refreshtoken", "");
    std::string dropboxPath = reader.Get("Dropbox", "path", "");
    bool hasDropbox = !dropboxToken.empty() || !dropboxRefreshToken.empty();

    std::string googleDriveToken = reader.Get("GoogleDrive", "token", "");
    std::string googleDriveClientId = reader.Get("GoogleDrive", "clientid", "");
    std::string googleDriveClientSecret = reader.Get("GoogleDrive", "clientsecret", "");
    std::string googleDriveRefreshToken = reader.Get("GoogleDrive", "refreshtoken", "");
    std::string googleDriveFolderId = reader.Get("GoogleDrive", "folderid", "");
    bool hasGoogleDrive = !googleDriveToken.empty() || !googleDriveRefreshToken.empty();

    std::string smbServer = reader.Get("SMB", "server", "");
    std::string smbShare = reader.Get("SMB", "share", "");
    bool hasSmb = !smbServer.empty() && !smbShare.empty();

    std::string ftpHost = reader.Get("FTP", "host", "");
    bool hasFtp = !ftpHost.empty();

    std::string webdavUrl = reader.Get("WebDAV", "url", "");
    bool hasWebdav = !webdavUrl.empty();

    if (!hasDropbox && !hasGoogleDrive && !hasSmb && !hasFtp && !hasWebdav)
    {
        printf("No remote configured in 3DSync.ini\n");
        printf("Add a [Dropbox], [GoogleDrive], [SMB], [FTP] or [WebDAV] section.\n");
        return;
    }

    std::vector<SyncEntry> syncEntries = getConfiguredSyncPaths(reader);
    if (syncEntries.empty())
    {
        printf("No sync paths configured in 3DSync.ini\n");
        return;
    }

    Manifest manifest("/3ds/3DSync/manifest.json");
    manifest.load();
    SyncSummary summary;

    // --- Dropbox ---
    if (hasDropbox && !g_cancelRequested)
    {
        Dropbox dropbox(dropboxAppKey, dropboxAppSecret, dropboxRefreshToken,
                        dropboxPath, dropboxToken);
        syncProvider(dropbox, syncEntries, manifest, summary);
    }

    // --- Google Drive ---
    if (hasGoogleDrive && !g_cancelRequested)
    {
        GoogleDrive drive(googleDriveClientId, googleDriveClientSecret,
                          googleDriveRefreshToken, googleDriveFolderId,
                          googleDriveToken);
        syncProvider(drive, syncEntries, manifest, summary);
    }

    // --- SMB2/3 file server (NAS, Windows share) ---
    if (hasSmb && !g_cancelRequested)
    {
        SmbRemote smb(smbServer, smbShare,
                      reader.Get("SMB", "user", ""),
                      reader.Get("SMB", "password", ""),
                      reader.Get("SMB", "domain", ""),
                      reader.Get("SMB", "path", ""));
        syncProvider(smb, syncEntries, manifest, summary);
    }

    // --- FTP / FTPS ---
    if (hasFtp && !g_cancelRequested)
    {
        std::string mode = reader.Get("FTP", "mode", "passive");
        FtpRemote ftp(ftpHost,
                      (int)reader.GetInteger("FTP", "port", 0),
                      reader.Get("FTP", "user", "anonymous"),
                      reader.Get("FTP", "password", ""),
                      reader.Get("FTP", "path", ""),
                      parseTlsMode(reader.Get("FTP", "tls", "try")),
                      mode == "active");
        syncProvider(ftp, syncEntries, manifest, summary);
    }

    // --- WebDAV ---
    if (hasWebdav && !g_cancelRequested)
    {
        WebDavRemote webdav(webdavUrl,
                            reader.Get("WebDAV", "user", ""),
                            reader.Get("WebDAV", "password", ""));
        syncProvider(webdav, syncEntries, manifest, summary);
    }

    manifest.save();

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

    if (g_cancelRequested)
        printf(CONSOLE_RED "\nSync cancelled. Progress has been saved.\n" CONSOLE_RESET);
    else
        printf("\nSync complete.\n");
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

    INIReader reader("/3ds/3DSync/3DSync.ini");
    if (reader.ParseError() < 0)
        printf("Can't load configuration\n");

    while (true)
    {
        if (!waitForMainMenuKey())
            break;

        if (reader.ParseError() >= 0)
            runSync(reader);
        else
            printf("Can't load configuration\n");

        // after sync: fall back to the menu (sync again or exit)
    }

    componentsExit();
    return 0;
}
