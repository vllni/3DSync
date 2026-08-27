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
#include "utils/fsutil.h"
#include "utils/timeutil.h"

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
// ConsoleSyncUi  — the 3DS side of SyncUi
// ---------------------------------------------------------------------------
// Cancellation is polled from the START button between files, and conflicts are
// resolved from the buttons.  "Apply to all" lives here rather than in the
// engine: it is a property of how this front-end asks, not of the sync rules.
// ---------------------------------------------------------------------------
class ConsoleSyncUi : public SyncUi
{
public:
    ConsoleSyncUi() : _cancelled(false), _applyAllChoice(-1) {}

    bool cancelRequested() override
    {
        if (_cancelled)
            return true;

        hidScanInput();
        if (hidKeysDown() & KEY_START)
        {
            printf("  -> Cancellation requested\n");
            _cancelled = true;
        }
        return _cancelled;
    }

    void requestCancel() override { _cancelled = true; }

    ConflictChoice resolveConflict(const std::string &localPath) override;

private:
    bool _cancelled;
    // -1 = not set; otherwise a ConflictChoice applied to every later conflict.
    int _applyAllChoice;
};

ConflictChoice ConsoleSyncUi::resolveConflict(const std::string &localPath)
{
    (void)localPath;
    if (_applyAllChoice >= 0)
        return (ConflictChoice)_applyAllChoice;

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
                    _applyAllChoice = CONFLICT_KEEP_LOCAL;
                    return CONFLICT_KEEP_LOCAL;
                }
                if (k2 & KEY_B)
                {
                    _applyAllChoice = CONFLICT_KEEP_REMOTE;
                    return CONFLICT_KEEP_REMOTE;
                }
                if (k2 & KEY_X)
                {
                    _applyAllChoice = CONFLICT_SKIP;
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
                         Manifest &manifest, SyncSummary &summary, ConsoleSyncUi &ui)
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

        if (!performSync(provider, manifest, entry, summary, uploadOnly, ui) &&
            !provider.hasFatalError())
            ui.requestCancel();

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
    ConsoleSyncUi ui;

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

    if (ui.cancelRequested())
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
