#include "canvas.h"

#include <math.h>
#include <string.h>

namespace ui
{

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
const Glyph &findGlyph(const Font &font, uint32_t cp)
{
    int lo = 0, hi = font.count - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (font.glyphs[mid].cp == cp)
            return font.glyphs[mid];
        if (font.glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    // ASCII starts the table, so '?' is at a fixed index.
    return font.glyphs['?' - 0x20];
}

uint32_t utf8Next(const char *&p, const char *end)
{
    unsigned char c = (unsigned char)*p++;
    if (c < 0x80)
        return c;

    int extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0)
    {
        extra = 1;
        cp = c & 0x1F;
    }
    else if ((c & 0xF0) == 0xE0)
    {
        extra = 2;
        cp = c & 0x0F;
    }
    else if ((c & 0xF8) == 0xF0)
    {
        extra = 3;
        cp = c & 0x07;
    }
    else
        return 0xFFFD;

    const char *q = p;
    for (int i = 0; i < extra; i++, q++)
    {
        if (q >= end || ((unsigned char)*q & 0xC0) != 0x80)
            return 0xFFFD; // p already moved past the lead byte only
        cp = (cp << 6) | ((unsigned char)*q & 0x3F);
    }
    p = q;
    return cp;
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------
Canvas::Canvas(int width, int height)
    : _w(width), _h(height), _px((size_t)width * height, 0)
{
    resetClip();
}

void Canvas::setClip(const Rect &r)
{
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > _w ? _w : r.x + r.w;
    int y1 = r.y + r.h > _h ? _h : r.y + r.h;
    _clip.x = x0;
    _clip.y = y0;
    _clip.w = x1 > x0 ? x1 - x0 : 0;
    _clip.h = y1 > y0 ? y1 - y0 : 0;
}

void Canvas::resetClip()
{
    _clip.x = 0;
    _clip.y = 0;
    _clip.w = _w;
    _clip.h = _h;
}

static inline Color blend(Color d, Color s, int a)
{
    int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    int sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
    dr += ((sr - dr) * a) >> 8;
    dg += ((sg - dg) * a) >> 8;
    db += ((sb - db) * a) >> 8;
    return (Color)((dr << 11) | (dg << 5) | db);
}

void Canvas::plot(int x, int y, Color c, int alpha)
{
    if (alpha <= 0 || !_clip.contains(x, y))
        return;
    Color &p = _px[(size_t)y * _w + x];
    p = alpha >= 256 ? c : blend(p, c, alpha);
}

void Canvas::clear(Color c)
{
    for (auto &p : _px)
        p = c;
}

void Canvas::fill(int x, int y, int w, int h, Color c)
{
    int x0 = x < _clip.x ? _clip.x : x, y0 = y < _clip.y ? _clip.y : y;
    int x1 = x + w > _clip.x + _clip.w ? _clip.x + _clip.w : x + w;
    int y1 = y + h > _clip.y + _clip.h ? _clip.y + _clip.h : y + h;
    for (int j = y0; j < y1; j++)
    {
        Color *row = &_px[(size_t)j * _w];
        for (int i = x0; i < x1; i++)
            row[i] = c;
    }
}

void Canvas::fillAlpha(int x, int y, int w, int h, Color c, int alpha)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            plot(x + i, y + j, c, alpha);
}

void Canvas::vgradient(int x, int y, int w, int h, Color top, Color bottom)
{
    for (int j = 0; j < h; j++)
        fill(x, y + j, w, 1, blend(top, bottom, h > 1 ? j * 256 / (h - 1) : 0));
}

// Coverage (0..256) of the pixel at (px, py) by a rectangle with corners of
// radius r: the distance from the pixel centre to the rectangle shrunk by r,
// compared against r, with a one-pixel ramp at the edge.
static int roundCoverage(int px, int py, int x, int y, int w, int h, float r)
{
    float cx = px + 0.5f, cy = py + 0.5f;
    float ix0 = x + r, ix1 = x + w - r, iy0 = y + r, iy1 = y + h - r;
    float dx = cx < ix0 ? ix0 - cx : (cx > ix1 ? cx - ix1 : 0);
    float dy = cy < iy0 ? iy0 - cy : (cy > iy1 ? cy - iy1 : 0);
    if (dx == 0 && dy == 0)
        return 256;
    float d = (dx == 0) ? dy : (dy == 0 ? dx : sqrtf(dx * dx + dy * dy));
    float cov = r - d + 0.5f;
    if (cov <= 0)
        return 0;
    if (cov >= 1)
        return 256;
    return (int)(cov * 256);
}

void Canvas::roundRect(int x, int y, int w, int h, int r, Color c)
{
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    fill(x, y + r, w, h - 2 * r, c);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < w; i++)
        {
            plot(x + i, y + j, c, roundCoverage(x + i, y + j, x, y, w, h, (float)r));
            plot(x + i, y + h - 1 - j, c,
                 roundCoverage(x + i, y + h - 1 - j, x, y, w, h, (float)r));
        }
}

void Canvas::roundRectOutline(int x, int y, int w, int h, int r, Color c)
{
    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
        {
            bool edge = i == 0 || j == 0 || i == w - 1 || j == h - 1;
            bool corner = (i < r || i >= w - r) && (j < r || j >= h - r);
            if (!edge && !corner)
                continue;
            int outer = roundCoverage(x + i, y + j, x, y, w, h, (float)r);
            int inner = roundCoverage(x + i, y + j, x + 1, y + 1, w - 2, h - 2,
                                      (float)(r > 1 ? r - 1 : 0));
            plot(x + i, y + j, c, outer - inner);
        }
}

void Canvas::circle(int cx, int cy, int r, Color c)
{
    roundRect(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

void Canvas::triangle(int cx, int cy, int halfWidth, int dir, Color c)
{
    // Rows run from the tip (width 0) to the base (width 2 * halfWidth); the
    // two slanted edges get half coverage on their outermost pixel.
    int rows = halfWidth;
    int tipY = dir < 0 ? cy - rows / 2 : cy + rows / 2;
    for (int k = 0; k < rows; k++)
    {
        int y = dir < 0 ? tipY + k : tipY - k;
        int half = k;
        fill(cx - half, y, 2 * half, 1, c);
        plot(cx - half - 1, y, c, 128);
        plot(cx + half, y, c, 128);
    }
}

void Canvas::glyph(const Font &f, uint32_t cp, int x, int y, Color c)
{
    const Glyph &g = findGlyph(f, cp);
    const uint8_t *bits = f.bits + g.off;
    int n = 0;
    for (int j = 0; j < g.h; j++)
        for (int i = 0; i < g.w; i++, n++)
        {
            int v = (bits[n >> 1] >> ((n & 1) * 4)) & 15;
            if (v)
                plot(x + g.x + i, y + g.y + j, c, v == 15 ? 256 : v * 17);
        }
}

int textWidth(const Font &f, const std::string &s)
{
    int w = 0;
    const char *p = s.c_str(), *end = p + s.size();
    while (p < end)
        w += findGlyph(f, utf8Next(p, end)).adv;
    return w;
}

static const uint32_t kEllipsis = 0x2026;

int Canvas::text(const Font &f, int x, int y, const std::string &s, Color c, int maxWidth)
{
    int ellipsisW = 0;
    if (maxWidth > 0 && textWidth(f, s) > maxWidth)
        ellipsisW = findGlyph(f, kEllipsis).adv;

    int x0 = x;
    const char *p = s.c_str(), *end = p + s.size();
    while (p < end)
    {
        uint32_t cp = utf8Next(p, end);
        int adv = findGlyph(f, cp).adv;
        if (ellipsisW && x + adv - x0 > maxWidth - ellipsisW)
        {
            glyph(f, kEllipsis, x, y, c);
            return x + ellipsisW;
        }
        glyph(f, cp, x, y, c);
        x += adv;
    }
    return x;
}

int Canvas::textMiddle(const Font &f, int x, int y, int h, const std::string &s, Color c,
                       int maxWidth)
{
    // Centre the capitals rather than the whole line box: a line box includes
    // descender space, which makes centred labels sit visibly high.
    int cap = f.ascent * 72 / 100;
    int top = y + (h - cap + 1) / 2 - (f.ascent - cap);
    return text(f, x, top, s, c, maxWidth);
}

void Canvas::textCentered(const Font &f, int cx, int y, const std::string &s, Color c)
{
    text(f, cx - textWidth(f, s) / 2, y, s, c);
}

void Canvas::textRight(const Font &f, int rx, int y, const std::string &s, Color c)
{
    text(f, rx - textWidth(f, s), y, s, c);
}

// ---------------------------------------------------------------------------
// wrapText
// ---------------------------------------------------------------------------
std::vector<std::string> wrapText(const Font &f, const std::string &s, int maxWidth,
                                  int maxLines)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < s.size() && (int)lines.size() < maxLines)
    {
        // Walk forward one code point at a time, remembering the last place a
        // break is allowed, until the line would overflow.
        const char *base = s.c_str();
        const char *p = base + start, *end = base + s.size();
        int w = 0;
        size_t lastBreak = std::string::npos, cut = s.size();
        while (p < end)
        {
            const char *before = p;
            uint32_t cp = utf8Next(p, end);
            if (cp == '\n')
            {
                cut = (size_t)(before - base);
                break;
            }
            w += findGlyph(f, cp).adv;
            if (w > maxWidth && before > base + start)
            {
                cut = (lastBreak != std::string::npos) ? lastBreak : (size_t)(before - base);
                break;
            }
            if (cp == ' ' || cp == '/')
                lastBreak = (size_t)(p - base);
        }

        std::string line = s.substr(start, cut - start);
        while (!line.empty() && line[line.size() - 1] == ' ')
            line.erase(line.size() - 1);
        lines.push_back(line);

        start = cut;
        if (start < s.size() && s[start] == '\n')
            start++;
        while (start < s.size() && s[start] == ' ')
            start++;
    }

    if (start < s.size() && !lines.empty())
    {
        // Out of lines: mark the last one as cut, trimming it so the ellipsis
        // still fits.
        std::string &last = lines.back();
        int ell = findGlyph(f, kEllipsis).adv;
        while (!last.empty() && textWidth(f, last) + ell > maxWidth)
        {
            size_t n = last.size() - 1;
            while (n > 0 && ((unsigned char)last[n] & 0xC0) == 0x80)
                n--;
            last.erase(n);
        }
        last += "\xE2\x80\xA6";
    }
    return lines;
}

} // namespace ui
