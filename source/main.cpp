#include <stdio.h>
#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <iostream>

#include <3ds.h>

#include <curl/curl.h>

#include "libs/inih/INIReader/INIReader.h"
#include "modules/dropbox.h"
#include "modules/googledrive.h"
#include "modules/manifest.h"

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
// mkdirs  — create every directory component of path
// ---------------------------------------------------------------------------
static void mkdirs(const std::string &path)
{
    size_t pos = 1;
    while ((pos = path.find('/', pos)) != std::string::npos)
    {
        std::string dir = path.substr(0, pos);
        mkdir(dir.c_str(), 0755);
        pos++;
    }
    mkdir(path.c_str(), 0755);
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
// parseRFC3339  — convert "2024-05-28T14:32:00.000Z" to time_t (UTC)
// ---------------------------------------------------------------------------
static time_t parseRFC3339(const std::string &s)
{
    struct tm t = {};
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) == 6)
    {
        t.tm_year = year - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = min;
        t.tm_sec = sec;
        t.tm_isdst = 0;
        // mktime uses local time; Drive timestamps are UTC.
        // On 3DS the clock is typically stored as UTC so this should be consistent.
        return mktime(&t);
    }
    return 0;
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
            printf("  A: 3DS wins all  B: remote wins all  X: skip all  L: cancel\n\n");
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
            printf("  A: keep 3DS version  B: keep remote version  X: skip  START: cancel  L: apply all\n\n");
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
    printf("\n    A: Run sync\n");
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
// performSync  — bidirectional sync for one SyncEntry
// Returns false if a fatal Drive error occurred or cancellation was requested.
// ---------------------------------------------------------------------------
static bool performSync(GoogleDrive &drive, Manifest &manifest, const SyncEntry &entry)
{
    if (drive.hasFatalError())
        return false;

    // Resolve (and create if missing) the Drive folder hierarchy
    std::string rootFolderId = drive.ensureFolderPath(entry.remoteName);
    if (rootFolderId.empty())
    {
        if (drive.hasFatalError())
            return false;
        printf("Cannot resolve Drive folder for %s — skipping\n", entry.remoteName.c_str());
        return true;
    }

    // List current Drive contents
    printf("Listing Drive folder: %s\n", entry.remoteName.c_str());
    auto driveFiles = drive.listFolderRecursive(rootFolderId);

    // Build the full set of relative paths to consider:
    // union of what is local and what is on Drive.
    std::set<std::string> allRelPaths;
    for (auto &f : entry.localFiles)
        allRelPaths.insert(f);
    for (auto &df : driveFiles)
        allRelPaths.insert(df.first);

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

        // --- Local file info ---
        struct stat localSt = {};
        bool localExists = (stat(localPath.c_str(), &localSt) == 0);
        time_t localMtime = localExists ? localSt.st_mtime : 0;

        // --- Drive file info ---
        auto driveIt = driveFiles.find(relPath);
        bool driveExists = (driveIt != driveFiles.end());
        const DriveFileInfo *dfi = driveExists ? &driveIt->second : nullptr;

        // --- Manifest entry ---
        bool inManifest = manifest.has(localPath);
        ManifestEntry mEntry = inManifest ? manifest.get(localPath) : ManifestEntry{};

        bool localChanged = inManifest && (localMtime != mEntry.localMtime);
        bool driveChanged = inManifest && driveExists && (dfi->md5 != mEntry.driveMd5);

        printf("  %s: local=%s drive=%s manifest=%s\n",
               relPath.c_str(),
               localExists ? "yes" : "no",
               driveExists ? "yes" : "no",
               inManifest ? "yes" : "no");

        if (drive.hasFatalError())
            break;

        // ----------------------------------------------------------------
        // Decision table
        // ----------------------------------------------------------------

        if (!localExists && !driveExists)
        {
            // Both gone — clean up manifest
            if (inManifest)
                manifest.remove(localPath);
            continue;
        }

        if (!localExists && driveExists)
        {
            // File only on Drive (new or local was deleted) — download
            printf("  -> Downloading %s\n", relPath.c_str());
            // Create parent directories
            size_t slash = localPath.rfind('/');
            if (slash != std::string::npos)
                mkdirs(localPath.substr(0, slash));

            if (drive.downloadFile(*dfi, localPath))
            {
                struct stat st = {};
                stat(localPath.c_str(), &st);
                manifest.set(localPath, {st.st_mtime, dfi->md5, dfi->id});
            }
            continue;
        }

        if (localExists && !driveExists)
        {
            // File only local (new, or deleted on Drive) — upload
            printf("  -> Uploading %s\n", relPath.c_str());
            std::string md5;
            std::string existingId = inManifest ? mEntry.driveId : "";
            std::string fileId = drive.syncUpload(rootFolderId, relPath, localPath, existingId, md5);
            if (!fileId.empty())
                manifest.set(localPath, {localMtime, md5, fileId});
            continue;
        }

        // Both exist
        if (!inManifest)
        {
            // First sync: no history to diff against — treat as a conflict so
            // neither side is silently overwritten.
            printf("\n  *** FIRST SYNC (both sides exist): %s\n", localPath.c_str());
            printf("  A: keep 3DS version  B: keep remote version  X: skip  START: cancel  L: apply all\n\n");
            ConflictChoice choice = waitForConflictKey();

            if (choice == CONFLICT_CANCEL)
            {
                printf("  -> Sync cancelled\n");
                return false;
            }
            if (choice == CONFLICT_SKIP)
            {
                printf("  -> Skipped\n");
                continue;
            }
            if (choice == CONFLICT_KEEP_LOCAL)
            {
                printf("  -> Keeping 3DS version, uploading\n");
                std::string md5;
                std::string fileId = drive.syncUpload(rootFolderId, relPath, localPath, dfi->id, md5);
                if (!fileId.empty())
                    manifest.set(localPath, {localMtime, md5, fileId});
            }
            else
            {
                printf("  -> Keeping remote version, downloading\n");
                if (drive.downloadFile(*dfi, localPath))
                {
                    struct stat st = {};
                    stat(localPath.c_str(), &st);
                    manifest.set(localPath, {st.st_mtime, dfi->md5, dfi->id});
                }
            }
            continue;
        }

        // Both exist and in manifest
        if (!localChanged && !driveChanged)
        {
            // No change — skip
            printf("  -> Up to date\n");
            continue;
        }

        if (localChanged && !driveChanged)
        {
            // Local changed — upload
            printf("  -> Local changed, uploading %s\n", relPath.c_str());
            std::string md5;
            std::string fileId = drive.syncUpload(rootFolderId, relPath, localPath, mEntry.driveId, md5);
            if (!fileId.empty())
                manifest.set(localPath, {localMtime, md5, fileId});
            continue;
        }

        if (!localChanged && driveChanged)
        {
            // Remote changed — download
            printf("  -> Remote changed, downloading %s\n", relPath.c_str());
            if (drive.downloadFile(*dfi, localPath))
            {
                struct stat st = {};
                stat(localPath.c_str(), &st);
                manifest.set(localPath, {st.st_mtime, dfi->md5, dfi->id});
            }
            continue;
        }

        // Both changed — conflict
        printf("\n  *** CONFLICT: %s\n", localPath.c_str());
        printf("  A: keep 3DS version  B: keep remote version  X: skip  START: cancel  L: apply all\n\n");
        ConflictChoice choice = waitForConflictKey();

        if (choice == CONFLICT_CANCEL)
        {
            printf("  -> Sync cancelled\n");
            return false;
        }
        if (choice == CONFLICT_SKIP)
        {
            printf("  -> Skipped\n");
            continue;
        }
        if (choice == CONFLICT_KEEP_LOCAL)
        {
            printf("  -> Keeping 3DS version, uploading\n");
            std::string md5;
            std::string fileId = drive.syncUpload(rootFolderId, relPath, localPath, mEntry.driveId, md5);
            if (!fileId.empty())
                manifest.set(localPath, {localMtime, md5, fileId});
        }
        else
        {
            printf("  -> Keeping remote version, downloading\n");
            if (drive.downloadFile(*dfi, localPath))
            {
                struct stat st = {};
                stat(localPath.c_str(), &st);
                manifest.set(localPath, {st.st_mtime, dfi->md5, dfi->id});
            }
        }

        if (drive.hasFatalError())
            break;
    }

    return !drive.hasFatalError() && !g_cancelRequested;
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
// runSync  — one full sync pass; called from main loop
// ---------------------------------------------------------------------------
static void runSync(const INIReader &reader)
{
    g_cancelRequested = false;
    g_applyAllChoice = -1;

    std::string dropboxToken = reader.Get("Dropbox", "token", "");
    std::string googleDriveToken = reader.Get("GoogleDrive", "token", "");
    std::string googleDriveClientId = reader.Get("GoogleDrive", "clientid", "");
    std::string googleDriveClientSecret = reader.Get("GoogleDrive", "clientsecret", "");
    std::string googleDriveRefreshToken = reader.Get("GoogleDrive", "refreshtoken", "");
    std::string googleDriveFolderId = reader.Get("GoogleDrive", "folderid", "");
    bool hasGoogleDrive = !googleDriveToken.empty() || !googleDriveRefreshToken.empty();

    std::vector<SyncEntry> syncEntries;
    if (dropboxToken != "" || hasGoogleDrive)
        syncEntries = getConfiguredSyncPaths(reader);

    // --- Dropbox ---
    if (dropboxToken != "" && !syncEntries.empty())
    {
        std::map<std::pair<std::string, std::string>, std::vector<std::string>> legacyPaths;
        for (auto &e : syncEntries)
            legacyPaths[std::make_pair(e.localBase, e.remoteName)] = e.localFiles;
        Dropbox dropbox(dropboxToken);
        if (!dropbox.upload(legacyPaths))
            g_cancelRequested = true;
    }

    // --- Google Drive ---
    if (hasGoogleDrive)
    {
        GoogleDrive drive(googleDriveClientId, googleDriveClientSecret,
                          googleDriveRefreshToken, googleDriveFolderId,
                          googleDriveToken);

        if (!drive.ensureToken())
        {
            printf("Failed to obtain Google Drive access token\n");
        }
        else
        {
            std::string serverTimeStr = drive.getServerTime();
            if (!serverTimeStr.empty())
            {
                time_t serverTime = parseRFC3339(serverTimeStr);
                time_t localTime = time(NULL);
                long skew = serverTime > localTime
                                ? (long)(serverTime - localTime)
                                : (long)(localTime - serverTime);
                if (skew > 60)
                {
                    printf("WARNING: 3DS clock skew detected (%ld s).\n", skew);
                    printf("Timestamps may be unreliable. Set the 3DS clock.\n\n");
                }
            }

            Manifest manifest("/3ds/3DSync/manifest.json");
            manifest.load();

            for (auto &entry : syncEntries)
            {
                if (entry.direction == SYNC_BOTH)
                {
                    printf("\nSyncing [%s] <-> Drive:%s\n",
                           entry.localBase.c_str(), entry.remoteName.c_str());
                    if (!performSync(drive, manifest, entry) && !drive.hasFatalError())
                        g_cancelRequested = true;
                }
                else
                {
                    printf("\nUploading [%s] -> Drive:%s\n",
                           entry.localBase.c_str(), entry.remoteName.c_str());
                    std::map<std::pair<std::string, std::string>, std::vector<std::string>> legacyPaths;
                    legacyPaths[{entry.localBase, entry.remoteName}] = entry.localFiles;
                    if (!drive.upload(legacyPaths) && !drive.hasFatalError())
                        g_cancelRequested = true;
                }

                if (g_cancelRequested)
                {
                    printf(CONSOLE_RED "\nSync cancelled by user.\n" CONSOLE_RESET);
                    break;
                }
                if (drive.hasFatalError())
                {
                    printf(CONSOLE_RED "\nSync aborted: remaining entries skipped.\n" CONSOLE_RESET);
                    break;
                }
            }

            manifest.save();
            if (drive.hasFatalError())
                printf(CONSOLE_RED "\nSync did not complete. Check the errors above.\n" CONSOLE_RESET);
            else if (g_cancelRequested)
                printf(CONSOLE_RED "\nSync cancelled. Progress has been saved.\n" CONSOLE_RESET);
            else
                printf("\nSync complete.\n");
        }
    }

    if (dropboxToken == "" && !hasGoogleDrive)
        printf("Can't load Dropbox or Google Drive token from 3DSync.ini\n");
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
