#ifndef UTILS_DEBUG_H
#define UTILS_DEBUG_H

#include <string>

// ---------------------------------------------------------------------------
// Runtime verbose logging
// ---------------------------------------------------------------------------
// A normal sync is deliberately quiet (see the output rules in AGENTS.md), so
// most failures are handled where they happen: a listing that 404s is treated
// as an empty folder, a per-file HTTP error is skipped, a stat() that fails is
// read as "unchanged".  That is right for the console but leaves nothing to go
// on in a bug report, so every one of those places also calls debugf(), which
// prints only when the sync was started in debug mode (Y on the main menu).
//
// Nothing here is compiled out: the flag is set once before the first transfer
// and checked per call, so a release build can be put into debug mode without
// reflashing.  Free of <3ds.h> so the sync engine can log from the host tests.
// ---------------------------------------------------------------------------

void setDebugEnabled(bool enabled);
bool debugEnabled();

// printf to the console, prefixed with "dbg: " and dimmed, when debug mode is
// on; a no-op otherwise.  Callers do not need to check debugEnabled() first.
void debugf(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Dump a response body (or any blob) under a label, truncated to keep one bad
// response from scrolling the rest of the run off a 30-line screen.  Passed
// through debugRedact() first, so an OAuth response cannot print a token.
void debugBody(const char *label, const std::string &body);

// Same, for a body that was streamed to a file instead of buffered — a failed
// download writes the server's error page into the temp file, which is then
// removed unread.  Reads it back and dumps it before the caller deletes it.
void debugFileBody(const char *label, const std::string &path);

// Report a failed libc call together with strerror(errno).
void debugErrno(const char *what, const std::string &path);

// Mask secrets in text about to be printed.  Credentials live in 3DSync.ini in
// plaintext and OAuth responses carry tokens, and a debug session is exactly
// the output someone pastes into an issue, so bearer tokens and the JSON token
// fields are replaced with "<redacted>".  Exposed for the tests.
std::string debugRedact(const std::string &text);

// ---------------------------------------------------------------------------
// Session log
// ---------------------------------------------------------------------------
// The console is 30 lines and scrolls, so by the time a debug run reaches the
// failure worth reporting, the connect, the config summary and the first
// listing have long gone off the top.  Everything written to the console is
// therefore also kept here while debug mode is on, and the end of the run
// offers to write it to the SD card.
//
// The capture is fed by whoever owns stdout — on the console that is a tee
// installed over the libctru console devoptab in main.cpp, so nothing else has
// to be routed through debugf() to end up in the log.  The host tests install
// nothing, and the log simply stays empty there.
// ---------------------------------------------------------------------------

// Drop whatever was captured and start a fresh session.  Called when a run
// begins, so a second sync does not save the first one's log with it.
void debugLogReset();

// Append raw console bytes.  A no-op unless debug mode is on.  Only the head
// and the tail of a long run are kept; see debugLogDropped().
void debugLogAppend(const char *data, size_t length);

// Bytes currently held, and how many were dropped from the middle of a run
// that outgrew the buffer.
size_t debugLogSize();
size_t debugLogDropped();

// Write the captured session to path: ANSI colour codes stripped, the whole
// text passed through debugRedact(), parent directories created.  Returns
// false if the file could not be written.
bool debugLogSave(const std::string &path);

#endif
