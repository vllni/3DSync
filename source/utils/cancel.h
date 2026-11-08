#ifndef UTILS_CANCEL_H
#define UTILS_CANCEL_H

// START during a run, as a transfer in progress sees it.  The engine asks
// SyncUi::cancelRequested() between files, but one file can take minutes (a
// ROM over FTP), so a download or upload also checks this while it runs and
// stops part way.  The check is installed by the app (frontend's cancel
// flag); without one, nothing is ever cancelled here.
void setTransferCancelCheck(bool (*check)());
bool transferCancelled();

#endif
