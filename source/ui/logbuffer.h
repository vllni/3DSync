#ifndef UI_LOGBUFFER_H
#define UI_LOGBUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <deque>
#include <vector>

// ---------------------------------------------------------------------------
// LogBuffer  — the scrollback behind the top-screen log
// ---------------------------------------------------------------------------
// Everything the app prints lands here (frontend.cpp owns stdout), already cut
// into screen rows: the log font is monospaced, so wrapping is a column count
// and is done once, as text arrives, rather than on every frame.  A line too
// long for a row breaks after its last space where there is one.
//
// The CONSOLE_* escape sequences the rest of the app prints are interpreted
// here as colours, which is all libctru's console did with them.  Rows are
// numbered from the start of the session; once more than maxRows exist the
// oldest are dropped, and firstRow() says how many have gone, so a scroll
// position held as a row number stays put while the front is trimmed.
// Not thread-safe: the frontend holds a lock around it.
// ---------------------------------------------------------------------------
namespace ui
{

enum LogColor
{
    LOG_DEFAULT,
    LOG_BLACK, // SGR 30 — drawn as a dim grey, black is the background
    LOG_RED,
    LOG_GREEN,
    LOG_YELLOW,
    LOG_BLUE,
    LOG_MAGENTA,
    LOG_CYAN,
    LOG_WHITE
};

struct LogCell
{
    uint16_t cp; // code points above U+FFFF are stored as U+FFFD
    uint8_t color;
};

typedef std::vector<LogCell> LogRow;

class LogBuffer
{
public:
    LogBuffer(int columns, size_t maxRows);

    void append(const char *data, size_t length);
    void clear();

    int columns() const { return _columns; }

    // Row numbers run from firstRow() to endRow() - 1.  An empty row the
    // cursor is waiting on, after a trailing newline, is not counted.
    size_t firstRow() const { return _dropped; }
    size_t endRow() const;
    const LogRow &row(size_t number) const { return _rows[number - _dropped]; }

    // Bumped on every change, so a renderer can tell whether to redraw.
    uint32_t revision() const { return _revision; }

private:
    void _put(uint32_t cp);
    void _newline();
    void _applySgr();

    int _columns;
    size_t _maxRows;
    std::deque<LogRow> _rows; // never empty; the back is the row being written
    size_t _dropped;
    uint8_t _color;
    uint32_t _revision;

    // Escape-sequence and UTF-8 decoding state, carried across append() calls:
    // printf may hand a sequence over in two writes.
    int _escState; // 0 text, 1 after ESC, 2 inside CSI
    std::vector<int> _escParams;
    int _escCurrent;
    uint32_t _utf8Cp;
    int _utf8Need;
};

// ---------------------------------------------------------------------------
// LogScroll  — which rows the top screen shows
// ---------------------------------------------------------------------------
// Follows the end of the log until the user scrolls up, then holds still —
// new output must not yank the text someone is reading — and resumes following
// once they scroll back to the bottom.
// ---------------------------------------------------------------------------
class LogScroll
{
public:
    explicit LogScroll(int visibleRows);

    // Recompute the top row against the buffer's current extent.  Call after
    // every append and before drawing.
    void update(const LogBuffer &log);

    void scrollBy(const LogBuffer &log, long rows);
    // Jump so that fraction (0 = oldest, 1 = newest) of the scrollable range
    // is above the view; from dragging the scroll bar.
    void scrollToFraction(const LogBuffer &log, float fraction);
    void scrollToEnd(const LogBuffer &log);

    size_t top() const { return _top; }
    int visibleRows() const { return _visible; }
    bool following() const { return _follow; }
    // Rows that arrived below the view since the user scrolled away.
    size_t unseenRows(const LogBuffer &log) const;

private:
    size_t _maxTop(const LogBuffer &log) const;

    int _visible;
    size_t _top;
    bool _follow;
};

} // namespace ui

#endif
