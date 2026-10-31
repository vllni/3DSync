#include "syncengine.h"

#include <map>
#include <stdio.h>
#include <sys/stat.h>

#include "../utils/console.h"
#include "../utils/debug.h"
#include "../utils/fsutil.h"
#include "../utils/pathutil.h"

const char *const LOCAL_SIDE = "3DS";

// Manifest scope of the 3DS's own copy.  No provider prefix can collide with
// it: they are "drive", "dropbox" or a URL.
static const char *const LOCAL_PREFIX = "local|";

namespace
{

// One participant's view of one file.
struct Side
{
    Participant *p;
    std::string key;
    const RemoteFileInfo *file; // NULL: not there
    bool inManifest;
    ManifestEntry rec;
    long long held;   // generation it holds as of its last sync, 0: unknown
    bool changed;     // new there, or its tag moved since its last sync
    bool sameAsLocal; // provably holds the 3DS's current bytes
};

struct LocalFile
{
    bool exists;    // a regular file is there
    bool inTheWay;  // something else is: a folder of that name
    time_t mtime;
    long long size;
};

LocalFile statLocal(const std::string &path)
{
    struct stat st = {};
    LocalFile f;
    bool found = (stat(path.c_str(), &st) == 0);
    f.exists = found && S_ISREG(st.st_mode);
    f.inTheWay = found && !f.exists;
    f.mtime = f.exists ? st.st_mtime : 0;
    f.size = f.exists ? (long long)st.st_size : 0;
    return f;
}

// Whether a record describes this local mtime and size.  A size of 0 in the
// record means "unknown": manifests written before the size field had none.
bool sameStat(const ManifestEntry &rec, time_t mtime, long long size)
{
    return rec.localMtime == mtime && (rec.localSize == 0 || rec.localSize == size);
}

std::string join(const std::vector<std::string> &names)
{
    std::string out;
    for (auto &n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------
// Each records its own failure: one file failing must not stop the rest of
// the run, and leaves nothing on screen when the provider reported it quietly.
static bool uploadTo(Side &s, const std::string &relPath, const std::string &localPath,
                     long long generation, SyncSummary &summary)
{
    std::string tag, id;
    if (!s.p->provider->upload(s.p->root, relPath, localPath, s.file, tag, id))
    {
        debugf("upload failed: %s -> %s %s%s\n", localPath.c_str(), s.p->label.c_str(),
               s.p->root.c_str(), relPath.c_str());
        summary.failures.push_back({localPath, "upload", s.p->label});
        return false;
    }
    if (tag.empty())
        debugf("%s uploaded to %s without a change tag — it will look changed "
               "next run\n", relPath.c_str(), s.p->label.c_str());
    LocalFile now = statLocal(localPath);
    if (!now.exists)
        debugErrno("stat after upload", localPath);
    ManifestEntry rec = {now.mtime, now.size, tag, id, generation};
    s.rec = rec;
    s.inManifest = true;
    summary.uploaded++;
    return true;
}

static bool downloadFrom(Side &s, const std::string &localPath, SyncSummary &summary)
{
    // A remote folder that does not exist locally yet is the normal case on a
    // first sync, so the destination is prepared here rather than leaving every
    // provider to remember it.
    mkparents(localPath);

    if (!s.p->provider->download(*s.file, localPath))
    {
        debugf("download failed: %s %s -> %s\n", s.p->label.c_str(),
               s.file->relPath.c_str(), localPath.c_str());
        summary.failures.push_back({localPath, "download", s.p->label});
        return false;
    }
    if (s.file->tag.empty())
        debugf("%s downloaded without a change tag — it will look changed next "
               "run\n", s.file->relPath.c_str());
    summary.downloaded++;
    return true;
}

// ---------------------------------------------------------------------------
// syncFile  — decide one file with every side in view
// ---------------------------------------------------------------------------
// Returns false only when the user cancelled at a conflict prompt.
// ---------------------------------------------------------------------------
static bool syncFile(std::vector<Participant *> &live, Manifest &manifest,
                     const std::string &relPath, const std::string &localPath,
                     SyncSummary &summary, SyncUi &ui)
{
    LocalFile local = statLocal(localPath);
    const std::string localKey = LOCAL_PREFIX + localPath;
    // A remote file whose local name is a folder: downloading would move the
    // folder aside as a backup, and uploading has nothing to send.
    if (local.inTheWay)
    {
        printf(CONSOLE_RED "  Skipped, a folder has this name:\n" CONSOLE_RESET);
        printf("    %s\n", localPath.c_str());
        summary.problems.push_back(localPath + " (a folder has its name)");
        return true;
    }

    std::vector<Side> sides;
    for (Participant *p : live)
    {
        Side s;
        s.p = p;
        s.key = p->keyPrefix + "|" + localPath;
        auto it = p->files.find(relPath);
        s.file = (it != p->files.end()) ? &it->second : NULL;
        s.inManifest = manifest.has(s.key);
        s.rec = s.inManifest ? manifest.get(s.key) : ManifestEntry{};
        s.held = 0;
        s.changed = s.file != NULL && (!s.inManifest || s.file->tag != s.rec.remoteTag);
        s.sameAsLocal = false;
        sides.push_back(s);
    }

    // --- The 3DS's copy as of the last sync ---
    bool hasBase = manifest.has(localKey);
    ManifestEntry base = hasBase ? manifest.get(localKey) : ManifestEntry{};
    if (!hasBase)
    {
        // A manifest from before the n-way engine has per-remote records only.
        // The newest one is the last state any remote agreed on, which makes
        // the first run after an upgrade decide what the pairwise sync would.
        for (auto &s : sides)
            if (s.inManifest && (!hasBase || s.rec.localMtime > base.localMtime))
            {
                base = s.rec;
                hasBase = true;
            }
        if (hasBase)
            debugf("%s: no 3DS record yet, taking the newest remote one\n", relPath.c_str());
    }
    if (hasBase && base.generation == 0)
        base.generation = 1;

    for (auto &s : sides)
    {
        if (!s.inManifest)
            continue;
        if (s.rec.generation > 0)
            s.held = s.rec.generation;
        else if (hasBase && sameStat(s.rec, base.localMtime, base.localSize))
            s.held = base.generation; // a record without one, still in step
    }

    // --- Did the 3DS change it? ---
    bool localNew = local.exists && !hasBase;
    bool localChanged = local.exists && hasBase &&
                        (local.mtime != base.localMtime ||
                         (base.localSize != 0 && local.size != base.localSize));

    // FAT32 mtime has 2-second granularity and some emulators never update
    // the timestamp at all.  Where a remote's tag is derivable from file
    // contents (Drive's MD5, Dropbox's content_hash), compare contents.
    if (local.exists && hasBase && !localChanged)
        for (auto &s : sides)
        {
            if (s.held != base.generation || s.rec.remoteTag.empty())
                continue;
            std::string tag = s.p->provider->localTag(localPath);
            if (tag.empty())
                continue;
            if (tag != s.rec.remoteTag)
            {
                debugf("%s: mtime unchanged but contents differ from the tag "
                       "recorded for %s\n", relPath.c_str(), s.p->label.c_str());
                localChanged = true;
            }
            break; // one content check settles it
        }

    // The decision below is the whole engine, and in a quiet run nothing
    // about it reaches the screen for a file that ends up skipped.
    if (debugEnabled())
    {
        std::string state;
        for (auto &s : sides)
        {
            char held[24];
            snprintf(held, sizeof(held), "%lld", s.held);
            state += " " + s.p->label + "=" + (s.file ? "yes" : "no") + "/g" + held +
                     (s.changed ? "/changed" : "");
        }
        debugf("%s: 3DS=%s/g%lld%s%s\n", relPath.c_str(), local.exists ? "yes" : "no",
               hasBase ? base.generation : 0LL,
               localNew ? "/new" : (localChanged ? "/changed" : ""), state.c_str());
    }

    // --- Gone everywhere ---
    bool anywhere = local.exists;
    for (auto &s : sides)
        anywhere = anywhere || s.file != NULL;
    if (!anywhere)
    {
        bool dropped = false;
        for (auto &s : sides)
            if (s.inManifest)
            {
                manifest.remove(s.key);
                dropped = true;
            }
        if (manifest.has(localKey))
        {
            manifest.remove(localKey);
            dropped = true;
        }
        if (dropped)
            debugf("  -> gone on every side, dropping its records\n");
        return true;
    }

    // --- Which sides changed it ---
    // -1 stands for the 3DS, anything else indexes sides.
    std::vector<int> candidates;
    if (localNew || localChanged)
        candidates.push_back(-1);
    for (size_t i = 0; i < sides.size(); i++)
    {
        Side &s = sides[i];
        if (!s.changed || s.p->sink)
            continue;
        // Changed to the very bytes the 3DS has: no conflict, nothing to copy.
        if (local.exists)
        {
            std::string tag = s.p->provider->localTag(localPath);
            if (!tag.empty() && tag == s.file->tag)
            {
                debugf("  %s holds the same contents as the 3DS\n", s.p->label.c_str());
                s.sameAsLocal = true;
                continue;
            }
        }
        candidates.push_back((int)i);
    }

    // A remote that changed a version older than the 3DS's has not seen the
    // 3DS's: the two are separate edits, not one newer than the other.
    if (local.exists && hasBase && (candidates.empty() || candidates[0] != -1))
        for (int c : candidates)
            if (sides[c].held < base.generation)
            {
                debugf("  %s changed it without the 3DS's last version\n",
                       sides[c].p->label.c_str());
                candidates.insert(candidates.begin(), -1);
                break;
            }

    // --- Pick the version every side ends up with ---
    int source = -2; // -2: none
    if (candidates.size() == 1)
        source = candidates[0];
    else if (candidates.size() > 1)
    {
        std::vector<std::string> names;
        for (int c : candidates)
            names.push_back(c < 0 ? std::string(LOCAL_SIDE) : sides[c].p->label);

        printf("\n  *** CONFLICT:\n  %s\n", localPath.c_str());
        printf("  Changed on %s.\n\n", join(names).c_str());
        ConflictChoice choice = ui.resolveConflict(localPath, names);

        if (choice.action == CONFLICT_CANCEL)
        {
            printf("  -> Sync cancelled\n");
            ui.requestCancel();
            return false;
        }
        if (choice.action == CONFLICT_SKIP || choice.side < 0 ||
            choice.side >= (int)candidates.size())
        {
            printf("  -> Skipped\n");
            summary.changes.push_back({localPath, "skipped", ""});
            return true;
        }
        source = candidates[choice.side];
        printf("  -> Keeping the %s version\n", names[choice.side].c_str());
    }
    else if (local.exists)
        source = -1; // nothing changed: the 3DS's copy is the current one
    else
    {
        // Missing on the 3DS and changed nowhere: fetch it again from the side
        // with the newest version.  A sink never serves one.
        for (size_t i = 0; i < sides.size(); i++)
            if (sides[i].file && !sides[i].p->sink &&
                (source < 0 || sides[i].held > sides[source].held))
                source = (int)i;
        if (source < 0)
        {
            debugf("  -> only on a sink, left alone\n");
            return true;
        }
    }

    size_t failuresBefore = summary.failures.size();
    long long baseGen = hasBase ? base.generation : 0;
    long long generation = baseGen;
    if (source == -1)
    {
        if (localNew || localChanged)
            generation = baseGen + 1;
    }
    else
    {
        Side &s = sides[source];
        // Restoring the version every side already agreed on keeps its number,
        // so the sides that hold it are not sent it again.
        if (!(hasBase && !s.changed && s.held == baseGen))
            generation = baseGen + 1;
        if (!downloadFrom(s, localPath, summary))
            return true; // nothing is copied from a version that did not arrive
    }

    local = statLocal(localPath);
    if (!local.exists)
        debugErrno("stat after sync", localPath);

    std::vector<std::string> uploadedTo;
    for (size_t i = 0; i < sides.size(); i++)
    {
        Side &s = sides[i];
        if ((int)i == source)
        {
            ManifestEntry rec = {local.mtime, local.size, s.file->tag, s.file->id, generation};
            manifest.set(s.key, rec);
            continue;
        }
        if (s.p->provider->hasFatalError())
            continue;

        if (s.sameAsLocal && source == -1)
        {
            ManifestEntry rec = {local.mtime, local.size, s.file->tag, s.file->id, generation};
            manifest.set(s.key, rec);
            continue;
        }

        if (s.p->readOnly)
        {
            // Nothing is sent to it.  A change of its own that did not win is
            // taken note of, so it is not raised again next run, while the
            // generation stays the one it really holds: it is still behind.
            if (s.changed)
            {
                ManifestEntry rec = {0, 0, s.file->tag, s.file->id, s.held};
                manifest.set(s.key, rec);
            }
            else if (s.inManifest && s.rec.generation != s.held)
            {
                s.rec.generation = s.held; // an older record, now numbered
                manifest.set(s.key, s.rec);
            }
            if (s.file == NULL || s.held < generation)
                debugf("  %s is read-only, not sent this version\n", s.p->label.c_str());
            continue;
        }

        bool behind = s.file == NULL || s.held == 0 || s.held < generation;
        // A sink is only sent what it lacks; a change made on it is left alone.
        // Any other side that changed and is not the source lost a conflict.
        bool upload = s.p->sink ? behind : (behind || s.changed);
        if (!upload)
        {
            if (s.rec.generation != generation)
            {
                s.rec.generation = generation; // an older record, now numbered
                manifest.set(s.key, s.rec);
            }
            continue;
        }
        if (uploadTo(s, relPath, localPath, generation, summary))
        {
            manifest.set(s.key, s.rec);
            uploadedTo.push_back(s.p->label);
        }
    }

    if (local.exists)
    {
        ManifestEntry rec = {local.mtime, local.size, "", "", generation};
        manifest.set(localKey, rec);
    }

    if (source >= 0)
        summary.changes.push_back({localPath, "downloaded", sides[source].p->label});
    if (!uploadedTo.empty())
        summary.changes.push_back({localPath, "uploaded", join(uploadedTo)});
    if (source < 0 && uploadedTo.empty() && summary.failures.size() == failuresBefore)
        summary.checkedPaths.insert(localPath);
    return true;
}

// ---------------------------------------------------------------------------
// syncEntry
// ---------------------------------------------------------------------------
bool syncEntry(std::vector<Participant> &participants, Manifest &manifest,
               const SyncEntry &entry, SyncSummary &summary, SyncUi &ui)
{
    bool uploadOnly = (entry.direction == SYNC_UPLOAD_ONLY);

    // --- Every side is listed before anything is decided ---
    bool anyListed = false;
    for (auto &p : participants)
    {
        p.listed = false;
        p.files.clear();
        if (uploadOnly && p.readOnly)
        {
            debugf("%s is read-only, left out of upload-only %s\n", p.label.c_str(),
                   entry.remoteName.c_str());
            continue;
        }
        if (uploadOnly)
            p.sink = true;
        SyncProvider &provider = *p.provider;
        if (provider.hasFatalError() || ui.cancelRequested())
            continue;

        // Resolve (and create if missing) the remote folder for this entry
        p.root = provider.ensureRoot(entry.remoteName);
        if (p.root.empty() && !entry.remoteName.empty())
        {
            if (provider.hasFatalError())
                continue;
            printf("Cannot resolve %s folder for %s — skipping\n", p.label.c_str(),
                   entry.remoteName.c_str());
            debugf("ensureRoot(\"%s\") returned nothing\n", entry.remoteName.c_str());
            summary.problems.push_back(p.label + ":" + entry.remoteName + " (folder not found)");
            continue;
        }

        printf("Listing %s:%s\n", p.label.c_str(), entry.remoteName.c_str());
        if (!provider.list(p.root, p.files))
        {
            // A folder that cannot be listed is left out, not treated as
            // empty — which would upload every local file over whatever is
            // really there.
            if (provider.hasFatalError())
                continue;
            printf("Cannot list %s:%s — skipping\n", p.label.c_str(), entry.remoteName.c_str());
            debugf("list(\"%s\") failed; %d entries had been collected\n", p.root.c_str(),
                   (int)p.files.size());
            summary.problems.push_back(p.label + ":" + entry.remoteName + " (not listed)");
            p.files.clear();
            continue;
        }
        // A name that could reach outside the local folder ("/../x") is
        // refused before anything is decided: nothing is downloaded to it or
        // compared with it.  Said once per remote and entry, and counted as a
        // problem, so the summary does not report a clean run.
        int unsafe = 0;
        for (auto it = p.files.begin(); it != p.files.end();)
        {
            if (isSafeRelativePath(it->first))
            {
                ++it;
                continue;
            }
            debugf("unsafe name from %s skipped: \"%s\"\n", p.label.c_str(), it->first.c_str());
            it = p.files.erase(it);
            unsafe++;
        }
        if (unsafe > 0)
        {
            printf(CONSOLE_RED "%s:%s: %d file(s) with an unsafe\n" CONSOLE_RESET, p.label.c_str(),
                   entry.remoteName.c_str(), unsafe);
            printf(CONSOLE_RED "  name (such as ../) skipped\n" CONSOLE_RESET);
            summary.problems.push_back(p.label + ":" + entry.remoteName + " (unsafe file name skipped)");
        }
        debugf("%s root \"%s\": %d remote file(s), %d local file(s)\n", p.label.c_str(),
               p.root.c_str(), (int)p.files.size(), (int)entry.localFiles.size());
        p.listed = true;
        anyListed = true;
    }
    if (!anyListed)
        return !ui.cancelRequested();

    // Build the full set of relative paths to consider: everything local, plus
    // everything on a remote that is not only receiving.
    // The extension filter applies to every side, so a remote-only file it
    // excludes is not downloaded either.
    std::set<std::string> allRelPaths;
    std::set<std::string> filtered;
    for (auto &f : entry.localFiles)
        (entry.filter.matches(f) ? allRelPaths : filtered).insert(f);
    for (auto &p : participants)
        if (p.listed && !p.sink)
            for (auto &rf : p.files)
                (entry.filter.matches(rf.first) ? allRelPaths : filtered).insert(rf.first);
    if (!filtered.empty())
        debugf("%d file(s) left out by the extension filter (%s)\n", (int)filtered.size(),
               entry.filter.describe().c_str());

    for (auto &relPath : allRelPaths)
    {
        // Checked between files so a cancellation takes effect at a point
        // where the manifest is consistent.
        if (ui.cancelRequested())
            break;

        std::vector<Participant *> live;
        for (auto &p : participants)
            if (p.listed && !p.provider->hasFatalError())
                live.push_back(&p);
        if (live.empty())
            break;

        if (!syncFile(live, manifest, relPath, entry.localBase + relPath, summary, ui))
            return false;
    }

    return !ui.cancelRequested();
}

bool performSync(SyncProvider &provider, Manifest &manifest, const SyncEntry &entry,
                 SyncSummary &summary, bool uploadOnly, SyncUi &ui)
{
    std::vector<Participant> one;
    one.push_back(Participant(provider, provider.name(), provider.manifestPrefix(), uploadOnly));
    bool ok = syncEntry(one, manifest, entry, summary, ui);
    return ok && !provider.hasFatalError();
}

// ---------------------------------------------------------------------------
// The end-of-run report
// ---------------------------------------------------------------------------
std::vector<std::string> summaryLines(const SyncSummary &summary)
{
    std::vector<std::string> out;
    char buf[64];
    int unchanged = (int)summary.checkedPaths.size();

    if (summary.changes.empty() && !summary.hasErrors())
    {
        if (unchanged == 0)
            out.push_back("No files to sync.");
        else
        {
            snprintf(buf, sizeof(buf), "All %d files up to date.", unchanged);
            out.push_back(buf);
        }
        return out;
    }

    snprintf(buf, sizeof(buf), "Uploaded: %d  Downloaded: %d  Unchanged: %d", summary.uploaded,
             summary.downloaded, unchanged);
    out.push_back(buf);
    if (!summary.failures.empty())
    {
        snprintf(buf, sizeof(buf), CONSOLE_RED "Failed: %d" CONSOLE_RESET,
                 (int)summary.failures.size());
        out.push_back(buf);
    }

    if (!summary.changes.empty())
    {
        out.push_back("");
        out.push_back("Changed files:");
        for (auto &c : summary.changes)
            out.push_back("  " + c.action + (c.remote.empty() ? "" : " (" + c.remote + ")") +
                          ": " + c.path);
    }
    if (!summary.failures.empty())
    {
        out.push_back("");
        out.push_back(CONSOLE_RED "Failed, retried next sync:" CONSOLE_RESET);
        for (auto &f : summary.failures)
            out.push_back("  " + f.action + (f.remote.empty() ? "" : " (" + f.remote + ")") +
                          ": " + f.path);
    }
    if (!summary.problems.empty())
    {
        out.push_back("");
        out.push_back(CONSOLE_RED "Not synced:" CONSOLE_RESET);
        for (auto &p : summary.problems)
            out.push_back("  " + p);
    }
    return out;
}

std::string summaryResult(const SyncSummary &summary)
{
    char buf[64];
    if (!summary.failures.empty())
        snprintf(buf, sizeof(buf), "%d failed, %d up, %d down", (int)summary.failures.size(),
                 summary.uploaded, summary.downloaded);
    else if (!summary.problems.empty())
        snprintf(buf, sizeof(buf), "%d up, %d down, not all synced", summary.uploaded,
                 summary.downloaded);
    else if (summary.changes.empty())
        return "everything up to date";
    else
        snprintf(buf, sizeof(buf), "%d up, %d down", summary.uploaded, summary.downloaded);
    return buf;
}
