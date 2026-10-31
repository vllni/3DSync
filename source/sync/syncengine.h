#ifndef SYNC_SYNCENGINE_H
#define SYNC_SYNCENGINE_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "../modules/manifest.h"
#include "../modules/syncprovider.h"
#include "extfilter.h"

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
    // Files on either side that do not match are left out of the sync.
    ExtensionFilter filter;
};

// ---------------------------------------------------------------------------
// ConflictChoice / SyncUi
// ---------------------------------------------------------------------------
// The engine needs two things from the outside world that are not a remote:
// a way to notice the user asking to stop, and a way to resolve a conflict.
// Both arrive through SyncUi, so the decision table below can be driven by the
// 3DS console in the app and by a scripted stand-in in the tests.
// ---------------------------------------------------------------------------
enum ConflictAction
{
    CONFLICT_KEEP, // keep the side at ConflictChoice::side
    CONFLICT_SKIP,
    CONFLICT_CANCEL
};

struct ConflictChoice
{
    ConflictAction action;
    int side; // index into the sides offered, for CONFLICT_KEEP

    static ConflictChoice keep(int side) { return {CONFLICT_KEEP, side}; }
    static ConflictChoice skip() { return {CONFLICT_SKIP, -1}; }
    static ConflictChoice cancel() { return {CONFLICT_CANCEL, -1}; }
};

// The name the 3DS goes by among the sides of a conflict.  It is always the
// first side offered when it is one of them.
extern const char *const LOCAL_SIDE;

class SyncUi
{
public:
    virtual ~SyncUi() {}

    // Polled between files.  Latches: once it answers true it keeps doing so.
    virtual bool cancelRequested() = 0;

    // Called when the engine itself decides to stop, so the caller's view of
    // cancellation matches the engine's.
    virtual void requestCancel() = 0;

    // localPath changed on more than one side since they last agreed.  sides
    // names each of them, LOCAL_SIDE or a participant's label; there are at
    // least two.
    virtual ConflictChoice resolveConflict(const std::string &localPath,
                                           const std::vector<std::string> &sides) = 0;
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
        // Which remote(s) it happened with: "Dropbox", or "SMB, WebDAV" for
        // one upload to each.  Empty for "skipped", which concerns them all.
        std::string remote;
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
// syncEntry  — sync one SyncEntry across every remote that holds it
// ---------------------------------------------------------------------------
// An n-way sync: every participant is listed first, then each file is decided
// once with all sides in view.  The side that changed wins and its version is
// copied to every other; when several changed, the user picks one.  Order does
// not matter, a remote that missed a change is brought up to date rather than
// taken for one that made it, and two remotes changing the same file are a
// conflict between them instead of one silently overwriting the other.
//
// A file missing from a side is fetched again, never deleted elsewhere.
// ---------------------------------------------------------------------------
struct Participant
{
    SyncProvider *provider;
    std::string label;     // for the log, prompts and summary: "SMB"
    std::string keyPrefix; // manifest key scope, without the '|'
    // Only ever receives: it is never a source of a change, is never asked
    // about, and files that exist only there are left alone.  Every remote
    // is one for an upload-only entry.
    bool sink;
    // The reverse: never written to.  A change made there is copied to the
    // 3DS and every other side, but one made elsewhere never reaches it, and
    // it takes no part in an upload-only entry.
    bool readOnly;

    // Filled in by syncEntry().
    std::string root;
    std::map<std::string, RemoteFileInfo> files;
    bool listed;

    Participant(SyncProvider &p, const std::string &label, const std::string &keyPrefix,
                bool sink = false, bool readOnly = false)
        : provider(&p), label(label), keyPrefix(keyPrefix), sink(sink), readOnly(readOnly),
          listed(false)
    {
    }
};

// Returns false once cancellation was requested.  A participant that hits a
// fatal error is dropped for the rest of the entry and the others carry on:
// the caller checks hasFatalError() on each afterwards.
bool syncEntry(std::vector<Participant> &participants, Manifest &manifest,
               const SyncEntry &entry, SyncSummary &summary, SyncUi &ui);

// syncEntry() with a single remote.  uploadOnly makes it a sink.  Returns false
// if a fatal provider error occurred or cancellation was requested.
bool performSync(SyncProvider &provider, Manifest &manifest, const SyncEntry &entry,
                 SyncSummary &summary, bool uploadOnly, SyncUi &ui);

#endif
