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
    SYNC_BOTH,        // bidirectional (download + upload)
    SYNC_UPLOAD_ONLY  // legacy / one-way upload
};

struct SyncEntry
{
    std::string localBase;   // absolute local path, e.g. /3ds/Checkpoint/saves
    std::string remoteName;  // Drive folder path, e.g. Checkpoint/saves
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

        if      (kv.first.rfind("paths=", 0) == 0)             { dir = SYNC_BOTH;        recursive = true;  prefix = "paths="; }
        else if (kv.first.rfind("shallowpaths=", 0) == 0)      { dir = SYNC_BOTH;        recursive = false; prefix = "shallowpaths="; }
        else if (kv.first.rfind("uploadpaths=", 0) == 0)       { dir = SYNC_UPLOAD_ONLY; recursive = true;  prefix = "uploadpaths="; }
        else if (kv.first.rfind("uploadshallowpaths=", 0) == 0){ dir = SYNC_UPLOAD_ONLY; recursive = false; prefix = "uploadshallowpaths="; }
        else continue;

        SyncEntry entry;
        entry.localBase   = kv.second;
        entry.remoteName  = kv.first.substr(prefix.size());
        entry.localFiles  = recurse_dir(kv.second, "", recursive);
        entry.direction   = dir;
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
        t.tm_year  = year - 1900;
        t.tm_mon   = month - 1;
        t.tm_mday  = day;
        t.tm_hour  = hour;
        t.tm_min   = min;
        t.tm_sec   = sec;
        t.tm_isdst = 0;
        // mktime uses local time; Drive timestamps are UTC.
        // On 3DS the clock is typically stored as UTC so this should be consistent.
        return mktime(&t);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// waitForAorB  — block until the user presses A or B, return true for A
// ---------------------------------------------------------------------------
static bool waitForAorB()
{
    while (aptMainLoop())
    {
        hidScanInput();
        u32 k = hidKeysDown();
        if (k & KEY_A) return true;
        if (k & KEY_B) return false;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    return true;
}

// ---------------------------------------------------------------------------
// performSync  — bidirectional sync for one SyncEntry
// Returns false if a fatal Drive error occurred (caller should abort).
// ---------------------------------------------------------------------------
static bool performSync(GoogleDrive &drive, Manifest &manifest, const SyncEntry &entry)
{
    if (drive.hasFatalError()) return false;

    // Resolve (and create if missing) the Drive folder hierarchy
    std::string rootFolderId = drive.ensureFolderPath(entry.remoteName);
    if (rootFolderId.empty())
    {
        if (drive.hasFatalError()) return false;
        printf("Cannot resolve Drive folder for %s — skipping\n", entry.remoteName.c_str());
        return true;
    }

    // List current Drive contents
    printf("Listing Drive folder: %s\n", entry.remoteName.c_str());
    auto driveFiles = drive.listFolderRecursive(rootFolderId);

    // Build the full set of relative paths to consider:
    // union of what is local and what is on Drive.
    std::set<std::string> allRelPaths;
    for (auto &f : entry.localFiles)  allRelPaths.insert(f);
    for (auto &df : driveFiles)       allRelPaths.insert(df.first);

    for (auto &relPath : allRelPaths)
    {
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
               inManifest  ? "yes" : "no");

        if (drive.hasFatalError()) break;

        // ----------------------------------------------------------------
        // Decision table
        // ----------------------------------------------------------------

        if (!localExists && !driveExists)
        {
            // Both gone — clean up manifest
            if (inManifest) manifest.remove(localPath);
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
            printf("  Press A to keep the 3DS version (upload)\n");
            printf("  Press B to keep the Drive version (download)\n\n");
            bool keepLocal = waitForAorB();

            if (keepLocal)
            {
                printf("  -> Keeping 3DS version, uploading\n");
                std::string md5;
                std::string fileId = drive.syncUpload(rootFolderId, relPath, localPath, "", md5);
                if (!fileId.empty())
                    manifest.set(localPath, {localMtime, md5, fileId});
            }
            else
            {
                printf("  -> Keeping Drive version, downloading\n");
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
            // Drive changed — download
            printf("  -> Drive changed, downloading %s\n", relPath.c_str());
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
        printf("  Press A to keep the 3DS version (upload)\n");
        printf("  Press B to keep the Drive version (download)\n\n");
        bool keepLocal = waitForAorB();

        if (keepLocal)
        {
            printf("  -> Keeping 3DS version, uploading\n");
            std::string md5;
            std::string fileId = drive.syncUpload(rootFolderId, relPath, localPath, mEntry.driveId, md5);
            if (!fileId.empty())
                manifest.set(localPath, {localMtime, md5, fileId});
        }
        else
        {
            printf("  -> Keeping Drive version, downloading\n");
            if (drive.downloadFile(*dfi, localPath))
            {
                struct stat st = {};
                stat(localPath.c_str(), &st);
                manifest.set(localPath, {st.st_mtime, dfi->md5, dfi->id});
            }
        }

        if (drive.hasFatalError()) break;
    }

    return !drive.hasFatalError();
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
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    if (!componentsInit())
        componentsExit();

    INIReader reader("/3ds/3DSync/3DSync.ini");

    if (reader.ParseError() < 0)
    {
        printf("Can't load configuration\n");
    }
    else
    {
        std::string dropboxToken            = reader.Get("Dropbox",    "token",         "");
        std::string googleDriveToken        = reader.Get("GoogleDrive","token",         "");
        std::string googleDriveClientId     = reader.Get("GoogleDrive","clientid",      "");
        std::string googleDriveClientSecret = reader.Get("GoogleDrive","clientsecret",  "");
        std::string googleDriveRefreshToken = reader.Get("GoogleDrive","refreshtoken",  "");
        std::string googleDriveFolderId     = reader.Get("GoogleDrive","folderid",      "");
        bool hasGoogleDrive = !googleDriveToken.empty() || !googleDriveRefreshToken.empty();

        // Collect all configured paths
        std::vector<SyncEntry> syncEntries;
        if (dropboxToken != "" || hasGoogleDrive)
            syncEntries = getConfiguredSyncPaths(reader);

        // --- Dropbox (upload-only, unchanged) ---
        if (dropboxToken != "" && !syncEntries.empty())
        {
            // Build legacy map for Dropbox
            std::map<std::pair<std::string,std::string>, std::vector<std::string>> legacyPaths;
            for (auto &e : syncEntries)
            {
                auto key = std::make_pair(e.localBase, e.remoteName);
                legacyPaths[key] = e.localFiles;
            }
            Dropbox dropbox(dropboxToken);
            dropbox.upload(legacyPaths);
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
                // Clock skew check: compare 3DS time with Drive server time
                // We trigger a lightweight API call to fetch the Date header.
                // The token refresh response also carries a Date header; if the
                // server time was already captured there, reuse it.
                std::string serverTimeStr = drive.getServerTime();
                if (!serverTimeStr.empty())
                {
                    time_t serverTime = parseRFC3339(serverTimeStr);
                    time_t localTime  = time(NULL);
                    long skew = serverTime > localTime
                                ? (long)(serverTime - localTime)
                                : (long)(localTime  - serverTime);
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
                        performSync(drive, manifest, entry);
                    }
                    else
                    {
                        // Upload-only: use the legacy flat-name uploader
                        printf("\nUploading [%s] -> Drive:%s\n",
                               entry.localBase.c_str(), entry.remoteName.c_str());
                        std::map<std::pair<std::string,std::string>, std::vector<std::string>> legacyPaths;
                        legacyPaths[{entry.localBase, entry.remoteName}] = entry.localFiles;
                        drive.upload(legacyPaths);
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
                else
                    printf("\nSync complete.\n");
            }
        }

        if (dropboxToken == "" && !hasGoogleDrive)
        {
            printf("Can't load Dropbox or Google Drive token from 3DSync.ini\n");
        }
    }

    printf("\n\nPress START to exit...");
    while (aptMainLoop())
    {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    componentsExit();
    return 0;
}
