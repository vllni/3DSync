#include "screens.h"

#include <stdio.h>

#include "../version.h"

namespace ui
{

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
static const Color C_BG = rgb(0x10, 0x13, 0x1A);
static const Color C_BG2 = rgb(0x18, 0x1C, 0x26);
static const Color C_PANEL = rgb(0x1E, 0x23, 0x2E);
static const Color C_RAISED = rgb(0x2A, 0x31, 0x3F);
static const Color C_PRESSED = rgb(0x3A, 0x45, 0x5A);
static const Color C_LINE = rgb(0x2C, 0x33, 0x40);
static const Color C_TEXT = rgb(0xEC, 0xEF, 0xF4);
static const Color C_DIM = rgb(0x95, 0x9E, 0xB0);
static const Color C_FAINT = rgb(0x55, 0x5E, 0x70);
static const Color C_ACCENT = rgb(0x4F, 0x8E, 0xF7);
static const Color C_ACCENT_DOWN = rgb(0x6C, 0xA2, 0xFF);
static const Color C_DANGER = rgb(0xC9, 0x4A, 0x4A);
static const Color C_DANGER_DOWN = rgb(0xE0, 0x62, 0x62);
static const Color C_GOOD = rgb(0x5F, 0xD3, 0x93);
static const Color C_BAD = rgb(0xFF, 0x70, 0x70);
static const Color C_WARN = rgb(0xFF, 0xC1, 0x5C);
static const Color C_DEBUG = rgb(0xC7, 0x7D, 0xFF);

// Log colours for LogColor, readable on C_BG.
static const Color kLogColors[] = {
    rgb(0xD5, 0xDA, 0xE3), // default
    rgb(0x6B, 0x74, 0x86), // black
    rgb(0xFF, 0x70, 0x70), // red
    rgb(0x6E, 0xDB, 0x9C), // green
    rgb(0xFF, 0xC8, 0x6A), // yellow
    rgb(0x6F, 0xA3, 0xFF), // blue
    rgb(0xC7, 0x8C, 0xFF), // magenta — debugf() lines
    rgb(0x5E, 0xD6, 0xE6), // cyan
    rgb(0xFF, 0xFF, 0xFF), // white
};

BottomModel::BottomModel()
    : mode(SCREEN_MENU), debugRun(false), starting(true), configLoaded(false), pathCount(0),
      lastResultOk(true), elapsedSeconds(0), spinnerFrame(0), cancelling(false),
      conflictPick(0), applyToAll(false), logKilobytes(0), logTrimmed(false), heldKeys(0), logFirst(0),
      logEnd(0), logTop(0), logVisible(LOG_ROWS)
{
}

static const char *keyName(uint32_t key)
{
    switch (key)
    {
    case K_A: return "A";
    case K_B: return "B";
    case K_X: return "X";
    case K_Y: return "Y";
    case K_L: return "L";
    case K_R: return "R";
    case K_START: return "START";
    case K_SELECT: return "SELECT";
    default: return "";
    }
}

// The face buttons are round and lettered on the console; the shoulder and
// START buttons are drawn as a pill so the badge reads as the same shape.
static bool isFaceKey(uint32_t key)
{
    return key == K_A || key == K_B || key == K_X || key == K_Y;
}

static int badgeWidth(uint32_t key)
{
    if (!keyName(key)[0])
        return 0;
    return isFaceKey(key) ? 18 : textWidth(kFontSmall, keyName(key)) + 12;
}

// Badge, gap and label: what a button needs before any padding.
static int contentWidth(const Button &b)
{
    return badgeWidth(b.key) + 7 + textWidth(kFontBold, b.label);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
// Left column: the card (36..154) and two rows of buttons.  Right column:
// the scroll strip.
static const int COL_X = 6, COL_W = 270;
static const int ROW1_Y = 160, ROW2_Y = 200, BTN_H = 34, GAP = 6;

static Button makeButton(int x, int y, int w, uint32_t key, const std::string &label,
                         ButtonStyle style = BUTTON_NORMAL, bool enabled = true)
{
    Button b;
    b.rect.x = x;
    b.rect.y = y;
    b.rect.w = w;
    b.rect.h = BTN_H;
    b.key = key;
    b.label = label;
    b.style = style;
    b.enabled = enabled;
    b.repeats = false;
    return b;
}

// n equal buttons across the column at height y, starting at slot `from`.
static int slotX(int i, int n) { return COL_X + i * (COL_W + GAP) / n; }
static int slotW(int n) { return (COL_W - (n - 1) * GAP) / n; }

// "Keep Dropbox", or just "Dropbox" where that would not fit with a little
// padding either side.
static std::string keepLabel(uint32_t key, const std::string &side, int width)
{
    Button b = makeButton(0, 0, width, key, "Keep " + side);
    if (contentWidth(b) + 14 > width)
        b.label = side;
    return b.label;
}

std::vector<Button> bottomButtons(const BottomModel &m)
{
    std::vector<Button> b;
    switch (m.mode)
    {
    case SCREEN_MENU:
        b.push_back(makeButton(COL_X, ROW1_Y, COL_W, K_A, "Sync now", BUTTON_PRIMARY,
                               !m.starting));
        b.push_back(makeButton(slotX(0, 2), ROW2_Y, slotW(2), K_Y, "Debug sync", BUTTON_NORMAL,
                               !m.starting));
        b.push_back(makeButton(slotX(1, 2), ROW2_Y, slotW(2), K_START, "Exit", BUTTON_NORMAL,
                               !m.starting));
        break;
    case SCREEN_RUNNING:
        b.push_back(makeButton(COL_X, ROW2_Y, COL_W, K_START,
                               m.cancelling ? "Cancelling\xE2\x80\xA6" : "Cancel sync",
                               BUTTON_DANGER, !m.cancelling));
        break;
    case SCREEN_CONFLICT:
        if (m.conflictSides.size() <= 2)
        {
            const std::vector<std::string> &s = m.conflictSides;
            std::string first = s.size() > 0 ? s[0] : "3DS";
            std::string second = s.size() > 1 ? s[1] : "remote";
            std::string a = keepLabel(K_A, first, slotW(2));
            std::string b2 = keepLabel(K_B, second, slotW(2));
            // Both say "Keep", or neither does.
            if (a == first || b2 == second)
            {
                a = first;
                b2 = second;
            }
            b.push_back(makeButton(slotX(0, 2), ROW1_Y, slotW(2), K_A, a, BUTTON_PRIMARY));
            b.push_back(makeButton(slotX(1, 2), ROW1_Y, slotW(2), K_B, b2, BUTTON_PRIMARY));
        }
        else
        {
            // Too many sides for a key each: R steps through them, A keeps the
            // one it names.
            Button next = makeButton(0, ROW1_Y, 0, K_R, "Next");
            next.rect.w = contentWidth(next) + 24;
            next.rect.x = COL_X + COL_W - next.rect.w;
            int pick = m.conflictPick >= 0 && m.conflictPick < (int)m.conflictSides.size()
                           ? m.conflictPick
                           : 0;
            int w = COL_W - next.rect.w - GAP;
            b.push_back(makeButton(COL_X, ROW1_Y, w, K_A,
                                   keepLabel(K_A, m.conflictSides[pick], w), BUTTON_PRIMARY));
            b.push_back(next);
        }
        {
            // Three to a row is too tight for equal thirds once START's badge
            // is in one of them, so each gets its content plus a share of
            // what is left over.
            Button row[3] = {makeButton(0, ROW2_Y, 0, K_X, "Skip"),
                             makeButton(0, ROW2_Y, 0, K_L, "For all", BUTTON_TOGGLE),
                             makeButton(0, ROW2_Y, 0, K_START, "Stop", BUTTON_DANGER)};
            int natural[3], used = 2 * GAP;
            for (int i = 0; i < 3; i++)
            {
                natural[i] = contentWidth(row[i]) + 16;
                used += natural[i];
            }
            int x = COL_X, spare = COL_W - used;
            for (int i = 0; i < 3; i++)
            {
                row[i].rect.x = x;
                row[i].rect.w = i == 2 ? COL_X + COL_W - x : natural[i] + spare / 3;
                x += row[i].rect.w + GAP;
                b.push_back(row[i]);
            }
        }
        break;
    case SCREEN_SAVE_LOG:
        b.push_back(makeButton(slotX(0, 2), ROW2_Y, slotW(2), K_A, "Save log", BUTTON_PRIMARY));
        b.push_back(makeButton(slotX(1, 2), ROW2_Y, slotW(2), K_B, "Discard"));
        break;
    }

    Button up = makeButton(0, 0, 0, K_DUP, "");
    up.rect = STRIP_UP;
    up.repeats = true;
    b.push_back(up);
    Button down = makeButton(0, 0, 0, K_DDOWN, "");
    down.rect = STRIP_DOWN;
    down.repeats = true;
    b.push_back(down);
    return b;
}

int hitButton(const std::vector<Button> &buttons, int x, int y)
{
    for (size_t i = 0; i < buttons.size(); i++)
        if (buttons[i].enabled && buttons[i].rect.contains(x, y))
            return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// Pieces
// ---------------------------------------------------------------------------
static int drawKeyBadge(Canvas &c, int x, int cy, uint32_t key, Color bg, Color fg)
{
    const char *name = keyName(key);
    if (isFaceKey(key))
    {
        c.circle(x + 9, cy, 9, bg);
        c.textCentered(kFontBold, x + 9, cy - 9, name, fg);
        return 18;
    }
    int w = textWidth(kFontSmall, name) + 12;
    c.roundRect(x, cy - 8, w, 16, 8, bg);
    c.textCentered(kFontSmall, x + w / 2, cy - 7, name, fg);
    return w;
}

static void drawButton(Canvas &c, const Button &b, const BottomModel &m)
{
    bool down = b.enabled && (m.heldKeys & b.key);
    bool lit = b.style == BUTTON_TOGGLE && m.applyToAll;
    Color bg, fg = C_TEXT, badgeBg = C_BG, badgeFg = C_TEXT;
    switch (b.style)
    {
    case BUTTON_PRIMARY:
        bg = down ? C_ACCENT_DOWN : C_ACCENT;
        badgeBg = rgb(0x24, 0x4F, 0x9E);
        break;
    case BUTTON_DANGER:
        bg = down ? C_DANGER_DOWN : C_DANGER;
        badgeBg = rgb(0x7A, 0x22, 0x22);
        break;
    default:
        bg = down ? C_PRESSED : (lit ? rgb(0x2B, 0x4A, 0x7C) : C_RAISED);
        break;
    }
    if (!b.enabled)
    {
        bg = C_PANEL;
        fg = C_FAINT;
        badgeBg = C_BG2;
        badgeFg = C_FAINT;
    }

    const Rect &r = b.rect;
    c.roundRect(r.x, r.y, r.w, r.h, 9, bg);
    if (lit)
        c.roundRectOutline(r.x, r.y, r.w, r.h, 9, C_ACCENT);

    int badgeW = badgeWidth(b.key);
    int contentW = contentWidth(b);
    int x = r.x + (r.w - contentW) / 2;
    if (x < r.x + 7)
        x = r.x + 7;
    drawKeyBadge(c, x, r.y + r.h / 2, b.key, badgeBg, badgeFg);
    c.textMiddle(kFontBold, x + badgeW + 7, r.y, r.h, b.label, fg,
                 r.x + r.w - (x + badgeW + 7) - 4);

    if (b.style == BUTTON_TOGGLE)
    {
        // The state as a tick on the corner, so the label can keep saying
        // what the key does and the button need not be any wider.
        int cx = r.x + r.w - 4, cy = r.y + 4;
        c.circle(cx, cy, 7, lit ? C_ACCENT : C_PANEL);
        c.roundRectOutline(cx - 7, cy - 7, 14, 14, 7, lit ? C_TEXT : C_FAINT);
        if (lit)
        {
            c.fill(cx - 3, cy, 2, 2, C_TEXT);
            c.fill(cx - 1, cy + 1, 2, 2, C_TEXT);
            c.fill(cx + 1, cy - 3, 2, 5, C_TEXT);
        }
    }
}

static void drawArrowButton(Canvas &c, const Button &b, const BottomModel &m, int dir)
{
    bool down = (m.heldKeys & b.key) != 0;
    const Rect &r = b.rect;
    c.roundRect(r.x, r.y, r.w, r.h, 9, down ? C_PRESSED : C_RAISED);
    c.triangle(r.x + r.w / 2, r.y + r.h / 2, 7, dir, down ? C_TEXT : C_DIM);
}

static void drawScrollTrack(Canvas &c, const BottomModel &m)
{
    const Rect &t = STRIP_TRACK;
    c.roundRect(t.x, t.y, t.w, t.h, 9, C_PANEL);
    int rail = t.x + t.w / 2 - 1;
    c.fill(rail, t.y + 8, 2, t.h - 16, C_LINE);

    size_t total = m.logEnd > m.logFirst ? m.logEnd - m.logFirst : 0;
    int inner = t.h - 8;
    int thumb = inner, pos = 0;
    if (total > (size_t)m.logVisible)
    {
        thumb = (int)(inner * (size_t)m.logVisible / total);
        if (thumb < 16)
            thumb = 16;
        size_t range = total - m.logVisible;
        pos = (int)((inner - thumb) * (m.logTop - m.logFirst) / range);
    }
    c.roundRect(t.x + 9, t.y + 4 + pos, t.w - 18, thumb, 6, total ? C_FAINT : C_LINE);
}

static void drawStatusPill(Canvas &c, int rx, const char *text, Color dot)
{
    int w = textWidth(kFontSmall, text) + 24;
    c.roundRect(rx - w, 8, w, 18, 9, C_PANEL);
    c.circle(rx - w + 10, 17, 3, dot);
    c.text(kFontSmall, rx - w + 17, 10, text, C_DIM);
}

static void drawHeader(Canvas &c, const BottomModel &m)
{
    c.vgradient(0, 0, BOTTOM_W, 32, C_BG2, C_BG);
    int x = c.text(kFontTitle, 10, 5, "3DSync", C_TEXT);
    c.text(kFontSmall, x + 6, 14, "v" APP_VERSION, C_FAINT);

    switch (m.mode)
    {
    case SCREEN_MENU:
        drawStatusPill(c, BOTTOM_W - 6, m.starting ? "Starting" : "Ready",
                       m.starting ? C_FAINT : C_GOOD);
        break;
    case SCREEN_RUNNING:
        drawStatusPill(c, BOTTOM_W - 6, m.debugRun ? "Syncing \xC2\xB7 debug" : "Syncing",
                       m.debugRun ? C_DEBUG : C_ACCENT);
        break;
    case SCREEN_CONFLICT:
        drawStatusPill(c, BOTTOM_W - 6, "Your call", C_WARN);
        break;
    case SCREEN_SAVE_LOG:
        drawStatusPill(c, BOTTOM_W - 6, "Debug log", C_DEBUG);
        break;
    }
}

static void drawSpinner(Canvas &c, int cx, int cy, unsigned frame)
{
    // Eight dots, the brightest one stepping round; a tail fades behind it.
    static const int dx[8] = {0, 6, 8, 6, 0, -6, -8, -6};
    static const int dy[8] = {-8, -6, 0, 6, 8, 6, 0, -6};
    for (int i = 0; i < 8; i++)
    {
        int age = (int)((frame - i) & 7);
        Color col = age == 0 ? C_ACCENT_DOWN : (age < 3 ? C_ACCENT : C_LINE);
        c.circle(cx + dx[i], cy + dy[i], 2, col);
    }
}

static void drawLines(Canvas &c, const Font &f, int x, int y, int lineH,
                      const std::vector<std::string> &lines, Color col)
{
    for (size_t i = 0; i < lines.size(); i++)
        c.text(f, x, y + (int)i * lineH, lines[i], col);
}

// ---------------------------------------------------------------------------
// Cards
// ---------------------------------------------------------------------------
static const int CARD_X = COL_X, CARD_Y = 36, CARD_W = COL_W;
static const int CARD_IN = CARD_X + 12, CARD_TEXT_W = CARD_W - 24;

static void drawMenuCard(Canvas &c, const BottomModel &m)
{
    c.roundRect(CARD_X, CARD_Y, CARD_W, ROW1_Y - GAP - CARD_Y, 12, C_PANEL);
    if (m.starting)
    {
        c.text(kFontTitle, CARD_IN, CARD_Y + 8, "Starting\xE2\x80\xA6", C_DIM);
        return;
    }
    if (!m.configLoaded)
    {
        c.text(kFontTitle, CARD_IN, CARD_Y + 8, "No configuration", C_WARN);
        drawLines(c, kFontBody, CARD_IN, CARD_Y + 38, 17,
                  wrapText(kFontBody, "3DSync.ini could not be read from /3ds/3DSync/. "
                           "Create it with the configurator:", CARD_TEXT_W, 3), C_DIM);
        c.text(kFontBody, CARD_IN, CARD_Y + 89, "3dsync.vllni.dev", C_ACCENT);
        return;
    }

    c.text(kFontTitle, CARD_IN, CARD_Y + 8, "Ready to sync", C_TEXT);
    char paths[48];
    snprintf(paths, sizeof(paths), "%d sync path%s", m.pathCount, m.pathCount == 1 ? "" : "s");
    c.text(kFontSmall, CARD_IN, CARD_Y + 40, "REMOTES", C_FAINT);
    c.text(kFontBody, CARD_IN + 62, CARD_Y + 38,
           m.remotes.empty() ? std::string("none configured") : m.remotes,
           m.remotes.empty() ? C_WARN : C_TEXT, CARD_TEXT_W - 62);
    c.text(kFontSmall, CARD_IN, CARD_Y + 60, "PATHS", C_FAINT);
    c.text(kFontBody, CARD_IN + 62, CARD_Y + 58, paths, m.pathCount ? C_TEXT : C_WARN);
    // Dim while it is the default, so a filter that is on stands out.
    bool allFiles = m.fileFilter.empty() || m.fileFilter == "all files";
    c.text(kFontSmall, CARD_IN, CARD_Y + 80, "FILES", C_FAINT);
    c.text(kFontBody, CARD_IN + 62, CARD_Y + 78, allFiles ? std::string("all files") : m.fileFilter,
           allFiles ? C_DIM : C_TEXT, CARD_TEXT_W - 62);
    c.text(kFontSmall, CARD_IN, CARD_Y + 100, "LAST RUN", C_FAINT);
    if (m.lastResult.empty())
        c.text(kFontBody, CARD_IN + 62, CARD_Y + 98, "not yet", C_DIM);
    else
    {
        c.circle(CARD_IN + 66, CARD_Y + 106, 3, m.lastResultOk ? C_GOOD : C_BAD);
        c.text(kFontBody, CARD_IN + 74, CARD_Y + 98, m.lastResult, C_TEXT, CARD_TEXT_W - 74);
    }
}

static void drawRunningCard(Canvas &c, const BottomModel &m)
{
    c.roundRect(CARD_X, CARD_Y, CARD_W, ROW2_Y - GAP - CARD_Y, 12, C_PANEL);
    if (m.cancelling)
        c.circle(CARD_IN + 10, CARD_Y + 20, 6, C_WARN);
    else
        drawSpinner(c, CARD_IN + 10, CARD_Y + 20, m.spinnerFrame);
    c.text(kFontTitle, CARD_IN + 30, CARD_Y + 8, m.cancelling ? "Stopping" : "Syncing", C_TEXT);

    char clock[16];
    snprintf(clock, sizeof(clock), "%u:%02u", m.elapsedSeconds / 60, m.elapsedSeconds % 60);
    c.textRight(kFontBody, CARD_X + CARD_W - 12, CARD_Y + 12, clock, C_DIM);

    int y = CARD_Y + 44;
    if (!m.provider.empty())
    {
        c.text(kFontSmall, CARD_IN, y + 2, "REMOTE", C_FAINT);
        c.text(kFontBold, CARD_IN + 62, y, m.provider, C_TEXT, CARD_TEXT_W - 62);
        y += 24;
    }
    if (!m.entry.empty())
    {
        c.text(kFontSmall, CARD_IN, y + 2, "PATH", C_FAINT);
        drawLines(c, kFontBody, CARD_IN + 62, y, 17,
                  wrapText(kFontBody, m.entry, CARD_TEXT_W - 62, 3), C_TEXT);
    }

    const char *hint = m.cancelling ? "Finishing the current file, then stopping."
                                    : "The log above shows each step.";
    c.text(kFontSmall, CARD_IN, ROW2_Y - GAP - 22, hint, C_DIM, CARD_TEXT_W);
}

static void drawConflictCard(Canvas &c, const BottomModel &m)
{
    c.roundRect(CARD_X, CARD_Y, CARD_W, ROW1_Y - GAP - CARD_Y, 12, C_PANEL);
    c.fill(CARD_X, CARD_Y + 12, 3, ROW1_Y - GAP - CARD_Y - 24, C_WARN);
    c.text(kFontTitle, CARD_IN, CARD_Y + 8, "Conflict", C_WARN);

    std::string who = "Both sides changed since the last sync.";
    const std::vector<std::string> &s = m.conflictSides;
    if (!s.empty())
    {
        who = "Changed on ";
        for (size_t i = 0; i < s.size(); i++)
            who += (i == 0 ? "" : (i + 1 == s.size() ? " and " : ", ")) + s[i];
        who += ". Keep which?";
    }
    std::vector<std::string> whoLines = wrapText(kFontSmall, who, CARD_TEXT_W, 2);
    drawLines(c, kFontSmall, CARD_IN, CARD_Y + 33, 14, whoLines, C_DIM);

    int extra = 14 * ((int)whoLines.size() - 1);
    drawLines(c, kFontBody, CARD_IN, CARD_Y + 52 + extra, 17,
              wrapText(kFontBody, m.conflictPath, CARD_TEXT_W, extra ? 2 : 3), C_TEXT);
    if (m.applyToAll)
        c.text(kFontSmall, CARD_IN, ROW1_Y - GAP - 20,
               "Used again for every later conflict.", C_ACCENT, CARD_TEXT_W);
}

static void drawSaveLogCard(Canvas &c, const BottomModel &m)
{
    c.roundRect(CARD_X, CARD_Y, CARD_W, ROW2_Y - GAP - CARD_Y, 12, C_PANEL);
    c.text(kFontTitle, CARD_IN, CARD_Y + 8, "Save the debug log?", C_TEXT);
    char size[64];
    snprintf(size, sizeof(size), "%u KB captured%s", m.logKilobytes,
             m.logTrimmed ? ", middle of the run trimmed" : "");
    c.text(kFontBody, CARD_IN, CARD_Y + 38, size, C_DIM, CARD_TEXT_W);
    drawLines(c, kFontBody, CARD_IN, CARD_Y + 64, 17,
              wrapText(kFontBody, "Tokens and passwords are masked, but save paths are not "
                       "\xE2\x80\x94 read it before sharing it.", CARD_TEXT_W, 3), C_TEXT);
    c.text(kFontSmall, CARD_IN, ROW2_Y - GAP - 22, "Saved to /3ds/3DSync/logs/", C_FAINT);
}

void drawBottom(Canvas &c, const BottomModel &m, const std::vector<Button> &buttons)
{
    c.resetClip();
    c.clear(C_BG);
    drawHeader(c, m);

    switch (m.mode)
    {
    case SCREEN_MENU: drawMenuCard(c, m); break;
    case SCREEN_RUNNING: drawRunningCard(c, m); break;
    case SCREEN_CONFLICT: drawConflictCard(c, m); break;
    case SCREEN_SAVE_LOG: drawSaveLogCard(c, m); break;
    }

    drawScrollTrack(c, m);
    for (auto &b : buttons)
    {
        if (b.key == K_DUP)
            drawArrowButton(c, b, m, -1);
        else if (b.key == K_DDOWN)
            drawArrowButton(c, b, m, 1);
        else
            drawButton(c, b, m);
    }
}

// ---------------------------------------------------------------------------
// Top screen
// ---------------------------------------------------------------------------
void drawTop(Canvas &c, const TopModel &m, const LogBuffer &log, const LogScroll &scroll)
{
    c.resetClip();
    c.clear(C_BG);

    // Header: what this is, and where in it the view is.
    c.fill(0, 0, TOP_W, LOG_HEADER_H, C_BG2);
    c.fill(0, LOG_HEADER_H - 1, TOP_W, 1, C_LINE);
    int x = c.text(kFontBold, LOG_X, 2, "Log", C_TEXT);
    if (m.debugRun)
    {
        c.roundRect(x + 8, 4, 46, 13, 6, rgb(0x4A, 0x2F, 0x6E));
        c.textCentered(kFontSmall, x + 31, 4, "DEBUG", C_DEBUG);
    }

    size_t first = log.firstRow(), end = log.endRow(), top = scroll.top();
    char where[64];
    if (scroll.following())
    {
        c.circle(TOP_W - 52, 10, 3, m.busy ? C_GOOD : C_FAINT);
        c.text(kFontSmall, TOP_W - 44, 4, m.busy ? "live" : "end", C_DIM);
    }
    else
    {
        snprintf(where, sizeof(where), "%u\xE2\x80\x93%u of %u",
                 (unsigned)(top - first + 1),
                 (unsigned)(top - first + scroll.visibleRows() < end - first
                                ? top - first + scroll.visibleRows()
                                : end - first),
                 (unsigned)(end - first));
        c.textRight(kFontSmall, TOP_W - LOG_X, 4, where, C_DIM);
    }

    // Rows.
    Rect area = {0, LOG_HEADER_H + 2, TOP_W, TOP_H - LOG_HEADER_H - 2};
    c.setClip(area);
    for (int i = 0; i < scroll.visibleRows(); i++)
    {
        size_t n = top + i;
        if (n >= end)
            break;
        const LogRow &row = log.row(n);
        int y = LOG_HEADER_H + 2 + i * LOG_ROW_H - 1;
        int px = LOG_X;
        for (auto &cell : row)
        {
            if (cell.cp != ' ')
                c.glyph(kFontMono, cell.cp, px, y,
                        kLogColors[cell.color < 9 ? cell.color : 0]);
            px += LOG_ADVANCE;
        }
    }
    c.resetClip();

    // Scroll bar, only when there is somewhere to scroll.
    size_t total = end - first;
    if (total > (size_t)scroll.visibleRows())
    {
        int trackY = LOG_HEADER_H + 4, trackH = TOP_H - trackY - 4;
        int thumb = (int)(trackH * (size_t)scroll.visibleRows() / total);
        if (thumb < 12)
            thumb = 12;
        int pos = (int)((trackH - thumb) * (top - first) / (total - scroll.visibleRows()));
        c.roundRect(TOP_W - LOG_SCROLLBAR_W - 2, trackY + pos, LOG_SCROLLBAR_W, thumb, 2,
                    scroll.following() ? C_LINE : C_FAINT);
    }

    // Scrolled away while output kept coming: say so where the eye is.
    size_t unseen = scroll.following() ? 0 : scroll.unseenRows(log);
    if (unseen > 0)
    {
        char more[48];
        snprintf(more, sizeof(more), "%u new line%s below", (unsigned)unseen,
                 unseen == 1 ? "" : "s");
        int w = textWidth(kFontSmall, more) + 34;
        int px = (TOP_W - w) / 2, py = TOP_H - 24;
        c.roundRect(px, py, w, 18, 9, C_ACCENT);
        c.triangle(px + 14, py + 9, 5, 1, C_TEXT);
        c.text(kFontSmall, px + 24, py + 2, more, C_TEXT);
    }
}

} // namespace ui
