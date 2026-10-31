// The on-device UI: the log's scrollback, the touch buttons, and the layouts.
// Drawing itself is checked by eye (make preview); what is tested here is what
// would be wrong without looking wrong — a colour that leaks into the next
// line, a scroll position that jumps while someone reads, a tap that lands on
// a different button than the one pressed, a label cut to "F…".

#include <string.h>

#include "framework.h"

#include "../source/ui/canvas.h"
#include "../source/ui/logbuffer.h"
#include "../source/ui/screens.h"
#include "../source/ui/touchinput.h"
#include "../source/utils/console.h"

using namespace ui;

static void put(LogBuffer &log, const std::string &s) { log.append(s.data(), s.size()); }

static std::string rowText(const LogBuffer &log, size_t n)
{
    std::string out;
    for (auto &cell : log.row(n))
        out += cell.cp < 0x80 ? (char)cell.cp : '#';
    return out;
}

// ---------------------------------------------------------------------------
// LogBuffer
// ---------------------------------------------------------------------------
TEST(logbuffer, splits_lines_and_ignores_the_empty_cursor_row)
{
    LogBuffer log(20, 100);
    put(log, "one\ntwo\n");
    CHECK_EQ(log.endRow(), (size_t)2);
    CHECK_STR_EQ(rowText(log, 0), "one");
    CHECK_STR_EQ(rowText(log, 1), "two");

    put(log, "thr");
    CHECK_EQ(log.endRow(), (size_t)3); // a partial line is shown as it arrives
    CHECK_STR_EQ(rowText(log, 2), "thr");
}

TEST(logbuffer, colours_follow_console_escapes_and_reset)
{
    LogBuffer log(40, 100);
    put(log, "a" CONSOLE_RED "b" CONSOLE_RESET "c\n" CONSOLE_MAGENTA "dbg: x" CONSOLE_RESET);
    CHECK_STR_EQ(rowText(log, 0), "abc");
    CHECK_EQ((int)log.row(0)[0].color, (int)LOG_DEFAULT);
    CHECK_EQ((int)log.row(0)[1].color, (int)LOG_RED);
    CHECK_EQ((int)log.row(0)[2].color, (int)LOG_DEFAULT);
    CHECK_EQ((int)log.row(1)[0].color, (int)LOG_MAGENTA);
}

TEST(logbuffer, escape_and_utf8_split_across_writes)
{
    // printf is unbuffered and may hand a sequence over in two pieces.
    LogBuffer log(40, 100);
    put(log, "\x1b[3");
    put(log, "2;1mok\xC3");
    put(log, "\xA9\n");
    CHECK_EQ(log.row(0).size(), (size_t)3);
    CHECK_EQ((int)log.row(0)[0].cp, (int)'o');
    CHECK_EQ((int)log.row(0)[0].color, (int)LOG_GREEN);
    CHECK_EQ((int)log.row(0)[2].cp, 0xE9);
}

TEST(logbuffer, broken_utf8_shows_a_replacement_and_keeps_going)
{
    LogBuffer log(40, 100);
    put(log, "a\xC3z\xFF\n");
    CHECK_EQ(log.row(0).size(), (size_t)4);
    CHECK_EQ((int)log.row(0)[1].cp, 0xFFFD);
    CHECK_EQ((int)log.row(0)[2].cp, (int)'z');
    CHECK_EQ((int)log.row(0)[3].cp, 0xFFFD);
}

TEST(logbuffer, wraps_at_the_last_space)
{
    LogBuffer log(10, 100);
    put(log, "Listing Dropbox:saves\n");
    CHECK_STR_EQ(rowText(log, 0), "Listing ");
    CHECK_STR_EQ(rowText(log, 1), "Dropbox:sa");
    CHECK_STR_EQ(rowText(log, 2), "ves");
    CHECK_EQ(log.endRow(), (size_t)3);
}

TEST(logbuffer, a_line_of_exactly_the_width_takes_one_row)
{
    LogBuffer log(5, 100);
    put(log, "abcde\nf\n");
    CHECK_EQ(log.endRow(), (size_t)2);
    CHECK_STR_EQ(rowText(log, 1), "f");
}

TEST(logbuffer, keeps_the_newest_rows_and_counts_the_rest)
{
    LogBuffer log(20, 4);
    for (int i = 0; i < 10; i++)
        put(log, "line " + std::to_string(i) + "\n");
    CHECK(log.firstRow() > 0);
    CHECK_STR_EQ(rowText(log, log.endRow() - 1), "line 9");
    CHECK(log.endRow() - log.firstRow() <= 4);
}

// ---------------------------------------------------------------------------
// LogScroll
// ---------------------------------------------------------------------------
static void lines(LogBuffer &log, int n)
{
    for (int i = 0; i < n; i++)
        put(log, "x\n");
}

TEST(logscroll, follows_new_output)
{
    LogBuffer log(20, 1000);
    LogScroll scroll(5);
    lines(log, 3);
    scroll.update(log);
    CHECK_EQ(scroll.top(), (size_t)0);
    lines(log, 10);
    scroll.update(log);
    CHECK_EQ(scroll.top(), (size_t)8);
    CHECK(scroll.following());
}

TEST(logscroll, holds_still_while_scrolled_back)
{
    LogBuffer log(20, 1000);
    LogScroll scroll(5);
    lines(log, 20);
    scroll.update(log);
    scroll.scrollBy(log, -3);
    CHECK(!scroll.following());
    CHECK_EQ(scroll.top(), (size_t)12);

    lines(log, 7);
    scroll.update(log);
    CHECK_EQ(scroll.top(), (size_t)12);
    CHECK_EQ(scroll.unseenRows(log), (size_t)10);

    // Back at the bottom it follows again.
    scroll.scrollBy(log, 100);
    CHECK(scroll.following());
    lines(log, 1);
    scroll.update(log);
    CHECK_EQ(scroll.top(), (size_t)23);
}

TEST(logscroll, clamps_at_the_top_and_when_rows_are_dropped)
{
    LogBuffer log(20, 10);
    LogScroll scroll(4);
    lines(log, 8);
    scroll.scrollBy(log, -100);
    CHECK_EQ(scroll.top(), log.firstRow());

    lines(log, 20); // the rows being read are trimmed away underneath
    scroll.update(log);
    CHECK(scroll.top() >= log.firstRow());
    CHECK(scroll.top() + 4 <= log.endRow());
}

TEST(logscroll, drag_fraction_maps_onto_the_range)
{
    LogBuffer log(20, 1000);
    LogScroll scroll(5);
    lines(log, 25);
    scroll.scrollToFraction(log, 0);
    CHECK_EQ(scroll.top(), (size_t)0);
    CHECK(!scroll.following());
    scroll.scrollToFraction(log, 0.5f);
    CHECK_EQ(scroll.top(), (size_t)10);
    scroll.scrollToFraction(log, 1);
    CHECK(scroll.following());
}

TEST(logscroll, short_log_cannot_scroll)
{
    LogBuffer log(20, 1000);
    LogScroll scroll(5);
    lines(log, 2);
    scroll.scrollBy(log, -1);
    CHECK_EQ(scroll.top(), (size_t)0);
    CHECK(scroll.following());
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------
TEST(text, wraps_paths_between_folders)
{
    std::string path = "/3ds/Checkpoint/saves/0x0055D Pokemon Y/main";
    int width = textWidth(kFontBody, "/3ds/Checkpoint/saves/0x");
    std::vector<std::string> wrapped = wrapText(kFontBody, path, width, 4);
    CHECK(wrapped.size() >= 2);
    CHECK_STR_EQ(wrapped[0], "/3ds/Checkpoint/saves/");
    for (auto &l : wrapped)
        CHECK(textWidth(kFontBody, l) <= width);
    CHECK_STR_EQ(wrapped.back().substr(wrapped.back().size() - 5), "/main");
}

TEST(text, out_of_lines_ends_in_an_ellipsis)
{
    std::vector<std::string> wrapped =
        wrapText(kFontBody, "one two three four five six seven", 40, 2);
    CHECK_EQ(wrapped.size(), (size_t)2);
    const std::string &last = wrapped[1];
    CHECK(last.size() >= 3 && last.compare(last.size() - 3, 3, "\xE2\x80\xA6") == 0);
    CHECK(textWidth(kFontBody, last) <= 40);
}

TEST(text, every_font_carries_the_punctuation_the_app_prints)
{
    // "—" and "…" are all over 3DSync's messages; a missing glyph renders as
    // '?', which is legible but wrong.
    const Font *fonts[] = {&kFontSmall, &kFontBody, &kFontBold, &kFontTitle, &kFontMono};
    for (const Font *f : fonts)
    {
        CHECK_EQ((int)findGlyph(*f, 0x2014).cp, 0x2014);
        CHECK_EQ((int)findGlyph(*f, 0x2026).cp, 0x2026);
        CHECK_EQ((int)findGlyph(*f, 0xE9).cp, 0xE9);
        CHECK_EQ((int)findGlyph(*f, 0x4E00).cp, (int)'?');
    }
}

TEST(text, mono_font_matches_the_log_grid)
{
    for (uint32_t cp = 0x21; cp < 0x7F; cp++)
        CHECK_EQ((int)findGlyph(kFontMono, cp).adv, LOG_ADVANCE);
    CHECK(LOG_COLUMNS >= 50); // what the app's messages were written for
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------
static std::vector<Button> menuButtons()
{
    BottomModel m;
    m.mode = SCREEN_MENU;
    m.starting = false;
    return bottomButtons(m);
}

static const Button &buttonFor(const std::vector<Button> &b, uint32_t key)
{
    for (auto &x : b)
        if (x.key == key)
            return x;
    static Button none;
    return none;
}

TEST(touch, tap_fires_the_key_on_release)
{
    std::vector<Button> b = menuButtons();
    const Rect &r = buttonFor(b, K_A).rect;
    TouchInput t;
    TouchResult res = t.update(true, r.x + 5, r.y + 5, b);
    CHECK_EQ(res.pressed, 0u);
    CHECK_EQ(res.held, K_A);
    res = t.update(true, r.x + 8, r.y + 6, b);
    CHECK_EQ(res.pressed, 0u);
    res = t.update(false, 0, 0, b);
    CHECK_EQ(res.pressed, K_A);
}

TEST(touch, sliding_off_takes_the_tap_back)
{
    std::vector<Button> b = menuButtons();
    const Rect &a = buttonFor(b, K_A).rect;
    const Rect &y = buttonFor(b, K_Y).rect;
    TouchInput t;
    t.update(true, a.x + 5, a.y + 5, b);
    TouchResult res = t.update(true, y.x + 5, y.y + 5, b);
    CHECK_EQ(res.held, 0u); // not A any more, and never Y
    res = t.update(false, 0, 0, b);
    CHECK_EQ(res.pressed, 0u);
}

TEST(touch, scroll_arrows_fire_at_once_and_repeat)
{
    std::vector<Button> b = menuButtons();
    TouchInput t;
    int x = STRIP_UP.x + 5, y = STRIP_UP.y + 5;
    TouchResult res = t.update(true, x, y, b);
    CHECK_EQ(res.pressed, K_DUP);
    int fired = 0;
    for (int i = 1; i < REPEAT_DELAY; i++)
        fired += t.update(true, x, y, b).pressed ? 1 : 0;
    CHECK_EQ(fired, 0);
    CHECK_EQ(t.update(true, x, y, b).pressed, K_DUP);
    CHECK_EQ(t.update(false, 0, 0, b).pressed, 0u); // no extra press on release
}

TEST(touch, track_reports_a_drag_position)
{
    std::vector<Button> b = menuButtons();
    TouchInput t;
    TouchResult top = t.update(true, STRIP_TRACK.x + 10, STRIP_TRACK.y + 1, b);
    CHECK(top.dragging);
    CHECK(top.dragFraction == 0);
    TouchResult bottom = t.update(true, STRIP_TRACK.x + 10, STRIP_TRACK.y + STRIP_TRACK.h - 1, b);
    CHECK(bottom.dragging);
    CHECK(bottom.dragFraction == 1);
}

TEST(touch, a_screen_change_swallows_the_touch_in_progress)
{
    std::vector<Button> b = menuButtons();
    const Rect &r = buttonFor(b, K_A).rect;
    TouchInput t;
    t.update(true, r.x + 5, r.y + 5, b);
    t.cancel();
    CHECK_EQ(t.update(true, r.x + 5, r.y + 5, b).held, 0u);
    CHECK_EQ(t.update(false, 0, 0, b).pressed, 0u);
    CHECK_EQ(t.update(true, r.x + 5, r.y + 5, b).held, K_A); // the next one counts
}

TEST(touch, menu_is_inert_until_the_configuration_is_read)
{
    BottomModel m; // starting
    std::vector<Button> b = bottomButtons(m);
    const Rect &r = buttonFor(b, K_A).rect;
    TouchInput t;
    t.update(true, r.x + 5, r.y + 5, b);
    CHECK_EQ(t.update(false, 0, 0, b).pressed, 0u);
}

TEST(touch, a_disabled_button_does_nothing)
{
    BottomModel m;
    m.mode = SCREEN_RUNNING;
    m.cancelling = true;
    std::vector<Button> b = bottomButtons(m);
    const Rect &r = buttonFor(b, K_START).rect;
    TouchInput t;
    t.update(true, r.x + 5, r.y + 5, b);
    CHECK_EQ(t.update(false, 0, 0, b).pressed, 0u);
}

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------
static uint32_t keysOf(const std::vector<Button> &b)
{
    uint32_t k = 0;
    for (auto &x : b)
        k |= x.key;
    return k;
}

TEST(layout, each_screen_offers_the_keys_it_handles)
{
    BottomModel m;
    const uint32_t scroll = K_DUP | K_DDOWN;
    m.mode = SCREEN_MENU;
    CHECK_EQ(keysOf(bottomButtons(m)), K_A | K_Y | K_START | scroll);
    m.mode = SCREEN_RUNNING;
    CHECK_EQ(keysOf(bottomButtons(m)), K_START | scroll);
    m.mode = SCREEN_CONFLICT;
    CHECK_EQ(keysOf(bottomButtons(m)), K_A | K_B | K_X | K_L | K_START | scroll);
    m.mode = SCREEN_SAVE_LOG;
    CHECK_EQ(keysOf(bottomButtons(m)), K_A | K_B | scroll);
}

TEST(layout, a_conflict_between_more_than_two_steps_with_r)
{
    BottomModel m;
    m.mode = SCREEN_CONFLICT;
    m.conflictSides = {"3DS", "Drive"};
    const uint32_t scroll = K_DUP | K_DDOWN;
    CHECK_EQ(keysOf(bottomButtons(m)), K_A | K_B | K_X | K_L | K_START | scroll);
    CHECK_STR_EQ(buttonFor(bottomButtons(m), K_A).label, "Keep 3DS");
    CHECK_STR_EQ(buttonFor(bottomButtons(m), K_B).label, "Keep Drive");
    // "Keep Dropbox" does not fit half the row: both drop "Keep", not one.
    m.conflictSides = {"3DS", "Dropbox"};
    CHECK_STR_EQ(buttonFor(bottomButtons(m), K_A).label, "3DS");
    CHECK_STR_EQ(buttonFor(bottomButtons(m), K_B).label, "Dropbox");

    m.conflictSides = {"3DS", "Dropbox", "WebDAV"};
    m.conflictPick = 2;
    CHECK_EQ(keysOf(bottomButtons(m)), K_A | K_R | K_X | K_L | K_START | scroll);
    CHECK_STR_EQ(buttonFor(bottomButtons(m), K_A).label, "Keep WebDAV");
}

// Every screen, and the conflict screen with the side counts and name lengths
// it can be given.
static std::vector<BottomModel> everyLayout()
{
    std::vector<BottomModel> out;
    ScreenMode modes[] = {SCREEN_MENU, SCREEN_RUNNING, SCREEN_CONFLICT, SCREEN_SAVE_LOG};
    for (ScreenMode mode : modes)
    {
        BottomModel m;
        m.mode = mode;
        out.push_back(m);
    }
    std::vector<std::vector<std::string> > sides = {
        {"3DS", "Dropbox"},
        {"Dropbox", "WebDAV"},
        {"3DS", "SMB (a long label here)"},
        {"3DS", "Dropbox", "Drive", "SMB", "FTP", "WebDAV (a long label here)"}};
    for (auto &s : sides)
        for (int pick = 0; pick < (int)s.size(); pick++)
        {
            BottomModel m;
            m.mode = SCREEN_CONFLICT;
            m.conflictSides = s;
            m.conflictPick = pick;
            out.push_back(m);
        }
    return out;
}

TEST(layout, buttons_are_on_screen_apart_and_big_enough)
{
    for (const BottomModel &m : everyLayout())
    {
        std::vector<Button> b = bottomButtons(m);
        for (size_t i = 0; i < b.size(); i++)
        {
            const Rect &r = b[i].rect;
            CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= BOTTOM_W && r.y + r.h <= BOTTOM_H);
            CHECK(r.w >= 30 && r.h >= 30); // a fingertip, not just a stylus
            for (size_t j = i + 1; j < b.size(); j++)
            {
                const Rect &o = b[j].rect;
                bool apart = r.x + r.w <= o.x || o.x + o.w <= r.x || r.y + r.h <= o.y ||
                             o.y + o.h <= r.y;
                CHECK(apart);
            }
        }
    }
}

TEST(layout, no_label_is_cut_short)
{
    // The key badge, a gap and the label, with a little padding either side,
    // must fit — otherwise the label ends in "…" and says nothing.
    for (const BottomModel &m : everyLayout())
    {
        for (auto &b : bottomButtons(m))
        {
            if (b.label.empty())
                continue;
            // A side's own name is the one label that cannot be made to fit
            // in every case; it is clipped, and "Keep" is dropped first.
            if (b.label.find("a long label") != std::string::npos)
                continue;
            bool face = b.key == K_A || b.key == K_B || b.key == K_X || b.key == K_Y;
            const char *badgeText = b.key == K_START ? "START" : (b.key == K_R ? "R" : "L");
            int badge = face ? 18 : textWidth(kFontSmall, badgeText) + 12;
            int need = badge + 7 + textWidth(kFontBold, b.label) + 14;
            if (need > b.rect.w)
                testFail(__FILE__, __LINE__, "label does not fit: " + b.label);
        }
    }
}
