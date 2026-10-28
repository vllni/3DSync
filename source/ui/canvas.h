#ifndef UI_CANVAS_H
#define UI_CANVAS_H

#include <stdint.h>
#include <string>
#include <vector>

#include "font.h"

// ---------------------------------------------------------------------------
// Software drawing into an RGB565 buffer
// ---------------------------------------------------------------------------
// Both screens are drawn here, row-major and the right way up, and copied to
// the rotated 3DS framebuffers in frontend.cpp.  Nothing in this file touches
// libctru, so the screens can be rendered and checked on the host.
// ---------------------------------------------------------------------------
namespace ui
{

typedef uint16_t Color;

constexpr Color rgb(unsigned r, unsigned g, unsigned b)
{
    return (Color)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

struct Rect
{
    int x, y, w, h;
    bool contains(int px, int py) const
    {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

class Canvas
{
public:
    Canvas(int width, int height);

    int width() const { return _w; }
    int height() const { return _h; }
    const Color *pixels() const { return _px.data(); }

    // Drawing outside the clip rectangle is discarded.  Defaults to the whole
    // canvas; the log uses it so a half-visible row is cut at the panel edge.
    void setClip(const Rect &r);
    void resetClip();

    void clear(Color c);
    void fill(int x, int y, int w, int h, Color c);
    // alpha 0..256
    void fillAlpha(int x, int y, int w, int h, Color c, int alpha);
    void vgradient(int x, int y, int w, int h, Color top, Color bottom);
    void roundRect(int x, int y, int w, int h, int r, Color c);
    void roundRectOutline(int x, int y, int w, int h, int r, Color c);
    void circle(int cx, int cy, int r, Color c);
    // Filled isosceles triangle pointing up (dir < 0) or down (dir > 0),
    // centred on (cx, cy), half as tall as it is wide.
    void triangle(int cx, int cy, int halfWidth, int dir, Color c);

    // Draw UTF-8 text with the top of the line at y.  With maxWidth > 0 the
    // text is cut to fit and ends in "…".  Returns the pen x after the text.
    int text(const Font &f, int x, int y, const std::string &s, Color c, int maxWidth = 0);
    // Text vertically centred in a box of height h at y.
    int textMiddle(const Font &f, int x, int y, int h, const std::string &s, Color c,
                   int maxWidth = 0);
    void textCentered(const Font &f, int cx, int y, const std::string &s, Color c);
    void textRight(const Font &f, int rx, int y, const std::string &s, Color c);

    // Draw one glyph at pen position (x, y); used by the log, which colours
    // per character.
    void glyph(const Font &f, uint32_t cp, int x, int y, Color c);

    // Blend c into one pixel; alpha 0..256.
    void plot(int x, int y, Color c, int alpha);

private:
    int _w, _h;
    Rect _clip;
    std::vector<Color> _px;
};

int textWidth(const Font &f, const std::string &s);

// Greedy word wrap to at most maxLines lines of maxWidth pixels.  Lines break
// after a space or a '/', so a long save path wraps between folder names; a
// run with no break in it is cut mid-word.  Text that does not fit in maxLines
// ends in "…".
std::vector<std::string> wrapText(const Font &f, const std::string &s, int maxWidth,
                                  int maxLines);

} // namespace ui

#endif
