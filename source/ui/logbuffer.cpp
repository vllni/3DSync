#include "logbuffer.h"

namespace ui
{

// ---------------------------------------------------------------------------
// LogBuffer
// ---------------------------------------------------------------------------
LogBuffer::LogBuffer(int columns, size_t maxRows)
    : _columns(columns > 0 ? columns : 1), _maxRows(maxRows > 1 ? maxRows : 2),
      _dropped(0), _color(LOG_DEFAULT), _revision(0), _escState(0), _escCurrent(-1),
      _utf8Cp(0), _utf8Need(0)
{
    _rows.push_back(LogRow());
}

void LogBuffer::clear()
{
    _rows.clear();
    _rows.push_back(LogRow());
    _dropped = 0;
    _color = LOG_DEFAULT;
    _escState = 0;
    _utf8Need = 0;
    _revision++;
}

size_t LogBuffer::endRow() const
{
    size_t n = _dropped + _rows.size();
    if (_rows.back().empty())
        n--;
    return n;
}

void LogBuffer::_newline()
{
    _rows.push_back(LogRow());
    while (_rows.size() > _maxRows)
    {
        _rows.pop_front();
        _dropped++;
    }
}

void LogBuffer::_put(uint32_t cp)
{
    if (cp > 0xFFFF)
        cp = 0xFFFD;

    LogRow *cur = &_rows.back();
    if ((int)cur->size() >= _columns)
    {
        // Word wrap: carry the word being written over to the next row when
        // the row has an earlier space to break at.  A space arriving at the
        // edge just ends the row.
        if (cp == ' ')
        {
            _newline();
            return;
        }
        size_t brk = cur->size();
        while (brk > 0 && (*cur)[brk - 1].cp != ' ')
            brk--;
        LogRow carry;
        if (brk > 0)
        {
            carry.assign(cur->begin() + brk, cur->end());
            cur->erase(cur->begin() + brk, cur->end());
        }
        _newline();
        cur = &_rows.back();
        *cur = carry;
    }
    LogCell cell;
    cell.cp = (uint16_t)cp;
    cell.color = _color;
    cur->push_back(cell);
}

void LogBuffer::_applySgr()
{
    if (_escParams.empty())
        _escParams.push_back(0);
    for (int p : _escParams)
    {
        if (p == 0 || p == 39)
            _color = LOG_DEFAULT;
        else if (p >= 30 && p <= 37)
            _color = (uint8_t)(LOG_BLACK + (p - 30));
        // Bold, dim and the background colours have no counterpart here;
        // every CONSOLE_* colour carries ";1" and it is simply ignored.
    }
}

void LogBuffer::append(const char *data, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        unsigned char c = (unsigned char)data[i];

        if (_escState == 1)
        {
            if (c == '[')
            {
                _escState = 2;
                _escParams.clear();
                _escCurrent = -1;
            }
            else
                _escState = 0; // not a CSI sequence: drop it
            continue;
        }
        if (_escState == 2)
        {
            if (c >= '0' && c <= '9')
                _escCurrent = (_escCurrent < 0 ? 0 : _escCurrent * 10) + (c - '0');
            else if (c == ';')
            {
                _escParams.push_back(_escCurrent < 0 ? 0 : _escCurrent);
                _escCurrent = -1;
            }
            else if (c >= 0x40 && c <= 0x7E)
            {
                if (_escCurrent >= 0 || !_escParams.empty())
                    _escParams.push_back(_escCurrent < 0 ? 0 : _escCurrent);
                if (c == 'm')
                    _applySgr();
                // Cursor movement and clearing are not used by the app, and a
                // scrollback has no cursor to move.
                _escState = 0;
            }
            continue;
        }

        if (_utf8Need > 0)
        {
            if ((c & 0xC0) == 0x80)
            {
                _utf8Cp = (_utf8Cp << 6) | (c & 0x3F);
                if (--_utf8Need == 0)
                    _put(_utf8Cp);
                continue;
            }
            // Broken sequence: show it, then treat this byte afresh.
            _utf8Need = 0;
            _put(0xFFFD);
        }

        if (c == 0x1B)
            _escState = 1;
        else if (c == '\n')
            _newline();
        else if (c == '\r')
            ; // every line the app prints ends in "\n"; nothing rewrites one
        else if (c == '\t')
        {
            do
                _put(' ');
            while (_rows.back().size() % 4 != 0 && (int)_rows.back().size() < _columns);
        }
        else if (c < 0x20 || c == 0x7F)
            ; // other control characters have no glyph
        else if (c < 0x80)
            _put(c);
        else if ((c & 0xE0) == 0xC0)
        {
            _utf8Cp = c & 0x1F;
            _utf8Need = 1;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            _utf8Cp = c & 0x0F;
            _utf8Need = 2;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            _utf8Cp = c & 0x07;
            _utf8Need = 3;
        }
        else
            _put(0xFFFD);
    }
    _revision++;
}

// ---------------------------------------------------------------------------
// LogScroll
// ---------------------------------------------------------------------------
LogScroll::LogScroll(int visibleRows)
    : _visible(visibleRows > 0 ? visibleRows : 1), _top(0), _follow(true)
{
}

size_t LogScroll::_maxTop(const LogBuffer &log) const
{
    size_t end = log.endRow();
    if (end - log.firstRow() <= (size_t)_visible)
        return log.firstRow();
    return end - _visible;
}

void LogScroll::update(const LogBuffer &log)
{
    size_t maxTop = _maxTop(log);
    if (_follow || _top > maxTop)
        _top = maxTop;
    if (_top < log.firstRow())
        _top = log.firstRow();
}

void LogScroll::scrollBy(const LogBuffer &log, long rows)
{
    update(log);
    size_t maxTop = _maxTop(log);
    long top = (long)_top + rows;
    if (top < (long)log.firstRow())
        top = (long)log.firstRow();
    if (top > (long)maxTop)
        top = (long)maxTop;
    _top = (size_t)top;
    _follow = (_top == maxTop);
}

void LogScroll::scrollToFraction(const LogBuffer &log, float fraction)
{
    if (fraction < 0)
        fraction = 0;
    if (fraction > 1)
        fraction = 1;
    size_t first = log.firstRow(), maxTop = _maxTop(log);
    _top = first + (size_t)((maxTop - first) * fraction + 0.5f);
    _follow = (_top == maxTop);
}

void LogScroll::scrollToEnd(const LogBuffer &log)
{
    _follow = true;
    update(log);
}

size_t LogScroll::unseenRows(const LogBuffer &log) const
{
    size_t bottom = _top + _visible;
    size_t end = log.endRow();
    return end > bottom ? end - bottom : 0;
}

} // namespace ui
