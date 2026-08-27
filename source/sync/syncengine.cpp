#include "syncengine.h"

#include <map>
#include <stdio.h>
#include <sys/stat.h>

#include "../utils/console.h"
#include "../utils/debug.h"
#include "../utils/fsutil.h"

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
    {
        // Every caller ignores this: one file failing must not stop the rest of
        // the run.  That leaves nothing on screen for a per-file failure the
        // provider reported quietly, so it is named here.
        debugf("upload failed: %s -> %s%s\n", localPath.c_str(), root.c_str(),
               relPath.c_str());
        return false;
    }

    struct stat st = {};
    if (stat(localPath.c_str(), &st) != 0)
        debugErrno("stat after upload", localPath);
    if (tag.empty())
        debugf("%s uploaded without a change tag — it will look changed next "
               "run\n", relPath.c_str());
    manifest.set(key, {st.st_mtime, (long long)st.st_size, tag, id});
    summary.uploaded++;
    summary.changes.push_back({localPath, "uploaded"});
    return true;
}

static bool recordDownload(SyncProvider &provider, Manifest &manifest,
                           const std::string &key, const RemoteFileInfo &file,
                           const std::string &localPath, SyncSummary &summary)
{
    // A remote folder that does not exist locally yet is the normal case on a
    // first sync, so the destination is prepared here rather than leaving every
    // provider to remember it.
    mkparents(localPath);

    if (!provider.download(file, localPath))
    {
        debugf("download failed: %s -> %s\n", file.relPath.c_str(), localPath.c_str());
        return false;
    }

    struct stat st = {};
    if (stat(localPath.c_str(), &st) != 0)
        debugErrno("stat after download", localPath);
    if (file.tag.empty())
        debugf("%s downloaded without a change tag — it will look changed next "
               "run\n", file.relPath.c_str());
    manifest.set(key, {st.st_mtime, (long long)st.st_size, file.tag, file.id});
    summary.downloaded++;
    summary.changes.push_back({localPath, "downloaded"});
    return true;
}

bool performSync(SyncProvider &provider, Manifest &manifest, const SyncEntry &entry,
                 SyncSummary &summary, bool uploadOnly, SyncUi &ui)
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
        debugf("ensureRoot(\"%s\") returned nothing\n", entry.remoteName.c_str());
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
        debugf("list(\"%s\") failed; %d entries had been collected\n", root.c_str(),
               (int)remoteFiles.size());
        return true;
    }

    debugf("%s root \"%s\": %d remote file(s), %d local file(s)\n", provider.name(),
           root.c_str(), (int)remoteFiles.size(), (int)entry.localFiles.size());

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
        // Checked between file operations so a cancellation takes effect at a
        // point where the manifest is consistent.
        if (ui.cancelRequested())
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
            {
                debugf("%s: mtime unchanged but contents differ from the "
                       "recorded tag\n", relPath.c_str());
                localChanged = true;
            }
        }

        if (provider.hasFatalError())
            break;

        // The decision below is the whole engine, and in a quiet run nothing
        // about it reaches the screen for a file that ends up skipped.
        debugf("%s: local=%s remote=%s manifest=%s%s%s\n", relPath.c_str(),
               localExists ? "yes" : "no", remoteExists ? "yes" : "no",
               inManifest ? "yes" : "no", localChanged ? " localChanged" : "",
               remoteChanged ? " remoteChanged" : "");

        // ----------------------------------------------------------------
        // Decision table
        // ----------------------------------------------------------------

        if (!localExists && !remoteExists)
        {
            // Both gone — clean up manifest
            if (inManifest)
            {
                debugf("  -> gone on both sides, dropping the manifest entry\n");
                manifest.remove(key);
            }
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
        ConflictChoice choice = ui.resolveConflict(localPath);

        if (choice == CONFLICT_CANCEL)
        {
            printf("  -> Sync cancelled\n");
            ui.requestCancel();
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

    return !provider.hasFatalError() && !ui.cancelRequested();
}
