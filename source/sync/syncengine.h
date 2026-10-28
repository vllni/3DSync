#ifndef SYNC_SYNCENGINE_H
#define SYNC_SYNCENGINE_H

#include <set>
#include <string>
#include <vector>

#include "../modules/manifest.h"
#include "../modules/syncprovider.h"

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
// ConflictChoice / SyncUi
// ---------------------------------------------------------------------------
// The engine needs two things from the outside world that are not a remote:
// a way to notice the user asking to stop, and a way to resolve a conflict.
// Both arrive through SyncUi, so the decision table below can be driven by the
// 3DS console in the app and by a scripted stand-in in the tests.
// ---------------------------------------------------------------------------
enum ConflictChoice
{
    CONFLICT_KEEP_LOCAL,
    CONFLICT_KEEP_REMOTE,
    CONFLICT_SKIP,
    CONFLICT_CANCEL
};

class SyncUi
{
public:
    virtual ~SyncUi() {}

    // Polled between files.  Latches: once it answers true it keeps doing so.
    virtual bool cancelRequested() = 0;

    // Called when the engine itself decides to stop, so the caller's view of
    // cancellation matches the engine's.
    virtual void requestCancel() = 0;

    // Both sides of localPath changed since the last sync.
    virtual ConflictChoice resolveConflict(const std::string &localPath) = 0;
};

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
    // Transfers that failed ("upload" / "download" and the local path).  They
    // are retried next run, but this run did not do them, and saying "up to
    // date" over them is how a backup silently stops being one.
    std::vector<FileAction> failures;
    // Whole entries or remotes that were not synced: not reached, a folder
    // that could not be resolved or listed, a run stopped by a fatal error.
    std::vector<std::string> problems;
    SyncSummary() : uploaded(0), downloaded(0) {}

    bool hasErrors() const { return !failures.empty() || !problems.empty(); }
};

// The "--- Sync Summary ---" block, one console line per element, colour
// codes included.  Kept here rather than in main.cpp so it can be tested.
std::vector<std::string> summaryLines(const SyncSummary &summary);

// The one-line result for the bottom screen, e.g. "2 up, 1 down".
std::string summaryResult(const SyncSummary &summary);

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
bool performSync(SyncProvider &provider, Manifest &manifest, const SyncEntry &entry,
                 SyncSummary &summary, bool uploadOnly, SyncUi &ui);

#endif
