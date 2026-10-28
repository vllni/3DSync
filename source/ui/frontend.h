#ifndef UI_FRONTEND_H
#define UI_FRONTEND_H

#include <string>

#include "../sync/syncengine.h"

// ---------------------------------------------------------------------------
// frontend  — the 3DS side of the UI: screens, input, and the sync thread
// ---------------------------------------------------------------------------
// The log scrolls and the touch buttons answer while a sync is running, which
// a single thread cannot do: a sync spends most of its time blocked inside
// libcurl or libsmb2.  So the sync runs on a worker thread (runWorker) and the
// main thread stays in a 60 Hz loop that reads the buttons and touch screen,
// scrolls the log and redraws.  The worker never touches HID or the screens;
// it prints, sets the status line, and blocks in a prompt when it needs an
// answer — WorkerSyncUi below is the engine's view of that.
//
// stdout belongs to the frontend: everything printed, from either thread, is
// appended to the top-screen log and to the debug-log capture.
// ---------------------------------------------------------------------------
namespace frontend
{

// Graphics, stdout and input.  Call first; nothing printed before it shows.
bool init();
void exit();

// What the idle screen says about the configuration.  fileFilter is
// ExtensionFilter::describe(): "all files" or ".sav .bin".
void setMenuInfo(bool configLoaded, const std::string &remotes, int pathCount,
                 const std::string &fileFilter);

enum MenuChoice
{
    MENU_EXIT,
    MENU_SYNC,
    MENU_SYNC_DEBUG
};

// The idle screen: A syncs, Y syncs in debug mode, START exits.  Returns
// MENU_EXIT when the system asks the app to close as well.
MenuChoice waitForMenu();

// Run fn(arg) on the worker thread while this thread keeps the UI live, and
// return once it has finished.  If the app is closed meanwhile, the run is
// cancelled and any open prompt answered as "stop", then awaited.
void runWorker(void (*fn)(void *), void *arg);

// ---- from the worker ------------------------------------------------------

// The remote and path the running card shows.
void setActivity(const std::string &provider, const std::string &entry);

// The menu card's "last run" line.
void setLastResult(const std::string &text, bool ok);

// End of a debug run: true to save the captured log.
bool askSaveDebugLog(unsigned kilobytes, bool trimmed);

// SyncUi for one run.  Cancellation is START (or its touch button) on the UI
// thread; conflicts are asked on the bottom screen.  "For all" (L) lives here
// rather than in the engine: it is a property of how this front-end asks, not
// of the sync rules.
class WorkerSyncUi : public SyncUi
{
public:
    WorkerSyncUi();
    bool cancelRequested() override;
    void requestCancel() override;
    ConflictChoice resolveConflict(const std::string &localPath) override;

private:
    bool _cancelled;
    bool _announced;
    int _applyAllChoice; // -1, or the ConflictChoice for every later conflict
};

} // namespace frontend

#endif
